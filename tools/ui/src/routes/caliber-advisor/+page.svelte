<script lang="ts">
	import { onDestroy, onMount } from 'svelte';
	import {
		Activity,
		BarChart3,
		CheckCircle2,
		ChevronRight,
		Cpu,
		Download,
		FileJson,
		Gauge,
		Info,
		Play,
		RefreshCw,
		Search,
		Settings2,
		Trash2,
		Wrench
	} from '@lucide/svelte';
	import {
		CaliberAdvisorService,
		type CaliberModel,
		type CaliberPlanItem,
		type CaliberReportSummary,
		type CaliberSweepStatus
	} from '$lib/services/caliber-advisor.service';
	import {
		FitAdvisorService,
		type FitAdvisorDownloadJob,
		type FitAdvisorModel,
		type FitAdvisorSystem
	} from '$lib/services/fit-advisor.service';

	type TabId = 'start' | 'candidates' | 'compare' | 'reports' | 'diagnostics';
	type ProfileId = 'overall' | 'speed' | 'efficiency' | 'safety';
	type RunScope = 'quick' | 'standard' | 'deep';
	type CaliberRow = Record<string, unknown>;

	const contextOptions = [
		{ label: '8k', value: 8192, hint: 'Short chat and smoke tests' },
		{ label: '32k', value: 32768, hint: 'Most coding and analysis sessions' },
		{ label: '64k', value: 65536, hint: 'Large files and longer conversations' },
		{ label: '131k', value: 131072, hint: 'Long-context models and serious repo work' },
		{ label: '262k', value: 262144, hint: 'Only when the model really supports it' }
	];
	const tabs: { id: TabId; label: string }[] = [
		{ id: 'start', label: 'Start here' },
		{ id: 'candidates', label: 'Choose models' },
		{ id: 'compare', label: 'Compare winners' },
		{ id: 'reports', label: 'Reports' },
		{ id: 'diagnostics', label: 'Diagnostics' }
	];
	const profileLabels: Record<ProfileId, { title: string; help: string }> = {
		overall: {
			title: 'Best daily driver',
			help: 'Balances speed, fit, context and KV quality. This is the default answer most users want.'
		},
		speed: {
			title: 'Fastest response',
			help: 'Ranks raw token speed first. Useful for autocomplete, simple chat and batch work.'
		},
		efficiency: {
			title: 'Best speed per watt/GB',
			help: 'Prefers strong throughput without wasting memory or power.'
		},
		safety: {
			title: 'Safest fit',
			help: 'Prefers configs that avoid memory pressure and risky spill.'
		}
	};
	const scopeOptions: Record<RunScope, { title: string; help: string; workload: 'baseline' | 'all' }> = {
		quick: {
			title: 'Quick comparison',
			help: 'Benchmarks normal generation only. Use this first to get a useful answer quickly.',
			workload: 'baseline'
		},
		standard: {
			title: 'Decision run',
			help: 'Adds long-prompt and KV-fill checks so the report explains context behavior.',
			workload: 'all'
		},
		deep: {
			title: 'Deep calibration',
			help: 'Keeps the full sweep visible for diagnosis. Slower, but closer to the original Calibr flow.',
			workload: 'all'
		}
	};
	const workflow = [
		['Setup', 'Check hardware, llama.cpp, model folders and report storage.'],
		['Acquire', 'Use installed GGUF files or download catalog candidates one at a time.'],
		['Plan', 'Expand safe configs: vanilla control, context, KV cache, GPU split and MoE/offload candidates.'],
		['Benchmark', 'Run measured configs server-side, even if this browser tab is closed.'],
		['Decide', 'Rank winners, explain tradeoffs, save launch/FIT settings.']
	];
	const strategies = [
		{ id: 'hybrid_offload', label: 'Hybrid' },
		{ id: 'multi_gpu', label: 'MultiGPU' },
		{ id: 'moe_offload', label: 'MoE offload' },
		{ id: 'balanced', label: 'Balanced' }
	];

	let activeTab = $state<TabId>('start');
	let profile = $state<ProfileId>('overall');
	let runScope = $state<RunScope>('quick');
	let contextSize = $state(131072);
	let models = $state<CaliberModel[]>([]);
	let selectedLocalIds = $state<string[]>([]);
	let plan = $state<CaliberPlanItem[]>([]);
	let reports = $state<CaliberReportSummary[]>([]);
	let selectedReport = $state<Record<string, unknown> | null>(null);
	let selectedReportId = $state('');
	let results = $state<Record<string, unknown> | null>(null);
	let status = $state<CaliberSweepStatus | null>(null);
	let eventLog = $state<string[]>([]);
	let loading = $state(false);
	let running = $state(false);
	let error = $state('');
	let message = $state('');
	let loadAfterConfigure = $state(false);

	let fitSystem = $state<FitAdvisorSystem | null>(null);
	let catalogModels = $state<FitAdvisorModel[]>([]);
	let downloads = $state<FitAdvisorDownloadJob[]>([]);
	let catalogLoading = $state(false);
	let catalogSearch = $state('');
	let catalogStrategy = $state('hybrid_offload');
	let catalogLimit = $state(120);
	let catalogMinFit = $state('marginal');
	let catalogMessage = $state('');
	let downloadLastSeq = $state(0);
	let downloadAbort: AbortController | null = null;
	let sweepAbort: AbortController | null = null;

	const resultRows = $derived(asRows(results?.rows).filter((row) => rowNum(row, ['eval_tps', 'tps']) > 0));
	const reportRows = $derived(asRows(selectedReport?.rows));
	const reportPlan = $derived(asRows(selectedReport?.plan));
	const chartRows = $derived((reportRows.length > 0 ? reportRows : resultRows).filter((row) => rowNum(row, ['eval_tps', 'tps']) > 0));
	const chartMaxTps = $derived(Math.max(1, ...chartRows.map((row) => rowNum(row, ['eval_tps', 'tps']))));
	const chartMaxVram = $derived(Math.max(1, ...chartRows.map((row) => rowNum(row, ['vram_peak_mib', 'memory_required_mib']))));
	const winners = $derived((results?.winners as Record<ProfileId, Record<string, CaliberRow>> | undefined) ?? ({} as Record<ProfileId, Record<string, CaliberRow>>));
	const profileWinners = $derived(Object.entries(winners[profile] ?? {}).filter(([, row]) => rowNum(row, ['eval_tps', 'tps']) > 0));
	const bestWinner = $derived(profileWinners[0]?.[1] ?? null);
	const completedReports = $derived(reports.filter((report) => report.rows > 0 || isCompleteStatus(report.status)));
	const pendingReports = $derived(reports.filter((report) => canDeleteReport(report)));
	const selectedModels = $derived(models.filter((model) => selectedLocalIds.includes(model.id)));
	const selectableModels = $derived(models.filter((model) => Boolean(model.path)));
	const planModels = $derived(uniqueStrings(plan.map((item) => item.model)).length);
	const targetContext = $derived(contextOptions.find((item) => item.value === contextSize));
	const readyToRun = $derived(selectedLocalIds.length > 0 && !running);
	const nextAction = $derived(nextActionText());

	onMount(() => {
		void refreshAll();
		void loadCatalog();
		void refreshDownloads();
		startDownloadStream();
	});

	onDestroy(() => {
		downloadAbort?.abort();
		sweepAbort?.abort();
	});

	function asRows(value: unknown): CaliberRow[] {
		return Array.isArray(value) ? (value.filter((row) => row && typeof row === 'object') as CaliberRow[]) : [];
	}

	function uniqueStrings(values: string[]): string[] {
		return [...new Set(values.filter(Boolean))];
	}

	function rowNum(row: CaliberRow, keys: string[], fallback = 0): number {
		for (const key of keys) {
			const value = row[key];
			const number = typeof value === 'number' ? value : Number(value);
			if (Number.isFinite(number)) return number;
		}
		return fallback;
	}

	function rowText(row: CaliberRow, keys: string[], fallback = ''): string {
		for (const key of keys) {
			const value = row[key];
			if (value !== undefined && value !== null && String(value) !== '') return String(value);
		}
		return fallback;
	}

	function fmtNumber(value: number, digits = 1): string {
		if (!Number.isFinite(value)) return '-';
		return value.toFixed(digits);
	}

	function fmtMib(value: number): string {
		if (!Number.isFinite(value) || value <= 0) return '-';
		return value >= 1024 ? `${fmtNumber(value / 1024, 1)} GB` : `${fmtNumber(value, 0)} MiB`;
	}

	function fmtGb(value: number): string {
		if (!Number.isFinite(value) || value <= 0) return '-';
		return `${fmtNumber(value, 1)} GB`;
	}

	function isCompleteStatus(status: string): boolean {
		return ['complete', 'completed', 'done', 'measured'].includes((status || '').toLowerCase());
	}

	function canDeleteReport(report: CaliberReportSummary): boolean {
		return report.rows <= 0 || !isCompleteStatus(report.status);
	}

	function reportStatusLabel(report: CaliberReportSummary): string {
		if (report.rows > 0) return 'complete';
		return report.status || 'pending';
	}

	function modelParamLabel(model: CaliberModel): string {
		const params = rowNum((model.plan_meta ?? {}) as CaliberRow, ['size_mib']);
		const ctx = rowNum((model.plan_meta ?? {}) as CaliberRow, ['gguf_context_length']);
		const meta = [];
		if (params > 0) meta.push(fmtMib(params));
		if (ctx > 0) meta.push(`${Math.round(ctx / 1024)}k ctx`);
		if (model.status) meta.push(model.status);
		return meta.join(' / ');
	}

	function toggleLocalModel(id: string) {
		selectedLocalIds = selectedLocalIds.includes(id)
			? selectedLocalIds.filter((item) => item !== id)
			: [...selectedLocalIds, id];
		plan = [];
	}

	function selectRecommendedLocal() {
		selectedLocalIds = selectableModels.slice(0, 4).map((model) => model.id);
		plan = [];
	}

	function clearSelection() {
		selectedLocalIds = [];
		plan = [];
	}

	function nextActionText(): string {
		if (running) return 'Benchmark is running on the server. You can close this page and come back to Reports.';
		if (selectedLocalIds.length === 0) return 'Choose at least one installed model, or download/configure a catalog model first.';
		if (plan.length === 0) return 'Review the benchmark plan so you know how many configs will run.';
		if (completedReports.length === 0) return 'Start the benchmark. The report will appear automatically when it finishes.';
		return 'Open Compare winners to choose the model/config to FIT.';
	}

	function payload(): Record<string, unknown> {
		return {
			models: selectedLocalIds,
			opts: { workloadSweep: scopeOptions[runScope].workload },
			cfg: {
				context_candidates: [{ ctx: contextSize, kv: 'q8_0' }],
				max_context_cap: contextSize
			}
		};
	}

	function pushEvent(line: string) {
		eventLog = [`${new Date().toLocaleTimeString()} ${line}`, ...eventLog].slice(0, 80);
	}

	async function refreshAll() {
		loading = true;
		error = '';
		try {
			const [modelsResult, reportsResult, resultsResult] = await Promise.all([
				CaliberAdvisorService.models(),
				CaliberAdvisorService.reports(),
				CaliberAdvisorService.results()
			]);
			models = modelsResult.data;
			reports = reportsResult.data.sort((a, b) => b.created_at.localeCompare(a.created_at));
			results = resultsResult;
			if (selectedLocalIds.length === 0) selectedLocalIds = models.slice(0, 3).map((model) => model.id);
			message = `${completedReports.length} completed benchmark reports, ${resultRows.length} measured rows`;
		} catch (e) {
			error = e instanceof Error ? e.message : String(e);
		} finally {
			loading = false;
		}
	}

	async function previewPlan() {
		if (selectedLocalIds.length === 0) return;
		loading = true;
		error = '';
		try {
			const result = await CaliberAdvisorService.plan(payload());
			plan = result.plan;
			message = `${result.plan_count} configs planned across ${selectedLocalIds.length} model(s)`;
			activeTab = 'start';
		} catch (e) {
			error = e instanceof Error ? e.message : String(e);
		} finally {
			loading = false;
		}
	}

	async function startSweep() {
		if (!readyToRun) return;
		running = true;
		error = '';
		eventLog = [];
		sweepAbort?.abort();
		sweepAbort = new AbortController();
		try {
			const started = await CaliberAdvisorService.sweep(payload());
			status = { job_id: started.job_id, status: started.status };
			pushEvent(`Queued campaign ${started.job_id}`);
			void CaliberAdvisorService.streamSweepEvents(
				started.job_id,
				(event) => {
					status = event.data;
					pushEvent(humanEvent(event.event, event.data));
					if (event.event === 'done' || event.event === 'error') sweepAbort?.abort();
				},
				sweepAbort.signal
			).catch((e) => {
				if (!(e instanceof DOMException && e.name === 'AbortError')) pushEvent(e instanceof Error ? e.message : String(e));
			});
			for (let i = 0; i < 240; i += 1) {
				await new Promise((resolve) => setTimeout(resolve, 1000));
				status = await CaliberAdvisorService.sweepStatus(started.job_id);
				if (status.finished) break;
			}
			await refreshAll();
			if (status?.report_id) {
				const summary = reports.find((report) => report.id === status?.report_id);
				if (summary) await openReport(summary, false);
			}
			activeTab = 'compare';
		} catch (e) {
			error = e instanceof Error ? e.message : String(e);
		} finally {
			running = false;
		}
	}

	function humanEvent(event: string, data: CaliberSweepStatus & Record<string, unknown>): string {
		if (event === 'bench') return `Testing ${String(data.item ?? 'configuration')}`;
		if (event === 'row') return data.ok ? `Measured ${fmtNumber(Number(data.eval_tps ?? 0), 1)} tok/s` : `Failed: ${String(data.error ?? 'configuration failed')}`;
		if (event === 'report') return `Saved report ${String(data.report_id ?? '')}`;
		if (event === 'done') return 'Campaign finished';
		if (event === 'error') return `Error: ${String(data.error ?? 'unknown')}`;
		return event;
	}

	async function loadCatalog(refresh = false) {
		catalogLoading = true;
		error = '';
		try {
			if (refresh) await FitAdvisorService.refreshCatalog();
			const result = await FitAdvisorService.models({
				use_case: 'coding',
				min_fit: catalogMinFit,
				quant: 'Q4, Q5, Q6, Q8, IQ',
				search: catalogSearch,
				strategy: catalogStrategy,
				context: contextSize,
				limit: catalogLimit,
				include_too_tight: true
			});
			fitSystem = result.system;
			catalogModels = result.models;
			catalogMessage = `${result.returned_models} downloadable candidates ranked for this machine`;
		} catch (e) {
			error = e instanceof Error ? e.message : String(e);
		} finally {
			catalogLoading = false;
		}
	}

	async function refreshDownloads() {
		try {
			const result = await FitAdvisorService.listDownloads();
			downloads = result.data;
			downloadLastSeq = Math.max(downloadLastSeq, ...downloads.map((job) => job.seq ?? 0));
		} catch {
			/* secondary */
		}
	}

	function startDownloadStream() {
		downloadAbort?.abort();
		downloadAbort = new AbortController();
		void FitAdvisorService.streamDownloads(
			(event) => {
				upsertDownload(event.data);
				downloadLastSeq = Math.max(downloadLastSeq, event.data.seq ?? downloadLastSeq);
			},
			downloadAbort.signal,
			downloadLastSeq
		).catch(() => {});
	}

	function upsertDownload(job: FitAdvisorDownloadJob) {
		const index = downloads.findIndex((item) => item.id === job.id);
		downloads = index === -1 ? [job, ...downloads] : downloads.map((item, i) => (i === index ? job : item));
	}

	function downloadFor(model: FitAdvisorModel): FitAdvisorDownloadJob | null {
		return (
			downloads.find(
				(job) =>
					job.model_id === model.id ||
					job.local_path === model.local_path ||
					(model.download?.hf_ref && job.hf_ref === model.download.hf_ref)
			) ?? model.download_progress ?? null
		);
	}

	function downloadStatus(model: FitAdvisorModel): string {
		return downloadFor(model)?.status ?? model.download_status ?? (model.installed ? 'configured' : 'available');
	}

	function isDownloading(model: FitAdvisorModel): boolean {
		return ['queued', 'resolving', 'downloading'].includes(downloadStatus(model));
	}

	function canConfigureFit(model: FitAdvisorModel): boolean {
		return Boolean(model.installed || model.downloaded || model.configured || downloadStatus(model) === 'downloaded');
	}

	async function downloadModel(model: FitAdvisorModel) {
		error = '';
		try {
			const result = await FitAdvisorService.download(model);
			if (result.job) upsertDownload(result.job);
			catalogMessage = result.already_present ? 'Model already present' : `Download queued: ${model.name}`;
			await refreshDownloads();
		} catch (e) {
			error = e instanceof Error ? e.message : String(e);
		}
	}

	async function configureFitModel(model: FitAdvisorModel) {
		error = '';
		try {
			const job = downloadFor(model);
			const enriched = { ...model, local_path: model.local_path ?? job?.local_path ?? null };
			const result = await FitAdvisorService.configure(enriched, loadAfterConfigure);
			catalogMessage = `${result.model} added to local models${result.loaded ? ' and loaded' : ''}`;
			await loadCatalog();
			await refreshAll();
			if (!selectedLocalIds.includes(result.model)) selectedLocalIds = [...selectedLocalIds, result.model];
			activeTab = 'start';
		} catch (e) {
			error = e instanceof Error ? e.message : String(e);
		}
	}

	async function openReport(report: CaliberReportSummary, switchTab = true) {
		error = '';
		try {
			selectedReport = await CaliberAdvisorService.report(report.id);
			selectedReportId = report.id;
			if (switchTab) activeTab = 'reports';
		} catch (e) {
			error = e instanceof Error ? e.message : String(e);
		}
	}

	async function deletePendingReport(report: CaliberReportSummary) {
		if (!canDeleteReport(report)) return;
		error = '';
		try {
			await CaliberAdvisorService.deleteReport(report.id);
			if (selectedReportId === report.id) {
				selectedReport = null;
				selectedReportId = '';
			}
			await refreshAll();
		} catch (e) {
			error = e instanceof Error ? e.message : String(e);
		}
	}

	async function configureCaliberRow(row: CaliberRow) {
		const model = rowText(row, ['model', 'model_id']);
		if (!model) return;
		error = '';
		try {
			const result = await CaliberAdvisorService.configure({
				model,
				extra_args: rowText(row, ['extra_args']),
				load_now: loadAfterConfigure,
				tags: ['caliber-winner', profile]
			});
			message = `${result.model} configured${result.loaded ? ' and loaded' : ''}`;
		} catch (e) {
			error = e instanceof Error ? e.message : String(e);
		}
	}

	function scatterX(row: CaliberRow): number {
		return 44 + (rowNum(row, ['eval_tps', 'tps']) / chartMaxTps) * 548;
	}

	function scatterY(row: CaliberRow): number {
		return 184 - (rowNum(row, ['vram_peak_mib', 'memory_required_mib']) / chartMaxVram) * 146;
	}

	function winnerScore(row: CaliberRow): number {
		return rowNum(row, ['winner_score', 'score', 'eval_tps']);
	}

	function barWidth(row: CaliberRow): number {
		const maxScore = Math.max(1, ...profileWinners.map(([, winner]) => winnerScore(winner)));
		return Math.max(4, Math.min(100, (winnerScore(row) / maxScore) * 100));
	}

	function selectedReportSummary(): CaliberReportSummary | null {
		return reports.find((report) => report.id === selectedReportId) ?? null;
	}

	function planLauncher(row: CaliberRow | null): string {
		if (!row) return '';
		const model = rowText(row, ['path', 'model_path', 'model']);
		const args = rowText(row, ['extra_args']);
		return `#!/usr/bin/env bash\nexec ./build/bin/llama-server -m "${model}" ${args}`;
	}
