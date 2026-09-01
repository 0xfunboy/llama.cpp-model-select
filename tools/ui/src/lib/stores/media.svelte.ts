import { browser } from '$app/environment';
import {
	type MediaAssetOwner,
	type MediaBackendStatus,
	MediaHttpError,
	type MediaJobRequest,
	type MediaJobState,
	type MediaModelState,
	MediaService
} from '$lib/services/media.service';
import { SvelteMap } from 'svelte/reactivity';

const MEDIA_OWNERSHIP_OUTBOX_KEY = 'llama-ui-media-ownership-outbox-v1';
const MEDIA_MODEL_CATALOG_REFRESH_MS = 15_000;
const MEDIA_MODEL_SELECTION_TTL_MS = 5 * 60_000;
const MEDIA_MODEL_SELECTION_MAX_TTL_MS = 10 * 60_000;

type MediaKind = 'image' | 'video';

interface PendingMediaModelSelection {
	expiresAt: number;
	kind: MediaKind;
	model: string;
	token: string;
}

type MediaOwnershipOutboxEntry =
	| { type: 'add'; conversationId: string; assets: MediaAssetOwner[] }
	| { type: 'cleanup-conversation'; conversationId: string }
	| { type: 'cleanup-message'; messageId: string };

class MediaStore {
	private backend = $state<MediaBackendStatus | null>(null);
	private backendConnection = $state<'connecting' | 'live' | 'reconnecting' | 'stopped'>('stopped');
	private jobs = $state(new SvelteMap<string, MediaJobState>());
	private modelCatalog = $state<MediaModelState[] | null>(null);
	private modelCatalogUpdatedAt = 0;
	private outboxFlushPromise: Promise<void> | null = null;
	private pendingModelSelections = new Map<string, PendingMediaModelSelection>();
	private selectionSequence = 0;

	get catalog(): readonly MediaModelState[] {
		return this.modelCatalog ?? [];
	}

	get catalogReady(): boolean {
		return this.modelCatalog !== null;
	}

	get connection(): 'connecting' | 'live' | 'reconnecting' | 'stopped' {
		return this.backendConnection;
	}

	get isAvailable(): boolean {
		return this.backend?.runtime_ready === true;
	}

	get status(): MediaBackendStatus | null {
		return this.backend;
	}

	async addAssetOwners(conversationId: string, assets: MediaAssetOwner[]): Promise<void> {
		if (!assets.length) return;

		if (!browser) return;

		this.enqueueOwnershipOperation({ assets, conversationId, type: 'add' });
		await this.flushOwnershipOutbox();
	}

	armModelSelection(
		conversationId: string,
		kind: MediaKind,
		model: string,
		ttlMs = MEDIA_MODEL_SELECTION_TTL_MS
	): string {
		if (!conversationId.trim())
			throw new Error('A conversation is required to select a media model.');

		if (!this.toolModels(kind).some((candidate) => candidate.id === model)) {
			throw new Error(`The selected local ${kind} model is no longer available: ${model}`);
		}

		const lifetime = Number.isFinite(ttlMs)
			? Math.max(1_000, Math.min(MEDIA_MODEL_SELECTION_MAX_TTL_MS, ttlMs))
			: MEDIA_MODEL_SELECTION_TTL_MS;
		const token = `${Date.now()}-${++this.selectionSequence}`;

		this.pendingModelSelections.set(conversationId, {
			expiresAt: Date.now() + lifetime,
			kind,
			model,
			token
		});

		return token;
	}

	async cleanupConversations(conversationIds: string[]): Promise<void> {
		for (const conversationId of conversationIds)
			this.pendingModelSelections.delete(conversationId);

		if (!browser) return;

		this.queueConversationCleanup(conversationIds);
		await this.flushOwnershipOutbox();
	}

	async cleanupMessages(messageIds: string[]): Promise<void> {
		if (!browser) return;

		this.queueMessageCleanup(messageIds);
		await this.flushOwnershipOutbox();
	}

	clearModelSelection(conversationId: string, token: string): void {
		const pending = this.pendingModelSelections.get(conversationId);

		if (pending?.token === token) this.pendingModelSelections.delete(conversationId);
	}

	consumeModelSelection(conversationId: string, kind: MediaKind): string | null {
		const pending = this.pendingModelSelections.get(conversationId);

		if (!pending) return null;

		if (pending.expiresAt <= Date.now()) {
			this.pendingModelSelections.delete(conversationId);

			return null;
		}

		if (pending.kind !== kind) {
			throw new Error(
				`The media dialog selected an ${pending.kind} model, so the ${kind} tool cannot replace it.`
			);
		}

		if (!this.toolModels(kind).some((candidate) => candidate.id === pending.model)) {
			this.pendingModelSelections.delete(conversationId);

			throw new Error(`The selected local ${kind} model is no longer available: ${pending.model}`);
		}

		this.pendingModelSelections.delete(conversationId);

		return pending.model;
	}

