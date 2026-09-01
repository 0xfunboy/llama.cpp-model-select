import { base } from '$app/paths';
import { API_MEDIA } from '$lib/constants';
import { AttachmentType, ContentPartType } from '$lib/enums';
import type {
	ApiChatMessageContentPart,
	DatabaseMessageExtraGeneratedMedia,
	DatabaseMessageExtraGeneratedMediaCompleted,
	DatabaseMessageExtraGeneratedMediaPending,
	DatabaseMessageExtraGeneratedMediaTerminal
} from '$lib/types';
import { getAuthHeaders, getJsonHeaders } from '$lib/utils/api-headers';

export type MediaJobStatus =
	| 'queued'
	| 'preparing'
	| 'loading_model'
	| 'generating'
	| 'encoding'
	| 'releasing_memory'
	| 'completed'
	| 'failed'
	| 'cancelled';

export interface MediaJobState {
	id: string;
	kind: 'image' | 'video';
	model: string;
	model_label?: string | null;
	status: MediaJobStatus;
	phase: string;
	message: string;
	progress: number;
	/** `measured` when backed by sampler steps, otherwise an ETA-based estimate. */
	progress_source?: 'measured' | 'estimated' | null;
	eta_seconds: number | null;
	elapsed_seconds: number;
	estimated_total_seconds?: number | null;
	generation_elapsed_seconds?: number | null;
	queue_position: number;
	sample_step?: number | null;
	sample_steps?: number | null;
	step_seconds?: number | null;
	average_step_seconds?: number | null;
	width?: number | null;
	height?: number | null;
	preview_url?: string | null;
	preview_step?: number | null;
	preview_width?: number | null;
	preview_height?: number | null;
	can_cancel?: boolean;
	result: {
		asset_id?: string;
		asset_url: string;
		filename: string;
		mime_type: string;
		size: number;
		width?: number;
		height?: number;
		quality?: string | null;
		prompt?: string;
	} | null;
	error: string | null;
	completed_at?: number | null;
}

export interface MediaBackendStatus {
	status: 'busy' | 'idle';
	active_job: MediaJobState | null;
	last_job: MediaJobState | null;
	queue_size: number;
	runtime_ready: boolean;
	available_kinds: Array<'image' | 'video'>;
	memory: { total: number; used: number; free: number } | null;
}

export interface MediaModelState {
	id: string;
	label: string;
	kind: 'image' | 'video';
	description: string;
	chat_enabled: boolean;
	default_for_chat: boolean;
	available: boolean;
	download_progress: number;
	defaults: Record<string, unknown>;
	tool: {
		/** Authoritative model-selection guidance supplied by the local media harness. */
		routing: string;
	};
}

export interface MediaJobRequest extends Record<string, unknown> {
	kind: 'image' | 'video';
	model: string;
	prompt: string;
}

export type MediaStatusCallback = (state: MediaJobState) => void;

export interface MediaJobRunOptions {
	conversationId: string;
	ownerMessageId: string;
	signal?: AbortSignal;
	onStatus?: MediaStatusCallback;
	onSubmitted?: (
		job: MediaJobState,
		attachment: DatabaseMessageExtraGeneratedMediaPending
	) => void | Promise<void>;
	onCompleted?: (
		job: MediaJobState,
		attachment: DatabaseMessageExtraGeneratedMediaCompleted,
		content: string
	) => void | Promise<void>;
	onTerminal?: (
		job: MediaJobState,
		attachment: DatabaseMessageExtraGeneratedMediaTerminal
	) => void | Promise<void>;
	/** Materialize a transient multimodal content part for the current LLM turn. */
	includeContextPart?: boolean;
}

export interface MediaJobRunResult {
	job: MediaJobState;
	content: string;
	attachment: DatabaseMessageExtraGeneratedMediaCompleted;
	/** Never persisted; callers may append it to the in-memory LLM transcript. */
	contextPart?: ApiChatMessageContentPart;
}

export interface MediaAssetOwner {
	assetId: string;
	messageId?: string;
}

export class MediaHttpError extends Error {
	constructor(
		readonly status: number,
		message: string
	) {
		super(message);
		this.name = 'MediaHttpError';
	}
}