</script>

<svelte:head>
	<title>Caliber Advisor</title>
</svelte:head>

<main class="caliber-page">
	<header class="hero">
		<div>
			<p class="eyebrow">Caliber Advisor</p>
			<h1>Find the best local model for this machine.</h1>
			<p>
				Pick the kind of answer you need, choose candidate models, run the benchmark, then FIT the
				winning configuration into the router.
			</p>
		</div>
		<div class="hero-actions">
			<button type="button" onclick={() => (activeTab = 'start')} class="primary">
				Start guided run
				<ChevronRight size={16} />
			</button>
			<button type="button" onclick={() => (activeTab = 'compare')}>
				<BarChart3 size={16} />
				View winners
			</button>
		</div>
	</header>

	{#if error}
		<div class="error">{error}</div>
	{/if}

	<section class="answer-strip">
		<div>
			<span>Best answer</span>
			<strong>{bestWinner ? rowText(bestWinner, ['model'], '-') : 'No winner yet'}</strong>
			<p>{bestWinner ? `${fmtNumber(rowNum(bestWinner, ['eval_tps', 'tps']), 1)} tok/s at ${rowNum(bestWinner, ['ctx_size'], contextSize)} ctx` : 'Run a campaign to populate this.'}</p>
		</div>
		<div>
			<span>Hardware</span>
			<strong>{fitSystem?.gpu_name ?? 'Detecting GPU'}</strong>
			<p>{fitSystem ? `${fitSystem.gpu_count} GPU(s), ${fmtGb(fitSystem.total_gpu_vram_gb)} aggregate VRAM` : 'Fit Advisor system scan pending.'}</p>
		</div>
		<div>
			<span>Next step</span>
			<strong>{running ? 'Running' : selectedLocalIds.length ? 'Review and run' : 'Choose models'}</strong>
			<p>{nextAction}</p>
		</div>
	</section>

	<nav class="tabs" aria-label="Caliber Advisor sections">
		{#each tabs as tab}
			<button type="button" class:active={activeTab === tab.id} onclick={() => (activeTab = tab.id)}>
				{#if tab.id === 'start'}<Gauge size={16} />{/if}
				{#if tab.id === 'candidates'}<Download size={16} />{/if}
				{#if tab.id === 'compare'}<BarChart3 size={16} />{/if}
				{#if tab.id === 'reports'}<FileJson size={16} />{/if}
				{#if tab.id === 'diagnostics'}<Wrench size={16} />{/if}
				{tab.label}
			</button>
		{/each}
	</nav>

	{#if activeTab === 'start'}
		<section class="wizard-grid">
			<div class="panel">
				<div class="panel-head">
					<div>
						<h2>1. Choose the question</h2>
						<p>Caliber will rank models differently depending on what you care about.</p>
					</div>
				</div>
				<div class="choice-grid">
					{#each Object.entries(profileLabels) as [id, item]}
						<button type="button" class="choice" class:active={profile === id} onclick={() => (profile = id as ProfileId)}>
							<strong>{item.title}</strong>
							<span>{item.help}</span>
						</button>
					{/each}
				</div>
			</div>

			<div class="panel">
				<div class="panel-head">
					<div>
						<h2>2. Pick benchmark scope</h2>
						<p>Start small; use deeper sweeps only when the winner needs diagnosis.</p>
					</div>
				</div>
				<div class="choice-grid">
					{#each Object.entries(scopeOptions) as [id, item]}
						<button type="button" class="choice" class:active={runScope === id} onclick={() => (runScope = id as RunScope)}>
							<strong>{item.title}</strong>
							<span>{item.help}</span>
						</button>
					{/each}
				</div>
			</div>
		</section>

		<section class="wizard-grid">
			<div class="panel">
				<div class="panel-head">
					<div>
						<h2>3. Select candidate models</h2>
						<p>{selectedLocalIds.length} selected. These are the models Caliber will compare.</p>
					</div>
					<div class="row-actions">
						<button type="button" onclick={selectRecommendedLocal}>Pick first 4</button>
						<button type="button" onclick={clearSelection}>Clear</button>
					</div>
				</div>
				<div class="model-list">
					{#each selectableModels as model}
						<button type="button" class="model-option" class:active={selectedLocalIds.includes(model.id)} onclick={() => toggleLocalModel(model.id)}>
							<span class="checkbox">{selectedLocalIds.includes(model.id) ? '✓' : ''}</span>
							<div>
								<strong>{model.name || model.id}</strong>
								<span>{modelParamLabel(model)}</span>
							</div>
						</button>
					{/each}
				</div>
			</div>

			<div class="panel">
				<div class="panel-head">
					<div>
						<h2>4. Review and run</h2>
						<p>No command-line knowledge required. The report stores the technical details.</p>
					</div>
				</div>
				<div class="run-card">
					<dl>
						<div><dt>Selected models</dt><dd>{selectedLocalIds.length}</dd></div>
						<div><dt>Target context</dt><dd>{targetContext?.label ?? contextSize}</dd></div>
						<div><dt>Benchmark scope</dt><dd>{scopeOptions[runScope].title}</dd></div>
						<div><dt>Planned configs</dt><dd>{plan.length || '-'}</dd></div>
					</dl>
					<label>
						<span>Context target</span>
						<select bind:value={contextSize}>
							{#each contextOptions as option}
								<option value={option.value}>{option.label} - {option.hint}</option>
							{/each}
						</select>
					</label>
					<label class="check">
						<input type="checkbox" bind:checked={loadAfterConfigure} />
						<span>Load winner immediately after FIT</span>
					</label>
					<div class="button-row">
						<button type="button" onclick={previewPlan} disabled={selectedLocalIds.length === 0 || loading}>
							<Settings2 size={16} />
							Review plan
						</button>
						<button type="button" class="primary" onclick={startSweep} disabled={!readyToRun}>
							<Play size={16} />
							Start benchmark
						</button>
					</div>
					<p class="note">{nextAction}</p>
				</div>
			</div>
		</section>

		<section class="panel">
			<div class="panel-head">
				<div>
					<h2>What will happen</h2>
					<p>The old CLI stages are still there, but the UI handles them as one campaign.</p>
				</div>
			</div>
			<div class="workflow">
				{#each workflow as item, index}
					<div class="workflow-step">
						<span>{index + 1}</span>
						<strong>{item[0]}</strong>
						<p>{item[1]}</p>
					</div>
				{/each}
			</div>
		</section>

		{#if plan.length > 0 || running || status}
			<section class="grid">
				<div class="panel">
					<div class="panel-head">
						<div>
							<h2>Benchmark plan</h2>
							<p>{plan.length} configs across {planModels || selectedLocalIds.length} model(s)</p>
						</div>
					</div>
					<div class="table plan-table">
						<div class="table-head">
							<span>Model</span>
							<span>Purpose</span>
							<span>Workload</span>
							<span>Human summary</span>
						</div>
						{#each plan.slice(0, 120) as row}
							<div class="table-row">
								<strong>{row.model}</strong>
								<span>{row.row_role === 'candidate' ? 'Can win' : 'Diagnostic control'}</span>
								<span>{row.workload_kind}</span>
								<span>{row.label}</span>
							</div>
						{/each}
					</div>
				</div>

				<div class="panel">
					<div class="panel-head">
						<div>
							<h2>Live campaign</h2>
							<p>{status?.status ?? 'idle'}</p>
						</div>
						<Activity size={18} />
					</div>
					<div class="job">
						<div class="progress">
							<span style={`width:${status?.total ? Math.min(100, ((status.current ?? 0) / status.total) * 100) : running ? 12 : 0}%`}></span>
						</div>
						<dl>
							<div><dt>Done</dt><dd>{status?.current ?? 0}</dd></div>
							<div><dt>Total</dt><dd>{status?.total ?? 0}</dd></div>
							<div><dt>Report</dt><dd>{status?.report_id ?? '-'}</dd></div>
						</dl>
						<div class="event-log">
							{#each eventLog as line}
								<code>{line}</code>
							{/each}
						</div>
					</div>
				</div>
			</section>
		{/if}
	{/if}

	{#if activeTab === 'candidates'}
		<section class="panel explain">
			<Info size={18} />
			<div>
				<h2>Need more models?</h2>
				<p>
					Download candidates here, then press FIT to add them to local models. They will appear in
					Start here and can be selected for the benchmark campaign.
				</p>
			</div>
		</section>
		<section class="controls">
			<label>
				<span>Search</span>
				<div class="input-icon">
					<Search size={15} />
					<input bind:value={catalogSearch} placeholder="qwen, coder, moe..." />
				</div>
			</label>
			<label>
				<span>Fit strategy</span>
				<select bind:value={catalogStrategy}>
					{#each strategies as strategy}
						<option value={strategy.id}>{strategy.label}</option>
					{/each}
				</select>
			</label>
			<label>
				<span>Minimum fit</span>
				<select bind:value={catalogMinFit}>
					<option value="marginal">Marginal or better</option>
					<option value="good">Good or better</option>
					<option value="perfect">Perfect only</option>
					<option value="too_tight">Include risky fits</option>
				</select>
			</label>
			<button type="button" onclick={() => loadCatalog(false)} disabled={catalogLoading}>
				<RefreshCw size={16} />
				Apply filters
			</button>
			<button type="button" onclick={() => loadCatalog(true)} disabled={catalogLoading}>
				<Download size={16} />
				Refresh catalog
			</button>
		</section>

		<section class="panel">
			<div class="panel-head">
				<div>
					<h2>Downloadable recommendations</h2>
					<p>{catalogMessage}</p>
				</div>
				<Cpu size={18} />
			</div>
			<div class="table catalog-table">
				<div class="table-head">
					<span>Fit</span>
					<span>Score</span>
					<span>Model</span>
					<span>Memory</span>
					<span>Speed</span>
					<span>Status</span>
					<span>Action</span>
				</div>
				{#each catalogModels as model}
					{@const job = downloadFor(model)}
					<div class="table-row">
						<span class={`fit ${model.fit_level}`}>{model.fit_level}</span>
						<strong>{fmtNumber(model.score, 1)}</strong>
						<div class="model-cell">
							<strong>{model.name}</strong>
							<span>{model.provider} / {model.quant} / {model.gpu_mode}</span>
						</div>
						<span>{fmtGb(model.memory_required_gb)}</span>
						<span>{fmtNumber(model.estimated_tps, 1)} tok/s</span>
						<div class="status-cell">
							<span>{downloadStatus(model)}</span>
							{#if job}
								<div class="mini-progress"><span style={`width:${Math.min(100, job.percent || 0)}%`}></span></div>
							{/if}
						</div>
						<div class="row-actions">
							<button type="button" onclick={() => downloadModel(model)} disabled={!model.download || isDownloading(model)}>
								<Download size={15} />
								Download
							</button>
							<button type="button" onclick={() => configureFitModel(model)} disabled={!canConfigureFit(model)}>
								<CheckCircle2 size={15} />
								FIT
							</button>
						</div>
					</div>
				{/each}
			</div>
		</section>
	{/if}

	{#if activeTab === 'compare'}
		<section class="profile-bar">
			{#each Object.entries(profileLabels) as [id, item]}
				<button type="button" class:active={profile === id} onclick={() => (profile = id as ProfileId)}>
					{item.title}
				</button>
			{/each}
		</section>

		<section class="results-grid">
			<div class="panel">
				<div class="panel-head">
					<div>
						<h2>Recommended winners</h2>
						<p>{profileLabels[profile].help}</p>
					</div>
					<BarChart3 size={18} />
				</div>
				<div class="winner-list">
					{#each profileWinners as [model, winner], index}
						<div class="winner-row">
							<div>
								<strong>{index + 1}. {model}</strong>
								<span>{fmtNumber(rowNum(winner, ['eval_tps', 'tps']), 1)} tok/s · {fmtMib(rowNum(winner, ['vram_peak_mib']))} VRAM · ctx {rowNum(winner, ['ctx_size'])}</span>
							</div>
							<div class="bar"><span style={`width:${barWidth(winner)}%`}></span></div>
							<button type="button" onclick={() => configureCaliberRow(winner)}>
								<CheckCircle2 size={15} />
								FIT winner
							</button>
						</div>
					{/each}
					{#if profileWinners.length === 0}
						<p class="empty">No completed benchmark rows yet. Start a campaign first.</p>
					{/if}
				</div>
			</div>

			<div class="panel">
				<div class="panel-head">
					<div>
						<h2>Speed vs memory</h2>
						<p>Each point is one measured config. Right is faster; lower uses less memory.</p>
					</div>
					<Gauge size={18} />
				</div>
				<svg class="scatter" viewBox="0 0 640 220" role="img" aria-label="Throughput and memory chart">
					<line x1="44" y1="184" x2="600" y2="184" />
					<line x1="44" y1="28" x2="44" y2="184" />
					<text x="46" y="210">speed</text>
					<text x="8" y="32">memory</text>
					{#each chartRows.slice(0, 160) as row}
						<circle cx={scatterX(row)} cy={scatterY(row)} r="5" />
					{/each}
				</svg>
			</div>
		</section>

		<section class="panel">
			<div class="panel-head">
				<div>
					<h2>Measured configurations</h2>
					<p>Use this when you need the detailed tradeoff table.</p>
				</div>
			</div>
			<div class="table result-table">
				<div class="table-head">
					<span>Model</span>
					<span>Purpose</span>
					<span>Ctx</span>
					<span>Speed</span>
					<span>Memory</span>
					<span>Fit</span>
					<span>Action</span>
				</div>
				{#each resultRows as row}
					<div class="table-row">
						<strong>{rowText(row, ['model'], '-')}</strong>
						<span>{rowText(row, ['row_role', 'sweep'], '-')}</span>
						<span>{rowNum(row, ['ctx_size', 'context'], 0)}</span>
						<span>{fmtNumber(rowNum(row, ['eval_tps', 'tps']), 1)} tok/s</span>
						<span>{fmtMib(rowNum(row, ['vram_peak_mib']))}</span>
						<span>{rowText(row, ['decode_usability', 'fit_status'], '-')}</span>
						<button type="button" onclick={() => configureCaliberRow(row)}>
							<CheckCircle2 size={15} />
							FIT
						</button>
					</div>
				{/each}
			</div>
		</section>
	{/if}

	{#if activeTab === 'reports'}
		<section class="grid">
			<div class="panel">
				<div class="panel-head">
					<div>
						<h2>Saved reports</h2>
						<p>Completed reports stay available for future comparisons. Pending reports can be deleted.</p>
					</div>
					<FileJson size={18} />
				</div>
				<div class="table reports-table">
					<div class="table-head">
						<span>Status</span>
						<span>Rows</span>
						<span>Model/Campaign</span>
						<span>Date</span>
						<span></span>
					</div>
					{#each reports as report}
						<div class="table-row" class:active={selectedReportId === report.id}>
							<button type="button" class="linkish" onclick={() => openReport(report)}>{reportStatusLabel(report)}</button>
							<span>{report.rows}</span>
							<strong>{report.model || report.id}</strong>
							<span>{report.created_at}</span>
							<button type="button" onclick={() => deletePendingReport(report)} disabled={!canDeleteReport(report)}>
								<Trash2 size={15} />
							</button>
						</div>
					{/each}
				</div>
			</div>

			<div class="panel">
				<div class="panel-head">
					<div>
						<h2>Report detail</h2>
						<p>{selectedReportId || 'Select a report'}</p>
					</div>
				</div>
				{#if selectedReport}
					<div class="detail">
						<dl>
							<div><dt>Status</dt><dd>{String(selectedReport.status ?? '-')}</dd></div>
							<div><dt>Measured rows</dt><dd>{reportRows.length}</dd></div>
							<div><dt>Planned configs</dt><dd>{reportPlan.length}</dd></div>
							<div><dt>Created</dt><dd>{String(selectedReport.created_at ?? '-')}</dd></div>
						</dl>
						{#if selectedReportSummary() && canDeleteReport(selectedReportSummary()!)}
							<button type="button" onclick={() => deletePendingReport(selectedReportSummary()!)}>
								<Trash2 size={15} />
								Delete pending report
							</button>
						{/if}
						{#if reportRows[0]}
							<h3>Launcher for first measured row</h3>
							<pre>{planLauncher(reportRows[0])}</pre>
						{/if}
					</div>
				{:else}
					<p class="empty">No report selected.</p>
				{/if}
			</div>
		</section>
	{/if}

	{#if activeTab === 'diagnostics'}
		<section class="grid">
			<div class="panel">
				<div class="panel-head">
					<div>
						<h2>System readiness</h2>
						<p>What Caliber sees before planning and benchmarking.</p>
					</div>
					<Wrench size={18} />
				</div>
				<div class="detail">
					<dl>
						<div><dt>CPU</dt><dd>{fitSystem?.cpu_name ?? '-'}</dd></div>
						<div><dt>Threads</dt><dd>{fitSystem?.cpu_cores ?? '-'}</dd></div>
						<div><dt>RAM total</dt><dd>{fitSystem ? fmtGb(fitSystem.total_ram_gb) : '-'}</dd></div>
						<div><dt>GPU</dt><dd>{fitSystem?.gpu_name ?? '-'}</dd></div>
						<div><dt>GPU count</dt><dd>{fitSystem?.gpu_count ?? '-'}</dd></div>
						<div><dt>Aggregate VRAM</dt><dd>{fitSystem ? fmtGb(fitSystem.total_gpu_vram_gb) : '-'}</dd></div>
					</dl>
				</div>
			</div>
			<div class="panel">
				<div class="panel-head">
					<div>
						<h2>Implementation notes</h2>
						<p>Visible limits, stated plainly.</p>
					</div>
				</div>
				<div class="detail">
					<ul>
						<li>Downloads and initial FIT reuse Fit Advisor.</li>
						<li>Campaigns run server-side; browser closure does not cancel them.</li>
						<li>Winner comparisons ignore reports without measured rows.</li>
						<li>The current runner uses `llama-bench`; the full original llama-server telemetry loop remains a deeper parity target.</li>
					</ul>
				</div>
			</div>
		</section>
	{/if}
</main>

<style>
	.caliber-page {
		display: flex;
		min-height: 100%;
		flex-direction: column;
		gap: 18px;
		padding: 24px;
		color: #f7f7f7;
		background: #0d0f10;
	}

	.hero,
	.answer-strip,
	.controls,
	.panel,
	.tabs,
	.profile-bar {
		border: 1px solid rgba(255, 255, 255, 0.16);
		background: rgba(255, 255, 255, 0.045);
	}

	.hero {
		display: grid;
		grid-template-columns: minmax(0, 1fr) auto;
		gap: 18px;
		align-items: center;
		padding: 18px;
	}

	h1,
	h2,
	h3,
	p {
		margin: 0;
	}

	h1 {
		max-width: 760px;
		font-size: 30px;
		line-height: 1.12;
	}

	h2 {
		font-size: 16px;
	}

	h3 {
		margin-top: 10px;
		font-size: 13px;
	}

	p,
	span,
	code,
	dt,
	dd,
	li {
		color: rgba(255, 255, 255, 0.7);
	}

	.eyebrow {
		color: #67e8f9;
		font-size: 12px;
		font-weight: 700;
		text-transform: uppercase;
	}

	.hero-actions,
	.row-actions,
	.button-row,
	.controls,
	.tabs,
	.profile-bar {
		display: flex;
		flex-wrap: wrap;
		gap: 10px;
	}

	.answer-strip {
		display: grid;
		grid-template-columns: repeat(3, minmax(0, 1fr));
		gap: 1px;
	}

	.answer-strip > div {
		display: grid;
		gap: 6px;
		padding: 14px;
		background: rgba(0, 0, 0, 0.18);
	}

	.answer-strip strong {
		font-size: 18px;
	}

	button,
	input,
	select {
		min-height: 38px;
		border: 1px solid rgba(255, 255, 255, 0.18);
		background: rgba(0, 0, 0, 0.35);
		color: inherit;
	}

	button {
		display: inline-flex;
		align-items: center;
		justify-content: center;
		gap: 8px;
		padding: 0 12px;
		cursor: pointer;
	}

	button.primary,
	button.active,
	.choice.active,
	.model-option.active {
		background: rgba(255, 255, 255, 0.92);
		color: #111;
	}

	button:disabled {
		cursor: not-allowed;
		opacity: 0.5;
	}

	input,
	select {
		padding: 0 10px;
	}

	label {
		display: grid;
		gap: 6px;
		min-width: 190px;
	}

	.check {
		display: flex;
		min-width: auto;
		align-items: center;
		gap: 8px;
	}

	.tabs,
	.profile-bar,
	.controls {
		align-items: center;
		padding: 8px;
	}

	.wizard-grid,
	.grid,
	.results-grid {
		display: grid;
		grid-template-columns: minmax(0, 1fr) minmax(360px, 0.65fr);
		gap: 16px;
	}

	.results-grid {
		grid-template-columns: minmax(360px, 0.75fr) minmax(420px, 1fr);
	}

	.panel {
		overflow: hidden;
	}

	.panel-head {
		display: flex;
		align-items: center;
		justify-content: space-between;
		gap: 12px;
		border-bottom: 1px solid rgba(255, 255, 255, 0.14);
		padding: 12px;
	}

	.choice-grid {
		display: grid;
		grid-template-columns: repeat(2, minmax(0, 1fr));
		gap: 10px;
		padding: 12px;
	}

	.choice {
		display: grid;
		min-height: 98px;
		align-content: start;
		justify-items: start;
		gap: 8px;
		padding: 12px;
		text-align: left;
	}

	.model-list {
		display: grid;
		max-height: 420px;
		overflow: auto;
	}

	.model-option {
		display: grid;
		grid-template-columns: 28px minmax(0, 1fr);
		justify-content: stretch;
		gap: 10px;
		min-height: 62px;
		border-width: 0 0 1px 0;
		padding: 10px 12px;
		text-align: left;
	}

	.model-option > div {
		display: grid;
		gap: 3px;
		min-width: 0;
	}

	.checkbox {
		display: inline-flex;
		width: 22px;
		height: 22px;
		align-items: center;
		justify-content: center;
		border: 1px solid rgba(255, 255, 255, 0.25);
	}

	.run-card,
	.job,
	.detail {
		display: grid;
		gap: 14px;
		padding: 12px;
	}

	dl {
		display: grid;
		grid-template-columns: repeat(2, minmax(0, 1fr));
		gap: 10px;
		margin: 0;
	}

	dl div {
		display: grid;
		gap: 4px;
		border: 1px solid rgba(255, 255, 255, 0.12);
		padding: 10px;
	}

	dt {
		font-size: 12px;
	}

	dd {
		margin: 0;
		color: #fff;
		font-weight: 700;
	}

	.note {
		border-left: 3px solid #67e8f9;
		padding-left: 10px;
	}

	.workflow {
		display: grid;
		grid-template-columns: repeat(5, minmax(0, 1fr));
		gap: 10px;
		padding: 12px;
	}

	.workflow-step {
		display: grid;
		gap: 8px;
		border: 1px solid rgba(255, 255, 255, 0.12);
		padding: 12px;
	}

	.workflow-step > span {
		display: inline-flex;
		width: 24px;
		height: 24px;
		align-items: center;
		justify-content: center;
		background: rgba(103, 232, 249, 0.16);
		color: #67e8f9;
	}

	.table {
		max-height: 620px;
		overflow: auto;
	}

	.table-head,
	.table-row {
		display: grid;
		align-items: center;
		gap: 12px;
		border-bottom: 1px solid rgba(255, 255, 255, 0.12);
		padding: 10px 12px;
	}

	.table-head {
		position: sticky;
		top: 0;
		z-index: 1;
		background: #202020;
		font-size: 12px;
		font-weight: 700;
	}

	.plan-table .table-head,
	.plan-table .table-row {
		grid-template-columns: minmax(160px, 0.9fr) 130px 100px minmax(240px, 1fr);
	}

	.catalog-table .table-head,
	.catalog-table .table-row {
		grid-template-columns: 90px 70px minmax(260px, 1fr) 90px 90px 140px 220px;
	}

	.result-table .table-head,
	.result-table .table-row {
		grid-template-columns: minmax(220px, 1fr) 120px 80px 95px 95px minmax(130px, 0.7fr) 100px;
	}

	.reports-table .table-head,
	.reports-table .table-row {
		grid-template-columns: 90px 60px minmax(220px, 1fr) 180px 48px;
	}

	.table-row.active {
		background: rgba(103, 232, 249, 0.08);
	}

	.table-row strong,
	.model-cell span {
		overflow: hidden;
		text-overflow: ellipsis;
		white-space: nowrap;
	}

	.input-icon {
		display: flex;
		align-items: center;
		gap: 8px;
		border: 1px solid rgba(255, 255, 255, 0.18);
		background: rgba(0, 0, 0, 0.35);
		padding: 0 10px;
	}

	.input-icon input {
		min-height: 36px;
		border: 0;
		background: transparent;
		padding: 0;
	}

	.fit {
		display: inline-flex;
		width: fit-content;
		border: 1px solid rgba(103, 232, 249, 0.35);
		background: rgba(103, 232, 249, 0.12);
		color: #67e8f9;
		padding: 3px 8px;
		font-size: 12px;
	}

	.fit.perfect,
	.fit.good {
		border-color: rgba(52, 211, 153, 0.35);
		background: rgba(52, 211, 153, 0.12);
		color: #34d399;
	}

	.fit.marginal {
		border-color: rgba(251, 191, 36, 0.35);
		background: rgba(251, 191, 36, 0.12);
		color: #fbbf24;
	}

	.model-cell,
	.status-cell {
		display: grid;
		gap: 4px;
		min-width: 0;
	}

	.progress,
	.mini-progress,
	.bar {
		width: 100%;
		overflow: hidden;
		background: rgba(255, 255, 255, 0.1);
	}

	.progress {
		height: 10px;
	}

	.mini-progress {
		height: 4px;
	}

	.bar {
		height: 8px;
	}

	.progress span,
	.mini-progress span,
	.bar span {
		display: block;
		height: 100%;
		background: #67e8f9;
	}

	.event-log {
		display: grid;
		gap: 6px;
		max-height: 300px;
		overflow: auto;
	}

	.winner-list {
		display: grid;
		gap: 10px;
		padding: 12px;
	}

	.winner-row {
		display: grid;
		grid-template-columns: minmax(220px, 1fr) minmax(120px, 0.45fr) auto;
		align-items: center;
		gap: 12px;
		border-bottom: 1px solid rgba(255, 255, 255, 0.1);
		padding-bottom: 10px;
	}

	.winner-row div:first-child {
		display: grid;
		gap: 4px;
		min-width: 0;
	}

	.scatter {
		width: 100%;
		min-height: 260px;
		padding: 12px;
	}

	.scatter line {
		stroke: rgba(255, 255, 255, 0.22);
	}

	.scatter circle {
		fill: #34d399;
		opacity: 0.78;
	}

	.scatter text {
		fill: rgba(255, 255, 255, 0.65);
		font-size: 12px;
	}

	.explain {
		display: flex;
		gap: 12px;
		align-items: flex-start;
		padding: 12px;
	}

	.linkish {
		justify-content: flex-start;
		border: 0;
		background: transparent;
		padding: 0;
		color: #67e8f9;
	}

	pre {
		max-height: 220px;
		margin: 0;
		overflow: auto;
		border: 1px solid rgba(255, 255, 255, 0.12);
		background: rgba(0, 0, 0, 0.25);
		padding: 12px;
		font-size: 12px;
	}

	.empty {
		padding: 12px;
	}

	.error {
		border: 1px solid rgba(255, 174, 0, 0.6);
		background: rgba(255, 174, 0, 0.1);
		padding: 12px;
	}

	@media (max-width: 1100px) {
		.hero,
		.answer-strip,
		.wizard-grid,
		.grid,
		.results-grid,
		.workflow {
			display: flex;
			flex-direction: column;
		}

		.choice-grid {
			grid-template-columns: 1fr;
		}

		.table-head {
			display: none;
		}

		.plan-table .table-row,
		.catalog-table .table-row,
		.result-table .table-row,
		.reports-table .table-row {
			grid-template-columns: 1fr;
		}
	}
</style>
