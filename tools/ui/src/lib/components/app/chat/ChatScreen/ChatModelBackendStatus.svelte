<script lang="ts">
	import { CheckCircle2, CircleAlert, Loader2, Radio, Server } from '@lucide/svelte';
	import { ServerModelStatus } from '$lib/enums';
	import { modelsStore, serverStore } from '$lib/stores';
	import { modelLoadFraction } from '$lib/utils';
	import { modelLoadStageLabel } from '$lib/utils/progress';
	import { onDestroy, onMount } from 'svelte';

	interface Props {
		isEmpty?: boolean;
	}

	let { isEmpty = false }: Props = $props();
	let now = $state(Date.now());
	let clock: ReturnType<typeof setInterval> | null = null;

	const MIB = 1024 * 1024;
	const FAST_LOAD_BYTES_PER_SECOND = 800 * MIB;
	const SLOW_LOAD_BYTES_PER_SECOND = 550 * MIB;

	let operation = $derived(modelsStore.status.backendOperation);
	let transitionModelId = $derived(modelsStore.status.getTransitionModelId());
	let transitionActive = $derived(modelsStore.status.isTransitionInProgress());
	let transitionModel = $derived(
		transitionModelId
			? (modelsStore.routerModels.find((model) => model.id === transitionModelId) ?? null)
			: null
	);
	let loadedModel = $derived(
		modelsStore.routerModels.find(
			(model) =>
				model.status.value === ServerModelStatus.LOADED ||
				model.status.value === ServerModelStatus.SLEEPING
		) ?? null
	);
	let activeRequests = $derived(loadedModel?.status.active_requests ?? 0);
	let backendBusy = $derived(!transitionActive && activeRequests > 0);
	let operationFailed = $derived(
		(operation?.phase === 'failed' &&
			(!transitionModelId || operation.target === transitionModelId)) ||
			Boolean(
				operation?.target &&
				modelsStore.routerModels.find((model) => model.id === operation?.target)?.status.failed
			)
	);
	let targetId = $derived(transitionModelId || operation?.target || loadedModel?.id || null);
	let targetName = $derived(targetId ? modelsStore.toDisplayName(targetId) : 'the selected model');
	let progress = $derived(
		transitionModelId ? modelsStore.status.getLoadProgress(transitionModelId) : null
	);
	let fraction = $derived(modelLoadFraction(progress));
	let percent = $derived(Math.round(fraction * 100));
	let hasMeasuredProgress = $derived(Boolean(progress && fraction > 0));
	let startedAt = $derived(
		transitionModelId ? modelsStore.status.getLoadStartedAt(transitionModelId) : null
	);
	let elapsedMs = $derived(startedAt ? Math.max(0, now - startedAt) : 0);
	let artifactBytes = $derived(transitionModel?.status.artifact_bytes ?? 0);
	let remainingRange = $derived.by(() => {
		if (!transitionActive || artifactBytes <= 0 || !hasMeasuredProgress) return null;

		const remainingBytes = artifactBytes * Math.max(0, 1 - fraction);
		const low = (remainingBytes / FAST_LOAD_BYTES_PER_SECOND) * 1000;
		const high = (remainingBytes / SLOW_LOAD_BYTES_PER_SECOND) * 1000;

		return { high, low };
	});
	let phase = $derived(
		operation?.active && operation.target === transitionModelId
			? operation.phase
			: transitionModel?.status.value === ServerModelStatus.LOADING
				? 'loading'
				: transitionActive
					? 'requesting'
					: operation?.phase
	);
	let detailed = $derived(transitionActive || operationFailed || backendBusy);

	function formatDuration(ms: number): string {
		const seconds = Math.max(0, Math.round(ms / 1000));
		const minutes = Math.floor(seconds / 60);
		const remainder = seconds % 60;

		return minutes > 0 ? `${minutes}m ${remainder.toString().padStart(2, '0')}s` : `${seconds}s`;
	}

	function formatEta(): string {
		if (!remainingRange) return 'ETA will appear as soon as the backend reports enough data.';

		const low = formatDuration(remainingRange.low);
		const high = formatDuration(remainingRange.high);

		return low === high ? `About ${low} remaining.` : `Roughly ${low}-${high} remaining.`;
	}

	let title = $derived.by(() => {
		if (operationFailed) return `I could not load ${targetName}`;

		if (backendBusy && loadedModel) {
			return `Backend busy: ${modelsStore.toDisplayName(loadedModel.id)} is answering`;
		}

		if (phase === 'requesting') return `I am contacting the backend for ${targetName}`;

		if (phase === 'waiting_for_requests') return `I am waiting to switch to ${targetName}`;

		if (transitionActive && operation?.action === 'unload') return `I am releasing ${targetName}`;

		if (phase === 'unloading') return `Your switch to ${targetName} is in progress`;

		if (phase === 'preparing' || phase === 'preflight') return `I am preparing ${targetName}`;

		if (phase === 'loading' && !hasMeasuredProgress) return `I am reading ${targetName} from disk`;

		if (phase === 'loading') return `I am loading ${targetName}`;

		if (phase === 'finalizing') return `I am finishing ${targetName}`;

		if (loadedModel) return `Backend ready: ${modelsStore.toDisplayName(loadedModel.id)}`;

		return 'Backend ready for a model';
	});

	let description = $derived.by(() => {
		if (operationFailed) {
			return (
				operation?.error ||
				'The router reports that the model process stopped before becoming ready.'
			);
		}

		if (backendBusy) {
			return `${activeRequests} active request${activeRequests === 1 ? ' is' : 's are'} using the worker. The model is healthy, but another generation must finish before its slot is free.`;
		}

		if (phase === 'requesting') {
			return 'Your request is being handed to the router. The interface will show the backend-owned phase as soon as it is acknowledged.';
		}

		if (phase === 'waiting_for_requests') {
			return 'The current model is still generating a response. The router will not interrupt it; this switch will continue automatically when that request releases the worker.';
		}

		if (phase === 'unloading') {
			return 'The backend accepted your choice. It is unloading the previous model and waiting for GPU memory to return to its safe baseline. Sending and further model changes are locked until this finishes.';
		}

		if (phase === 'preparing' || phase === 'preflight') {
			return 'The backend owns this operation and is checking memory, files and the Vulkan worker. You can safely wait here, even if this page is refreshed.';
		}

		if (phase === 'loading') {
			if (!hasMeasuredProgress) {
				return 'The worker is actively reading and mapping the model shards from SSD. llama.cpp does not publish a percentage during this first phase; zero would not mean that the load is stalled.';
			}

			const stage = progress
				? modelLoadStageLabel(progress.current).toLowerCase()
				: 'opening model shards';

			return `The real llama.cpp worker is ${stage}. The chat will unlock only after the router reports that the model is ready.`;
		}

		if (phase === 'finalizing') {
			return 'The worker has reached its final readiness checks. I am waiting for the router to confirm that requests can be served.';
		}

		if (loadedModel)
			return 'The router confirms that this model is loaded and can receive messages.';

		return 'No model is resident. Choose one from the selector when you are ready.';
	});

	let connectionLabel = $derived.by(() => {
		if (modelsStore.status.connection === 'live') return 'Live backend status';

		if (modelsStore.status.connection === 'reconnecting')
			return 'Reconnecting; snapshot polling is active';

		if (modelsStore.status.connection === 'connecting') return 'Connecting to backend status';

		return 'Backend status feed stopped';
	});

	onMount(() => {
		clock = setInterval(() => (now = Date.now()), 1000);
	});

	onDestroy(() => {
		if (clock) clearInterval(clock);
	});