export class MediaService {
	static async addAssetOwners(
		conversationId: string,
		assets: MediaAssetOwner[],
		signal?: AbortSignal
	): Promise<void> {
		if (assets.length === 0) return;

		await this.fetchJson(API_MEDIA.ASSET_OWNERS, {
			body: JSON.stringify({
				assets: assets.map(({ assetId, messageId }) => ({
					asset_id: assetId,
					...(messageId ? { message_id: messageId } : {})
				})),
				conversation_id: conversationId
			}),
			headers: { 'Content-Type': 'application/json' },
			method: 'POST',
			signal
		});
	}

	static async cancelJob(jobId: string, signal?: AbortSignal): Promise<MediaJobState> {
		return this.fetchJson<MediaJobState>(`${API_MEDIA.JOBS}/${jobId}/cancel`, {
			body: '{}',
			headers: getJsonHeaders(),
			method: 'POST',
			signal
		});
	}

	static async cleanupConversationAssets(
		conversationId: string,
		signal?: AbortSignal
	): Promise<void> {
		await this.fetchJson(API_MEDIA.CONVERSATION_ASSETS(conversationId), {
			method: 'DELETE',
			signal
		});
		await this.clearGeneratedMediaCaches();
	}

	static async cleanupMessageAssets(messageId: string, signal?: AbortSignal): Promise<void> {
		await this.fetchJson(API_MEDIA.MESSAGE_ASSETS(messageId), { method: 'DELETE', signal });
		await this.clearGeneratedMediaCaches();
	}

	static completedAttachment(
		job: MediaJobState,
		payload: MediaJobRequest,
		conversationId: string,
		ownerMessageId: string
	): DatabaseMessageExtraGeneratedMediaCompleted {
		if (!job.result) throw new Error('Completed media job has no asset result.');

		return {
			assetId: job.result.asset_id || job.id,
			assetUrl: job.result.asset_url,
			conversationId,
			height: job.result.height,
			jobId: job.id,
			kind: payload.kind,
			mimeType: job.result.mime_type,
			model: job.model || payload.model,
			name: job.result.filename,
			ownerMessageId,
			prompt: job.result.prompt || payload.prompt,
			quality: job.result.quality,
			size: job.result.size,
			status: 'completed',
			type: AttachmentType.GENERATED_MEDIA,
			width: job.result.width
		};
	}

	static completedSummary(
		job: MediaJobState,
		attachment: DatabaseMessageExtraGeneratedMediaCompleted
	): string {
		const label = attachment.kind === 'image' ? 'Image' : 'Video';
		const duration = job.elapsed_seconds > 0 ? ` in ${job.elapsed_seconds}s` : '';

		return [
			`${label} generated locally with ${attachment.model}${duration}.`,
			`Prompt used: ${attachment.prompt}`,
			`Asset ID: ${attachment.assetId}`
		].join('\n');
	}

	static async contextPartForAttachment(
		attachment: DatabaseMessageExtraGeneratedMedia,
		signal?: AbortSignal
	): Promise<ApiChatMessageContentPart | undefined> {
		if (attachment.status !== 'completed' || !attachment.assetUrl) return undefined;

		const response = await fetch(`${base}${attachment.assetUrl}`, {
			cache: 'no-store',
			headers: getAuthHeaders(),
			signal
		});

		if (!response.ok) {
			throw new Error(`Unable to load generated media: HTTP ${response.status}`);
		}

		const dataUrl = await this.blobToDataUrl(await response.blob());

		if (attachment.kind === 'image') {
			return { image_url: { url: dataUrl }, type: ContentPartType.IMAGE_URL };
		}

		return {
			input_video: {
				data: dataUrl.slice(dataUrl.indexOf(',') + 1),
				format: attachment.mimeType.includes('mp4')
					? 'mp4'
					: attachment.mimeType.includes('ogg')
						? 'ogg'
						: 'auto'
			},
			type: ContentPartType.INPUT_VIDEO
		};
	}

