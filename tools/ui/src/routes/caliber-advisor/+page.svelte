<script lang="ts">
	import { onDestroy, onMount } from 'svelte';
	import {
		Activity,
		BarChart3,
		CheckCircle2,
		Cpu,
		Download,
		FileJson,
		Gauge,
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

	type TabId = 'guided' | 'catalog' | 'results' | 'reports' | 'doctor';
	type ProfileId = 'speed' | 'efficiency' | 'safety' | 'overall';
	type CaliberRow = Record<string, unknown>;

	const contextOptions = [
		{ label: '8k', value: 8192 },
		{ label: '32k', value: 32768 },
		{ label: '64k', value: 65536 },
		{ label: '131k', value: 131072 },
		{ label: '262k', value: 262144 }
	];
	const tabs: { id: TabId; label: string }[] = [
		{ id: 'guided', label: 'Guided Run' },
		{ id: 'catalog', label: 'Catalog' },
		{ id: 'results', label: 'Results' },
		{ id: 'reports', label: 'Reports' },
		{ id: 'doctor', label: 'Doctor' }
	];
	const profiles: ProfileId[] = ['overall', 'speed', 'efficiency', 'safety'];
	const strategies = [
		{ id: 'hybrid_offload', label: 'Hybrid' },
		{ id: 'multi_gpu', label: 'MultiGPU' },
		{ id: 'moe_offload', label: 'MoE offload' },
		{ id: 'balanced', label: 'Balanced' }
	];

	let activeTab = $state<TabId>('guided');
	let profile = $state<ProfileId>('overall');
	let models = $state<CaliberModel[]>([]);
	let selectedModel = $state('');
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
	let contextSize = $state(131072);
	let workloadSweep = $state<'baseline' | 'prefill' | 'kv-fill' | 'all'>('all');
	let loadAfterConfigure = $state(false);

	let fitSystem = $state<FitAdvisorSystem | null>(null);
	let catalogModels = $state<FitAdvisorModel[]>([]);
	let downloads = $state<FitAdvisorDownloadJob[]>([]);
	let catalogLoading = $state(false);
	let catalogSearch = $state('');
	let catalogStrategy = $state('hybrid_offload');
	let catalogLimit = $state(300);
	let catalogMinFit = $state('marginal');
	let catalogMessage = $state('');
	let downloadLastSeq = $state(0);
	let downloadAbort: AbortController | null = null;
	let sweepAbort: AbortController | null = null;

	const selected = $derived(models.find((model) => model.id === selectedModel) ?? models[0] ?? null);
	const visiblePlan = $derived(plan.slice(0, 240));
	const resultRows = $derived(asRows(results?.rows));
	const reportRows = $derived(asRows(selectedReport?.rows));
	const reportPlan = $derived(asRows(selectedReport?.plan));
	const reportModels = $derived(asRows(selectedReport?.models));
	const chartRows = $derived(reportRows.length > 0 ? reportRows : resultRows);
	const chartMaxTps = $derived(Math.max(1, ...chartRows.map((row) => rowNum(row, ['eval_tps', 'tps']))));
	const chartMaxVram = $derived(
		Math.max(1, ...chartRows.map((row) => rowNum(row, ['vram_peak_mib', 'memory_required_mib'])))
	);
	const winners = $derived((results?.winners as Record<ProfileId, Record<string, CaliberRow>> | undefined) ?? ({} as Record<ProfileId, Record<string, CaliberRow>>));
	const profileWinners = $derived(Object.entries(winners[profile] ?? {}));
	const completedReports = $derived(reports.filter((report) => report.rows > 0 || isCompleteStatus(report.status)));
	const pendingReports = $derived(reports.filter((report) => canDeleteReport(report)));

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

	function modelLabel(model: CaliberModel | null): string {
		return model?.name || model?.id || 'No model';
	}

	function reportStatusLabel(report: CaliberReportSummary): string {
		if (report.rows > 0) return 'complete';
		return report.status || 'pending';
	}

	function isCompleteStatus(status: string): boolean {
		return ['complete', 'completed', 'done', 'measured'].includes((status || '').toLowerCase());
	}

	function canDeleteReport(report: CaliberReportSummary): boolean {
		return report.rows <= 0 || !isCompleteStatus(report.status);
	}

	function selectedReportSummary(): CaliberReportSummary | null {
		return reports.find((report) => report.id === selectedReportId) ?? null;
	}

	function payload(): Record<string, unknown> {
		return {
			model: selectedModel,
			opts: { workloadSweep },
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
			selectedModel = selectedModel || models[0]?.id || '';
			message = `${models.length} local models, ${completedReports.length} completed reports`;
		} catch (e) {
			error = e instanceof Error ? e.message : String(e);
		} finally {
			loading = false;
		}
	}

	async function previewPlan() {
		if (!selected) return;
		loading = true;
		error = '';
		try {
			const result = await CaliberAdvisorService.plan(payload());
			plan = result.plan;
			message = `${result.plan_count} planned rows for ${modelLabel(selected)}`;
		} catch (e) {
			error = e instanceof Error ? e.message : String(e);
		} finally {
			loading = false;
		}
	}

	async function startSweep() {
		if (!selected) return;
		running = true;
		error = '';
		eventLog = [];
		sweepAbort?.abort();
		sweepAbort = new AbortController();
		try {
			const started = await CaliberAdvisorService.sweep(payload());
			status = { job_id: started.job_id, status: started.status };
			pushEvent(`queued ${started.job_id}`);
			void CaliberAdvisorService.streamSweepEvents(
				started.job_id,
				(event) => {
					status = event.data;
					pushEvent(`${event.event} ${event.data.report_id ? `report=${event.data.report_id}` : ''}`);
					if (event.event === 'done' || event.event === 'error') sweepAbort?.abort();
				},
				sweepAbort.signal
			).catch((e) => {
				if (!(e instanceof DOMException && e.name === 'AbortError')) {
					pushEvent(e instanceof Error ? e.message : String(e));
				}
			});
			for (let i = 0; i < 240; i += 1) {
				await new Promise((resolve) => setTimeout(resolve, 1000));
				status = await CaliberAdvisorService.sweepStatus(started.job_id);
				if (status.finished) break;
			}
			await refreshAll();
			if (status?.report_id) {
				const summary = reports.find((report) => report.id === status?.report_id);
				if (summary) await openReport(summary);
			}
		} catch (e) {
			error = e instanceof Error ? e.message : String(e);
		} finally {
			running = false;
		}
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
			catalogMessage = `${result.returned_models} recommendations from ${result.total_catalog_models} catalog entries`;
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
			/* download status is secondary */
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
		if (index === -1) {
			downloads = [job, ...downloads];
			return;
		}
		downloads = downloads.map((item, i) => (i === index ? job : item));
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
			catalogMessage = `${result.model} configured${result.loaded ? ' and loaded' : ''}`;
			await loadCatalog();
			await refreshAll();
		} catch (e) {
			error = e instanceof Error ? e.message : String(e);
		}
	}

	async function openReport(report: CaliberReportSummary) {
		error = '';
		try {
			selectedReport = await CaliberAdvisorService.report(report.id);
			selectedReportId = report.id;
			activeTab = 'reports';
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
		const model = rowText(row, ['model', 'model_id'], selectedModel);
		if (!model) return;
		error = '';
		try {
			const result = await CaliberAdvisorService.configure({
				model,
				extra_args: rowText(row, ['extra_args']),
				load_now: loadAfterConfigure
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

	function planLauncher(row: CaliberRow | CaliberPlanItem | null): string {
		if (!row) return '';
		const model = rowText(row as CaliberRow, ['path', 'model_path', 'model'], selectedModel);
		const args = rowText(row as CaliberRow, ['extra_args']);
		return `#!/usr/bin/env bash\nexec ./build/bin/llama-server -m "${model}" ${args}`;
	}
</script>

<svelte:head>
	<title>Caliber Advisor</title>
</svelte:head>

<main class="caliber-page">
	<header class="topbar">
		<div>
			<p class="eyebrow">Calibr logic, native llama.cpp router</p>
			<h1>Caliber Advisor</h1>
			<p>{message || catalogMessage || 'Plan, benchmark, compare, then write the winning runtime preset.'}</p>
		</div>
		<div class="actions">
			<button type="button" onclick={refreshAll} disabled={loading}>
				<RefreshCw size={16} />
				Refresh
			</button>
			<button type="button" class="primary" onclick={startSweep} disabled={!selected || running}>
				<Play size={16} />
				Sweep
			</button>
		</div>
	</header>

	{#if error}
		<div class="error">{error}</div>
	{/if}

	<nav class="tabs" aria-label="Caliber Advisor sections">
		{#each tabs as tab}
			<button type="button" class:active={activeTab === tab.id} onclick={() => (activeTab = tab.id)}>
				{#if tab.id === 'guided'}<Gauge size={16} />{/if}
				{#if tab.id === 'catalog'}<Download size={16} />{/if}
				{#if tab.id === 'results'}<BarChart3 size={16} />{/if}
				{#if tab.id === 'reports'}<FileJson size={16} />{/if}
				{#if tab.id === 'doctor'}<Wrench size={16} />{/if}
				{tab.label}
			</button>
		{/each}
	</nav>

	<section class="system-grid">
		<div class="stat">
			<span>Local Models</span>
			<strong>{models.length}</strong>
		</div>
		<div class="stat">
			<span>Completed Reports</span>
			<strong>{completedReports.length}</strong>
		</div>
		<div class="stat">
			<span>Pending Reports</span>
			<strong>{pendingReports.length}</strong>
		</div>
		<div class="stat">
			<span>Target Context</span>
			<strong>{contextOptions.find((item) => item.value === contextSize)?.label ?? contextSize}</strong>
		</div>
		<div class="stat">
			<span>VRAM</span>
			<strong>{fitSystem ? fmtGb(fitSystem.total_gpu_vram_gb) : '-'}</strong>
		</div>
	</section>

	{#if activeTab === 'guided'}
		<section class="controls">
			<label>
				<span>Model</span>
				<select bind:value={selectedModel}>
					{#each models as model}
						<option value={model.id}>{model.name || model.id}</option>
					{/each}
				</select>
			</label>
			<label>
				<span>Context</span>
				<select bind:value={contextSize}>
					{#each contextOptions as option}
						<option value={option.value}>{option.label}</option>
					{/each}
				</select>
			</label>
			<label>
				<span>Workload</span>
				<select bind:value={workloadSweep}>
					<option value="all">All</option>
					<option value="baseline">Baseline</option>
					<option value="prefill">Prefill</option>
					<option value="kv-fill">KV fill</option>
				</select>
			</label>
			<label class="check">
				<input type="checkbox" bind:checked={loadAfterConfigure} />
				<span>Load after configure</span>
			</label>
			<button type="button" onclick={previewPlan} disabled={!selected || loading}>
				<Settings2 size={16} />
				Plan
			</button>
			<button type="button" class="primary" onclick={startSweep} disabled={!selected || running}>
				<Play size={16} />
				Sweep
			</button>
		</section>

		<section class="grid">
			<div class="panel">
				<div class="panel-head">
					<div>
						<h2>Plan Preview</h2>
						<p>{visiblePlan.length} rows</p>
					</div>
					<span class="pill">{modelLabel(selected)}</span>
				</div>
				<div class="table plan-table">
					<div class="table-head">
						<span>Sweep</span>
						<span>Role</span>
						<span>Label</span>
						<span>Args</span>
					</div>
					{#each visiblePlan as row}
						<div class="table-row">
							<span>{row.sweep}</span>
							<span>{row.row_role}</span>
							<strong>{row.label}</strong>
							<code>{row.extra_args}</code>
						</div>
					{/each}
				</div>
			</div>

			<div class="panel">
				<div class="panel-head">
					<div>
						<h2>Live Job</h2>
						<p>{status?.status ?? 'idle'}</p>
					</div>
					<Activity size={18} />
				</div>
				<div class="job">
					<div class="progress">
						<span
							style={`width:${status?.total ? Math.min(100, ((status.current ?? 0) / status.total) * 100) : running ? 12 : 0}%`}
						></span>
					</div>
					<dl>
						<div><dt>Current</dt><dd>{status?.current ?? 0}</dd></div>
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

	{#if activeTab === 'catalog'}
		<section class="controls">
			<label>
				<span>Search</span>
				<div class="input-icon">
					<Search size={15} />
					<input bind:value={catalogSearch} placeholder="qwen, coder, moe..." />
				</div>
			</label>
			<label>
				<span>Context</span>
				<select bind:value={contextSize}>
					{#each contextOptions as option}
						<option value={option.value}>{option.label}</option>
					{/each}
				</select>
			</label>
			<label>
				<span>Minimum Fit</span>
				<select bind:value={catalogMinFit}>
					<option value="marginal">Marginal</option>
					<option value="good">Good</option>
					<option value="perfect">Perfect</option>
					<option value="too_tight">Too tight</option>
				</select>
			</label>
			<label>
				<span>Limit</span>
				<input type="number" min="25" max="1000" bind:value={catalogLimit} />
			</label>
			<div class="segmented">
				{#each strategies as strategy}
					<button
						type="button"
						class:active={catalogStrategy === strategy.id}
						onclick={() => {
							catalogStrategy = strategy.id;
							void loadCatalog();
						}}
					>
						{strategy.label}
					</button>
				{/each}
			</div>
			<button type="button" onclick={() => loadCatalog(false)} disabled={catalogLoading}>
				<RefreshCw size={16} />
				Apply
			</button>
			<button type="button" onclick={() => loadCatalog(true)} disabled={catalogLoading}>
				<Download size={16} />
				Refresh Catalog
			</button>
		</section>

		<section class="panel">
			<div class="panel-head">
				<div>
					<h2>Catalog Recommendations</h2>
					<p>{catalogMessage}</p>
				</div>
				<Cpu size={18} />
			</div>
			<div class="table catalog-table">
				<div class="table-head">
					<span>Fit</span>
					<span>Score</span>
					<span>Model</span>
					<span>Mem</span>
					<span>Tok/s</span>
					<span>Status</span>
					<span>Actions</span>
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
						<span>{fmtNumber(model.estimated_tps, 1)}</span>
						<div class="status-cell">
							<span>{downloadStatus(model)}</span>
							{#if job}
								<div class="mini-progress"><span style={`width:${Math.min(100, job.percent || 0)}%`}></span></div>
							{/if}
						</div>
						<div class="row-actions">
							<button type="button" onclick={() => downloadModel(model)} disabled={!model.download || isDownloading(model)}>
								<Download size={15} />
								DL
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

	{#if activeTab === 'results'}
		<section class="profile-bar">
			{#each profiles as item}
				<button type="button" class:active={profile === item} onclick={() => (profile = item)}>{item}</button>
			{/each}
		</section>

		<section class="results-grid">
			<div class="panel">
				<div class="panel-head">
					<div>
						<h2>Global Winners</h2>
						<p>{profileWinners.length} models in {profile} profile</p>
					</div>
					<BarChart3 size={18} />
				</div>
				<div class="winner-list">
					{#each profileWinners as [model, winner]}
						<div class="winner-row">
							<div>
								<strong>{model}</strong>
								<span>{rowText(winner, ['reason', 'recommendation', 'decode_usability'], 'winner')}</span>
							</div>
							<div class="bar"><span style={`width:${barWidth(winner)}%`}></span></div>
							<button type="button" onclick={() => configureCaliberRow(winner)}>
								<CheckCircle2 size={15} />
								FIT
							</button>
						</div>
					{/each}
					{#if profileWinners.length === 0}
						<p class="empty">No completed Caliber rows available yet.</p>
					{/if}
				</div>
			</div>

			<div class="panel">
				<div class="panel-head">
					<div>
						<h2>Throughput vs VRAM</h2>
						<p>{chartRows.length} measured rows</p>
					</div>
					<Gauge size={18} />
				</div>
				<svg class="scatter" viewBox="0 0 640 220" role="img" aria-label="Throughput and memory chart">
					<line x1="44" y1="184" x2="600" y2="184" />
					<line x1="44" y1="28" x2="44" y2="184" />
					<text x="46" y="210">tok/s</text>
					<text x="8" y="32">VRAM</text>
					{#each chartRows.slice(0, 120) as row}
						<circle cx={scatterX(row)} cy={scatterY(row)} r="5" />
					{/each}
				</svg>
			</div>
		</section>

		<section class="panel">
			<div class="panel-head">
				<div>
					<h2>All Results</h2>
					<p>Aggregated across completed saved reports</p>
				</div>
			</div>
			<div class="table result-table">
				<div class="table-head">
					<span>Model</span>
					<span>Role</span>
					<span>Ctx</span>
					<span>Tok/s</span>
					<span>VRAM</span>
					<span>Shared</span>
					<span>Usability</span>
					<span>FIT</span>
				</div>
				{#each resultRows as row}
					<div class="table-row">
						<strong>{rowText(row, ['model'], '-')}</strong>
						<span>{rowText(row, ['row_role', 'sweep'], '-')}</span>
						<span>{rowNum(row, ['ctx_size', 'context'], 0)}</span>
						<span>{fmtNumber(rowNum(row, ['eval_tps', 'tps']), 1)}</span>
						<span>{fmtMib(rowNum(row, ['vram_peak_mib']))}</span>
						<span>{fmtMib(rowNum(row, ['shared_peak_mib']))}</span>
						<span>{rowText(row, ['decode_usability', 'fit_status'], '-')}</span>
						<button type="button" onclick={() => configureCaliberRow(row)}>
							<CheckCircle2 size={15} />
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
						<h2>Saved Reports</h2>
						<p>{reports.length} total, {pendingReports.length} pending/deletable</p>
					</div>
					<FileJson size={18} />
				</div>
				<div class="table reports-table">
					<div class="table-head">
						<span>Status</span>
						<span>Rows</span>
						<span>Model</span>
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
						<h2>Report Detail</h2>
						<p>{selectedReportId || 'Select a report'}</p>
					</div>
				</div>
				{#if selectedReport}
					<div class="detail">
						<dl>
							<div><dt>Status</dt><dd>{String(selectedReport.status ?? '-')}</dd></div>
							<div><dt>Rows</dt><dd>{reportRows.length}</dd></div>
							<div><dt>Plan</dt><dd>{reportPlan.length}</dd></div>
							<div><dt>Models</dt><dd>{reportModels.length}</dd></div>
						</dl>
						{#if selectedReportSummary() && canDeleteReport(selectedReportSummary()!)}
							<button type="button" onclick={() => deletePendingReport(selectedReportSummary()!)}>
								<Trash2 size={15} />
								Delete pending report
							</button>
						{/if}
						{#if reportRows[0]}
							<h3>Launcher</h3>
							<pre>{planLauncher(reportRows[0])}</pre>
						{:else if reportPlan[0]}
							<h3>Planned Launcher</h3>
							<pre>{planLauncher(reportPlan[0])}</pre>
						{/if}
					</div>
				{:else}
					<p class="empty">No report selected.</p>
				{/if}
			</div>
		</section>

		{#if selectedReport}
			<section class="panel">
				<div class="panel-head">
					<div>
						<h2>Report Rows</h2>
						<p>{reportRows.length > 0 ? 'Measured rows' : 'Planned rows'}</p>
					</div>
				</div>
				<div class="table result-table">
					<div class="table-head">
						<span>Label</span>
						<span>Role</span>
						<span>Ctx</span>
						<span>Tok/s</span>
						<span>VRAM</span>
						<span>Args</span>
					</div>
					{#each (reportRows.length > 0 ? reportRows : reportPlan) as row}
						<div class="table-row">
							<strong>{rowText(row, ['label', 'id', 'model'], '-')}</strong>
							<span>{rowText(row, ['row_role', 'sweep'], '-')}</span>
							<span>{rowNum(row, ['ctx_size', 'context'], 0) || '-'}</span>
							<span>{fmtNumber(rowNum(row, ['eval_tps', 'tps']), 1)}</span>
							<span>{fmtMib(rowNum(row, ['vram_peak_mib']))}</span>
							<code>{rowText(row, ['extra_args'], '-')}</code>
						</div>
					{/each}
				</div>
			</section>
		{/if}
	{/if}

	{#if activeTab === 'doctor'}
		<section class="grid">
			<div class="panel">
				<div class="panel-head">
					<div>
						<h2>Runtime</h2>
						<p>Router and hardware snapshot</p>
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
						<h2>Current Limits</h2>
						<p>What is implemented in this branch</p>
					</div>
				</div>
				<div class="detail">
					<ul>
						<li>Catalog, download, resume status and FIT use the Fit Advisor backend.</li>
						<li>Caliber plans and reports are native API data, not static HTML.</li>
						<li>Real llama-server benchmark execution is still the next backend slice.</li>
						<li>Planned or incomplete reports stay out of winner comparisons because they have no measured rows.</li>
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
		gap: 20px;
		padding: 24px;
		color: var(--color-text-primary, #f5f5f5);
		background: #0d0f10;
	}

	.topbar,
	.controls,
	.panel,
	.stat,
	.tabs,
	.profile-bar {
		border: 1px solid rgba(255, 255, 255, 0.16);
		background: rgba(255, 255, 255, 0.04);
	}

	.topbar {
		display: flex;
		align-items: center;
		justify-content: space-between;
		gap: 16px;
		padding: 16px;
	}

	h1,
	h2,
	h3,
	p {
		margin: 0;
	}

	h1 {
		font-size: 26px;
		line-height: 1.15;
	}

	h2 {
		font-size: 15px;
	}

	h3 {
		font-size: 13px;
		margin-top: 14px;
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
		color: #7dd3fc;
		font-size: 12px;
	}

	.actions,
	.controls {
		display: flex;
		flex-wrap: wrap;
		align-items: end;
		gap: 12px;
	}

	.controls {
		padding: 12px;
	}

	label {
		display: grid;
		gap: 6px;
		min-width: 180px;
	}

	.check {
		display: flex;
		min-width: auto;
		align-items: center;
		gap: 8px;
	}

	input,
	select,
	button {
		min-height: 38px;
		border: 1px solid rgba(255, 255, 255, 0.18);
		background: rgba(0, 0, 0, 0.35);
		color: inherit;
	}

	input,
	select {
		padding: 0 10px;
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
	button.active {
		background: rgba(255, 255, 255, 0.9);
		color: #111;
	}

	button:disabled {
		cursor: not-allowed;
		opacity: 0.5;
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

	.tabs,
	.profile-bar,
	.segmented {
		display: flex;
		flex-wrap: wrap;
		gap: 8px;
		padding: 8px;
	}

	.segmented {
		border: 0;
		padding: 0;
	}

	.system-grid {
		display: grid;
		grid-template-columns: repeat(5, minmax(130px, 1fr));
		gap: 12px;
	}

	.stat {
		display: grid;
		gap: 6px;
		padding: 14px;
	}

	.stat strong {
		font-size: 18px;
	}

	.grid,
	.results-grid {
		display: grid;
		grid-template-columns: minmax(0, 1.45fr) minmax(320px, 0.55fr);
		gap: 16px;
	}

	.results-grid {
		grid-template-columns: minmax(320px, 0.7fr) minmax(360px, 1fr);
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

	.pill,
	.fit {
		display: inline-flex;
		align-items: center;
		width: fit-content;
		border: 1px solid rgba(125, 211, 252, 0.35);
		background: rgba(125, 211, 252, 0.12);
		color: #7dd3fc;
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
		grid-template-columns: 90px 120px minmax(180px, 0.6fr) minmax(260px, 1fr);
	}

	.catalog-table .table-head,
	.catalog-table .table-row {
		grid-template-columns: 90px 70px minmax(260px, 1fr) 80px 70px 130px 150px;
	}

	.result-table .table-head,
	.result-table .table-row {
		grid-template-columns: minmax(200px, 1fr) 110px 80px 80px 90px 90px minmax(120px, 0.6fr) 60px;
	}

	.reports-table .table-head,
	.reports-table .table-row {
		grid-template-columns: 90px 60px minmax(220px, 1fr) 180px 48px;
	}

	.table-row.active {
		background: rgba(125, 211, 252, 0.08);
	}

	.table-row strong,
	.table-row code,
	.model-cell span {
		overflow: hidden;
		text-overflow: ellipsis;
		white-space: nowrap;
	}

	.model-cell {
		display: grid;
		gap: 3px;
		min-width: 0;
	}

	.row-actions,
	.status-cell {
		display: flex;
		align-items: center;
		gap: 8px;
	}

	.status-cell {
		flex-direction: column;
		align-items: flex-start;
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
		background: #7dd3fc;
	}

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
		grid-template-columns: minmax(180px, 1fr) minmax(120px, 0.6fr) auto;
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

	.linkish {
		justify-content: flex-start;
		border: 0;
		background: transparent;
		padding: 0;
		color: #7dd3fc;
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
		.system-grid {
			grid-template-columns: repeat(2, minmax(0, 1fr));
		}

		.topbar,
		.grid,
		.results-grid {
			display: flex;
			flex-direction: column;
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
