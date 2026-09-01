<script lang="ts">
	import ToolCallBlock from './ToolCallBlock.svelte';
	import {
		Clock3,
		Cuboid,
		Gauge,
		Image as ImageIcon,
		Layers3,
		ListOrdered,
		Timer,
		X
	} from '@lucide/svelte';
	import { base } from '$app/paths';
	import { DialogGeneratedImagePreview } from '$lib/components/app/dialogs';
	import { AttachmentType, BuiltInTool } from '$lib/enums';
	import { DatabaseService } from '$lib/services/database.service';
	import { type MediaJobRequest, MediaService } from '$lib/services/media.service';
	import { conversationsStore, mediaStore } from '$lib/stores';
	import type {
		AgenticSection,
		DatabaseMessageExtra,
		DatabaseMessageExtraGeneratedMedia,
		DatabaseMessageExtraGeneratedMediaTerminal,
		DatabaseMessageExtraImageFile,
		DatabaseMessageExtraVideoFile
	} from '$lib/types';
	import { createBase64DataUrl } from '$lib/utils/data-url';
	import { onDestroy, onMount } from 'svelte';

	interface Props {
		section: AgenticSection;
		open: boolean;
		isStreaming: boolean;
		attachments?: DatabaseMessageExtra[];
		onToggle?: () => void;
	}

	let { attachments = [], isStreaming, onToggle, open, section }: Props = $props();

	let resumeController: AbortController | null = null;
	let resumeError = $state('');
	let cancelError = $state('');
	let cancelling = $state(false);
	let imagePreviewOpen = $state(false);
	let livePreviewOpen = $state(false);
	const jobState = $derived(mediaStore.get(section.toolCallId));
	const isImage = $derived(section.toolName === BuiltInTool.BROWSER_GENERATE_IMAGE);
	const resultAttachments = $derived(section.toolResultExtras ?? attachments);
	const generatedAttachments = $derived(
		resultAttachments.filter(
			(item): item is DatabaseMessageExtraGeneratedMedia =>
				item.type === AttachmentType.GENERATED_MEDIA
		)
	);
	const generatedAttachment = $derived(
		[...generatedAttachments].reverse().find((item) => item.status !== 'pending') ??
			generatedAttachments[generatedAttachments.length - 1] ??
			null
	);
	const legacyAttachments = $derived(
		resultAttachments.filter(
			(item): item is DatabaseMessageExtraImageFile | DatabaseMessageExtraVideoFile =>
				item.type === AttachmentType.IMAGE || item.type === AttachmentType.VIDEO
		)
	);
	const persistedTerminal = $derived(
		generatedAttachment?.status === 'failed' || generatedAttachment?.status === 'cancelled'
			? (generatedAttachment as DatabaseMessageExtraGeneratedMediaTerminal)
			: null
	);
	const liveJob = $derived(
		jobState && !['completed', 'failed', 'cancelled'].includes(jobState.status) ? jobState : null
	);
	const isLive = $derived(
		Boolean(
			generatedAttachment?.status !== 'completed' &&
			generatedAttachment?.status !== 'failed' &&
			generatedAttachment?.status !== 'cancelled' &&
			(liveJob || (!jobState && generatedAttachment?.status === 'pending'))
		)
	);
	const isPrompting = $derived(!jobState && !generatedAttachment);
	const title = $derived.by(() => {
		if (persistedTerminal?.status === 'cancelled' || jobState?.status === 'cancelled') {
			return isImage ? 'Image generation cancelled' : 'Video generation cancelled';
		}

		if (persistedTerminal?.status === 'failed' || jobState?.status === 'failed' || resumeError) {
			return isImage ? 'Image generation failed' : 'Video generation failed';
		}

		if (generatedAttachment?.status === 'completed' || jobState?.status === 'completed') {
			return isImage ? 'Image generated' : 'Video generated';
		}

		if (isLive) return isImage ? 'Generating image' : 'Generating video';

		if (isPrompting) return isImage ? 'Generating prompt' : 'Generating video prompt';

		return isImage ? 'Generate image' : 'Generate video';
	});
	const eta = $derived(
		jobState?.eta_seconds == null
			? 'calibrazione ETA in corso'
			: jobState.eta_seconds <= 0
				? 'quasi pronto'
				: `circa ${formatDuration(jobState.eta_seconds)} rimanenti`
	);
	const expectedTotalSeconds = $derived(
		jobState?.estimated_total_seconds ??
			(jobState?.eta_seconds == null
				? null
				: Math.max(0, jobState.elapsed_seconds) + Math.max(0, jobState.eta_seconds))
	);
	const progressPercent = $derived(Math.max(0, Math.min(100, Number(jobState?.progress ?? 0))));
	const modelLabel = $derived(
		jobState?.model_label || jobState?.model || generatedAttachment?.model || 'modello media'
	);
	const resolution = $derived(
		jobState?.width && jobState?.height ? `${jobState.width}x${jobState.height}` : null
	);
	const hasMeasuredSteps = $derived(
		typeof jobState?.sample_step === 'number' &&
			jobState.sample_step > 0 &&
			typeof jobState?.sample_steps === 'number' &&
			jobState.sample_steps > 0
	);
	const progressIsMeasured = $derived(jobState?.progress_source === 'measured');
	const previewSrc = $derived(
		jobState?.preview_url ? cacheBustedMediaUrl(jobState.preview_url, jobState.preview_step) : null
	);

	function formatDuration(seconds: number): string {
		if (seconds < 60) return `${Math.max(0, Math.round(seconds))}s`;

		const minutes = Math.floor(seconds / 60);
		const remainder = Math.round(seconds % 60);

		return remainder ? `${minutes}m ${remainder}s` : `${minutes}m`;
	}

	function phaseLabel(phase: string | undefined): string {
		const labels: Record<string, string> = {
			cancelled: 'Annullata',
			completed: 'Completata',
			conditioning: 'Conditioning del prompt',
			failed: 'Errore',
			generating: 'Diffusione',
			loading_model: 'Caricamento modello',
			memory_check: 'Controllo UMA',
			queued: 'In coda',
			releasing_memory: 'Rilascio UMA',
			sampling: 'Sampling',
			saving: 'Codifica e salvataggio',
			unloading_llm: 'Scaricamento LLM'
		};

		if (!phase) return 'Connessione al backend';

		return labels[phase] ?? phase.replaceAll('_', ' ');
	}

	function mediaUrl(path: string): string {
		return /^(?:blob:|data:|https?:\/\/)/i.test(path) ? path : `${base}${path}`;
	}

	function cacheBustedMediaUrl(path: string, step: number | null | undefined): string {
		const resolved = mediaUrl(path);

		if (!Number.isFinite(step)) return resolved;

		return `${resolved}${resolved.includes('?') ? '&' : '?'}preview_step=${step}`;
	}

	async function cancelGeneration() {
		const jobId = jobState?.id || generatedAttachment?.jobId;

		if (!jobId || cancelling) return;

		cancelError = '';
		cancelling = true;

		try {
			const next = await MediaService.cancelJob(jobId);

			if (section.toolCallId) mediaStore.set(section.toolCallId, next);
		} catch (error) {
			cancelError = error instanceof Error ? error.message : String(error);
		} finally {
			cancelling = false;
		}
	}

	function replaceGeneratedAttachment(next: DatabaseMessageExtraGeneratedMedia) {
		const merged = [...resultAttachments];
		const index = merged.findIndex(
			(candidate) =>
				candidate.type === AttachmentType.GENERATED_MEDIA && candidate.jobId === next.jobId
		);

		if (index >= 0) merged[index] = next;
		else merged.push(next);

		return merged;
	}

	async function persistAttachment(
		content: string,
		attachment: DatabaseMessageExtraGeneratedMedia
	) {
		const extra = replaceGeneratedAttachment(attachment);

		await DatabaseService.updateMessage(attachment.ownerMessageId, { content, extra });

		const index = conversationsStore.findMessageIndex(attachment.ownerMessageId);

		if (index >= 0) conversationsStore.updateMessageAtIndex(index, { content, extra });
	}

	async function persistResumedJob(attachment: DatabaseMessageExtraGeneratedMedia) {
		if (attachment.status !== 'pending' || !section.toolCallId) return;

		// A live in-memory entry means the original agentic flow is already polling it.
		if (mediaStore.get(section.toolCallId)) return;

		resumeController = new AbortController();

		try {
			const job = await mediaStore.track(
				attachment.jobId,
				section.toolCallId,
				resumeController.signal
			);
			const terminal =
				job.status === 'completed' && job.result
					? MediaService.completedAttachment(
							job,
							{
								kind: attachment.kind,
								model: attachment.model,
								prompt: attachment.prompt
							} as MediaJobRequest,
							attachment.conversationId,
							attachment.ownerMessageId
						)
					: MediaService.terminalAttachment(
							job,
							{
								kind: attachment.kind,
								model: attachment.model,
								prompt: attachment.prompt
							} as MediaJobRequest,
							attachment.conversationId,
							attachment.ownerMessageId
						);
			const content =
				terminal.status === 'completed'
					? MediaService.completedSummary(job, terminal)
					: MediaService.terminalSummary(terminal);

			await persistAttachment(content, terminal);
		} catch (error) {
			if (!resumeController.signal.aborted) {
				const message = error instanceof Error ? error.message : String(error);
				const failed: DatabaseMessageExtraGeneratedMediaTerminal = {
					...attachment,
					error: message,
					status: 'failed'
				};

				resumeError = message;
				await persistAttachment(MediaService.terminalSummary(failed), failed);
			}
		}
	}

	onMount(() => {
		if (generatedAttachment?.status === 'pending') {
			void persistResumedJob(generatedAttachment);
		}
	});

	onDestroy(() => resumeController?.abort());