	static async getJob(jobId: string, signal?: AbortSignal): Promise<MediaJobState> {
		return this.fetchJson<MediaJobState>(`${API_MEDIA.JOBS}/${jobId}`, { signal });
	}

	static async getModels(signal?: AbortSignal): Promise<MediaModelState[]> {
		const response = await this.fetchJson<{ data: MediaModelState[] }>(API_MEDIA.MODELS, {
			signal
		});

		return response.data;
	}

	static async getStatus(signal?: AbortSignal): Promise<MediaBackendStatus> {
		return this.fetchJson<MediaBackendStatus>(API_MEDIA.STATUS, { signal });
	}

	static pendingAttachment(
		job: MediaJobState,
		payload: MediaJobRequest,
		conversationId: string,
		ownerMessageId: string
	): DatabaseMessageExtraGeneratedMediaPending {
		return {
			conversationId,
			jobId: job.id,
			kind: payload.kind,
			mimeType: payload.kind === 'image' ? 'image/png' : 'video/webm',
			model: payload.model,
			name: '',
			ownerMessageId,
			prompt: payload.prompt,
			size: 0,
			status: 'pending',
			type: AttachmentType.GENERATED_MEDIA
		};
	}

	static pendingSummary(attachment: DatabaseMessageExtraGeneratedMediaPending): string {
		const label = attachment.kind === 'image' ? 'image' : 'video';

		return `Generating ${label} locally with ${attachment.model}. Prompt: ${attachment.prompt}`;
	}

	static async runJob(
		payload: MediaJobRequest,
		options: MediaJobRunOptions
	): Promise<MediaJobRunResult> {
		const {
			conversationId,
			includeContextPart,
			onCompleted,
			onStatus,
			onSubmitted,
			onTerminal,
			ownerMessageId,
			signal
		} = options;

		let jobId: string | null = null;
		let current: MediaJobState | null = null;
		let terminalPersisted = false;

		try {
			const submitted = await this.submitJob(
				{
					...payload,
					conversation_id: conversationId,
					owner_message_id: ownerMessageId
				},
				signal
			);

			jobId = submitted.id;
			current = submitted;
			onStatus?.(submitted);
			await onSubmitted?.(
				submitted,
				this.pendingAttachment(submitted, payload, conversationId, ownerMessageId)
			);

			while (!['completed', 'failed', 'cancelled'].includes(current.status)) {
				await this.delay(1000, signal);
				current = await this.fetchJson<MediaJobState>(`${API_MEDIA.JOBS}/${jobId}`, { signal });
				onStatus?.(current);
			}

			if (current.status !== 'completed' || !current.result) {
				const terminal = this.terminalAttachment(current, payload, conversationId, ownerMessageId);

				terminalPersisted = true;
				await onTerminal?.(current, terminal);

				throw new Error(terminal.error);
			}

			const attachment = this.completedAttachment(current, payload, conversationId, ownerMessageId);
			const content = this.completedSummary(current, attachment);

			// Persist the ref before any transient context hydration or caller-side
			// abort check can interrupt the post-tool path.
			await onCompleted?.(current, attachment, content);

			let contextPart: ApiChatMessageContentPart | undefined;

			if (includeContextPart) {
				try {
					contextPart = await this.contextPartForAttachment(attachment, signal);
				} catch (error) {
					// The generated asset remains valid even if transient LLM-context hydration fails.
					console.warn(
						'[media] Unable to hydrate generated media for the current LLM turn:',
						error
					);
				}
			}

			return {
				attachment,
				content,
				contextPart,
				job: current
			};
		} catch (error) {
			if (signal?.aborted) {
				if (jobId) {
					try {
						await this.cancelJob(jobId);
					} catch (cancelError) {
						console.error('[media] Failed to cancel job:', cancelError);
					}
				} else {
					// The controller may have accepted the POST before the aborted browser
					// received its 202 response. Ownership is known even when the job id is not,
					// so cancel by message to avoid an invisible GPU job and orphaned output.
					try {
						await this.cleanupMessageAssets(ownerMessageId);
					} catch (cleanupError) {
						console.error('[media] Failed to cancel an unacknowledged job:', cleanupError);
					}
				}
			}

			if (current && current.status !== 'completed' && !terminalPersisted) {
				const status = signal?.aborted ? 'cancelled' : 'failed';
				const fallbackJob: MediaJobState = {
					...current,
					error: error instanceof Error ? error.message : String(error),
					message:
						status === 'cancelled' ? 'Media generation cancelled.' : 'Media generation failed.',
					status
				};

				await onTerminal?.(
					fallbackJob,
					this.terminalAttachment(fallbackJob, payload, conversationId, ownerMessageId)
				);
			}

			throw error;
		}
	}

