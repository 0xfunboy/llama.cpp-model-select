/**
 * ModelStatusManager - Model load/unload operations and the /models/sse feed
 *
 * Owns the status feed subscription, load progress tracking, and the
 * awaiters that settle load/unload operations. SSE provides low-latency
 * updates while authoritative /v1/models snapshots recover state after a
 * refresh or feed interruption. Created and owned by modelsStore; the host
 * owns the router model rows both paths update.
 */

import { ServerModelsSseEventType, ServerModelStatus } from '$lib/enums';
import { ModelsService } from '$lib/services/models.service';
import type { ModelPropsManager } from '$lib/stores/models/props.svelte';
// direct imports between stores, not via the barrel, to avoid circular deps
import { serverStore } from '$lib/stores/server.svelte';
import { SvelteMap } from 'svelte/reactivity';
import { toast } from 'svelte-sonner';

/**
 * The slice of modelsStore the manager drives. Kept narrow on purpose so it
 * cannot reach around the host's full surface; modelsStore implements this
 * structurally.
 */
export interface ModelStatusHost {
	error: string | null;
	readonly props: ModelPropsManager;
	/** Router model rows the status feed updates. */
	routerModels: ApiModelDataEntry[];
	fetchRouterModels(): Promise<void>;
	isModelLoaded(modelId: string): boolean;
	toDisplayName(id: string): string;
}

export type ModelBackendConnection = 'connecting' | 'live' | 'reconnecting' | 'stopped';

export class ModelStatusManager {
	backendOperation = $state<ApiRouterModelOperation | null>(null);
	connection = $state<ModelBackendConnection>('stopped');
	lastBackendSyncAt = $state(0);
	private loadingStates = new SvelteMap<string, boolean>();
	private loadProgress = new SvelteMap<string, ModelLoadProgress>();
	private loadStartedAt = new SvelteMap<string, number>();
	private reconcileTimer: ReturnType<typeof setTimeout> | null = null;
	// /models/sse feed state; periodic snapshots reconcile missed or stale events
	private statusAbort: AbortController | null = null;
	private statusReaderActive = false;
	private statusWaiters = new SvelteMap<
		string,
		{ target: ServerModelStatus; resolve: () => void; reject: (e: Error) => void }
	>();

	constructor(private host: ModelStatusHost) {}

	async ensureLoaded(modelId: string): Promise<void> {
		if (this.host.isModelLoaded(modelId)) return;

		await this.load(modelId);
	}

	/**
	 * Current load progress for a model, or null when not loading.
	 */
	getLoadProgress(modelId: string): ModelLoadProgress | null {
		return this.loadProgress.get(modelId) ?? null;
	}

	getLoadStartedAt(modelId: string): number | null {
		if (this.backendOperation?.target === modelId && this.backendOperation.started_at > 0) {
			return this.backendOperation.started_at;
		}

		return this.loadStartedAt.get(modelId) ?? null;
	}

	getTransitionModelId(): string | null {
		if (this.backendOperation?.active && this.backendOperation.target) {
			return this.backendOperation.target;
		}

		const loading = this.host.routerModels.find(
			(model) => model.status.value === ServerModelStatus.LOADING
		);

		if (loading) return loading.id;

		for (const [modelId, active] of this.loadingStates) {
			if (active) return modelId;
		}

		return null;
	}

	hasActiveRequests(): boolean {
		return this.host.routerModels.some((model) => (model.status.active_requests ?? 0) > 0);
	}

	isOperationInProgress(modelId: string): boolean {
		return (
			(this.loadingStates.get(modelId) ?? false) ||
			Boolean(this.backendOperation?.active && this.backendOperation.target === modelId)
		);
	}

	isTransitionInProgress(): boolean {
		return this.getTransitionModelId() !== null;
	}

	async load(modelId: string): Promise<void> {
		if (this.host.isModelLoaded(modelId)) return;

		if (this.loadingStates.get(modelId)) return;

		const activeTarget = this.getTransitionModelId();

		if (activeTarget && activeTarget !== modelId) {
			throw new Error(
				`The backend is already switching to ${this.host.toDisplayName(activeTarget)}`
			);
		}

		this.loadingStates.set(modelId, true);
		this.loadStartedAt.set(modelId, Date.now());
		this.host.error = null;
		this.requestReconcile();

		// the feed drives completion, so it must be live before the request
		this.subscribe();

		const reachedLoaded = this.waitForStatus(modelId, ServerModelStatus.LOADED);

		reachedLoaded.catch(() => {});

		try {
			await ModelsService.load(modelId);
			await reachedLoaded;
			toast.success(`Model loaded: ${this.host.toDisplayName(modelId)}`);
		} catch (error) {
			this.rejectStatus(modelId, error instanceof Error ? error : new Error('load failed'));
			this.host.error = error instanceof Error ? error.message : 'Failed to load model';
			toast.error(this.host.error ?? `Failed to load ${this.host.toDisplayName(modelId)}`);

			throw error;
		} finally {
			this.loadingStates.set(modelId, false);
			this.requestReconcile();
		}
	}

