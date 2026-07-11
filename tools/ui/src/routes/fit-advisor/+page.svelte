<script lang="ts">
	import { onMount } from 'svelte';
	import { Download, Gauge, RefreshCw, SlidersHorizontal, Zap } from '@lucide/svelte';
	import {
		FitAdvisorService,
		type FitAdvisorDownloadJob,
		type FitAdvisorModel,
		type FitAdvisorModelsResponse
	} from '$lib/services/fit-advisor.service';
	import { modelsStore } from '$lib/stores/models.svelte';
	import { compactModelName, normalizeModelName, uniqueModelTags } from '$lib/utils/model-display';

	let response = $state<FitAdvisorModelsResponse | null>(null);
	let selectedModel = $state<FitAdvisorModel | null>(null);
	let downloadJobs = $state<FitAdvisorDownloadJob[]>([]);
	let downloadStreamController: AbortController | null = null;
	let downloadLastSeq = 0;
	let isLoading = $state(false);
	let isRefreshing = $state(false);
	let isDownloading = $state(false);
	let isConfiguring = $state(false);
	let error = $state('');
	let message = $state('');
	let useCase = $state('all');
	let minFit = $state('marginal');
	let quant = $state('');
	let search = $state('');
	let strategy = $state('balanced');
	let context = $state(131072);
	let limit = $state(300);
	let includeTooTight = $state(false);
	let loadNow = $state(false);

	const models = $derived(response?.models ?? []);
	const system = $derived(response?.system ?? null);
	const catalog = $derived(response?.catalog ?? null);
	const hasPendingDownload = $derived(
		downloadJobs.some((job) => isActiveDownloadStatus(job.status))
	);
	const contextOptions = [
		{ value: 4096, label: '4k' },
		{ value: 8192, label: '8k' },
		{ value: 16384, label: '16k' },
		{ value: 32768, label: '32k' },
		{ value: 65536, label: '64k' },
		{ value: 131072, label: '131k' },
		{ value: 262144, label: '262k' },
		{ value: 1048576, label: '1M' }
	];

	onMount(() => {
		void loadModels(true);
		void loadDownloads();
		startDownloadStream();
		return () => {
			downloadStreamController?.abort();
		};
	});

	async function loadDownloads() {
		try {
			const result = await FitAdvisorService.listDownloads();
			downloadJobs = result.data;
			downloadLastSeq = Math.max(downloadLastSeq, ...result.data.map((job) => job.seq ?? 0), 0);
		} catch (e) {
			console.warn('Fit Advisor downloads list failed', e);
		}
	}

	function upsertDownloadJob(job: FitAdvisorDownloadJob) {
		downloadLastSeq = Math.max(downloadLastSeq, job.seq ?? 0);
		const index = downloadJobs.findIndex((item) => item.id === job.id);
		downloadJobs =
			index === -1
				? [job, ...downloadJobs]
				: downloadJobs.map((item, itemIndex) => (itemIndex === index ? job : item));

		if (
			selectedModel &&
			(job.model_id === selectedModel.id || job.hf_ref === selectedModel.download?.hf_ref)
		) {
			selectedModel = {
				...selectedModel,
				local_path: job.local_path ?? selectedModel.local_path,
				target_dir: job.target_dir ?? selectedModel.target_dir,
				download_progress: job,
				download_status:
					job.status === 'downloaded'
						? 'downloaded'
						: job.status === 'partial'
							? 'partial'
							: job.status === 'failed'
								? 'failed'
								: 'downloading',
				downloaded: job.status === 'downloaded'
			};
		}
		if (job.status === 'downloaded' || job.status === 'failed') {
			void loadModels(false);
		}
	}

	function startDownloadStream() {
		downloadStreamController?.abort();
		const controller = new AbortController();
		downloadStreamController = controller;
		const run = async () => {
			while (!controller.signal.aborted) {
				try {
					await FitAdvisorService.streamDownloads(
						(event) => upsertDownloadJob(event.data),
						controller.signal,
						downloadLastSeq
					);
				} catch (e) {
					if (controller.signal.aborted) break;
					console.warn('Fit Advisor download stream disconnected', e);
					await new Promise((resolve) => setTimeout(resolve, 2000));
				}
			}
		};
		void run();
	}

	async function loadModels(refresh = false) {
		isLoading = true;
		error = '';
		message = refresh ? 'Refreshing llmfit catalog...' : 'Loading recommendations...';
		try {
			const next = await FitAdvisorService.models({
				refresh,
				use_case: useCase,
				min_fit: minFit,
				quant,
				search,
				strategy,
				context,
				limit,
				include_too_tight: includeTooTight
			});
			response = next;
			if (selectedModel) {
				selectedModel =
					next.models.find((model) => model.id === selectedModel?.id) ?? next.models[0] ?? null;
			} else {
				selectedModel = next.models[0] ?? null;
			}
			message = `Loaded ${next.returned_models} recommendations from ${next.total_catalog_models} catalog entries.`;
		} catch (e) {
			error = e instanceof Error ? e.message : String(e);
			message = '';
		} finally {
			isLoading = false;
		}
	}

	async function refreshCatalog() {
		isRefreshing = true;
		error = '';
		message = 'Refreshing catalog cache...';
		try {
			await FitAdvisorService.refreshCatalog();
			await loadModels(false);
		} catch (e) {
			error = e instanceof Error ? e.message : String(e);
		} finally {
			isRefreshing = false;
		}
	}

	async function downloadModel(model: FitAdvisorModel) {
		if (!model.download) return;
		isDownloading = true;
		error = '';
		message = `${hasPendingDownload ? 'Queueing' : 'Starting'} download for ${model.download.hf_ref}...`;
		try {
			const result = await FitAdvisorService.download(model);
			if (result.job) {
				upsertDownloadJob(result.job);
			}
			message = result.already_present
				? `${result.model} is already present.`
				: `Download started for ${result.model}.`;
			await modelsStore.fetchRouterModels();
			await loadModels(false);
		} catch (e) {
			error = e instanceof Error ? e.message : String(e);
		} finally {
			isDownloading = false;
		}
	}

	async function configureModel(model: FitAdvisorModel) {
		isConfiguring = true;
		error = '';
		message = `Writing preset for ${model.id}...`;
		try {
			const job = downloadJobFor(model);
			const enrichedModel: FitAdvisorModel = {
				...model,
				local_path: model.local_path ?? job?.local_path ?? null,
				target_dir: model.target_dir ?? job?.target_dir,
				download_progress: job ?? model.download_progress
			};
			const result = await FitAdvisorService.configure(enrichedModel, loadNow);
			message = result.loaded
				? `Configured and loading ${result.model}.`
				: `Configured ${result.model} in ${result.models_preset}.`;
			await modelsStore.fetchRouterModels();
			await loadModels(false);
		} catch (e) {
			error = e instanceof Error ? e.message : String(e);
		} finally {
			isConfiguring = false;
		}
	}

	function fmtGb(value: number | undefined, digits = 1): string {
		if (typeof value !== 'number' || !Number.isFinite(value)) return 'n/a';
		return value.toFixed(digits) + ' GB';
	}

	function fmtNum(value: number | undefined, digits = 1): string {
		if (typeof value !== 'number' || !Number.isFinite(value)) return 'n/a';
		return value.toFixed(digits);
	}

	function fmtBytes(value: number | undefined): string {
		if (typeof value !== 'number' || !Number.isFinite(value) || value <= 0) return 'n/a';
		const units = ['B', 'KB', 'MB', 'GB', 'TB'];
		let scaled = value;
		let index = 0;
		while (scaled >= 1024 && index < units.length - 1) {
			scaled /= 1024;
			index += 1;
		}
		return `${scaled.toFixed(index === 0 ? 0 : 1)} ${units[index]}`;
	}

	function fmtSpeed(value: number | undefined): string {
		if (typeof value !== 'number' || !Number.isFinite(value) || value <= 0) return 'idle';
		return `${fmtBytes(value)}/s`;
	}

	function fitClass(level: string): string {
		if (level === 'perfect') return 'bg-emerald-500/15 text-emerald-300 border-emerald-500/30';
		if (level === 'good') return 'bg-cyan-500/15 text-cyan-300 border-cyan-500/30';
		if (level === 'marginal') return 'bg-amber-500/15 text-amber-300 border-amber-500/30';
		return 'bg-red-500/15 text-red-300 border-red-500/30';
	}

	function argsText(model: FitAdvisorModel): string {
		return model.recommended_args.join(' ');
	}

	function downloadJobFor(model: FitAdvisorModel): FitAdvisorDownloadJob | null {
		if (model.download?.hf_ref) {
			return (
				downloadJobs.find((job) => job.hf_ref === model.download?.hf_ref) ??
				model.download_progress ??
				null
			);
		}
		return downloadJobs.find((job) => job.model_id === model.id) ?? model.download_progress ?? null;
	}

	function isActiveDownloadStatus(status: string): boolean {
		return status === 'queued' || status === 'resolving' || status === 'downloading';
	}

	function statusFor(model: FitAdvisorModel): string {
		if (model.configured || model.installed) return 'configured';
		const job = downloadJobFor(model);
		if (job) {
			if (isActiveDownloadStatus(job.status)) return job.status;
			return job.status;
		}
		if (model.download_status) return model.download_status;
		return model.downloaded ? 'downloaded' : 'available';
	}

	function statusClass(status: string): string {
		if (status === 'configured') return 'bg-emerald-500/15 text-emerald-300 border-emerald-500/30';
		if (status === 'downloaded') return 'bg-cyan-500/15 text-cyan-300 border-cyan-500/30';
		if (status === 'partial') return 'bg-amber-500/15 text-amber-300 border-amber-500/30';
		if (status === 'downloading' || status === 'resolving' || status === 'queued')
			return 'bg-amber-500/15 text-amber-300 border-amber-500/30';
		if (status === 'failed') return 'bg-red-500/15 text-red-300 border-red-500/30';
		return 'bg-muted text-muted-foreground border-border';
	}

	function canDownload(model: FitAdvisorModel): boolean {
		const status = statusFor(model);
		return (
			Boolean(model.download) &&
			!['queued', 'resolving', 'downloading', 'downloaded', 'configured'].includes(status)
		);
	}

	function canFit(model: FitAdvisorModel): boolean {
		const status = statusFor(model);
		return status === 'downloaded' || status === 'configured';
	}

	function downloadButtonLabel(model: FitAdvisorModel): string {
		if (!model.download) return 'No GGUF source';
		const job = downloadJobFor(model);
		if (job && isActiveDownloadStatus(job.status)) {
			const progress =
				typeof job.percent === 'number' && Number.isFinite(job.percent)
					? ` ${Math.round(job.percent)}%`
					: '';
			if (job.status === 'queued') return 'Queued';
			if (job.status === 'resolving') return 'Resolving';
			return `Downloading${progress}`;
		}
		if (hasPendingDownload && statusFor(model) === 'available') return 'Queue DL';
		return statusFor(model) === 'partial' ? 'Resume DL' : 'Download';
	}
