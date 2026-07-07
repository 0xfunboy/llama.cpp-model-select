<script lang="ts">
	import { onMount } from 'svelte';
	import { Download, Gauge, RefreshCw, SlidersHorizontal, Zap } from '@lucide/svelte';
	import {
		FitAdvisorService,
		type FitAdvisorModel,
		type FitAdvisorModelsResponse
	} from '$lib/services/fit-advisor.service';
	import { modelsStore } from '$lib/stores/models.svelte';

	let response = $state<FitAdvisorModelsResponse | null>(null);
	let selectedModel = $state<FitAdvisorModel | null>(null);
	let isLoading = $state(false);
	let isRefreshing = $state(false);
	let isDownloading = $state(false);
	let isConfiguring = $state(false);
	let error = $state('');
	let message = $state('');
	let useCase = $state('coding');
	let minFit = $state('marginal');
	let quant = $state('');
	let search = $state('');
	let context = $state(8192);
	let limit = $state(300);
	let includeTooTight = $state(false);
	let loadNow = $state(false);

	const models = $derived(response?.models ?? []);
	const system = $derived(response?.system ?? null);
	const catalog = $derived(response?.catalog ?? null);

	onMount(() => {
		void loadModels(true);
	});

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
				context,
				limit,
				include_too_tight: includeTooTight
			});
			response = next;
			if (selectedModel) {
				selectedModel = next.models.find((model) => model.id === selectedModel?.id) ?? next.models[0] ?? null;
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
		message = `Starting download for ${model.download.hf_ref}...`;
		try {
			const result = await FitAdvisorService.download(model);
			message = result.already_present
				? `${result.model} is already present.`
				: `Download started for ${result.model}. Progress is visible in the model selector/SSE feed.`;
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
			const result = await FitAdvisorService.configure(model, loadNow);
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

	function fitClass(level: string): string {
		if (level === 'perfect') return 'bg-emerald-500/15 text-emerald-300 border-emerald-500/30';
		if (level === 'good') return 'bg-cyan-500/15 text-cyan-300 border-cyan-500/30';
		if (level === 'marginal') return 'bg-amber-500/15 text-amber-300 border-amber-500/30';
		return 'bg-red-500/15 text-red-300 border-red-500/30';
	}

	function argsText(model: FitAdvisorModel): string {
		return model.recommended_args.join(' ');
	}
</script>

<svelte:head>
	<title>Fit Advisor</title>
</svelte:head>

<main class="min-h-screen bg-background text-foreground">
	<div class="mx-auto flex max-w-[1500px] flex-col gap-5 px-4 py-5 lg:px-6">
		<header class="flex flex-col gap-3 border-b pb-4 md:flex-row md:items-end md:justify-between">
			<div>
				<div class="flex items-center gap-2 text-sm text-muted-foreground">
					<Gauge class="h-4 w-4" />
					llmfit logic, native llama.cpp router
				</div>
				<h1 class="mt-1 text-2xl font-semibold tracking-normal">Fit Advisor</h1>
				<p class="mt-1 max-w-3xl text-sm text-muted-foreground">
					Rank GGUF models against this machine, estimate memory and throughput, then download or write a router preset.
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
					<div class="mt-1 text-sm font-medium">{fmtGb(system.available_ram_gb)} available</div>
					<div class="mt-1 text-xs text-muted-foreground">{fmtGb(system.total_ram_gb)} total</div>
				</div>
				<div class="rounded-lg border bg-card p-3">
					<div class="text-xs text-muted-foreground">GPU</div>
					<div class="mt-1 truncate text-sm font-medium">{system.gpu_name || 'No GPU detected'}</div>
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
					<select class="mt-1 h-10 w-full rounded-md border bg-background px-2" bind:value={useCase}>
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
					<input class="mt-1 h-10 w-full rounded-md border bg-background px-2" placeholder="Q4, Q8, IQ..." bind:value={quant} />
				</label>
				<label class="text-sm">
					<span class="text-xs text-muted-foreground">Search</span>
					<input class="mt-1 h-10 w-full rounded-md border bg-background px-2" placeholder="qwen, coder..." bind:value={search} />
				</label>
				<label class="text-sm">
					<span class="text-xs text-muted-foreground">Context</span>
					<input class="mt-1 h-10 w-full rounded-md border bg-background px-2" type="number" min="512" step="512" bind:value={context} />
				</label>
				<label class="text-sm">
					<span class="text-xs text-muted-foreground">Limit</span>
					<input class="mt-1 h-10 w-full rounded-md border bg-background px-2" type="number" min="10" max="2000" step="10" bind:value={limit} />
				</label>
				<label class="flex items-end gap-2 pb-2 text-sm">
					<input type="checkbox" class="h-4 w-4" bind:checked={includeTooTight} />
					<span>Show too tight</span>
				</label>
			</div>
			<div class="mt-3 flex flex-wrap items-center justify-between gap-2 text-xs text-muted-foreground">
				<div>
					{#if catalog}
						Catalog: {catalog.from_cache ? 'cache' : 'fresh'} · {catalog.updated_at || 'not updated yet'} · {catalog.cache_path}
					{/if}
				</div>
				<label class="flex items-center gap-2">
					<input type="checkbox" class="h-4 w-4" bind:checked={loadNow} />
					Load immediately after configure
				</label>
			</div>
		</section>

		{#if error}
			<div class="rounded-md border border-red-500/30 bg-red-500/10 p-3 text-sm text-red-200">{error}</div>
		{:else if message}
			<div class="rounded-md border bg-muted/40 p-3 text-sm text-muted-foreground">{message}</div>
		{/if}

		<section class="grid min-h-[560px] gap-4 xl:grid-cols-[minmax(0,1fr)_420px]">
			<div class="overflow-hidden rounded-lg border bg-card">
				<div class="flex items-center justify-between border-b px-3 py-2">
					<div class="text-sm font-medium">Recommendations</div>
					<div class="text-xs text-muted-foreground">{isLoading ? 'Loading...' : `${models.length} visible`}</div>
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
								<th class="px-3 py-2">Installed</th>
							</tr>
						</thead>
						<tbody>
							{#each models as model (model.id + model.quant)}
								<tr
									class="cursor-pointer border-b transition-colors hover:bg-muted/50 {selectedModel?.id === model.id ? 'bg-muted' : ''}"
									onclick={() => (selectedModel = model)}
								>
									<td class="px-3 py-2">
										<span class="rounded-full border px-2 py-0.5 text-xs {fitClass(model.fit_level)}">{model.fit_level}</span>
									</td>
									<td class="px-3 py-2 font-medium">{fmtNum(model.score)}</td>
									<td class="px-3 py-2">
										<div class="font-medium">{model.name}</div>
										<div class="truncate text-xs text-muted-foreground">{model.id}</div>
									</td>
									<td class="px-3 py-2">{model.quant}</td>
									<td class="px-3 py-2">{fmtGb(model.memory_required_gb)}</td>
									<td class="px-3 py-2">{fmtNum(model.estimated_tps)}</td>
									<td class="px-3 py-2">{model.effective_context_length.toLocaleString()}</td>
									<td class="px-3 py-2">{model.installed ? 'yes' : 'no'}</td>
								</tr>
							{/each}
						</tbody>
					</table>
					{#if !isLoading && models.length === 0}
						<div class="p-8 text-center text-sm text-muted-foreground">No model matches the current filters.</div>
					{/if}
				</div>
			</div>

			<aside class="rounded-lg border bg-card">
				<div class="border-b px-4 py-3">
					<div class="text-sm font-medium">Plan Details</div>
					<div class="text-xs text-muted-foreground">Memory estimates are advisory, not a replacement for a real bench.</div>
				</div>
				{#if selectedModel}
					<div class="space-y-4 p-4">
						<div>
							<div class="text-lg font-semibold">{selectedModel.name}</div>
							<div class="break-all text-xs text-muted-foreground">{selectedModel.id}</div>
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
								<div class="text-xs text-muted-foreground">Available Pool</div>
								<div class="mt-1 font-medium">{fmtGb(selectedModel.memory_available_gb)}</div>
							</div>
						</div>

						<div>
							<div class="mb-2 text-xs font-medium uppercase text-muted-foreground">Notes</div>
							<ul class="space-y-1 text-sm text-muted-foreground">
								{#each selectedModel.notes as note}
									<li>{note}</li>
								{/each}
							</ul>
						</div>

						<div>
							<div class="mb-2 text-xs font-medium uppercase text-muted-foreground">Recommended Command Args</div>
							<pre class="max-h-48 overflow-auto rounded-md bg-muted p-3 text-xs leading-relaxed whitespace-pre-wrap">{argsText(selectedModel)}</pre>
						</div>

						<div class="flex flex-wrap gap-2">
							<button
								type="button"
								class="inline-flex h-10 items-center gap-2 rounded-md border px-3 text-sm hover:bg-muted disabled:opacity-50"
								disabled={!selectedModel.download || selectedModel.installed || isDownloading}
								onclick={() => downloadModel(selectedModel!)}
							>
								<Download class="h-4 w-4" />
								Download
							</button>
							<button
								type="button"
								class="inline-flex h-10 items-center gap-2 rounded-md bg-primary px-3 text-sm text-primary-foreground hover:bg-primary/90 disabled:opacity-50"
								disabled={isConfiguring || (!selectedModel.local_path && !selectedModel.download)}
								onclick={() => configureModel(selectedModel!)}
							>
								<Zap class="h-4 w-4" />
								Fit / Configure
							</button>
						</div>
					</div>
				{:else}
					<div class="p-8 text-sm text-muted-foreground">Select a recommendation to inspect the launch plan.</div>
				{/if}
			</aside>
		</section>
	</div>
</main>