</script>

<ToolCallBlock
	extraLiveStreaming={isLive}
	{isStreaming}
	meta={persistedTerminal?.error || jobState?.error || resumeError
		? { errorMessage: persistedTerminal?.error || jobState?.error || resumeError }
		: null}
	{onToggle}
	{open}
	{section}
	spinIconWhenActive
	{title}
>
	{#snippet children(_meta, _ctx)}
		{#if persistedTerminal}
			<div class="text-xs text-destructive">
				{persistedTerminal.status === 'cancelled'
					? 'Generazione annullata'
					: 'Generazione fallita'}:
				{persistedTerminal.error}
			</div>
		{:else if jobState?.status === 'failed' || jobState?.status === 'cancelled'}
			<div class="text-xs text-destructive">
				{jobState.error || jobState.message}
			</div>
		{:else if isLive}
			<div class="text-xs text-muted-foreground">
				Lo stato live dettagliato rimane visibile sotto questo blocco.
			</div>
		{:else if jobState}
			<div class="space-y-3">
				<div class="flex items-center justify-between gap-3 text-xs">
					<span class="font-medium">{jobState.message}</span>

					<span class="shrink-0 tabular-nums text-muted-foreground"
						>{jobState.progress.toFixed(0)}%</span
					>
				</div>

				<div class="h-1.5 overflow-hidden rounded-full bg-muted">
					<div
						style:width={`${jobState.progress}%`}
						class="h-full rounded-full bg-primary transition-[width] duration-500"
					></div>
				</div>

				<div class="flex flex-wrap gap-x-4 gap-y-1 text-[11px] text-muted-foreground">
					<span>{jobState.model}</span>

					<span>{formatDuration(jobState.elapsed_seconds)} trascorsi</span>

					{#if isLive}<span>{eta}</span>{/if}

					{#if jobState.queue_position > 0}<span>coda: {jobState.queue_position}</span>{/if}
				</div>
			</div>
		{:else if generatedAttachment?.status === 'pending'}
			<div class="text-xs text-muted-foreground">
				Ricollegamento al job {generatedAttachment.jobId}...
			</div>
		{:else if resumeError}
			<div class="text-xs text-destructive">{resumeError}</div>
		{/if}
	{/snippet}
</ToolCallBlock>

{#if isLive}
	<section
		aria-busy="true"
		aria-live="polite"
		class="my-3 w-full max-w-3xl overflow-hidden rounded-2xl border border-primary/20 bg-muted/15 shadow-sm"
		data-testid="media-live-card"
	>
		<div class="space-y-3 p-4">
			<div class="flex items-start justify-between gap-4">
				<div class="min-w-0 space-y-1">
					<div class="text-sm font-semibold text-foreground">
						{jobState?.message ?? 'Mi sto ricollegando al processo di generazione locale...'}
					</div>

					<div class="text-xs text-muted-foreground">
						Lo stato arriva direttamente dal backend media e continua ad aggiornarsi ogni secondo.
					</div>
				</div>

				<div class="shrink-0 text-right tabular-nums">
					<div class="text-lg font-semibold">
						{progressIsMeasured ? '' : '~'}{progressPercent.toFixed(0)}%
					</div>

					<div class="text-[10px] uppercase tracking-wide text-muted-foreground">
						{progressIsMeasured ? 'misurato' : 'stimato'}
					</div>
				</div>
			</div>

			<div
				aria-label="Avanzamento generazione media"
				aria-valuemax="100"
				aria-valuemin="0"
				aria-valuenow={progressPercent}
				class="h-2 overflow-hidden rounded-full bg-muted"
				role="progressbar"
			>
				<div
					style:width={`${progressPercent}%`}
					class="h-full rounded-full bg-primary transition-[width] duration-500"
				></div>
			</div>

			<div class="flex flex-wrap gap-2 text-xs text-muted-foreground">
				<span class="media-status-chip" title="Modello scelto dal modello linguistico">
					<Cuboid class="size-3.5" />
					{modelLabel}
				</span>

				<span class="media-status-chip" title="Fase corrente del backend">
					<Layers3 class="size-3.5" />
					{phaseLabel(jobState?.phase)}
				</span>

				<span class="media-status-chip" title="Tempo trascorso">
					<Clock3 class="size-3.5" />
					{formatDuration(jobState?.elapsed_seconds ?? 0)} elapsed
				</span>

				<span class="media-status-chip" title="Tempo rimanente previsto">
					<Timer class="size-3.5" />
					{eta}
				</span>

				{#if expectedTotalSeconds !== null}
					<span class="media-status-chip" title="Durata totale prevista">
						<Timer class="size-3.5" />
						~{formatDuration(expectedTotalSeconds)} total
					</span>
				{/if}

				{#if hasMeasuredSteps}
					<span class="media-status-chip" title="Passo di diffusione misurato">
						<ListOrdered class="size-3.5" />
						step {jobState?.sample_step}/{jobState?.sample_steps}
					</span>
				{/if}

				{#if typeof jobState?.step_seconds === 'number'}
					<span class="media-status-chip" title="Durata dell'ultimo passo di diffusione">
						<Gauge class="size-3.5" />
						{jobState.step_seconds.toFixed(1)}s/step
						{#if typeof jobState.average_step_seconds === 'number'}
							- avg {jobState.average_step_seconds.toFixed(1)}s
						{/if}
					</span>
				{/if}

				{#if typeof jobState?.generation_elapsed_seconds === 'number'}
					<span class="media-status-chip" title="Tempo impiegato dalla sola diffusione">
						<Clock3 class="size-3.5" />
						diffusion {formatDuration(jobState.generation_elapsed_seconds)}
					</span>
				{/if}

				{#if resolution}
					<span class="media-status-chip" title="Risoluzione richiesta">
						<ImageIcon class="size-3.5" />
						{resolution}
					</span>
				{/if}

				{#if (jobState?.queue_position ?? 0) > 0}
					<span class="media-status-chip" title="Posizione nella coda media">
						<ListOrdered class="size-3.5" />
						queue {jobState?.queue_position}
					</span>
				{/if}
			</div>

			{#if previewSrc}
				<figure class="overflow-hidden rounded-xl border bg-black/20">
					<button
						aria-label="Apri anteprima intermedia"
						class="block w-full cursor-zoom-in bg-transparent"
						onclick={() => (livePreviewOpen = true)}
						type="button"
					>
						<img
							alt={`Anteprima intermedia${jobState?.preview_step ? `, step ${jobState.preview_step}` : ''}`}
							class="max-h-[48vh] w-full object-contain"
							src={previewSrc}
						/>
					</button>

					<figcaption class="px-3 py-2 text-[11px] text-muted-foreground">
						Anteprima transitoria{jobState?.preview_step
							? ` - step ${jobState.preview_step}`
							: ''}{jobState?.preview_width && jobState?.preview_height
							? ` - ${jobState.preview_width}x${jobState.preview_height}`
							: ''}; non viene salvata nella chat.
					</figcaption>
				</figure>
			{/if}

			<div class="flex items-center justify-between gap-3">
				<div class="min-w-0 text-xs text-destructive">{cancelError}</div>

				{#if jobState?.can_cancel !== false}
					<button
						aria-label="Annulla generazione media"
						class="inline-flex shrink-0 items-center gap-1.5 rounded-lg border px-3 py-1.5 text-xs font-medium transition-colors hover:bg-destructive/10 hover:text-destructive disabled:cursor-wait disabled:opacity-60"
						disabled={cancelling}
						onclick={cancelGeneration}
						type="button"
					>
						<X class="size-3.5" />
						{cancelling ? 'Cancelling...' : 'Cancel'}
					</button>
				{/if}
			</div>
		</div>
	</section>

	{#if previewSrc}
		<DialogGeneratedImagePreview
			bind:open={livePreviewOpen}
			alt="Anteprima intermedia della generazione"
			src={previewSrc}
		/>
	{/if}
{/if}

{#if generatedAttachment?.status === 'completed'}
	<figure class="my-3 w-full max-w-3xl overflow-hidden rounded-2xl border bg-muted/10">
		{#if generatedAttachment.kind === 'image'}
			<button
				aria-label="Apri immagine generata a piena risoluzione"
				class="block w-full cursor-zoom-in bg-transparent"
				onclick={() => (imagePreviewOpen = true)}
				type="button"
			>
				<img
					alt={generatedAttachment.prompt}
					class="max-h-[75vh] w-full object-contain"
					loading="lazy"
					src={`${base}${generatedAttachment.assetUrl}`}
				/>
			</button>
		{:else}
			<video class="max-h-[75vh] w-full" controls playsinline preload="metadata">
				<source
					src={`${base}${generatedAttachment.assetUrl}`}
					type={generatedAttachment.mimeType}
				/>
			</video>
		{/if}

		<figcaption class="px-4 py-3 text-xs text-muted-foreground">
			{generatedAttachment.model} - {generatedAttachment.prompt}
		</figcaption>
	</figure>

	{#if generatedAttachment.kind === 'image'}
		<DialogGeneratedImagePreview
			bind:open={imagePreviewOpen}
			alt={generatedAttachment.prompt}
			src={`${base}${generatedAttachment.assetUrl}`}
		/>
	{/if}
{/if}

{#each legacyAttachments as attachment (attachment.name)}
	{#if attachment.type === AttachmentType.IMAGE}
		<img
			alt={attachment.name}
			class="my-3 h-auto max-h-[75vh] max-w-full rounded-2xl border object-contain"
			loading="lazy"
			src={attachment.base64Url}
		/>
	{:else}
		<video class="my-3 max-h-[75vh] max-w-full rounded-2xl border" controls playsinline>
			<source
				src={createBase64DataUrl(attachment.mimeType, attachment.base64Data)}
				type={attachment.mimeType}
			/>
		</video>
	{/if}
{/each}

<style>
	.media-status-chip {
		display: inline-flex;
		min-width: 0;
		align-items: center;
		gap: 0.35rem;
		border: 1px solid hsl(var(--border) / 0.7);
		border-radius: 0.5rem;
		background: hsl(var(--muted) / 0.35);
		padding: 0.3rem 0.5rem;
		font-variant-numeric: tabular-nums;
	}
</style>