</script>

<svelte:head>
	<title>Fit Advisor</title>
</svelte:head>

<main class="min-h-screen bg-background text-foreground">
	<div class="flex w-full flex-col gap-5 px-4 py-5 lg:px-6">
		<header class="flex flex-col gap-3 border-b pb-4 md:flex-row md:items-end md:justify-between">
			<div>
				<div class="flex items-center gap-2 text-sm text-muted-foreground">
					<Gauge class="h-4 w-4" />
					llmfit logic, native llama.cpp router
				</div>
				<h1 class="mt-1 text-2xl font-semibold tracking-normal">Fit Advisor</h1>
				<p class="mt-1 max-w-3xl text-sm text-muted-foreground">
					Rank GGUF models against this machine, estimate memory and throughput, then download or
					write a router preset.
				</p>
			</div>
			<div class="flex flex-wrap gap-2">
				<button
					type="button"
					class="inline-flex h-10 items-center gap-2 rounded-md border px-3 text-sm hover:bg-muted disabled:opacity-50"
					disabled={isRefreshing || isLoading}
					onclick={refreshCatalog}
				>
					<RefreshCw class="h-4 w-4" />
					Refresh Catalog
				</button>
				<button
					type="button"
					class="inline-flex h-10 items-center gap-2 rounded-md bg-primary px-3 text-sm text-primary-foreground hover:bg-primary/90 disabled:opacity-50"
					disabled={isLoading}
					onclick={() => loadModels(false)}
				>
					<SlidersHorizontal class="h-4 w-4" />
					Apply Filters
				</button>
			</div>
		</header>

		{#if system}
			<section class="grid gap-3 md:grid-cols-2 xl:grid-cols-5">
				<div class="rounded-lg border bg-card p-3">
					<div class="text-xs text-muted-foreground">CPU</div>
					<div class="mt-1 truncate text-sm font-medium">{system.cpu_name}</div>
					<div class="mt-1 text-xs text-muted-foreground">{system.cpu_cores} threads</div>
				</div>
				<div class="rounded-lg border bg-card p-3">
					<div class="text-xs text-muted-foreground">RAM</div>
					<div class="mt-1 text-sm font-medium">
						{fmtGb(system.fit_ram_capacity_gb ?? system.total_ram_gb)} total
					</div>
					<div class="mt-1 text-xs text-muted-foreground">
						{fmtGb(system.available_ram_gb)} currently free
					</div>
				</div>
				<div class="rounded-lg border bg-card p-3">
					<div class="text-xs text-muted-foreground">GPU</div>
					<div class="mt-1 truncate text-sm font-medium">
						{system.gpu_name || 'No GPU detected'}
					</div>
					<div class="mt-1 text-xs text-muted-foreground">{system.backend}</div>
				</div>
				<div class="rounded-lg border bg-card p-3">
					<div class="text-xs text-muted-foreground">VRAM Per GPU</div>
					<div class="mt-1 text-sm font-medium">{fmtGb(system.gpu_vram_gb)}</div>
					<div class="mt-1 text-xs text-muted-foreground">{system.gpu_count} GPU(s)</div>
				</div>
				<div class="rounded-lg border bg-card p-3">
					<div class="text-xs text-muted-foreground">Aggregate VRAM</div>
					<div class="mt-1 text-sm font-medium">{fmtGb(system.total_gpu_vram_gb)}</div>
					<div class="mt-1 text-xs text-muted-foreground">used only for split estimates</div>
				</div>
			</section>
		{/if}

		<section class="rounded-lg border bg-card p-3">
			<div class="grid gap-3 md:grid-cols-2 xl:grid-cols-7">
				<label class="text-sm">
					<span class="text-xs text-muted-foreground">Use Case</span>
					<select
						class="mt-1 h-10 w-full rounded-md border bg-background px-2"
						bind:value={useCase}
					>
						<option value="all">All</option>
						<option value="coding">Coding</option>
						<option value="reasoning">Reasoning</option>
						<option value="chat">Chat</option>
						<option value="general">General</option>
						<option value="multimodal">Multimodal</option>
					</select>
				</label>
				<label class="text-sm">
					<span class="text-xs text-muted-foreground">Minimum Fit</span>
					<select class="mt-1 h-10 w-full rounded-md border bg-background px-2" bind:value={minFit}>
						<option value="perfect">Perfect</option>
						<option value="good">Good</option>
						<option value="marginal">Marginal</option>
						<option value="too_tight">Too Tight</option>
					</select>
				</label>
				<label class="text-sm">
					<span class="text-xs text-muted-foreground">Quant</span>
					<input
						class="mt-1 h-10 w-full rounded-md border bg-background px-2"
						placeholder="Q4, Q8, IQ..."
						bind:value={quant}
					/>
				</label>
				<label class="text-sm">
					<span class="text-xs text-muted-foreground">Search</span>
					<input
						class="mt-1 h-10 w-full rounded-md border bg-background px-2"
						placeholder="qwen, coder..."
						bind:value={search}
					/>
				</label>
				<label class="text-sm">
					<span class="text-xs text-muted-foreground">Context</span>
					<select
						class="mt-1 h-10 w-full rounded-md border bg-background px-2"
						bind:value={context}
					>
						{#each contextOptions as option (option.value)}
							<option value={option.value}>{option.label}</option>
						{/each}
					</select>
				</label>
				<label class="text-sm">
					<span class="text-xs text-muted-foreground">Limit</span>
					<input
						class="mt-1 h-10 w-full rounded-md border bg-background px-2"
						type="number"
						min="10"
						max="2000"
						step="10"
						bind:value={limit}
					/>
				</label>
				<label class="flex items-end gap-2 pb-2 text-sm">
					<input type="checkbox" class="h-4 w-4" bind:checked={includeTooTight} />
					<span>Show too tight</span>
				</label>
			</div>
			<div
				class="mt-3 flex flex-wrap items-center justify-between gap-2 text-xs text-muted-foreground"
			>
				<div class="flex flex-wrap gap-2">
					<button
						type="button"
						class={strategy === 'balanced'
							? 'h-9 rounded-md bg-primary px-3 text-xs text-primary-foreground'
							: 'h-9 rounded-md border px-3 text-xs hover:bg-muted'}
						onclick={() => (strategy = 'balanced')}
					>
						Balanced
					</button>
					<button
						type="button"
						class={strategy === 'multi_gpu'
							? 'h-9 rounded-md bg-primary px-3 text-xs text-primary-foreground'
							: 'h-9 rounded-md border px-3 text-xs hover:bg-muted'}
						onclick={() => (strategy = 'multi_gpu')}
					>
						MultiGPU
					</button>
					<button
						type="button"
						class={strategy === 'moe_offload'
							? 'h-9 rounded-md bg-primary px-3 text-xs text-primary-foreground'
							: 'h-9 rounded-md border px-3 text-xs hover:bg-muted'}
						onclick={() => (strategy = 'moe_offload')}
					>
						MoE offload
					</button>
					<button
						type="button"
						class={strategy === 'hybrid_offload'
							? 'h-9 rounded-md bg-primary px-3 text-xs text-primary-foreground'
							: 'h-9 rounded-md border px-3 text-xs hover:bg-muted'}
						onclick={() => (strategy = 'hybrid_offload')}
					>
						Hybrid offload
					</button>
				</div>
			</div>
			<div
				class="mt-3 flex flex-wrap items-center justify-between gap-2 text-xs text-muted-foreground"
			>
				<div>
					{#if catalog}
						Catalog: {catalog.from_cache ? 'cache' : 'fresh'} · {catalog.updated_at ||
							'not updated yet'} · {catalog.cache_path}
					{/if}
				</div>
				<label class="flex items-center gap-2">
					<input type="checkbox" class="h-4 w-4" bind:checked={loadNow} />
					Load immediately after configure
				</label>
			</div>
		</section>

		{#if error}
			<div class="rounded-md border border-red-500/30 bg-red-500/10 p-3 text-sm text-red-200">
				{error}
			</div>
		{:else if message}
			<div class="rounded-md border bg-muted/40 p-3 text-sm text-muted-foreground">{message}</div>
		{/if}

		<section class="grid min-h-[560px] gap-4 xl:grid-cols-[minmax(0,1fr)_420px]">
			<div class="overflow-hidden rounded-lg border bg-card">
				<div class="flex items-center justify-between border-b px-3 py-2">
					<div class="text-sm font-medium">Recommendations</div>
					<div class="text-xs text-muted-foreground">
						{isLoading ? 'Loading...' : `${models.length} visible`}
					</div>
				</div>
				<div class="max-h-[70vh] overflow-auto">
					<table class="w-full text-left text-sm">
						<thead class="sticky top-0 bg-muted text-xs text-muted-foreground">
							<tr>
								<th class="px-3 py-2">Fit</th>
								<th class="px-3 py-2">Score</th>
								<th class="min-w-[300px] px-3 py-2">Model</th>
								<th class="px-3 py-2">Quant</th>
								<th class="px-3 py-2">Mem</th>
								<th class="px-3 py-2">Tok/s</th>
								<th class="px-3 py-2">Ctx</th>
								<th class="px-3 py-2">State</th>
							</tr>
						</thead>
						<tbody>
							{#each models as model (model.id + model.quant)}
								{@const status = statusFor(model)}
								{@const job = downloadJobFor(model)}
								<tr
									class="cursor-pointer border-b transition-colors hover:bg-muted/50 {selectedModel?.id ===
									model.id
										? 'bg-muted'
										: ''}"
									onclick={() => (selectedModel = model)}
								>
									<td class="px-3 py-2">
										<span
											class="rounded-full border px-2 py-0.5 text-xs {fitClass(model.fit_level)}"
											>{model.fit_level}</span
										>
									</td>
									<td class="px-3 py-2 font-medium">{fmtNum(model.score)}</td>
									<td class="px-3 py-2">
										<div class="font-medium" title={normalizeModelName(model.name)}>
											{compactModelName(model.name)}
										</div>
										<div class="truncate text-xs text-muted-foreground">
											{normalizeModelName(model.name)}
										</div>
										<div class="mt-1 flex flex-wrap gap-1">
											{#each uniqueModelTags([model.provider, ...(model.tags ?? [])], 6) as tag (tag)}
												<span
													class="rounded-full border px-1.5 py-0.5 text-[10px] text-muted-foreground"
													>{tag}</span
												>
											{/each}
										</div>
									</td>
									<td class="px-3 py-2">{model.quant}</td>
									<td class="px-3 py-2">{fmtGb(model.memory_required_gb)}</td>
									<td class="px-3 py-2">{fmtNum(model.estimated_tps)}</td>
									<td class="px-3 py-2">{model.effective_context_length.toLocaleString()}</td>
									<td class="px-3 py-2">
										<div class="flex min-w-32 flex-col gap-1">
											<span
												class="w-fit rounded-full border px-2 py-0.5 text-xs {statusClass(status)}"
												>{status}</span
											>
											{#if job && (status === 'queued' || status === 'resolving' || status === 'downloading')}
												<div class="h-1.5 w-28 overflow-hidden rounded-full bg-muted">
													<div
														class="h-full bg-amber-400"
														style={`width: ${Math.max(1, Math.min(100, job.percent || 0))}%`}
													></div>
												</div>
											{/if}
										</div>
									</td>
								</tr>
							{/each}
						</tbody>
					</table>
					{#if !isLoading && models.length === 0}
						<div class="p-8 text-center text-sm text-muted-foreground">
							No model matches the current filters.
						</div>
					{/if}
				</div>
			</div>

			<aside class="rounded-lg border bg-card">
				<div class="border-b px-4 py-3">
					<div class="text-sm font-medium">Plan Details</div>
					<div class="text-xs text-muted-foreground">
						Memory estimates are advisory, not a replacement for a real bench.
					</div>
				</div>
				{#if selectedModel}
					{@const selectedJob = downloadJobFor(selectedModel)}
					{@const selectedStatus = statusFor(selectedModel)}
					<div class="space-y-4 p-4">
						<div>
							<div class="text-lg font-semibold" title={normalizeModelName(selectedModel.name)}>
								{compactModelName(selectedModel.name)}
							</div>
							<div class="break-all text-xs text-muted-foreground">
								{normalizeModelName(selectedModel.name)}
							</div>
							<div class="mt-2 flex flex-wrap gap-1">
								{#each uniqueModelTags( [selectedModel.provider, ...(selectedModel.tags ?? [])] ) as tag (tag)}
									<span class="rounded-full border px-2 py-0.5 text-xs text-muted-foreground"
										>{tag}</span
									>
								{/each}
							</div>
						</div>
						<div class="grid grid-cols-2 gap-2 text-sm">
							<div class="rounded-md border p-2">
								<div class="text-xs text-muted-foreground">Fit</div>
								<div class="mt-1 font-medium">{selectedModel.fit_level}</div>
							</div>
							<div class="rounded-md border p-2">
								<div class="text-xs text-muted-foreground">Mode</div>
								<div class="mt-1 font-medium">{selectedModel.gpu_mode}</div>
							</div>
							<div class="rounded-md border p-2">
								<div class="text-xs text-muted-foreground">Score</div>
								<div class="mt-1 font-medium">{fmtNum(selectedModel.score)}</div>
							</div>
							<div class="rounded-md border p-2">
								<div class="text-xs text-muted-foreground">Context Score</div>
								<div class="mt-1 font-medium">{fmtNum(selectedModel.score_components.context)}</div>
							</div>
							<div class="rounded-md border p-2">
								<div class="text-xs text-muted-foreground">Weights</div>
								<div class="mt-1 font-medium">{fmtGb(selectedModel.weights_gb)}</div>
							</div>
							<div class="rounded-md border p-2">
								<div class="text-xs text-muted-foreground">KV q8_0</div>
								<div class="mt-1 font-medium">{fmtGb(selectedModel.kv_cache_gb)}</div>
							</div>
							<div class="rounded-md border p-2">
								<div class="text-xs text-muted-foreground">Required</div>
								<div class="mt-1 font-medium">{fmtGb(selectedModel.memory_required_gb)}</div>
							</div>
							<div class="rounded-md border p-2">
								<div class="text-xs text-muted-foreground">Fit Pool</div>
								<div class="mt-1 font-medium">{fmtGb(selectedModel.memory_available_gb)}</div>
							</div>
							{#if selectedModel.ram_available_now_gb && selectedModel.ram_capacity_gb && selectedModel.memory_available_gb === selectedModel.ram_capacity_gb}
								<div class="rounded-md border p-2">
									<div class="text-xs text-muted-foreground">Free RAM Now</div>
									<div class="mt-1 font-medium">{fmtGb(selectedModel.ram_available_now_gb)}</div>
								</div>
							{/if}
							{#if selectedModel.full_memory_required_gb && selectedModel.full_memory_required_gb > selectedModel.memory_required_gb + 0.1}
								<div class="rounded-md border p-2">
									<div class="text-xs text-muted-foreground">Full model</div>
									<div class="mt-1 font-medium">{fmtGb(selectedModel.full_memory_required_gb)}</div>
								</div>
							{/if}
							{#if selectedModel.moe_offloaded_gb && selectedModel.moe_offloaded_gb > 0}
								<div class="rounded-md border p-2">
									<div class="text-xs text-muted-foreground">MoE RAM offload</div>
									<div class="mt-1 font-medium">{fmtGb(selectedModel.moe_offloaded_gb)}</div>
								</div>
							{/if}
							<div class="rounded-md border p-2">
								<div class="text-xs text-muted-foreground">Quality / Speed</div>
								<div class="mt-1 font-medium">
									{fmtNum(selectedModel.score_components.quality)} / {fmtNum(
										selectedModel.score_components.speed
									)}
								</div>
							</div>
							<div class="rounded-md border p-2">
								<div class="text-xs text-muted-foreground">Fit Score</div>
								<div class="mt-1 font-medium">{fmtNum(selectedModel.score_components.fit)}</div>
							</div>
							<div class="rounded-md border p-2">
								<div class="text-xs text-muted-foreground">State</div>
								<div class="mt-1 font-medium">{selectedStatus}</div>
							</div>
							<div class="rounded-md border p-2">
								<div class="text-xs text-muted-foreground">Target</div>
								<div class="mt-1 truncate font-medium">
									{selectedModel.target_dir ||
										selectedModel.download?.target_dir ||
										selectedJob?.target_dir ||
										'n/a'}
								</div>
							</div>
						</div>

						{#if selectedJob}
							<div class="rounded-md border p-3">
								<div
									class="mb-2 flex items-center justify-between gap-3 text-xs text-muted-foreground"
								>
									<span>Download Progress</span>
									<span>{fmtSpeed(selectedJob.speed_bps)}</span>
								</div>
								<div class="h-2 overflow-hidden rounded-full bg-muted">
									<div
										class="h-full bg-primary transition-[width]"
										style={`width: ${Math.max(selectedJob.percent > 0 ? 1 : 0, Math.min(100, selectedJob.percent || 0))}%`}
									></div>
								</div>
								<div
									class="mt-2 flex flex-wrap justify-between gap-2 text-xs text-muted-foreground"
								>
									<span
										>{fmtBytes(selectedJob.downloaded_bytes)} / {fmtBytes(
											selectedJob.total_bytes
										)}</span
									>
									<span>{fmtNum(selectedJob.percent, 1)}%</span>
								</div>
								{#if selectedJob.local_path}
									<div class="mt-2 break-all text-xs text-muted-foreground">
										{selectedJob.local_path}
									</div>
								{/if}
								{#if selectedJob.error}
									<div
										class="mt-2 rounded border border-red-500/30 bg-red-500/10 p-2 text-xs text-red-200"
									>
										{selectedJob.error}
									</div>
								{/if}
							</div>
						{/if}

						<div>
							<div class="mb-2 text-xs font-medium uppercase text-muted-foreground">Notes</div>
							<ul class="space-y-1 text-sm text-muted-foreground">
								{#each selectedModel.notes as note, index (`${index}-${note}`)}
									<li>{note}</li>
								{/each}
							</ul>
						</div>

						<div>
							<div class="mb-2 text-xs font-medium uppercase text-muted-foreground">
								Recommended Command Args
							</div>
							<pre
								class="max-h-48 overflow-auto rounded-md bg-muted p-3 text-xs leading-relaxed whitespace-pre-wrap">{argsText(
									selectedModel
								)}</pre>
						</div>

						<div class="flex flex-wrap gap-2">
							<button
								type="button"
								class="inline-flex h-10 items-center gap-2 rounded-md border px-3 text-sm hover:bg-muted disabled:opacity-50"
								disabled={!canDownload(selectedModel) || isDownloading}
								onclick={() => downloadModel(selectedModel!)}
							>
								<Download class="h-4 w-4" />
								{downloadButtonLabel(selectedModel)}
							</button>
							<button
								type="button"
								class="inline-flex h-10 items-center gap-2 rounded-md bg-primary px-3 text-sm text-primary-foreground hover:bg-primary/90 disabled:opacity-50"
								disabled={isConfiguring || !canFit(selectedModel)}
								onclick={() => configureModel(selectedModel!)}
							>
								<Zap class="h-4 w-4" />
								FIT
							</button>
						</div>
					</div>
				{:else}
					<div class="p-8 text-sm text-muted-foreground">
						Select a recommendation to inspect the launch plan.
					</div>
				{/if}
			</aside>
		</section>
	</div>
</main>
