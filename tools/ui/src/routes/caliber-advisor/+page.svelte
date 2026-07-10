<script lang="ts">
	import { onDestroy, onMount } from 'svelte';
	import {
		Activity,
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
		Square,
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

	type TabId = 'start' | 'candidates' | 'reports' | 'diagnostics';
	type ProfileId = 'overall' | 'speed' | 'efficiency' | 'safety';
	type RunScope = 'quick' | 'standard' | 'deep';
	type ReportScope = 'latest' | 'all';
	type ReportMetric = 'eval' | 'prompt' | 'memory' | 'latency' | 'vram';
	type CaliberRow = Record<string, unknown>;
	type ReportModelGroup = { model: string; rows: CaliberRow[]; winner: CaliberRow | null };

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
	const reportMetrics: ReportMetric[] = ['eval', 'prompt', 'memory', 'latency', 'vram'];

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
	let reportScope = $state<ReportScope>('latest');
	let reportMetric = $state<ReportMetric>('eval');
	let downloadLastSeq = $state(0);
	let downloadAbort: AbortController | null = null;
	let sweepAbort: AbortController | null = null;
	let sweepFinalizedFor = '';

	const resultRows = $derived(asRows(results?.rows).filter((row) => rowNum(row, ['eval_tps', 'tps']) > 0));
	const reportRows = $derived(asRows(selectedReport?.rows));
	const reportPlan = $derived(asRows(selectedReport?.plan));
	const analyticsRows = $derived(resultRows.length > 0 ? resultRows : reportRows);
	const scopedAnalyticsRows = $derived(filterReportScope(analyticsRows, reportScope));
	const okAnalyticsRows = $derived(scopedAnalyticsRows.filter((row) => rowText(row, ['ok']) !== 'false' && rowNum(row, ['eval_tps', 'tps']) > 0));
	const reportGroups = $derived(buildReportGroups(scopedAnalyticsRows));
	const reportLeaderboard = $derived(rankRowsByMetric(reportGroups.map((group) => group.winner).filter(Boolean) as CaliberRow[]));
	const reportMetricMax = $derived(Math.max(1, ...reportLeaderboard.map((row) => reportMetricValue(row, reportMetric))));
	const reportScatterRows = $derived(okAnalyticsRows.filter((row) => reportTimeSec(row) > 0));
	const reportMaxTime = $derived(Math.max(1, ...reportScatterRows.map(reportTimeSec)));
	const reportMaxMemory = $derived(Math.max(1, ...reportScatterRows.map((row) => reportMemoryMib(row))));
	const syntheticRows = $derived(scopedAnalyticsRows.filter((row) => rowText(row, ['benchmark_backend']) === 'llama-bench').length);
	const profileWinners = $derived(buildProfileWinners(resultRows));
	const bestWinner = $derived(profileWinners[0]?.[1] ?? null);
	const completedReports = $derived(reports.filter((report) => report.rows > 0 && isCompleteStatus(report.status)));
	const pendingReports = $derived(reports.filter((report) => canDeleteReport(report)));
	const failedReports = $derived(reports.filter((report) => report.status === 'failed'));
	const hasPendingDownload = $derived(downloads.some((job) => isActiveDownloadStatus(job.status)));
	const selectedModels = $derived(models.filter((model) => selectedLocalIds.includes(model.id)));
	const pendingSelectedIds = $derived(selectedLocalIds.filter((id) => !hasHistoricModelResult(id)));
	const selectableModels = $derived(models.filter((model) => Boolean(model.path)));
	const planModels = $derived(uniqueStrings(plan.map((item) => item.model)).length);
	const targetContext = $derived(contextOptions.find((item) => item.value === contextSize));
	const readyToRun = $derived(pendingSelectedIds.length > 0 && !running);
	const nextAction = $derived(nextActionText());

	onMount(() => {
		void refreshAll();
		void loadCatalog();
		void refreshDownloads();
		void restoreActiveSweep();
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

	function reportSessionKey(row: CaliberRow): string {
		return rowText(row, ['bench_session_started_at', 'run_started_at', 'timestamp'], '');
	}

	function filterReportScope(rows: CaliberRow[], scope: ReportScope): CaliberRow[] {
		if (scope === 'all') return rows;
		const latest = rows.map(reportSessionKey).filter(Boolean).sort().at(-1) ?? '';
		return latest ? rows.filter((row) => reportSessionKey(row) === latest) : rows;
	}

	function isReportCandidate(row: CaliberRow): boolean {
		const role = rowText(row, ['row_role']);
		const workload = rowText(row, ['workload_kind'], 'baseline');
		return (role === 'candidate' || (!role && workload === 'baseline')) && rowText(row, ['ok']) !== 'false';
	}

	function reportMemoryMib(row: CaliberRow): number {
		const run = rowNum(row, ['vram_peak_mib', 'memory_required_mib']);
		const shared = rowNum(row, ['shared_peak_mib']);
		const ram = rowNum(row, ['ram_used_peak_mib']);
		return Math.max(0, run + shared + Math.max(0, ram - rowNum(row, ['ram_baseline_mib'], 0)));
	}

	function reportTimeSec(row: CaliberRow): number {
		const direct = rowNum(row, ['time_total_sec']);
		if (direct > 0) return direct;
		const requestMs = rowNum(row, ['total_request_ms', 'latency_total_request_ms']);
		if (requestMs > 0) return requestMs / 1000;
		const prompt = rowNum(row, ['prompt_n']) / Math.max(0.001, rowNum(row, ['prompt_tps']));
		const evalTime = rowNum(row, ['eval_n']) / Math.max(0.001, rowNum(row, ['eval_tps', 'tps']));
		const total = prompt + evalTime;
		return Number.isFinite(total) && total > 0 ? total : 0;
	}

	function reportMetricValue(row: CaliberRow, metric: ReportMetric): number {
		if (metric === 'prompt') return rowNum(row, ['prompt_tps']);
		if (metric === 'memory') return reportMemoryMib(row);
		if (metric === 'latency') return reportTimeSec(row);
		if (metric === 'vram') return rowNum(row, ['vram_peak_mib']);
		return rowNum(row, ['eval_tps', 'tps']);
	}

	function reportMetricLabel(metric: ReportMetric): string {
		if (metric === 'prompt') return 'Prompt tokens/s';
		if (metric === 'memory') return 'VRAM + RAM';
		if (metric === 'latency') return 'Total seconds';
		if (metric === 'vram') return 'VRAM peak';
		return 'Eval tokens/s';
	}

	function reportMetricUnit(metric: ReportMetric, value: number): string {
		if (metric === 'memory' || metric === 'vram') return fmtMib(value);
		if (metric === 'latency') return `${fmtNumber(value, 2)} s`;
		return `${fmtNumber(value, 1)} t/s`;
	}

	function metricHigherIsBetter(metric: ReportMetric): boolean {
		return metric === 'eval' || metric === 'prompt';
	}

	function rankRowsByMetric(rows: CaliberRow[]): CaliberRow[] {
		return [...rows].sort((a, b) => {
			const av = reportMetricValue(a, reportMetric);
			const bv = reportMetricValue(b, reportMetric);
			return metricHigherIsBetter(reportMetric) ? bv - av : av - bv;
		});
	}

	function reportScore(row: CaliberRow): number {
		const evalTps = rowNum(row, ['eval_tps', 'tps']);
		const promptTps = rowNum(row, ['prompt_tps']);
		const memory = Math.max(1, reportMemoryMib(row));
		const shared = rowNum(row, ['shared_peak_mib']);
		if (profile === 'speed') return evalTps;
		if (profile === 'efficiency') return evalTps / memory;
		if (profile === 'safety') return evalTps - shared * 0.02;
		return evalTps * 0.62 + promptTps * 0.03 - shared * 0.03 + rowNum(row, ['ctx_size']) / 8192;
	}

	function buildReportGroups(rows: CaliberRow[]): ReportModelGroup[] {
		const map = new Map<string, CaliberRow[]>();
		for (const row of rows) {
			const model = rowText(row, ['model', 'model_id'], 'unknown model');
			map.set(model, [...(map.get(model) ?? []), row]);
		}
		return [...map.entries()]
			.map(([model, groupRows]) => {
				const candidates = groupRows.filter(isReportCandidate);
				const winner = candidates.sort((a, b) => reportScore(b) - reportScore(a))[0] ?? null;
				return { model, rows: groupRows, winner };
			})
			.sort((a, b) => reportScore(b.winner ?? {}) - reportScore(a.winner ?? {}));
	}

	function buildProfileWinners(rows: CaliberRow[]): [string, CaliberRow][] {
		return buildReportGroups(rows)
			.filter((group) => Boolean(group.winner))
			.map((group) => [group.model, group.winner as CaliberRow]);
	}

	function rowIdentity(row: CaliberRow): string {
		return [
			rowText(row, ['id']),
			rowText(row, ['model', 'model_id']),
			rowText(row, ['row_role']),
			rowText(row, ['workload_kind']),
			String(rowNum(row, ['ctx_size'])),
			rowText(row, ['extra_args'])
		].join('|');
	}

	function reportFitClass(row: CaliberRow): string {
		const memory = reportMemoryMib(row);
		const vram = fitSystem ? fitSystem.total_gpu_vram_gb * 1024 : rowNum(row, ['vram_budget_mib', 'gpu_vram_mib'], 0);
		if (vram > 0 && memory > vram * 1.12) return 'ultra';
		if (vram > 0 && memory > vram * 0.85) return 'high';
		const params = rowNum(row, ['params_b', 'model_params_b']);
		if (params >= 20) return 'middle';
		return 'low';
	}

	function reportFitLabel(row: CaliberRow): string {
		const level = reportFitClass(row);
		if (level === 'ultra') return 'ultra';
		if (level === 'high') return 'high';
		if (level === 'middle') return 'middle';
		return 'low';
	}

	function normalizeIdentity(value: string): string {
		return value.toLowerCase().replace(/[^a-z0-9]+/g, '');
	}

	function hasHistoricModelResult(modelId: string): boolean {
		const model = models.find((item) => item.id === modelId);
		const identities = [modelId, model?.name ?? '', model?.path ?? '']
			.filter(Boolean)
			.map((value) => normalizeIdentity(String(value)));
		return resultRows.some((row) => {
			if (rowText(row, ['ok']) === 'false') return false;
			const rowIdentities = [
				rowText(row, ['model', 'model_id']),
				rowText(row, ['model_path', 'path'])
			]
				.filter(Boolean)
				.map((value) => normalizeIdentity(String(value)));
			return identities.some((identity) =>
				rowIdentities.some((rowIdentity) => rowIdentity === identity || rowIdentity.includes(identity) || identity.includes(rowIdentity))
			);
		});
	}

	function reportScatterX(row: CaliberRow): number {
		const ratio = Math.log10(reportTimeSec(row) + 1) / Math.log10(reportMaxTime + 1);
		return 58 + Math.max(0, Math.min(1, ratio)) * 662;
	}

	function reportScatterY(row: CaliberRow): number {
		const ratio = reportMemoryMib(row) / reportMaxMemory;
		return 298 - Math.max(0, Math.min(1, ratio)) * 244;
	}

	function reportBarWidth(row: CaliberRow): number {
		const value = reportMetricValue(row, reportMetric);
		if (reportMetric === 'memory' || reportMetric === 'latency' || reportMetric === 'vram') {
			return Math.max(3, Math.min(100, 100 - (value / reportMetricMax) * 88));
		}
		return Math.max(3, Math.min(100, (value / reportMetricMax) * 100));
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
		if (report.rows > 0 && isCompleteStatus(report.status)) return 'complete';
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

	function selectAllLocal() {
		selectedLocalIds = selectableModels.map((model) => model.id);
		plan = [];
	}

	function clearSelection() {
		selectedLocalIds = [];
		plan = [];
	}

	function nextActionText(): string {
		if (status?.cancel_requested) return 'Stop requested. Caliber will exit after the current benchmark config returns.';
		if (running) return 'Benchmark is running on the server. You can close this page and come back to Reports.';
		if (selectedLocalIds.length === 0) return 'Choose at least one installed model, or download/configure a catalog model first.';
		if (pendingSelectedIds.length === 0) return 'All selected models already have completed historical measurements. Open Reports to compare them without rerunning.';
		if (plan.length === 0) return 'Review the benchmark plan so you know how many configs will run.';
		if (completedReports.length === 0) return 'Start the benchmark. The report will appear automatically when it finishes.';
		return 'Open Reports to compare historical winners and choose the model/config to FIT.';
	}

	function payload(): Record<string, unknown> {
		return {
			models: pendingSelectedIds,
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

	function sweepIsLive(snapshot: CaliberSweepStatus | null): boolean {
		const state = (snapshot?.status ?? '').toLowerCase();
		return Boolean(snapshot?.job_id && !snapshot.finished && ['queued', 'running', 'stopping'].includes(state));
	}

	function delay(ms: number, signal: AbortSignal): Promise<void> {
		return new Promise((resolve) => {
			const timeout = window.setTimeout(resolve, ms);
			signal.addEventListener(
				'abort',
				() => {
					window.clearTimeout(timeout);
					resolve();
				},
				{ once: true }
			);
		});
	}

	function attachSweep(jobId: string) {
		sweepAbort?.abort();
		sweepAbort = new AbortController();
		const signal = sweepAbort.signal;
		running = true;
		void CaliberAdvisorService.streamSweepEvents(
			jobId,
			(event) => {
				status = event.data;
				running = sweepIsLive(event.data);
				pushEvent(humanEvent(event.event, event.data));
				if (!sweepIsLive(event.data) || ['done', 'error', 'cancelled'].includes(event.event)) {
					sweepAbort?.abort();
					void finishSweep(event.data);
				}
			},
			signal
		).catch((e) => {
			if (!(e instanceof DOMException && e.name === 'AbortError')) pushEvent(e instanceof Error ? e.message : String(e));
		});
		void monitorSweep(jobId, signal);
	}

	async function monitorSweep(jobId: string, signal: AbortSignal) {
		while (!signal.aborted) {
			await delay(2000, signal);
			if (signal.aborted) return;
			try {
				const latest = await CaliberAdvisorService.sweepStatus(jobId);
				status = latest;
				running = sweepIsLive(latest);
				if (!sweepIsLive(latest)) {
					await finishSweep(latest);
					return;
				}
			} catch (e) {
				pushEvent(e instanceof Error ? e.message : String(e));
			}
		}
	}

	async function finishSweep(snapshot: CaliberSweepStatus) {
		running = false;
		if (snapshot.job_id && status?.job_id === snapshot.job_id) sweepAbort?.abort();
		if (snapshot.job_id && sweepFinalizedFor === snapshot.job_id) return;
		if (snapshot.job_id) sweepFinalizedFor = snapshot.job_id;
		await refreshAll();
		if (snapshot.report_id) {
			const summary = reports.find((report) => report.id === snapshot.report_id);
			if (summary) await openReport(summary, false);
			activeTab = 'reports';
		}
	}

	async function restoreActiveSweep() {
		try {
			const snapshot = await CaliberAdvisorService.sweepStatus();
			if (!snapshot.job_id || snapshot.status === 'idle') {
				running = false;
				return;
			}
			status = snapshot;
			running = sweepIsLive(snapshot);
			if (running && snapshot.job_id) {
				activeTab = 'start';
				eventLog = [];
				pushEvent(`Restored campaign ${snapshot.job_id}`);
				attachSweep(snapshot.job_id);
			}
		} catch (e) {
			pushEvent(e instanceof Error ? e.message : String(e));
		}
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
		if (pendingSelectedIds.length === 0) {
			message = 'No benchmark needed: selected models are already in the historical archive.';
			activeTab = 'reports';
			return;
		}
		loading = true;
		error = '';
		try {
			const result = await CaliberAdvisorService.plan(payload());
			plan = result.plan;
			message = `${result.plan_count} configs planned across ${pendingSelectedIds.length} new model(s); ${selectedLocalIds.length - pendingSelectedIds.length} already archived.`;
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
			const planned = await CaliberAdvisorService.plan(payload());
			plan = planned.plan;
			message = `${planned.plan_count} configs planned across ${pendingSelectedIds.length} new model(s)`;
			const started = await CaliberAdvisorService.sweep(payload());
			status = { job_id: started.job_id, status: started.status };
			pushEvent(`Queued campaign ${started.job_id}`);
			attachSweep(started.job_id);
		} catch (e) {
			running = false;
			error = e instanceof Error ? e.message : String(e);
		}
	}

	async function stopSweep() {
		if (!status?.job_id) return;
		error = '';
		try {
			const stopped = await CaliberAdvisorService.stopSweep(status.job_id);
			status = stopped;
			running = sweepIsLive(stopped);
			pushEvent('Stop requested');
		} catch (e) {
			error = e instanceof Error ? e.message : String(e);
		}
	}

	function humanEvent(event: string, data: CaliberSweepStatus & Record<string, unknown>): string {
		if (event === 'queued') return `Campaign queued ${String(data.job_id ?? '')}`;
		if (event === 'started') return 'Campaign started';
		if (event === 'stop') return String(data.message ?? 'Stop requested');
		if (event === 'cancelled') return 'Campaign cancelled';
		if (event === 'preflight') return String(data.message ?? 'Preparing benchmark');
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
		catalogMessage = refresh ? 'Refreshing catalog...' : 'Loading catalog...';
		try {
			if (refresh) await FitAdvisorService.refreshCatalog();
			const result = await FitAdvisorService.models({
				use_case: 'coding',
				min_fit: catalogMinFit,
				quant: '',
				search: catalogSearch,
				strategy: catalogStrategy,
				context: contextSize,
				limit: catalogLimit,
				include_too_tight: true
			});
			fitSystem = result.system;
			catalogModels = result.models;
			catalogMessage = result.returned_models > 0
				? `${result.returned_models} downloadable candidates ranked for this machine`
				: 'No candidates matched these filters. Try a broader search or lower the minimum fit.';
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

	function sameNonEmpty(left: string | null | undefined, right: string | null | undefined): boolean {
		return Boolean(left && right && left === right);
	}

	function downloadFor(model: FitAdvisorModel): FitAdvisorDownloadJob | null {
		return (
			downloads.find(
				(job) =>
					sameNonEmpty(job.model_id, model.id) ||
					sameNonEmpty(job.hf_ref, model.download?.hf_ref) ||
					sameNonEmpty(job.target_dir, model.download?.target_dir ?? model.target_dir) ||
					sameNonEmpty(job.local_path, model.local_path)
			) ?? model.download_progress ?? null
		);
	}

	function isActiveDownloadStatus(status: string): boolean {
		return status === 'queued' || status === 'resolving' || status === 'downloading';
	}

	function downloadStatus(model: FitAdvisorModel): string {
		return downloadFor(model)?.status ?? model.download_status ?? (model.installed ? 'configured' : 'available');
	}

	function downloadActionLabel(model: FitAdvisorModel): string {
		const job = downloadFor(model);
		if (job && isActiveDownloadStatus(job.status)) {
			const progress = typeof job.percent === 'number' && Number.isFinite(job.percent) ? ` ${Math.round(job.percent)}%` : '';
			if (job.status === 'queued') return 'Queued';
			if (job.status === 'resolving') return 'Resolving';
			return `Downloading${progress}`;
		}
		const status = downloadStatus(model);
		if (status === 'partial' || model.partial) return 'Resume DL';
		if (status === 'failed') return 'Retry DL';
		if (status === 'downloaded' || status === 'configured') return 'Downloaded';
		if (hasPendingDownload) return 'Queue DL';
		return 'Download';
	}

	function isDownloading(model: FitAdvisorModel): boolean {
		return isActiveDownloadStatus(downloadStatus(model));
	}

	function canStartDownload(model: FitAdvisorModel): boolean {
		if (!model.download) return false;
		return !['queued', 'resolving', 'downloading', 'downloaded', 'configured'].includes(downloadStatus(model));
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
	<header class="page-header">
		<div>
			<div class="header-kicker">
				<Gauge size={16} />
				calibr logic, native llama.cpp router
			</div>
			<h1>Caliber Advisor</h1>
			<p>
				Benchmark local GGUF configurations, compare speed and memory tradeoffs, then FIT the
				winning launch settings into the router.
			</p>
		</div>
		<div class="hero-actions">
			<button type="button" onclick={() => (activeTab = 'start')} class="primary">
				Start guided run
				<ChevronRight size={16} />
			</button>
			<button type="button" onclick={() => (activeTab = 'reports')}>
				<FileJson size={16} />
				View reports
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
							<p>{selectedLocalIds.length} selected · {pendingSelectedIds.length} need benchmarking · {selectedLocalIds.length - pendingSelectedIds.length} already archived.</p>
						</div>
						<div class="row-actions">
							<button type="button" onclick={selectRecommendedLocal}>Pick first 4</button>
							<button type="button" onclick={selectAllLocal}>All available</button>
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
						{#if sweepIsLive(status)}
							<button type="button" class="danger" onclick={stopSweep} disabled={status?.cancel_requested}>
								<Square size={16} />
								Stop benchmark
							</button>
						{/if}
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
						{#if sweepIsLive(status)}
							<button type="button" class="icon-action danger" onclick={stopSweep} disabled={status?.cancel_requested} aria-label="Stop benchmark">
								<Square size={16} />
							</button>
						{:else}
							<Activity size={18} />
						{/if}
					</div>
					<div class="job">
						<div class="progress">
							<span style={`width:${status?.total ? Math.min(100, ((status.current ?? 0) / status.total) * 100) : running ? 12 : 0}%`}></span>
						</div>
						<dl>
							<div><dt>Done</dt><dd>{status?.current ?? 0}</dd></div>
							<div><dt>Total</dt><dd>{status?.total ?? 0}</dd></div>
							<div><dt>Current</dt><dd>{status?.current_item ?? '-'}</dd></div>
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
							<button type="button" onclick={() => downloadModel(model)} disabled={!canStartDownload(model)}>
								<Download size={15} />
								{downloadActionLabel(model)}
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

	{#if activeTab === 'reports'}
		<section class="reports-layout">
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

			<div class="panel report-detail-panel">
				<div class="panel-head">
					<div>
						<h2>Report detail</h2>
						<p>{selectedReportId || 'Select a report'}</p>
					</div>
				</div>
				{#if selectedReport}
					<div class="detail report-summary-strip">
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
							<details class="launcher-box">
								<summary>Launcher for first measured row</summary>
								<pre>{planLauncher(reportRows[0])}</pre>
							</details>
						{/if}
						</div>

					{/if}
					{#if selectedReport || resultRows.length > 0}
						<div class="calibr-report">
							<div class="calibr-title">
								<div>
									<h2><span>calibr</span> benchmark report</h2>
									<p>{selectedReport ? `Selected ${String(selectedReport.created_at ?? selectedReportId)}` : 'Historical archive across completed runs'}</p>
								</div>
								<strong>{okAnalyticsRows.length}/{scopedAnalyticsRows.length || resultRows.length || reportRows.length} successful configs</strong>
							</div>

						<div class="hardware-strip">
							<strong>Hardware:</strong>
							<span>{fitSystem?.gpu_name ?? 'GPU scan pending'}</span>
							<span>{fitSystem ? `${fmtGb(fitSystem.total_gpu_vram_gb)} aggregate VRAM` : 'VRAM unavailable'}</span>
							<span>{fitSystem ? `${fitSystem.cpu_name} / ${fitSystem.cpu_cores} threads` : 'CPU unavailable'}</span>
							<span>llama-server: native router</span>
						</div>

						<div class="filter-bar">
							<span>Data scope:</span>
							<div class="segmented">
								<button type="button" class:active={reportScope === 'latest'} onclick={() => (reportScope = 'latest')}>Latest session</button>
								<button type="button" class:active={reportScope === 'all'} onclick={() => (reportScope = 'all')}>All sessions</button>
							</div>
							<em>{reportGroups.length} models, {okAnalyticsRows.length} configs in view</em>
						</div>

						<div class="filter-bar">
							<span>Winner criterion:</span>
							<div class="segmented">
								{#each Object.entries(profileLabels) as [id, item]}
									<button type="button" class:active={profile === id} onclick={() => (profile = id as ProfileId)}>{item.title}</button>
								{/each}
							</div>
							<em>{profileLabels[profile].help}</em>
						</div>

						{#if syntheticRows > 0}
							<div class="methodology-warning">
								<strong>Synthetic benchmark</strong>
								<span>{syntheticRows} row(s) measured with llama-bench. Eval speed uses the generation row; full streaming timeline requires the server-runner telemetry backend.</span>
							</div>
						{/if}

						<section class="report-section">
							<div class="section-heading">
								<h3>Memory vs latency</h3>
								<p>One dot per successful config. X is total prompt + generation time; Y is peak VRAM plus observed spill. Hover a point for model details.</p>
							</div>
							<div class="memory-latency-grid">
								<div class="chart-shell">
									<svg class="report-scatter" viewBox="0 0 980 360" role="img" aria-label="Memory versus latency">
										<line class="axis" x1="70" y1="312" x2="930" y2="312" />
										<line class="axis" x1="70" y1="54" x2="70" y2="312" />
										<line class="budget" x1="70" y1="180" x2="930" y2="180" />
										<text x="72" y="344">Total time, log scale</text>
										<text x="12" y="58">Memory used</text>
										<text x="780" y="174">GPU VRAM budget</text>
										{#each reportScatterRows.slice(0, 360) as row, index}
											<circle
												cx={70 + ((reportScatterX(row) - 58) / 662) * 860}
												cy={54 + ((reportScatterY(row) - 54) / 244) * 258}
												r={isReportCandidate(row) ? 5.8 : 4}
												class={`dot-${index % 5}`}
												class:candidate={isReportCandidate(row)}
											>
												<title>{rowText(row, ['model'], '-')} / {fmtNumber(rowNum(row, ['eval_tps', 'tps']), 1)} t/s / {fmtMib(reportMemoryMib(row))} / ctx {rowNum(row, ['ctx_size']) || '-'}</title>
											</circle>
										{/each}
									</svg>
								</div>

								<div class="metric-panel">
									<div class="analytics-cards">
										<div><span>Models</span><strong>{reportGroups.length}</strong></div>
										<div><span>Measured configs</span><strong>{okAnalyticsRows.length}</strong></div>
										<div><span>Winner rule</span><strong>{profileLabels[profile].title}</strong></div>
										<div><span>Metric</span><strong>{reportMetricLabel(reportMetric)}</strong></div>
									</div>
									<div class="leader-bars compact">
										{#each reportLeaderboard.slice(0, 8) as row}
											<div class="leader-row">
												<span>{rowText(row, ['model'], '-')}</span>
												<div><i style={`width:${reportBarWidth(row)}%`}></i></div>
												<strong>{reportMetricUnit(reportMetric, reportMetricValue(row, reportMetric))}</strong>
											</div>
										{/each}
									</div>
								</div>
							</div>
						</section>

						<section class="report-section">
							<div class="section-heading">
								<h3>Models (winners per current filter)</h3>
								<p>Each row shows the selected winner for one model. Expand it to inspect all measured configs.</p>
							</div>
							<div class="analytics-models">
								{#each reportGroups as group, index}
									<details class="analytics-model" open={index === 0}>
										<summary>
											<div class="summary-main">
												<span class={`rank-tag ${group.winner ? reportFitClass(group.winner) : 'low'}`}>{group.winner ? reportFitLabel(group.winner) : 'n/a'}</span>
												<strong>{group.model}</strong>
												{#if group.winner}
													<code>{rowText(group.winner, ['variant', 'quant', 'kv_cache'], rowText(group.winner, ['row_role'], 'candidate'))}</code>
												{/if}
											</div>
											{#if group.winner}
												<div class="summary-metrics">
													<span>{fmtNumber(rowNum(group.winner, ['eval_tps', 'tps']), 1)} t/s</span>
													<span>{fmtMib(reportMemoryMib(group.winner))}</span>
													<span>{rowNum(group.winner, ['ctx_size']) || '-'} ctx</span>
													<button type="button" onclick={() => configureCaliberRow(group.winner as CaliberRow)}>
														<CheckCircle2 size={14} />
														FIT winner
													</button>
												</div>
											{:else}
												<span>no winner-eligible rows</span>
											{/if}
										</summary>
										<div class="config-matrix">
											<div class="table-head">
												<span>Configuration</span>
												<span>Workload</span>
												<span>Ctx</span>
												<span>Prompt</span>
												<span>Eval</span>
												<span>Memory</span>
												<span>Fit</span>
											</div>
											{#each group.rows as row}
												<div class="table-row" class:winner={Boolean(group.winner) && rowIdentity(group.winner as CaliberRow) === rowIdentity(row)}>
													<span>{rowText(row, ['row_role'], '-')}</span>
													<span>{rowText(row, ['workload_kind'], '-')}</span>
													<span>{rowNum(row, ['ctx_size']) || '-'}</span>
													<span>{fmtNumber(rowNum(row, ['prompt_tps']), 1)} t/s</span>
													<span>{fmtNumber(rowNum(row, ['eval_tps', 'tps']), 1)} t/s</span>
													<span>{fmtMib(reportMemoryMib(row))}</span>
													<span>{rowText(row, ['decode_usability', 'residency', 'memory_state', 'fit_status'], '-')}</span>
												</div>
											{/each}
										</div>
									</details>
								{/each}
							</div>
						</section>

						<section class="report-section">
							<div class="section-heading">
								<h3>Throughput & memory</h3>
								<p>The selected metric changes the ordering and bar scale.</p>
							</div>
							<div class="segmented metric-tabs">
								{#each reportMetrics as metric}
									<button type="button" class:active={reportMetric === metric} onclick={() => (reportMetric = metric)}>{reportMetricLabel(metric)}</button>
								{/each}
							</div>
							<div class="throughput-bars">
								{#each reportLeaderboard as row}
									<div class="throughput-row">
										<span>
											<b class={`rank-tag ${reportFitClass(row)}`}>{reportFitLabel(row)}</b>
											{rowText(row, ['model'], '-')}
										</span>
										<div class="throughput-track"><i style={`width:${reportBarWidth(row)}%`}></i></div>
										<strong>{reportMetricUnit(reportMetric, reportMetricValue(row, reportMetric))}</strong>
									</div>
								{/each}
							</div>
						</section>
						</div>
					{:else}
						<p class="empty">No historical Caliber rows yet.</p>
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
		--caliber-accent: #8b5cf6;
		--caliber-active: #5145cd;
		--caliber-green: #22c55e;
		--caliber-yellow: #f59e0b;
		--caliber-red: #ef4444;
		display: flex;
		min-height: 100%;
		flex-direction: column;
		gap: 18px;
		padding: 20px 24px;
		color: var(--foreground);
		background: var(--background);
	}

	.answer-strip,
	.controls,
	.panel,
	.tabs {
		border: 1px solid var(--border);
		border-radius: var(--radius);
		background: var(--card);
	}

	.page-header {
		display: grid;
		grid-template-columns: minmax(0, 1fr) auto;
		gap: 18px;
		align-items: end;
		border-bottom: 1px solid var(--border);
		padding-bottom: 16px;
	}

	h1,
	h2,
	h3,
	p {
		margin: 0;
	}

	h1 {
		max-width: 760px;
		margin-top: 4px;
		font-size: 24px;
		font-weight: 600;
		line-height: 1.2;
	}

	.page-header p {
		margin-top: 4px;
		max-width: 760px;
		font-size: 14px;
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
		color: var(--muted-foreground);
	}

	.header-kicker {
		display: flex;
		align-items: center;
		gap: 8px;
		color: var(--muted-foreground);
		font-size: 14px;
	}

	.hero-actions,
	.row-actions,
	.button-row,
	.controls,
	.tabs {
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
		background: color-mix(in oklch, var(--muted) 35%, transparent);
	}

	.answer-strip strong {
		font-size: 18px;
	}

	button,
	input,
	select {
		min-height: 38px;
		border: 1px solid var(--border);
		border-radius: var(--radius);
		background: var(--background);
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

	button.primary {
		background: var(--primary);
		color: var(--primary-foreground);
	}

	button.danger {
		border-color: color-mix(in oklch, var(--destructive) 55%, var(--border));
		background: color-mix(in oklch, var(--destructive) 14%, var(--background));
		color: var(--destructive);
	}

	button.icon-action {
		width: 38px;
		padding: 0;
	}

	button.active,
	.choice.active,
	.model-option.active {
		border-color: var(--caliber-accent);
		background: var(--caliber-active);
		color: #fff;
	}

	.choice.active span,
	.model-option.active span,
	button.active span {
		color: rgba(255, 255, 255, 0.86);
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
	.controls {
		align-items: center;
		padding: 8px;
	}

	.wizard-grid,
	.grid {
		display: grid;
		grid-template-columns: minmax(0, 1fr) minmax(360px, 0.65fr);
		gap: 16px;
	}

	.panel {
		overflow: hidden;
	}

	.panel-head {
		display: flex;
		align-items: center;
		justify-content: space-between;
		gap: 12px;
		border-bottom: 1px solid var(--border);
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
		border: 1px solid var(--border);
		border-radius: var(--radius);
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
		border: 1px solid var(--border);
		border-radius: var(--radius);
		padding: 10px;
	}

	dt {
		font-size: 12px;
	}

	dd {
		margin: 0;
		color: var(--foreground);
		font-weight: 700;
	}

	.note {
		border-left: 3px solid var(--primary);
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
		border: 1px solid var(--border);
		border-radius: var(--radius);
		padding: 12px;
	}

	.workflow-step > span {
		display: inline-flex;
		width: 24px;
		height: 24px;
		align-items: center;
		justify-content: center;
		border-radius: var(--radius);
		background: var(--muted);
		color: var(--foreground);
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
		border-bottom: 1px solid var(--border);
		padding: 10px 12px;
	}

	.table-head {
		position: sticky;
		top: 0;
		z-index: 1;
		background: var(--muted);
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

	.reports-table .table-head,
	.reports-table .table-row {
		grid-template-columns: 90px 60px minmax(220px, 1fr) 180px 48px;
	}

	.table-row.active {
		background: color-mix(in oklch, var(--muted) 60%, transparent);
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
		border: 1px solid var(--border);
		border-radius: var(--radius);
		background: var(--background);
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
	.mini-progress {
		width: 100%;
		overflow: hidden;
		border-radius: var(--radius);
		background: var(--muted);
	}

	.progress {
		height: 10px;
	}

	.mini-progress {
		height: 4px;
	}

	.progress span,
	.mini-progress span {
		display: block;
		height: 100%;
		background: var(--primary);
	}

	.event-log {
		display: grid;
		gap: 6px;
		max-height: 300px;
		overflow: auto;
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
		color: var(--foreground);
	}

	pre {
		max-height: 220px;
		margin: 0;
		overflow: auto;
		border: 1px solid var(--border);
		border-radius: var(--radius);
		background: var(--muted);
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

	.reports-layout {
		display: grid;
		grid-template-columns: 1fr;
		gap: 16px;
	}

	.report-detail-panel {
		overflow: visible;
	}

	.report-summary-strip {
		border-bottom: 1px solid var(--border);
	}

	.launcher-box summary {
		cursor: pointer;
		color: var(--foreground);
		font-weight: 700;
	}

	.calibr-report {
		display: grid;
		gap: 16px;
		padding: 16px;
		background: #1e1e1e;
		color: #e5e7eb;
	}

	.calibr-report p,
	.calibr-report span,
	.calibr-report em,
	.calibr-report code {
		color: #cbd5e1;
	}

	.calibr-title {
		display: flex;
		align-items: end;
		justify-content: space-between;
		gap: 16px;
	}

	.calibr-title h2 {
		color: #f8fafc;
		font-size: 22px;
	}

	.calibr-title h2 span {
		color: var(--caliber-accent);
	}

	.calibr-title strong {
		color: #c4b5fd;
	}

	.hardware-strip,
	.filter-bar,
	.report-section {
		border: 1px solid #4f4f4f;
		border-radius: 6px;
		background: #292929;
	}

	.hardware-strip,
	.filter-bar {
		display: flex;
		flex-wrap: wrap;
		gap: 10px;
		align-items: center;
		padding: 10px 12px;
	}

	.hardware-strip strong,
	.filter-bar > span {
		color: #f8fafc;
	}

	.filter-bar em {
		margin-left: auto;
		font-style: italic;
	}

	.segmented {
		display: flex;
		flex-wrap: wrap;
		gap: 6px;
		align-items: center;
		border: 1px solid #4f4f4f;
		border-radius: 6px;
		background: #111;
		padding: 4px;
	}

	.segmented button {
		min-height: 30px;
		border: 1px solid transparent;
		background: #181818;
		color: #e5e7eb;
	}

	.segmented button.active {
		border-color: var(--caliber-accent);
		background: #4c1d95;
		color: #fff;
	}

	.methodology-warning {
		display: grid;
		gap: 4px;
		border: 1px solid rgba(251, 191, 36, 0.45);
		border-radius: 6px;
		background: rgba(251, 191, 36, 0.1);
		padding: 10px;
	}

	.methodology-warning strong {
		color: #fde68a;
	}

	.report-section {
		display: grid;
		gap: 12px;
		padding: 14px;
	}

	.section-heading {
		display: grid;
		gap: 6px;
		border-bottom: 1px solid #4f4f4f;
		padding-bottom: 10px;
	}

	.section-heading h3 {
		margin: 0;
		color: #f8fafc;
		font-size: 18px;
	}

	.memory-latency-grid {
		display: grid;
		grid-template-columns: minmax(520px, 1.55fr) minmax(320px, 0.75fr);
		gap: 16px;
	}

	.chart-shell {
		min-height: 420px;
		overflow: hidden;
		background: #242424;
	}

	.report-scatter {
		width: 100%;
		min-height: 420px;
	}

	.report-scatter .axis {
		stroke: #6b7280;
	}

	.report-scatter .budget {
		stroke: #ef4444;
		stroke-dasharray: 4 4;
		opacity: 0.8;
	}

	.report-scatter circle {
		opacity: 0.78;
		stroke: transparent;
		stroke-width: 2;
	}

	.report-scatter circle.candidate {
		stroke: #a78bfa;
	}

	.report-scatter .dot-0 {
		fill: #22c55e;
	}

	.report-scatter .dot-1 {
		fill: #06b6d4;
	}

	.report-scatter .dot-2 {
		fill: #8b5cf6;
	}

	.report-scatter .dot-3 {
		fill: #f59e0b;
	}

	.report-scatter .dot-4 {
		fill: #ef4444;
	}

	.report-scatter text {
		fill: #cbd5e1;
		font-size: 12px;
	}

	.metric-panel,
	.analytics-cards,
	.analytics-models,
	.leader-bars,
	.throughput-bars {
		display: grid;
		gap: 8px;
	}

	.analytics-cards {
		grid-template-columns: repeat(2, minmax(0, 1fr));
	}

	.analytics-cards > div {
		display: grid;
		gap: 5px;
		border: 1px solid #4f4f4f;
		border-radius: 6px;
		background: #1f1f1f;
		padding: 10px;
	}

	.analytics-cards strong {
		color: #f8fafc;
		font-size: 18px;
	}

	.leader-row,
	.throughput-row {
		display: grid;
		align-items: center;
		gap: 10px;
	}

	.leader-row {
		grid-template-columns: minmax(180px, 1fr) minmax(150px, 0.9fr) 105px;
	}

	.throughput-row {
		grid-template-columns: minmax(260px, 0.9fr) minmax(280px, 1fr) 110px;
	}

	.leader-row > span,
	.throughput-row > span {
		overflow: hidden;
		text-overflow: ellipsis;
		white-space: nowrap;
	}

	.leader-row div,
	.throughput-track {
		height: 14px;
		overflow: hidden;
		border: 1px solid #4f4f4f;
		border-radius: 4px;
		background: #111;
	}

	.leader-row i,
	.throughput-track i {
		display: block;
		height: 100%;
		background: linear-gradient(90deg, #ef4444, #f59e0b 18%, #22c55e 42%, #14532d);
	}

	.leader-row strong,
	.throughput-row strong {
		text-align: right;
		color: #f8fafc;
		font-size: 12px;
	}

	.analytics-model {
		border: 1px solid #4f4f4f;
		border-radius: 6px;
		background: #262626;
	}

	.analytics-model[open] {
		border-color: var(--caliber-accent);
		box-shadow: inset 0 0 0 1px rgba(139, 92, 246, 0.24);
	}

	.analytics-model summary {
		display: grid;
		grid-template-columns: minmax(320px, 1fr) minmax(360px, 0.95fr);
		gap: 12px;
		align-items: center;
		padding: 10px 12px;
		cursor: pointer;
	}

	.summary-main,
	.summary-metrics {
		display: flex;
		flex-wrap: wrap;
		gap: 8px;
		align-items: center;
		min-width: 0;
	}

	.summary-main strong {
		color: #f8fafc;
	}

	.summary-main code,
	.summary-metrics span {
		color: #cbd5e1;
		font-size: 12px;
	}

	.summary-metrics {
		justify-content: flex-end;
	}

	.rank-tag {
		display: inline-flex;
		border: 1px solid currentColor;
		border-radius: 4px;
		padding: 2px 6px;
		font-size: 11px;
		font-weight: 800;
		line-height: 1.2;
	}

	.rank-tag.low {
		color: #38bdf8;
	}

	.rank-tag.middle {
		color: #22c55e;
	}

	.rank-tag.high {
		color: #f59e0b;
	}

	.rank-tag.ultra {
		color: #ef4444;
	}

	.config-matrix .table-head,
	.config-matrix .table-row {
		grid-template-columns: minmax(160px, 1fr) 110px 88px 105px 105px 110px minmax(140px, 0.7fr);
	}

	.config-matrix .table-row.winner {
		background: rgba(139, 92, 246, 0.22);
	}

	.metric-tabs {
		width: fit-content;
	}

	@media (max-width: 1100px) {
		.page-header,
			.answer-strip,
			.wizard-grid,
			.grid,
			.memory-latency-grid,
			.analytics-cards,
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
			.reports-table .table-row,
			.config-matrix .table-row,
			.leader-row,
			.throughput-row,
			.analytics-model summary {
				grid-template-columns: 1fr;
			}
		}
</style>