	async flushOwnershipOperations(): Promise<void> {
		await this.flushOwnershipOutbox();
	}

	get(toolCallId: string | undefined): MediaJobState | null {
		return toolCallId ? (this.jobs.get(toolCallId) ?? null) : null;
	}

	async models(signal?: AbortSignal): Promise<MediaModelState[]> {
		return this.refreshModelCatalog(signal);
	}

	monitor(): () => void {
		const controller = new AbortController();

		let timer: ReturnType<typeof setTimeout> | null = null;
		let stopped = false;

		this.backendConnection = this.backend ? 'reconnecting' : 'connecting';

		const poll = async () => {
			try {
				await this.refresh(controller.signal);
			} finally {
				const delay = this.backend?.active_job
					? 1000
					: this.backendConnection === 'live'
						? 3000
						: 30000;

				if (!stopped) timer = setTimeout(poll, delay);
			}
		};

		void poll();

		return () => {
			stopped = true;

			if (timer) clearTimeout(timer);

			controller.abort();
			this.backendConnection = 'stopped';
		};
	}

	peekModelSelection(conversationId: string): { kind: MediaKind; model: string } | null {
		const pending = this.pendingModelSelections.get(conversationId);

		if (!pending) return null;

		if (pending.expiresAt <= Date.now()) {
			this.pendingModelSelections.delete(conversationId);

			return null;
		}

		if (!this.toolModels(pending.kind).some((candidate) => candidate.id === pending.model)) {
			this.pendingModelSelections.delete(conversationId);

			return null;
		}

		return { kind: pending.kind, model: pending.model };
	}

	queueConversationCleanup(conversationIds: string[]): void {
		if (!browser) return;

		for (const conversationId of new Set(conversationIds.filter(Boolean))) {
			this.enqueueOwnershipOperation({ conversationId, type: 'cleanup-conversation' });
		}
	}

	queueMessageCleanup(messageIds: string[]): void {
		if (!browser) return;

		for (const messageId of new Set(messageIds.filter(Boolean))) {
			this.enqueueOwnershipOperation({ messageId, type: 'cleanup-message' });
		}
	}

	async refresh(signal?: AbortSignal): Promise<boolean> {
		try {
			this.backend = await MediaService.getStatus(signal);
			this.backendConnection = 'live';

			if (
				!this.catalogReady ||
				Date.now() - this.modelCatalogUpdatedAt >= MEDIA_MODEL_CATALOG_REFRESH_MS
			) {
				try {
					await this.refreshModelCatalog(signal);
				} catch {
					// Status polling remains useful when the optional catalog refresh fails.
					// Keep the last known-good catalog; an empty initial catalog suppresses
					// media tools instead of advertising models the backend cannot confirm.
				}
			}

			void this.flushOwnershipOutbox();

			return this.isAvailable;
		} catch {
			if (!signal?.aborted) this.backendConnection = 'reconnecting';

			return false;
		}
	}

	set(toolCallId: string, state: MediaJobState): void {
		this.jobs.set(toolCallId, state);
	}

	async submit(payload: MediaJobRequest, signal?: AbortSignal): Promise<MediaJobState> {
		const job = await MediaService.submitJob(payload, signal);

		if (this.backend) {
			this.backend = {
				...this.backend,
				active_job: job,
				queue_size: Math.max(this.backend.queue_size, job.queue_position),
				status: 'busy'
			};
		}

		return job;
	}

	supports(kind: MediaKind): boolean {
		return (
			this.backendConnection === 'live' &&
			this.isAvailable &&
			(this.backend?.available_kinds ?? []).includes(kind) &&
			this.toolModels(kind).length > 0
		);
	}

	toolModels(kind: MediaKind): MediaModelState[] {
		return this.catalog
			.filter(
				(model) =>
					model.kind === kind &&
					model.available === true &&
					model.chat_enabled === true &&
					typeof model.tool?.routing === 'string' &&
					model.tool.routing.trim().length > 0
			)
			.sort(
				(left, right) =>
					Number(right.default_for_chat === true) - Number(left.default_for_chat === true)
			);
	}

	/** Reconnect a persisted tool-result placeholder to its controller job after reload. */
	async track(
		jobId: string,
		correlationId: string,
		signal?: AbortSignal,
		onStatus?: (state: MediaJobState) => void | Promise<void>
	): Promise<MediaJobState> {
		let current = await MediaService.getJob(jobId, signal);

		this.set(correlationId, current);
		await onStatus?.(current);

		while (!['completed', 'failed', 'cancelled'].includes(current.status)) {
			await this.delay(1000, signal);
			current = await MediaService.getJob(jobId, signal);
			this.set(correlationId, current);
			await onStatus?.(current);
		}

		return current;
	}

