import { AttachmentType, ReasoningEffort, ToolSource } from '$lib/enums';
import type { ChatMessageTimings, ChatMessageType, ChatRole } from '$lib/types/chat';

/**
 * @deprecated Legacy per-conversation MCP server flags. MCP server enabled
 * state is global now; per-conversation tool policy lives in
 * `disabledTools` / `disabledToolCategories`. Read by the migration only.
 */
export interface McpServerOverride {
	serverId: string;
	enabled: boolean;
}

export interface DatabaseConversation {
	currNode: string | null;
	id: string;
	lastModified: number;
	name: string;
	/** @deprecated See {@link McpServerOverride}. Kept on rows for downgrade compatibility. */
	mcpServerOverrides?: McpServerOverride[];
	thinkingEnabled?: boolean;
	reasoningEffort?: ReasoningEffort;
	cwd?: string;
	/** Tool keys disabled for this conversation, incl. server-scoped MCP group keys (`mcp:<serverId>`) */
	disabledTools?: string[];
	/** Tool categories disabled for this conversation */
	disabledToolCategories?: ToolSource[];
	forkedFromConversationId?: string;
	pinned?: boolean;
}

export interface DatabaseMessageExtraAudioFile {
	type: AttachmentType.AUDIO;
	name: string;
	size?: number;
	base64Data: string;
	mimeType: string;
}

export interface DatabaseMessageExtraVideoFile {
	type: AttachmentType.VIDEO;
	name: string;
	size?: number;
	base64Data: string;
	mimeType: string;
}

export interface DatabaseMessageExtraImageFile {
	type: AttachmentType.IMAGE;
	name: string;
	size?: number;
	base64Url: string;
}

export interface DatabaseMessageExtraGeneratedMediaBase {
	type: AttachmentType.GENERATED_MEDIA;
	/** Controller job id. Stable across a browser reload while generation is running. */
	jobId: string;
	/** Conversation/message ownership used by the controller for lifecycle cleanup. */
	conversationId: string;
	ownerMessageId: string;
	kind: 'image' | 'video';
	mimeType: string;
	model: string;
	prompt: string;
	name: string;
	size: number;
	width?: number;
	height?: number;
	quality?: string | null;
}

/** Lightweight placeholder persisted while the controller owns an active job. */
export interface DatabaseMessageExtraGeneratedMediaPending extends DatabaseMessageExtraGeneratedMediaBase {
	status: 'pending';
	assetId?: string;
	assetUrl?: string;
}

/** Server-backed generated media. Bytes deliberately stay out of IndexedDB. */
export interface DatabaseMessageExtraGeneratedMediaCompleted extends DatabaseMessageExtraGeneratedMediaBase {
	status: 'completed';
	assetId: string;
	assetUrl: string;
}

/** Terminal job without a usable asset. This prevents a stale pending UI after reload/abort. */
export interface DatabaseMessageExtraGeneratedMediaTerminal extends DatabaseMessageExtraGeneratedMediaBase {
	status: 'failed' | 'cancelled';
	assetId?: string;
	assetUrl?: string;
	error: string;
}

export type DatabaseMessageExtraGeneratedMedia =
	| DatabaseMessageExtraGeneratedMediaPending
	| DatabaseMessageExtraGeneratedMediaCompleted
	| DatabaseMessageExtraGeneratedMediaTerminal;

/**
 * Legacy format from the old UI — pasted content was stored as "context" type
 * @deprecated Use DatabaseMessageExtraTextFile instead
 */
export interface DatabaseMessageExtraLegacyContext {
	type: AttachmentType.LEGACY_CONTEXT;
	name: string;
	size?: number;
	content: string;
}

export interface DatabaseMessageExtraPdfFile {
	type: AttachmentType.PDF;
	base64Data: string;
	name: string;
	size?: number;
	content: string;
	images?: string[];
	processedAsImages: boolean;
}

export interface DatabaseMessageExtraTextFile {
	type: AttachmentType.TEXT;
	name: string;
	size?: number;
	content: string;
}

export interface DatabaseMessageExtraMcpPrompt {
	type: AttachmentType.MCP_PROMPT;
	name: string;
	size?: number;
	serverName: string;
	promptName: string;
	content: string;
	arguments?: Record<string, string>;
}

export interface DatabaseMessageExtraMcpResource {
	type: AttachmentType.MCP_RESOURCE;
	name: string;
	size?: number;
	uri: string;
	serverName: string;
	content: string;
	mimeType?: string;
}

export type DatabaseMessageExtra =
	| DatabaseMessageExtraImageFile
	| DatabaseMessageExtraGeneratedMedia
	| DatabaseMessageExtraTextFile
	| DatabaseMessageExtraAudioFile
	| DatabaseMessageExtraVideoFile
	| DatabaseMessageExtraPdfFile
	| DatabaseMessageExtraMcpPrompt
	| DatabaseMessageExtraMcpResource
	| DatabaseMessageExtraLegacyContext;

export interface DatabaseMessage {
	id: string;
	convId: string;
	type: ChatMessageType;
	timestamp: number;
	role: ChatRole;
	content: string;
	parent: string | null;
	/**
	 * @deprecated - left for backward compatibility
	 */
	thinking?: string;
	/** Reasoning content produced by the model (separate from visible content) */
	reasoningContent?: string;
	/** Serialized JSON array of tool calls made by assistant messages */
	toolCalls?: string;
	/** Chat completion id streamed by the server, used to target realtime control (e.g. end reasoning) */
	completionId?: string;
	/** Tool call ID for tool result messages (role: 'tool') */
	toolCallId?: string;
	/** Working directory the tool call ran with (sent via the x-tool-cwd header), stored per call so the UI can show it accurately even after the conversation cwd changes */
	toolCwd?: string;
	/** Internal flag marking a UI-generated message (e.g. a cwd change). The row is sent to the model as a "user" turn so chat templates accept it; the flag is only read by the renderer. */
	isSynthetic?: boolean;
	children: string[];
	extra?: DatabaseMessageExtra[];
	timings?: ChatMessageTimings;
	model?: string;
}

export type ExportedConversation = {
	conv: DatabaseConversation;
	messages: DatabaseMessage[];
};

export type ExportedConversations = ExportedConversation | ExportedConversation[];
