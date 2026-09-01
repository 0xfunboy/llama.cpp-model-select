import { AttachmentType, ContentPartType, MessageRole, MessageType } from '$lib/enums';
import { ChatService } from '$lib/services/chat.service';
import { MediaService } from '$lib/services/media.service';
import { modelsStore } from '$lib/stores/models/index.svelte';
import type {
	ApiChatMessageContentPart,
	DatabaseMessage,
	DatabaseMessageExtraGeneratedMediaCompleted
} from '$lib/types';
import { afterEach, describe, expect, it, vi } from 'vitest';

function makeGeneratedImage(): DatabaseMessageExtraGeneratedMediaCompleted {
	return {
		assetId: 'asset-image-1',
		assetUrl: '/api/media/assets/asset-image-1',
		conversationId: 'conv-1',
		height: 1024,
		jobId: 'media_job_1',
		kind: 'image',
		mimeType: 'image/png',
		model: 'pony-v6-xl',
		name: 'asset-image-1.png',
		ownerMessageId: 'tool-message-1',
		prompt: 'a marmot wrapping chocolate, studio illustration',
		size: 2048,
		status: 'completed',
		type: AttachmentType.GENERATED_MEDIA,
		width: 1024
	};
}

function makeToolMessage(attachment: DatabaseMessageExtraGeneratedMediaCompleted): DatabaseMessage {
	return {
		children: [],
		content: 'Image generated locally.\nPrompt used: a marmot wrapping chocolate.',
		convId: 'conv-1',
		extra: [attachment],
		id: 'tool-message-1',
		parent: 'assistant-message-1',
		role: MessageRole.TOOL,
		timestamp: 1,
		toolCallId: 'call-generate-1',
		type: MessageType.TEXT
	};
}

afterEach(() => {
	vi.restoreAllMocks();
});

describe('ChatService generated-media context', () => {
	it('keeps the persisted asset lightweight when the selected model cannot see images', async () => {
		const attachment = makeGeneratedImage();
		const persistedBefore = structuredClone(attachment);
		const hydrate = vi.spyOn(MediaService, 'contextPartForAttachment');

		vi.spyOn(modelsStore.props, 'modelSupportsVision').mockReturnValue(false);

		const converted = await ChatService.convertDbMessageToApiChatMessageData(
			makeToolMessage(attachment),
			'text-only-model'
		);

		expect(hydrate).not.toHaveBeenCalled();
		expect(converted).toEqual({
			content: [
				{
					text: 'Image generated locally.\nPrompt used: a marmot wrapping chocolate.',
					type: ContentPartType.TEXT
				}
			],
			role: MessageRole.TOOL,
			tool_call_id: 'call-generate-1'
		});
		expect(attachment).toEqual(persistedBefore);
		expect(JSON.stringify(attachment)).not.toContain('base64');
	});

	it('materializes an image only in the outgoing model context', async () => {
		const attachment = makeGeneratedImage();
		const persistedBefore = structuredClone(attachment);
		const transientPart: ApiChatMessageContentPart = {
			image_url: { url: 'data:image/png;base64,dHJhbnNpZW50' },
			type: ContentPartType.IMAGE_URL
		};

		vi.spyOn(modelsStore.props, 'modelSupportsVision').mockReturnValue(true);
		const hydrate = vi
			.spyOn(MediaService, 'contextPartForAttachment')
			.mockResolvedValue(transientPart);
		const converted = await ChatService.convertDbMessageToApiChatMessageData(
			makeToolMessage(attachment),
			'vision-model'
		);

		expect(hydrate).toHaveBeenCalledOnce();
		expect(hydrate).toHaveBeenCalledWith(attachment);
		expect(converted.content).toEqual([
			{
				text: 'Image generated locally.\nPrompt used: a marmot wrapping chocolate.',
				type: ContentPartType.TEXT
			},
			transientPart
		]);
		expect(converted.role).toBe(MessageRole.TOOL);
		expect(converted.tool_call_id).toBe('call-generate-1');
		expect(attachment).toEqual(persistedBefore);
		expect(attachment.assetUrl).toBe('/api/media/assets/asset-image-1');
		expect(JSON.stringify(attachment)).not.toContain('dHJhbnNpZW50');
	});

	it('falls back to the persisted textual summary when transient hydration fails', async () => {
		const attachment = makeGeneratedImage();

		vi.spyOn(modelsStore.props, 'modelSupportsVision').mockReturnValue(true);
		vi.spyOn(MediaService, 'contextPartForAttachment').mockRejectedValue(
			new Error('asset temporarily unavailable')
		);
		vi.spyOn(console, 'warn').mockImplementation(() => {});

		const converted = await ChatService.convertDbMessageToApiChatMessageData(
			makeToolMessage(attachment),
			'vision-model'
		);

		expect(converted.content).toEqual([
			{
				text: 'Image generated locally.\nPrompt used: a marmot wrapping chocolate.',
				type: ContentPartType.TEXT
			}
		]);
		expect(converted.tool_call_id).toBe('call-generate-1');
		expect(console.warn).toHaveBeenCalledOnce();
	});
});
