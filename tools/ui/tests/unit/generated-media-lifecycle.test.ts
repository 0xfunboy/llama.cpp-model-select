const navigation = vi.hoisted(() => ({ goto: vi.fn(async () => {}) }));

vi.mock('$app/navigation', () => navigation);

import { AttachmentType, MessageRole, MessageType, StreamConnectionState } from '$lib/enums';
import { ChatService } from '$lib/services/chat.service';
import { DatabaseService } from '$lib/services/database.service';
import { agenticStore } from '$lib/stores/agentic/index.svelte';
import { type ChatFlowsHost, ChatMessageFlows } from '$lib/stores/chat/flows.svelte';
import { chatStore } from '$lib/stores/chat/index.svelte';
import { conversationsStore } from '$lib/stores/conversations/index.svelte';
import { mediaStore } from '$lib/stores/media.svelte';
import type {
	DatabaseConversation,
	DatabaseMessage,
	DatabaseMessageExtraGeneratedMediaCompleted,
	DatabaseMessageExtraGeneratedMediaPending,
	DatabaseMessageExtraGeneratedMediaTerminal
} from '$lib/types';
import { filterByLeafNodeId } from '$lib/utils';
import { afterEach, describe, expect, it, vi } from 'vitest';

function makeConversation(id: string, forkedFromConversationId?: string): DatabaseConversation {
	return {
		currNode: `${id}-node`,
		forkedFromConversationId,
		id,
		lastModified: 1,
		name: id
	};
}

function makeCompletedAsset(
	conversationId: string,
	ownerMessageId: string
): DatabaseMessageExtraGeneratedMediaCompleted {
	return {
		assetId: 'asset-fork-1',
		assetUrl: '/api/media/assets/asset-fork-1',
		conversationId,
		jobId: 'media_fork_1',
		kind: 'image',
		mimeType: 'image/png',
		model: 'pony-v6-xl',
		name: 'asset-fork-1.png',
		ownerMessageId,
		prompt: 'forked marmot',
		size: 1234,
		status: 'completed',
		type: AttachmentType.GENERATED_MEDIA
	};
}

function makePendingAsset(
	conversationId: string,
	ownerMessageId: string
): DatabaseMessageExtraGeneratedMediaPending {
	return {
		conversationId,
		jobId: 'media_pending_1',
		kind: 'image',
		mimeType: 'image/png',
		model: 'pony-v6-xl',
		name: '',
		ownerMessageId,
		prompt: 'pending marmot',
		size: 0,
		status: 'pending',
		type: AttachmentType.GENERATED_MEDIA
	};
}

function makeTerminalAsset(
	conversationId: string,
	ownerMessageId: string
): DatabaseMessageExtraGeneratedMediaTerminal {
	return {
		conversationId,
		error: 'generation failed',
		jobId: 'media_failed_1',
		kind: 'image',
		mimeType: 'image/png',
		model: 'pony-v6-xl',
		name: '',
		ownerMessageId,
		prompt: 'failed marmot',
		size: 0,
		status: 'failed',
		type: AttachmentType.GENERATED_MEDIA
	};
}

function makeMessage(
	id: string,
	role: MessageRole,
	parent: string | null,
	convId = 'conv-flow'
): DatabaseMessage {
	return {
		children: [],
		content: id,
		convId,
		id,
		parent,
		role,
		timestamp: Date.now(),
		toolCalls: '',
		type: MessageType.TEXT
	};
}

function makeFlowsHost(): ChatFlowsHost {
	return {
		cancelPreEncode: vi.fn(),
		cleanupStreaming: vi.fn(),
		clearChatStreaming: vi.fn(),
		createAssistantMessage: vi.fn(async () =>
			makeMessage('replacement-assistant', MessageRole.ASSISTANT, null)
		),
		getApiOptions: vi.fn(() => ({})),
		getOrCreateAbortController: vi.fn(() => new AbortController()),
		isChatLoadingInternal: vi.fn(() => false),
		processing: chatStore.processing,
		setChatLoading: vi.fn(),
		setChatReasoning: vi.fn(),
		setChatStreaming: vi.fn(),
		showErrorDialog: vi.fn(),
		stopGeneration: vi.fn(async () => {}),
		streamChatCompletion: vi.fn(async () => {}),
		streamConnectionState: StreamConnectionState.STREAMING
	};
}