	/** Reconcile an authoritative /v1/models snapshot after startup, refresh or SSE loss. */
	reconcileSnapshot(response: ApiRouterModelsListResponse): void {
		this.backendOperation = response.operation ?? null;
		this.lastBackendSyncAt = Date.now();

		for (const model of response.data) {
			const status = model.status.value;

			if (status === ServerModelStatus.LOADING) {
				if (!this.loadStartedAt.has(model.id)) {
					const backendStarted =
						response.operation?.target === model.id ? response.operation.started_at : 0;

					this.loadStartedAt.set(model.id, backendStarted || Date.now());
				}

				if (model.status.progress) this.loadProgress.set(model.id, model.status.progress);
			} else {
				this.loadProgress.delete(model.id);
				this.loadStartedAt.delete(model.id);
			}

			const failed =
				status === ServerModelStatus.FAILED ||
				Boolean(model.status.failed) ||
				(status === ServerModelStatus.UNLOADED && (model.status.exit_code ?? 0) !== 0);

			if (failed) {
				this.rejectStatus(
					model.id,
					new Error(`Model failed with exit code ${model.status.exit_code ?? 'unknown'}`)
				);
			} else {
				this.settleStatus(model.id, status);
			}
		}

		const operation = response.operation;

		if (operation?.phase === 'failed' && operation.target) {
			const error = new Error(operation.error || 'Backend model switch failed');

			this.host.error = error.message;
			this.rejectStatus(operation.target, error);
		}
	}

	/**
	 * Open the /models/sse feed and keep it live with auto reconnect.
	 * Idempotent and router mode only.
	 */
	subscribe(): void {
		if (this.statusReaderActive) return;

		if (!serverStore.isRouterMode) return;

		this.statusReaderActive = true;
		this.statusAbort = new AbortController();
		this.connection = 'connecting';
		void this.runStatusReader(this.statusAbort.signal);
		this.scheduleReconcile(1000);
	}

	async unload(modelId: string): Promise<void> {
		if (!this.host.isModelLoaded(modelId)) return;

		if (this.loadingStates.get(modelId)) return;

		this.loadingStates.set(modelId, true);
		this.host.error = null;
		this.requestReconcile();

		this.subscribe();

		const reachedUnloaded = this.waitForStatus(modelId, ServerModelStatus.UNLOADED);

		reachedUnloaded.catch(() => {});

		try {
			await ModelsService.unload(modelId);
			await reachedUnloaded;
			toast.info(`Model unloaded: ${this.host.toDisplayName(modelId)}`);
		} catch (error) {
			this.rejectStatus(modelId, error instanceof Error ? error : new Error('unload failed'));
			this.host.error = error instanceof Error ? error.message : 'Failed to unload model';
			toast.error(`Failed to unload model: ${this.host.toDisplayName(modelId)}`);

			throw error;
		} finally {
			this.loadingStates.set(modelId, false);
			this.requestReconcile();
		}
	}

	/**
	 * Close the /models/sse feed and drop transient progress.
	 */
	unsubscribe(): void {
		this.statusReaderActive = false;
		this.statusAbort?.abort();
		this.statusAbort = null;

		if (this.reconcileTimer) clearTimeout(this.reconcileTimer);

		this.reconcileTimer = null;
		this.loadProgress.clear();
		this.loadStartedAt.clear();
		this.connection = 'stopped';
	}

	/**
	 * Apply a status envelope: update the model row, track or clear progress,
	 * settle any pending load or unload awaiter.
	 */
	private applyModelStatus(event: ApiModelsSseEvent): void {
		const model = event.model;
		const data = event.data;

		if (!model || !data?.status) return;

		const status = data.status;

		this.setRouterModelStatus(model, data);
		this.lastBackendSyncAt = Date.now();

		if (status === ServerModelStatus.LOADING) {
			if (!this.loadStartedAt.has(model)) this.loadStartedAt.set(model, Date.now());

			if (data.progress) this.loadProgress.set(model, data.progress);
		} else {
			this.loadProgress.delete(model);
			this.loadStartedAt.delete(model);
		}

		if (status === ServerModelStatus.LOADED) {
			void this.host.props.updateModelModalities(model);
		}

		const failed =
			status === ServerModelStatus.FAILED ||
			(status === ServerModelStatus.UNLOADED && (data.exit_code ?? 0) !== 0);

		if (failed) {
			this.rejectStatus(model, new Error(`Model failed: ${this.host.toDisplayName(model)}`));

			return;
		}

		this.settleStatus(model, status);
	}