</script>

{#if serverStore.isRouterMode}
	<div
		aria-live="polite"
		class={[
			'pointer-events-auto mx-auto mb-3 w-full max-w-3xl rounded-xl border bg-background/95 shadow-sm backdrop-blur',
			detailed ? 'p-4' : 'px-3 py-2',
			isEmpty && detailed && 'mb-5'
		]}
		role="status"
	>
		<div class="flex items-start gap-3">
			<div
				class={[
					'mt-0.5 flex h-8 w-8 shrink-0 items-center justify-center rounded-full',
					operationFailed
						? 'bg-destructive/10 text-destructive'
						: transitionActive || backendBusy
							? 'bg-amber-500/10 text-amber-500'
							: 'bg-emerald-500/10 text-emerald-500'
				]}
			>
				{#if operationFailed}
					<CircleAlert class="h-4 w-4" />
				{:else if transitionActive || backendBusy}
					<Loader2 class="h-4 w-4 animate-spin" />
				{:else if loadedModel}
					<CheckCircle2 class="h-4 w-4" />
				{:else}
					<Server class="h-4 w-4" />
				{/if}
			</div>

			<div class="min-w-0 flex-1">
				<div class="flex flex-wrap items-center justify-between gap-x-3 gap-y-1">
					<p class="min-w-0 truncate text-sm font-medium" title={targetId ?? undefined}>{title}</p>

					<span class="inline-flex items-center gap-1.5 text-[11px] text-muted-foreground">
						{#if modelsStore.status.connection === 'live'}
							<Radio class="h-3 w-3 text-emerald-500" />
						{:else}
							<span class="h-2 w-2 rounded-full bg-amber-500"></span>
						{/if}
						{connectionLabel}
					</span>
				</div>

				{#if detailed}
					<p class="mt-1.5 text-sm leading-relaxed text-muted-foreground">{description}</p>

					{#if transitionActive}
						<div class="mt-3 h-1.5 overflow-hidden rounded-full bg-muted">
							<div
								style:width={hasMeasuredProgress ? `${Math.max(1, percent)}%` : '8%'}
								class={[
									'h-full rounded-full bg-amber-500 transition-[width] duration-300',
									!hasMeasuredProgress && 'animate-pulse'
								]}
							></div>
						</div>

						<div
							class="mt-2 flex flex-wrap justify-between gap-x-4 gap-y-1 text-xs text-muted-foreground"
						>
							<span
								>{hasMeasuredProgress
									? `${percent}% from backend`
									: 'Reading model shards from SSD - percentage not published yet'}</span
							>

							<span>
								Elapsed {formatDuration(elapsedMs)}. {formatEta()}
							</span>
						</div>
					{/if}
				{:else}
					<p class="mt-0.5 truncate text-xs text-muted-foreground">{description}</p>
				{/if}
			</div>
		</div>
	</div>
{/if}