	private delay(milliseconds: number, signal?: AbortSignal): Promise<void> {
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

	private enqueueOwnershipOperation(entry: MediaOwnershipOutboxEntry): void {
		if (!browser) return;

		const entries = this.readOwnershipOutbox();
		const serialized = JSON.stringify(entry);

		if (entries.some((candidate) => JSON.stringify(candidate) === serialized)) return;

		// A conversation cleanup supersedes pending owner additions for that conversation.
		const ownedMessageIds =
			entry.type === 'cleanup-conversation'
				? new Set(
						entries
							.filter(
								(candidate): candidate is Extract<MediaOwnershipOutboxEntry, { type: 'add' }> =>
									candidate.type === 'add' && candidate.conversationId === entry.conversationId
							)
							.flatMap((candidate) =>
								candidate.assets.flatMap((asset) => (asset.messageId ? [asset.messageId] : []))
							)
					)
				: new Set<string>();
		const next =
			entry.type === 'cleanup-conversation'
				? entries.filter((candidate) => {
						if (candidate.type === 'add' && candidate.conversationId === entry.conversationId) {
							return false;
						}

						return (
							candidate.type !== 'cleanup-message' || !ownedMessageIds.has(candidate.messageId)
						);
					})
				: entries;

		next.push(entry);
		this.writeOwnershipOutbox(next);
	}

	private flushOwnershipOutbox(): Promise<void> {
		if (!browser) return Promise.resolve();

		if (this.outboxFlushPromise) return this.outboxFlushPromise;

		this.outboxFlushPromise = (async () => {
			const entries = this.readOwnershipOutbox();
			const completed: MediaOwnershipOutboxEntry[] = [];

			for (const entry of entries) {
				try {
					if (entry.type === 'add') {
						await MediaService.addAssetOwners(entry.conversationId, entry.assets);
					} else if (entry.type === 'cleanup-conversation') {
						await MediaService.cleanupConversationAssets(entry.conversationId);
					} else {
						await MediaService.cleanupMessageAssets(entry.messageId);
					}
				} catch (error) {
					if (error instanceof MediaHttpError && [400, 404, 410, 422].includes(error.status)) {
						// Invalid or already-gone ownership work cannot succeed on retry. Drop it
						// and continue so one stale imported reference cannot poison the FIFO.
						console.warn('[media] Dropping permanent ownership operation:', error.message);
						completed.push(entry);

						continue;
					}

					// Preserve ordering: a cleanup must not overtake an earlier ownership claim.
					this.removeOwnershipOutboxEntries(completed);

					return;
				}

				completed.push(entry);
			}

			// Remove only the snapshot entries that succeeded. An enqueue may have
			// happened while an awaited request was in flight; never erase that new work.
			this.removeOwnershipOutboxEntries(completed);

			if (this.readOwnershipOutbox().length > 0) {
				// The remainder was enqueued after this flush took its snapshot. Drain it
				// on the next task, after outboxFlushPromise has been cleared by finally.
				setTimeout(() => void this.flushOwnershipOutbox(), 0);
			}
		})().finally(() => {
			this.outboxFlushPromise = null;
		});

		return this.outboxFlushPromise;
	}

	private readOwnershipOutbox(): MediaOwnershipOutboxEntry[] {
		if (!browser) return [];

		try {
			const parsed = JSON.parse(localStorage.getItem(MEDIA_OWNERSHIP_OUTBOX_KEY) || '[]');

			return Array.isArray(parsed) ? (parsed as MediaOwnershipOutboxEntry[]) : [];
		} catch {
			return [];
		}
	}

	private async refreshModelCatalog(signal?: AbortSignal): Promise<MediaModelState[]> {
		const models = await MediaService.getModels(signal);
		const resolved = Array.isArray(models) ? models : [];

		this.modelCatalog = resolved;
		this.modelCatalogUpdatedAt = Date.now();

		return resolved;
	}

	private removeOwnershipOutboxEntries(entries: MediaOwnershipOutboxEntry[]): void {
		if (!entries.length) return;

		const completed = new Set(entries.map((entry) => JSON.stringify(entry)));
		const live = this.readOwnershipOutbox();

		this.writeOwnershipOutbox(live.filter((entry) => !completed.has(JSON.stringify(entry))));
	}

	private writeOwnershipOutbox(entries: MediaOwnershipOutboxEntry[]): void {
		if (!browser) return;

		try {
			if (entries.length) localStorage.setItem(MEDIA_OWNERSHIP_OUTBOX_KEY, JSON.stringify(entries));
			else localStorage.removeItem(MEDIA_OWNERSHIP_OUTBOX_KEY);
		} catch (error) {
			console.warn('[media] Unable to persist ownership retry outbox:', error);
		}
	}
}

export const mediaStore = new MediaStore();