	/**
	 * Route one feed record by event kind. Only the status_* events carry a
	 * status payload, models_reload triggers a list refresh, model_remove drops
	 * the row, download_* belong to the download surface, not here.
	 */
	private applyStatusEvent(event: ApiModelsSseEvent): void {
		switch (event.event) {
			case ServerModelsSseEventType.STATUS_CHANGE:
			case ServerModelsSseEventType.MODEL_STATUS:
			case ServerModelsSseEventType.STATUS_UPDATE:
				this.applyModelStatus(event);

				break;
			case ServerModelsSseEventType.MODELS_RELOAD:
				void this.host.fetchRouterModels();

				break;
			case ServerModelsSseEventType.MODEL_REMOVE:
				this.removeRouterModel(event.model);

				break;
			case ServerModelsSseEventType.DOWNLOAD_PROGRESS:
				break;
		}
	}

	/**
	 * Reject and drop the awaiter for a model.
	 */
	private rejectStatus(modelId: string, error: Error): void {
		const waiter = this.statusWaiters.get(modelId);

		if (waiter) {
			this.statusWaiters.delete(modelId);
			waiter.reject(error);
		}
	}

	/**
	 * Drop a model row reported gone by the feed and settle its awaiters.
	 */
	private removeRouterModel(modelId: string): void {
		if (this.host.routerModels.findIndex((m) => m.id === modelId) === -1) return;

		this.host.routerModels = this.host.routerModels.filter((m) => m.id !== modelId);
		this.loadProgress.delete(modelId);
		this.rejectStatus(modelId, new Error(`Model removed: ${this.host.toDisplayName(modelId)}`));
	}

	private requestReconcile(): void {
		if (this.reconcileTimer) clearTimeout(this.reconcileTimer);

		this.reconcileTimer = null;
		this.scheduleReconcile(0);
	}

	/**
	 * Read the feed and reconnect until unsubscribed.
	 */
	private async runStatusReader(signal: AbortSignal): Promise<void> {
		await ModelsService.watchModelEvents(
			signal,
			(event) => this.applyStatusEvent(event),
			(connection) => (this.connection = connection)
		);
	}

	private scheduleReconcile(delay?: number): void {
		if (!this.statusReaderActive || this.reconcileTimer) return;

		this.reconcileTimer = setTimeout(
			async () => {
				this.reconcileTimer = null;

				try {
					await this.host.fetchRouterModels();
				} finally {
					this.scheduleReconcile(this.isTransitionInProgress() ? 1000 : 10000);
				}
			},
			delay ?? (this.isTransitionInProgress() ? 1000 : 10000)
		);
	}

	/**
	 * Update one model row status in place, reassigning to trigger reactivity.
	 */
	private setRouterModelStatus(modelId: string, data: ApiModelsSseData): void {
		const idx = this.host.routerModels.findIndex((m) => m.id === modelId);

		if (idx === -1) return;

		const current = this.host.routerModels[idx];
		const next = [...this.host.routerModels];

		next[idx] = {
			...current,
			status: {
				...current.status,
				active_requests: data.active_requests ?? current.status.active_requests,
				exit_code: data.exit_code,
				failed:
					data.status === ServerModelStatus.FAILED ||
					(data.status === ServerModelStatus.UNLOADED && (data.exit_code ?? 0) !== 0),
				progress: data.progress,
				value: data.status
			}
		};
		this.host.routerModels = next;
	}

	/**
	 * Resolve and drop the awaiter when the model reaches its target status.
	 */
	private settleStatus(modelId: string, status: ServerModelStatus): void {
		const waiter = this.statusWaiters.get(modelId);

		if (waiter && waiter.target === status) {
			this.statusWaiters.delete(modelId);
			waiter.resolve();
		}
	}

	/**
	 * Register an awaiter that resolves when the feed reports target status.
	 * One operation runs per model at a time, so one awaiter per model is kept.
	 */
	private waitForStatus(modelId: string, target: ServerModelStatus): Promise<void> {
		return new Promise((resolve, reject) => {
			this.statusWaiters.set(modelId, { reject, resolve, target });
		});
	}
}