afterEach(() => {
	vi.restoreAllMocks();
	navigation.goto.mockClear();
	conversationsStore.activeConversation = null;
	conversationsStore.activeMessages = [];
	conversationsStore.conversations = [];
	chatStore.cleanupStreaming('conv-flow');
	chatStore.cleanupStreaming('conv-multi-tool');
	chatStore.cleanupStreaming('conv-delete-active');
});

describe('generated-media conversation lifecycle', () => {
	it('cleans every deleted fork owner exactly once', async () => {
		const parent = makeConversation('conv-parent');
		const child = makeConversation('conv-child', parent.id);
		const grandchild = makeConversation('conv-grandchild', child.id);

		conversationsStore.conversations = [parent, child, grandchild];
		const queueCleanup = vi
			.spyOn(mediaStore, 'queueConversationCleanup')
			.mockImplementation(() => {});
		const deleteConversation = vi.spyOn(DatabaseService, 'deleteConversation').mockResolvedValue();
		const flushCleanup = vi.spyOn(mediaStore, 'flushOwnershipOperations').mockResolvedValue();

		await conversationsStore.deleteConversation(parent.id, { deleteWithForks: true });

		expect(DatabaseService.deleteConversation).toHaveBeenCalledWith(parent.id, {
			deleteWithForks: true
		});
		expect(queueCleanup).toHaveBeenCalledOnce();
		expect(new Set(queueCleanup.mock.calls[0][0])).toEqual(
			new Set([parent.id, child.id, grandchild.id])
		);
		expect(flushCleanup).toHaveBeenCalledOnce();
		expect(queueCleanup.mock.invocationCallOrder[0]).toBeLessThan(
			deleteConversation.mock.invocationCallOrder[0]
		);
		expect(deleteConversation.mock.invocationCallOrder[0]).toBeLessThan(
			flushCleanup.mock.invocationCallOrder[0]
		);
		expect(conversationsStore.conversations).toEqual([]);
	});

	it('binds completed generated assets to the cloned messages after a fork', async () => {
		const source = makeConversation('conv-source');
		const fork = makeConversation('conv-fork', source.id);
		const clonedToolMessage: DatabaseMessage = {
			children: [],
			content: 'Image generated locally.',
			convId: fork.id,
			extra: [makeCompletedAsset(fork.id, 'cloned-tool-message')],
			id: 'cloned-tool-message',
			parent: 'cloned-assistant-message',
			role: MessageRole.TOOL,
			timestamp: 1,
			toolCallId: 'call-generate-1',
			type: MessageType.TEXT
		};

		conversationsStore.activeConversation = source;
		conversationsStore.conversations = [source];
		vi.spyOn(DatabaseService, 'forkConversation').mockResolvedValue(fork);
		vi.spyOn(DatabaseService, 'getConversationMessages').mockResolvedValue([clonedToolMessage]);
		const bindOwners = vi.spyOn(mediaStore, 'addAssetOwners').mockResolvedValue();
		const forkId = await conversationsStore.forkConversation('source-message', {
			includeAttachments: true,
			name: 'Fork with media'
		});

		expect(forkId).toBe(fork.id);
		expect(bindOwners).toHaveBeenCalledOnce();
		expect(bindOwners).toHaveBeenCalledWith(fork.id, [
			{ assetId: 'asset-fork-1', messageId: clonedToolMessage.id }
		]);
		expect(navigation.goto).toHaveBeenCalledOnce();
	});

	it('does not add media owners when attachments are excluded from a fork', async () => {
		const source = makeConversation('conv-source-no-media');
		const fork = makeConversation('conv-fork-no-media', source.id);

		conversationsStore.activeConversation = source;
		conversationsStore.conversations = [source];
		vi.spyOn(DatabaseService, 'forkConversation').mockResolvedValue(fork);
		const getMessages = vi.spyOn(DatabaseService, 'getConversationMessages');
		const bindOwners = vi.spyOn(mediaStore, 'addAssetOwners');

		await conversationsStore.forkConversation('source-message', {
			includeAttachments: false,
			name: 'Fork without media'
		});

		expect(getMessages).not.toHaveBeenCalled();
		expect(bindOwners).not.toHaveBeenCalled();
	});

	it('rewrites persisted generated-media owners for a fork and omits pending jobs', () => {
		const completed = makeCompletedAsset('conv-source', 'source-tool');
		const pending = makePendingAsset('conv-source', 'source-tool');
		const failed = makeTerminalAsset('conv-source', 'source-tool');
		const cloned = DatabaseService.prepareForkExtras(
			[completed, pending, failed],
			true,
			'conv-fork',
			'fork-tool'
		);

		expect(cloned).toHaveLength(2);
		expect(cloned?.map((extra) => extra.type)).toEqual([
			AttachmentType.GENERATED_MEDIA,
			AttachmentType.GENERATED_MEDIA
		]);
		expect(
			cloned?.map((extra) => (extra as DatabaseMessageExtraGeneratedMediaCompleted).status)
		).toEqual(['completed', 'failed']);
		for (const extra of cloned ?? []) {
			if (extra.type !== AttachmentType.GENERATED_MEDIA) continue;

			expect(extra.conversationId).toBe('conv-fork');
			expect(extra.ownerMessageId).toBe('fork-tool');
		}

		expect(
			DatabaseService.prepareForkExtras([completed], false, 'conv-fork', 'fork-tool')
		).toBeUndefined();
	});

	it('chains multiple tool results so every result survives leaf-path reload', async () => {
		const conversation = makeConversation('conv-multi-tool');
		const assistant = makeMessage('assistant-start', MessageRole.ASSISTANT, null, conversation.id);
		const storedMessages = [assistant];

		let toolResultCount = 0;

		conversationsStore.activeConversation = conversation;
		conversationsStore.activeMessages = [assistant];
		conversationsStore.conversations = [conversation];
		const createBranch = vi
			.spyOn(DatabaseService, 'createMessageBranch')
			.mockImplementation(async (message: Omit<DatabaseMessage, 'id'>, parentId: string | null) => {
				const id =
					message.role === MessageRole.TOOL
						? `tool-result-${++toolResultCount}`
						: 'assistant-continuation';
				const created: DatabaseMessage = { ...message, children: [], id, parent: parentId };
				const parent = storedMessages.find((candidate) => candidate.id === parentId);

				if (parent) parent.children.push(id);

				storedMessages.push(created);

				return created;
			});

		vi.spyOn(DatabaseService, 'updateCurrentNode').mockResolvedValue();
		vi.spyOn(agenticStore, 'runAgenticFlow').mockImplementation(async ({ callbacks }) => {
			await callbacks.createToolResultMessage?.('call-1', 'first result');
			await callbacks.createToolResultMessage?.('call-2', 'second result');
			await callbacks.createAssistantMessage?.();
			callbacks.onFlowComplete?.();

			return { handled: true };
		});

		await chatStore.streamChatCompletion([assistant], assistant);

		expect(createBranch.mock.calls.map((call) => call[1])).toEqual([
			'assistant-start',
			'tool-result-1',
			'tool-result-2'
		]);
		const reloadedIds = filterByLeafNodeId(storedMessages, 'assistant-continuation', false).map(
			(message) => message.id
		);

		expect(reloadedIds).toHaveLength(4);
		expect(new Set(reloadedIds)).toEqual(
			new Set(['assistant-start', 'tool-result-1', 'tool-result-2', 'assistant-continuation'])
		);
	});

	it.each([
		['delete', 'message-delete'],
		['regenerate', 'assistant-regenerate'],
		['edit', 'assistant-edit']
	])('cleans all media owners removed by %s', async (operation, targetId) => {
		const conversation = makeConversation('conv-flow');
		const user = makeMessage('user-edit', MessageRole.USER, null);
		const assistant = makeMessage(targetId, MessageRole.ASSISTANT, user.id);
		const removedIds = [targetId, `${targetId}-tool`];
		const host = makeFlowsHost();
		const flows = new ChatMessageFlows(host);

		conversation.currNode = assistant.id;
		user.children = [assistant.id];
		conversationsStore.activeConversation = conversation;
		conversationsStore.activeMessages = [user, assistant];
		conversationsStore.conversations = [conversation];
		vi.spyOn(conversationsStore, 'getConversationMessages').mockResolvedValue([user, assistant]);
		vi.spyOn(conversationsStore, 'refreshActiveMessages').mockResolvedValue();
		vi.spyOn(DatabaseService, 'updateConversation').mockResolvedValue();
		vi.spyOn(DatabaseService, 'updateCurrentNode').mockResolvedValue();
		vi.spyOn(DatabaseService, 'updateMessage').mockResolvedValue();
		const getBranchIds = vi
			.spyOn(DatabaseService, 'getMessageBranchIds')
			.mockResolvedValue(removedIds);
		const queueCleanup = vi.spyOn(mediaStore, 'queueMessageCleanup').mockImplementation(() => {});
		const deleteBranch = vi
			.spyOn(DatabaseService, 'deleteMessageCascading')
			.mockResolvedValue(removedIds);
		const flushCleanup = vi.spyOn(mediaStore, 'flushOwnershipOperations').mockResolvedValue();

		if (operation === 'delete') {
			await flows.deleteMessage(targetId);
		} else if (operation === 'regenerate') {
			await flows.regenerateMessage(targetId);
		} else {
			await flows.updateMessage(user.id, 'edited user message');
		}

		expect(getBranchIds).toHaveBeenCalledWith(conversation.id, targetId);
		expect(queueCleanup).toHaveBeenCalledWith(removedIds);
		expect(flushCleanup).toHaveBeenCalledOnce();
		expect(getBranchIds.mock.invocationCallOrder[0]).toBeLessThan(
			queueCleanup.mock.invocationCallOrder[0]
		);
		expect(queueCleanup.mock.invocationCallOrder[0]).toBeLessThan(
			deleteBranch.mock.invocationCallOrder[0]
		);
		expect(deleteBranch.mock.invocationCallOrder[0]).toBeLessThan(
			flushCleanup.mock.invocationCallOrder[0]
		);
	});

	it('aborts and clears a deleted active conversation through the deletion listener', async () => {
		const conversation = makeConversation('conv-delete-active');
		const controller = chatStore.getOrCreateAbortController(conversation.id);

		conversationsStore.activeConversation = conversation;
		conversationsStore.conversations = [conversation];
		chatStore.setChatLoading(conversation.id, true);
		chatStore.setChatStreaming(conversation.id, 'partial', 'assistant-live', 'model-live');
		chatStore.processing.setActiveConversation(conversation.id);
		vi.spyOn(DatabaseService, 'getAllConversations').mockResolvedValue([conversation]);
		const queueCleanup = vi
			.spyOn(mediaStore, 'queueConversationCleanup')
			.mockImplementation(() => {});
		const deleteConversations = vi
			.spyOn(DatabaseService, 'bulkDeleteConversations')
			.mockResolvedValue();
		const flushCleanup = vi.spyOn(mediaStore, 'flushOwnershipOperations').mockResolvedValue();
		const cancelServerStream = vi.spyOn(ChatService, 'cancelServerStream').mockResolvedValue();

		await conversationsStore.deleteAll();

		expect(controller.signal.aborted).toBe(true);
		expect(cancelServerStream).toHaveBeenCalledWith(conversation.id, 'model-live');
		expect(chatStore.getChatStreaming(conversation.id)).toBeUndefined();
		expect(chatStore.isChatLoadingInternal(conversation.id)).toBe(false);
		expect(chatStore.processing.activeConversationId).toBeNull();
		expect(queueCleanup).toHaveBeenCalledWith([conversation.id]);
		expect(queueCleanup.mock.invocationCallOrder[0]).toBeLessThan(
			deleteConversations.mock.invocationCallOrder[0]
		);
		expect(deleteConversations.mock.invocationCallOrder[0]).toBeLessThan(
			flushCleanup.mock.invocationCallOrder[0]
		);
	});
});