	static async submitJob(payload: MediaJobRequest, signal?: AbortSignal): Promise<MediaJobState> {
		return this.fetchJson<MediaJobState>(API_MEDIA.JOBS, {
			body: JSON.stringify(payload),
			headers: { 'Content-Type': 'application/json' },
			method: 'POST',
			signal
		});
	}

	static terminalAttachment(
		job: MediaJobState,
		payload: MediaJobRequest,
		conversationId: string,
		ownerMessageId: string
	): DatabaseMessageExtraGeneratedMediaTerminal {
		const status = job.status === 'cancelled' ? 'cancelled' : 'failed';

		return {
			conversationId,
			error: job.error || job.message || `Media generation ${status}.`,
			jobId: job.id,
			kind: payload.kind,
			mimeType: payload.kind === 'image' ? 'image/png' : 'video/webm',
			model: job.model || payload.model,
			name: '',
			ownerMessageId,
			prompt: payload.prompt,
			size: 0,
			status,
			type: AttachmentType.GENERATED_MEDIA
		};
	}

	static terminalSummary(attachment: DatabaseMessageExtraGeneratedMediaTerminal): string {
		const label = attachment.kind === 'image' ? 'Image' : 'Video';
		const state = attachment.status === 'cancelled' ? 'cancelled' : 'failed';

		return `${label} generation ${state}: ${attachment.error}`;
	}

	private static blobToDataUrl(blob: Blob): Promise<string> {
		return new Promise((resolve, reject) => {
			const reader = new FileReader();

			reader.onerror = () => reject(reader.error ?? new Error('Unable to read generated media'));
			reader.onload = () => resolve(String(reader.result));
			reader.readAsDataURL(blob);
		});
	}

	private static async clearGeneratedMediaCaches(): Promise<void> {
		if (typeof caches === 'undefined') return;

		try {
			for (const cacheName of await caches.keys()) {
				const cache = await caches.open(cacheName);

				for (const request of await cache.keys()) {
					const url = new URL(request.url);

					if (url.pathname.includes('/api/media/assets/')) await cache.delete(request);
				}
			}
		} catch (error) {
			console.warn('[media] Unable to clear generated-media browser caches:', error);
		}
	}

	private static delay(milliseconds: number, signal?: AbortSignal): Promise<void> {
		return new Promise((resolve, reject) => {
			if (signal?.aborted) {
				reject(new DOMException('Aborted', 'AbortError'));

				return;
			}

			const cleanup = () => signal?.removeEventListener('abort', abort);
			const timeout = setTimeout(() => {
				cleanup();
				resolve();
			}, milliseconds);
			const abort = () => {
				clearTimeout(timeout);
				cleanup();
				reject(new DOMException('Aborted', 'AbortError'));
			};

			signal?.addEventListener('abort', abort, { once: true });
		});
	}

	private static async fetchJson<T>(path: string, init?: RequestInit): Promise<T> {
		const headers = new Headers(getAuthHeaders());

		for (const [name, value] of new Headers(init?.headers).entries()) {
			headers.set(name, value);
		}

		const response = await fetch(`${base}${path}`, { ...init, headers });

		if (!response.ok) {
			let message = `${response.status} ${response.statusText}`.trim();

			try {
				const body = (await response.json()) as {
					error?: string | { message?: string };
				};

				if (typeof body.error === 'string') message = body.error;
				else if (body.error?.message) message = body.error.message;
			} catch {
				// Keep the HTTP status when the server did not return JSON.
			}

			throw new MediaHttpError(response.status, message);
		}

		return (await response.json()) as T;
	}
}
