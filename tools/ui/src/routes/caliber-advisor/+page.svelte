<script lang="ts">
	import { onDestroy, onMount } from 'svelte';
	import { SvelteMap } from 'svelte/reactivity';
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
		Route,
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
	import { RouterService, type LocalRouteEvent } from '$lib/services/router.service';
	import { compactModelName, normalizeModelName } from '$lib/utils/model-display';

	type TabId = 'library' | 'test-lab' | 'recommendations' | 'router' | 'history' | 'doctor';
	type UseCaseId = 'general' | 'chat' | 'coding' | 'reasoning' | 'rag' | 'tools' | 'long-context';
	type ProfileId = 'overall' | 'speed' | 'efficiency' | 'safety';
	type RunScope = 'quick' | 'standard' | 'deep';
	type ReportScope = 'latest' | 'all';
	type ReportMetric = 'eval' | 'prompt' | 'memory' | 'latency' | 'vram';
	type CaliberRow = Record<string, unknown>;
	type ReportModelGroup = { model: string; rows: CaliberRow[]; winner: CaliberRow | null };
	type RecommendationDecision = Record<string, unknown>;

	const contextOptions = [
		{ label: '8k', value: 8192, hint: 'Short chat and smoke tests' },
		{ label: '32k', value: 32768, hint: 'Most coding and analysis sessions' },
		{ label: '64k', value: 65536, hint: 'Large files and longer conversations' },
		{ label: '131k', value: 131072, hint: 'Long-context models and serious repo work' },
		{ label: '262k', value: 262144, hint: 'Only when the model really supports it' }
	];
	const tabs: { id: TabId; label: string }[] = [
		{ id: 'library', label: 'Library' },
		{ id: 'test-lab', label: 'Test Lab' },
		{ id: 'recommendations', label: 'Recommendations' },
		{ id: 'router', label: 'Router' },
		{ id: 'history', label: 'History' },
		{ id: 'doctor', label: 'Doctor' }
	];
	const useCases: { id: UseCaseId; label: string; help: string }[] = [
		{
			id: 'general',
			label: 'Everyday assistant',
			help: 'Writing, questions and mixed daily work.'
		},
		{ id: 'chat', label: 'Fast chat', help: 'Responsive conversation and instruction following.' },
		{ id: 'coding', label: 'Coding', help: 'Repository work, debugging and code generation.' },
		{ id: 'reasoning', label: 'Deep reasoning', help: 'Math, science and difficult decisions.' },
		{ id: 'rag', label: 'Documents / RAG', help: 'Grounded answers over your own documents.' },
		{ id: 'tools', label: 'Agents & tools', help: 'Reliable structured output and tool calls.' },
		{
			id: 'long-context',
			label: 'Long context',
			help: 'Large files, long chats and needle retrieval.'
		}
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
	const scopeOptions: Record<
		RunScope,
		{ title: string; help: string; workload: 'baseline' | 'all' }
	> = {
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
		[
			'Plan',
			'Expand safe configs: vanilla control, context, KV cache, GPU split and MoE/offload candidates.'
		],
		['Benchmark', 'Run measured configs server-side, even if this browser tab is closed.'],
		['Decide', 'Rank winners, explain tradeoffs, save launch/FIT settings.']
	];
	const strategies = [
		{ id: 'hybrid_offload', label: 'Hybrid' },
		{ id: 'multi_gpu', label: 'MultiGPU' },
		{ id: 'moe_offload', label: 'MoE offload' },
		{ id: 'balanced', label: 'Balanced' }
	];
	const aliasCards = [
		{
			id: 'local-auto',
			description: 'Balanced measured default',
			tags: ['balanced', 'overall quality']
		},
		{
			id: 'local-fast',
			description: 'Lowest interactive latency',
			tags: ['latency', 'TTFT-aware']
		},
		{
			id: 'local-best',
			description: 'Highest qualified quality',
			tags: ['quality-first', 'quality-gated']
		},
		{ id: 'local-code', description: 'Coding and FIM qualified', tags: ['coding', 'FIM', 'tools'] },
		{
			id: 'local-long',
			description: 'Long-context retrieval qualified',
			tags: ['long context', 'retrieval']
		},
		{ id: 'local-vision', description: 'Vision-capable artifact', tags: ['vision', 'multimodal'] }
	];
	const reportMetrics: ReportMetric[] = ['eval', 'prompt', 'memory', 'latency', 'vram'];
	const technicalColumns = [
		'model',
		'variant',
		'row_role',
		'workload_kind',
		'benchmark_backend',
		'evidence_level',
		'quality_evidence_level',
		'ctx_size',
		'prompt_tps',
		'eval_tps',
		'e2e_ttft_ms',
		'itl_p95_ms',
		'vram_peak_mib',
		'process_working_set_peak_mib',
		'gpu_power_peak_w',
		'measurement_confidence',
		'fit_eligible'
	];

	let activeTab = $state<TabId>('library');
	let profile = $state<ProfileId>('overall');
	let useCase = $state<UseCaseId>('general');
	let installedOnly = $state(true);
	let allowedTestMinutes = $state(20);
	let runScope = $state<RunScope>('quick');
	let contextSize = $state(32768);
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
	let routeEvents = $state<LocalRouteEvent[]>([]);
	let doctorSystem = $state<Record<string, unknown> | null>(null);

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

	const resultRows = $derived(
		asRows(results?.rows).filter((row) => rowNum(row, ['eval_tps', 'tps']) > 0)
	);
	const reportRows = $derived(asRows(selectedReport?.rows));
	const reportPlan = $derived(asRows(selectedReport?.plan));
	const analyticsRows = $derived(selectedReport ? reportRows : resultRows);
	const scopedAnalyticsRows = $derived(
		selectedReport ? analyticsRows : filterReportScope(analyticsRows, reportScope)
	);
	const okAnalyticsRows = $derived(
		scopedAnalyticsRows.filter(
			(row) => rowText(row, ['ok']) !== 'false' && rowNum(row, ['eval_tps', 'tps']) > 0
		)
	);
	const recommendationSource = $derived(
		selectedReport ?? recommendationScope(results, reportScope)
	);
	const activeDecision = $derived(profileDecision(recommendationSource, profile));
	const reportGroups = $derived(buildReportGroups(scopedAnalyticsRows, activeDecision));
	const reportLeaderboard = $derived(
		rankRowsByMetric(reportGroups.map((group) => group.winner).filter(Boolean) as CaliberRow[])
	);
	const reportMetricMax = $derived(
		Math.max(1, ...reportLeaderboard.map((row) => reportMetricValue(row, reportMetric)))
	);
	const reportScatterRows = $derived(okAnalyticsRows.filter((row) => reportTimeSec(row) > 0));
	const loadCurveRows = $derived(buildLoadCurveRows(scopedAnalyticsRows));
	const reportMaxTime = $derived(Math.max(1, ...reportScatterRows.map(reportTimeSec)));
	const reportMaxMemory = $derived(
		Math.max(
			1,
			...reportScatterRows.map((row) => reportMemoryMib(row)),
			(fitSystem?.total_gpu_vram_gb ?? 0) * 1024
		)
	);
	const scatterTickRatios = [0, 0.25, 0.5, 0.75, 1];
	const reportVramBudgetMib = $derived((fitSystem?.total_gpu_vram_gb ?? 0) * 1024);
	const syntheticRows = $derived(
		scopedAnalyticsRows.filter((row) => rowText(row, ['benchmark_backend']) === 'llama-bench')
			.length
	);
	const bestWinner = $derived((asRecord(activeDecision?.winner) as CaliberRow | null) ?? null);
	const bestAlternatives = $derived(asRows(activeDecision?.alternatives).slice(0, 3));
	const bestTimeline = $derived(timelineSamples(bestWinner));
	const completedReports = $derived(
		reports.filter((report) => report.rows > 0 && isCompleteStatus(report.status))
	);
	const hasPendingDownload = $derived(downloads.some((job) => isActiveDownloadStatus(job.status)));
	const pendingSelectedIds = $derived(selectedLocalIds.filter((id) => !hasHistoricModelResult(id)));
	const selectableModels = $derived(
		models.filter((model) => model.loadable !== false && Boolean(model.path))
	);
	const planModels = $derived(uniqueStrings(plan.map((item) => item.model)).length);
	const targetContext = $derived(contextOptions.find((item) => item.value === contextSize));
	const readyToRun = $derived(pendingSelectedIds.length > 0 && !running);
	const nextAction = $derived(nextActionText());
	const doctorData = $derived(asRecord(doctorSystem?.doctor));

	onMount(() => {
		void refreshAll();
		void loadCatalog();
		void refreshDownloads();
		void restoreActiveSweep();
		void refreshRouteEvents();
		startDownloadStream();
	});

	onDestroy(() => {
		downloadAbort?.abort();
		sweepAbort?.abort();
	});

	function asRows(value: unknown): CaliberRow[] {
		return Array.isArray(value)
			? (value.filter((row) => row && typeof row === 'object') as CaliberRow[])
			: [];
	}

	function asRecord(value: unknown): Record<string, unknown> | null {
		return value && typeof value === 'object' && !Array.isArray(value)
			? (value as Record<string, unknown>)
			: null;
	}

	function recommendationScope(
		source: Record<string, unknown> | null,
		scope: ReportScope
	): Record<string, unknown> | null {
		if (!source || scope === 'all') return source;
		const scopes = asRecord(source.scopes);
		return asRecord(scopes?.latest_campaign) ?? source;
	}

	function profileDecision(
		source: Record<string, unknown> | null,
		selectedProfile: ProfileId
	): RecommendationDecision | null {
		const recommendations = asRecord(source?.recommendations);
		return asRecord(recommendations?.[selectedProfile]);
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

	function rowBool(row: CaliberRow, key: string, fallback = false): boolean {
		const value = row[key];
		if (typeof value === 'boolean') return value;
		if (value === 'true') return true;
		if (value === 'false') return false;
		return fallback;
	}

	function canFitCaliberRow(row: CaliberRow): boolean {
		return (
			rowBool(row, 'fit_eligible', false) &&
			rowText(row, ['ok']) !== 'false' &&
			(!rowBool(row, 'quality_gate_required', false) || rowBool(row, 'quality_gate_passed', false))
		);
	}

	function timelineSamples(row: CaliberRow | null): number[][] {
		const timeline = asRecord(row?.timeline);
		if (!timeline || timeline.encoding !== 'delta-columns-v1' || !Array.isArray(timeline.rows))
			return [];
		let elapsed = 0;
		return (timeline.rows as unknown[]).filter(Array.isArray).map((raw) => {
			const values = raw as unknown[];
			elapsed += Number(values[0] ?? 0);
			return [elapsed, Number(values[1] ?? 0), Number(values[4] ?? 0), Number(values[5] ?? 0)];
		});
	}

	function timelineX(sample: number[]): number {
		const max = Math.max(1, bestTimeline.at(-1)?.[0] ?? 1);
		return 44 + (sample[0] / max) * 676;
	}

	function timelineY(sample: number[], index: number): number {
		const max = Math.max(1, ...bestTimeline.map((item) => item[index]));
		return 250 - (sample[index] / max) * 190;
	}

	function qualityScore(row: CaliberRow | null): number {
		const evidence = asRecord(row?.quality_evidence);
		return Number(evidence?.score ?? 0);
	}

	function radarPoints(row: CaliberRow): string {
		const maxima = {
			eval: Math.max(1, ...okAnalyticsRows.map((item) => rowNum(item, ['eval_tps', 'tps']))),
			prompt: Math.max(1, ...okAnalyticsRows.map((item) => rowNum(item, ['prompt_tps']))),
			ctx: Math.max(1, ...okAnalyticsRows.map((item) => rowNum(item, ['ctx_size']))),
			memory: Math.max(1, ...okAnalyticsRows.map(reportMemoryMib))
		};
		const values = [
			rowNum(row, ['eval_tps', 'tps']) / maxima.eval,
			rowNum(row, ['prompt_tps']) / maxima.prompt,
			rowNum(row, ['ctx_size']) / maxima.ctx,
			1 - Math.min(1, reportMemoryMib(row) / maxima.memory),
			qualityScore(row)
		];
		return values
			.map((value, index) => {
				const angle = -Math.PI / 2 + index * ((Math.PI * 2) / 5);
				const radius = 78 * Math.max(0.08, Math.min(1, value));
				return `${110 + Math.cos(angle) * radius},${105 + Math.sin(angle) * radius}`;
			})
			.join(' ');
	}

	function matchedVanilla(row: CaliberRow): CaliberRow | null {
		return (
			scopedAnalyticsRows.find(
				(item) =>
					rowText(item, ['model']) === rowText(row, ['model']) &&
					rowText(item, ['control_kind']) === 'vanilla' &&
					rowNum(item, ['ctx_size']) === rowNum(row, ['ctx_size'])
			) ?? null
		);
	}

	function loadTarget(row: CaliberRow): number {
		return rowText(row, ['workload_kind']) === 'kv-fill'
			? rowNum(row, [
					'kv_fill_measured_tokens',
					'kv_fill_target_tokens',
					'benchmark_depth_tokens',
					'prompt_n'
				])
			: rowNum(row, ['prefill_target_tokens', 'benchmark_prompt_tokens', 'prompt_n']);
	}

	function loadCurveWidth(row: CaliberRow): number {
		const model = rowText(row, ['model', 'model_id']);
		const workload = rowText(row, ['workload_kind']);
		const peers = loadCurveRows.filter(
			(item) =>
				rowText(item, ['model', 'model_id']) === model &&
				rowText(item, ['workload_kind']) === workload
		);
		const max = Math.max(1, ...peers.map(loadCurveSpeed));
		return Math.max(2, Math.min(100, (loadCurveSpeed(row) / max) * 100));
	}

	function loadCurveSpeed(row: CaliberRow): number {
		return rowNum(row, ['prompt_tps', 'eval_tps']);
	}

	function loadCurveBand(row: CaliberRow): string {
		const retention = loadCurveWidth(row);
		if (retention >= 80) return 'healthy';
		if (retention >= 55) return 'degraded';
		return 'collapse';
	}

	function medianValues(values: number[]): number {
		const finite = values.filter(Number.isFinite).sort((a, b) => a - b);
		if (finite.length === 0) return 0;
		const middle = Math.floor(finite.length / 2);
		return finite.length % 2 ? finite[middle] : (finite[middle - 1] + finite[middle]) / 2;
	}

	function buildLoadCurveRows(rows: CaliberRow[]): CaliberRow[] {
		const groups = new SvelteMap<string, CaliberRow[]>();
		for (const row of rows) {
			const workload = rowText(row, ['workload_kind']);
			if (!['prefill', 'kv-fill'].includes(workload) || rowText(row, ['ok']) === 'false') continue;
			const target = loadTarget(row);
			if (target <= 0) continue;
			const key = `${rowText(row, ['model', 'model_id'])}|${workload}|${target}`;
			groups.set(key, [...(groups.get(key) ?? []), row]);
		}
		return [...groups.values()]
			.map((samples) => ({
				...samples[0],
				prompt_tps: medianValues(samples.map((row) => rowNum(row, ['prompt_tps', 'eval_tps']))),
				eval_tps: medianValues(samples.map((row) => rowNum(row, ['eval_tps']))),
				curve_samples: samples.length
			}))
			.sort((a, b) => {
				const modelOrder = modelDisplayName(rowText(a, ['model', 'model_id'])).localeCompare(
					modelDisplayName(rowText(b, ['model', 'model_id']))
				);
				if (modelOrder !== 0) return modelOrder;
				const workloadOrder = rowText(a, ['workload_kind']).localeCompare(
					rowText(b, ['workload_kind'])
				);
				return workloadOrder !== 0 ? workloadOrder : loadTarget(a) - loadTarget(b);
			});
	}

	function fmtTokens(value: number): string {
		if (value < 1024) return `${Math.round(value)} tokens`;
		const thousands = value / 1024;
		return `${fmtNumber(thousands, thousands >= 10 ? 0 : 1)}k`;
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
		return (
			(role === 'candidate' || (!role && workload === 'baseline')) &&
			rowText(row, ['ok']) !== 'false'
		);
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

	function buildReportGroups(
		rows: CaliberRow[],
		decision: RecommendationDecision | null
	): ReportModelGroup[] {
		const map = new SvelteMap<string, CaliberRow[]>();
		for (const row of rows) {
			const model = rowText(row, ['model', 'model_id'], 'unknown model');
			map.set(model, [...(map.get(model) ?? []), row]);
		}
		const bestByModel = asRecord(decision?.best_by_model) ?? {};
		const ordered = [decision?.winner, ...asRows(decision?.alternatives)]
			.map((row) => asRecord(row))
			.filter(Boolean)
			.map((row) => rowText(row as CaliberRow, ['model', 'model_id']));
		return [...map.entries()]
			.map(([model, groupRows]) => {
				const fallback = [...groupRows]
					.filter(
						(row) =>
							rowText(row, ['ok']) !== 'false' &&
							['candidate', ''].includes(rowText(row, ['row_role'])) &&
							rowText(row, ['workload_kind'], 'baseline') === 'baseline'
					)
					.sort((a, b) => rowNum(b, ['eval_tps', 'tps']) - rowNum(a, ['eval_tps', 'tps']))[0];
				return {
					model,
					rows: groupRows,
					winner: (asRecord(bestByModel[model]) as CaliberRow | null) ?? fallback ?? null
				};
			})
			.sort((a, b) => {
				const ai = ordered.indexOf(a.model);
				const bi = ordered.indexOf(b.model);
				if (ai === -1 && bi === -1) return a.model.localeCompare(b.model);
				if (ai === -1) return 1;
				if (bi === -1) return -1;
				return ai - bi;
			});
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
		const vram = fitSystem
			? fitSystem.total_gpu_vram_gb * 1024
			: rowNum(row, ['vram_budget_mib', 'gpu_vram_mib'], 0);
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

	function registryModel(value: string): CaliberModel | undefined {
		const identity = normalizeIdentity(value);
		if (!identity) return undefined;
		return models.find((model) =>
			[model.id, model.name, ...(model.configured_ids ?? []), ...(model.aliases ?? [])]
				.filter(Boolean)
				.some((candidate) => {
					const normalized = normalizeIdentity(candidate);
					return (
						normalized === identity ||
						normalized.includes(identity) ||
						identity.includes(normalized)
					);
				})
		);
	}

	function modelFullName(value: string): string {
		const matched = registryModel(value);
		const source = matched?.name || value || 'Unknown model';
		return normalizeModelName(source);
	}

	function modelDisplayName(value: string): string {
		return compactModelName(modelFullName(value));
	}

	function modelTags(model: CaliberModel): string[] {
		const metadata = asRecord(model.plan_meta);
		return uniqueStrings([
			...(model.tags ?? []),
			rowText(metadata ?? {}, ['variant']),
			rowText(metadata ?? {}, ['gguf_architecture']),
			model.configured ? 'configured' : '',
			...(model.aliases ?? []).slice(0, 2).map((alias) => `alias: ${alias}`)
		]).slice(0, 7);
	}

	function reportRowTags(row: CaliberRow): string[] {
		const quality = asRecord(row.quality_evidence);
		return uniqueStrings([
			rowText(row, ['variant']),
			rowNum(row, ['ctx_size']) > 0 ? `${fmtTokens(rowNum(row, ['ctx_size']))} ctx` : '',
			rowText(row, ['evidence_level']),
			rowBool(row, 'quality_gate_passed')
				? `quality ${Math.round(Number(quality?.score ?? 0) * 100)}%`
				: '',
			rowText(row, ['residency', 'memory_state'])
		]).slice(0, 6);
	}

	function fitBlockReason(row: CaliberRow): string {
		if (canFitCaliberRow(row)) return '';
		if (rowText(row, ['benchmark_backend']) !== 'llama-server-streaming')
			return 'Needs a streaming Test Lab run; synthetic llama-bench rows cannot become presets.';
		if (!rowBool(row, 'context_target_met')) return 'The requested context was not proved.';
		if (rowBool(row, 'quality_gate_required') && !rowBool(row, 'quality_gate_passed'))
			return 'The required quality pack has not passed.';
		if (!rowBool(row, 'fit_eligible'))
			return 'Memory or measurement confidence is not decision-grade.';
		return 'This row is not eligible for a production preset.';
	}

	function selectRowForTest(row: CaliberRow) {
		const identity = rowText(row, ['model', 'model_id']);
		const match = registryModel(identity);
		if (match && !selectedLocalIds.includes(match.id))
			selectedLocalIds = [...selectedLocalIds, match.id];
		activeTab = 'test-lab';
		message = match
			? `${modelDisplayName(identity)} selected. Review scope and start a Decision run to unlock FIT.`
			: `Open Test Lab and select ${modelDisplayName(identity)} for a streaming Decision run.`;
	}

	function aliasDecision(alias: string): Record<string, unknown> | null {
		for (const event of routeEvents) {
			if (event.event_type !== 'decision') continue;
			if (String(event.payload.alias ?? '') === alias && event.payload.ok !== false)
				return event.payload;
		}
		return null;
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
				rowIdentities.some(
					(rowIdentity) =>
						rowIdentity === identity ||
						rowIdentity.includes(identity) ||
						identity.includes(rowIdentity)
				)
			);
		});
	}

	function reportScatterX(row: CaliberRow): number {
		const ratio = Math.log10(reportTimeSec(row) + 1) / Math.log10(reportMaxTime + 1);
		return 70 + Math.max(0, Math.min(1, ratio)) * 860;
	}

	function reportScatterY(row: CaliberRow): number {
		const ratio = reportMemoryMib(row) / reportMaxMemory;
		return 312 - Math.max(0, Math.min(1, ratio)) * 258;
	}

	function scatterTimeTick(ratio: number): number {
		return Math.expm1(Math.log1p(reportMaxTime) * ratio);
	}

	function scatterMemoryTick(ratio: number): number {
		return reportMaxMemory * ratio;
	}

	function reportBudgetY(): number {
		if (reportVramBudgetMib <= 0) return 312;
		return 312 - Math.min(1, reportVramBudgetMib / reportMaxMemory) * 258;
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
		const memoryBudget = Math.max(
			1,
			((fitSystem?.total_gpu_vram_gb ?? 0) + (fitSystem?.available_ram_gb ?? 0) * 0.7) * 1024
		);
		selectedLocalIds = [...selectableModels]
			.filter(
				(model) => rowNum((model.plan_meta ?? {}) as CaliberRow, ['size_mib'], 0) <= memoryBudget
			)
			.sort(
				(a, b) =>
					rowNum((b.plan_meta ?? {}) as CaliberRow, ['size_mib']) -
					rowNum((a.plan_meta ?? {}) as CaliberRow, ['size_mib'])
			)
			.slice(0, allowedTestMinutes <= 10 ? 2 : allowedTestMinutes <= 30 ? 4 : 8)
			.map((model) => model.id);
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
		if (status?.cancel_requested)
			return 'Stop requested. Caliber will exit after the current benchmark config returns.';
		if (running)
			return 'Benchmark is running on the server. You can close this page and come back to Reports.';
		if (selectedLocalIds.length === 0)
			return 'Choose at least one installed model, or download/configure a catalog model first.';
		if (pendingSelectedIds.length === 0)
			return 'All selected models already have completed historical measurements. Open Reports to compare them without rerunning.';
		if (plan.length === 0)
			return 'Review the benchmark plan so you know how many configs will run.';
		if (completedReports.length === 0)
			return 'Start the benchmark. The report will appear automatically when it finishes.';
		return 'Open Reports to compare historical winners and choose the model/config to FIT.';
	}

	function payload(): Record<string, unknown> {
		return {
			models: pendingSelectedIds,
			opts: { workloadSweep: scopeOptions[runScope].workload },
			cfg: {
				quality: {
					required: true,
					pack: useCase === 'long-context' ? 'long-context' : useCase,
					min_score: 0.5,
					min_samples: 1
				},
				product_intent: {
					use_case: useCase,
					installed_only: installedOnly,
					allowed_test_minutes: allowedTestMinutes,
					objective: profile
				},
				hardware: fitSystem
					? {
							backend: fitSystem.backend,
							unified_memory: fitSystem.unified_memory ?? false,
							vram_budget_mib: Math.round(fitSystem.total_gpu_vram_gb * 1024),
							vram_driver_usable_mib: Math.round(fitSystem.total_gpu_vram_gb * 1024),
							system_ram_available_mib: Math.round(fitSystem.available_ram_gb * 1024),
							cpu_threads_logical: fitSystem.cpu_cores,
							cpu_cores_physical: Math.max(1, Math.floor(fitSystem.cpu_cores / 2)),
							gpus: fitSystem.gpus.map((gpu) => ({
								name: gpu.name,
								backend: gpu.backend,
								vram_total_mib: Math.round(gpu.vram_gb * 1024),
								vram_driver_usable_mib: Math.round(gpu.vram_gb * 1024)
							}))
						}
					: {},
				context_candidates: [{ ctx: contextSize, kv: 'q8_0' }],
				max_context_cap: contextSize
			}
		};
	}

	function pushEvent(line: string) {
		eventLog = [`${new Date().toLocaleTimeString()} ${line}`, ...eventLog].slice(0, 80);
	}

	async function refreshRouteEvents() {
		try {
			routeEvents = (await RouterService.localRouteEvents()).data;
		} catch {
			routeEvents = [];
		}
	}

	function sweepIsLive(snapshot: CaliberSweepStatus | null): boolean {
		const state = (snapshot?.status ?? '').toLowerCase();
		return Boolean(
			snapshot?.job_id && !snapshot.finished && ['queued', 'running', 'stopping'].includes(state)
		);
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
			if (!(e instanceof DOMException && e.name === 'AbortError'))
				pushEvent(e instanceof Error ? e.message : String(e));
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
			activeTab = 'recommendations';
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
				activeTab = 'test-lab';
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
			const [modelsResult, reportsResult, resultsResult, systemResult] = await Promise.all([
				CaliberAdvisorService.models(),
				CaliberAdvisorService.reports(),
				CaliberAdvisorService.results(),
				CaliberAdvisorService.system()
			]);
			models = modelsResult.data;
			reports = reportsResult.data.sort((a, b) => b.created_at.localeCompare(a.created_at));
			results = resultsResult;
			doctorSystem = systemResult;
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
			activeTab = 'recommendations';
			return;
		}
		loading = true;
		error = '';
		try {
			const result = await CaliberAdvisorService.plan(payload());
			plan = result.plan;
			message = `${result.plan_count} configs planned across ${pendingSelectedIds.length} new model(s); ${selectedLocalIds.length - pendingSelectedIds.length} already archived.`;
			activeTab = 'test-lab';
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
		if (event === 'row')
			return data.ok
				? `Measured ${fmtNumber(Number(data.eval_tps ?? 0), 1)} tok/s`
				: `Failed: ${String(data.error ?? 'configuration failed')}`;
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
				use_case: useCase,
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
			catalogMessage =
				result.returned_models > 0
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
		downloads =
			index === -1 ? [job, ...downloads] : downloads.map((item, i) => (i === index ? job : item));
	}

	function sameNonEmpty(
		left: string | null | undefined,
		right: string | null | undefined
	): boolean {
		return Boolean(left && right && left === right);
	}

	function downloadFor(model: FitAdvisorModel): FitAdvisorDownloadJob | null {
		const exact = model.download?.hf_ref
			? downloads.find((job) => sameNonEmpty(job.hf_ref, model.download?.hf_ref))
			: downloads.find(
					(job) =>
						sameNonEmpty(job.target_dir, model.target_dir) ||
						sameNonEmpty(job.local_path, model.local_path)
				);
		if (exact) return exact;
		if (!model.download)
			return downloads.find((job) => sameNonEmpty(job.model_id, model.id)) ?? null;
		return model.download_progress ?? null;
	}

	function isActiveDownloadStatus(status: string): boolean {
		return status === 'queued' || status === 'resolving' || status === 'downloading';
	}

	function downloadStatus(model: FitAdvisorModel): string {
		return (
			downloadFor(model)?.status ??
			model.download_status ??
			(model.installed ? 'configured' : 'available')
		);
	}

	function downloadActionLabel(model: FitAdvisorModel): string {
		const job = downloadFor(model);
		if (job && isActiveDownloadStatus(job.status)) {
			const progress =
				typeof job.percent === 'number' && Number.isFinite(job.percent)
					? ` ${Math.round(job.percent)}%`
					: '';
			if (job.status === 'queued') return 'Queued';
			if (job.status === 'resolving') return 'Resolving';
			return `Downloading${progress}`;
		}
		const status = downloadStatus(model);
		if (!model.download || status === 'unavailable') return 'No GGUF source';
		if (status === 'partial' || model.partial) return 'Resume DL';
		if (status === 'failed') return 'Retry DL';
		if (status === 'downloaded' || status === 'configured') return 'Downloaded';
		if (hasPendingDownload) return 'Queue DL';
		return 'Download';
	}

	function canStartDownload(model: FitAdvisorModel): boolean {
		if (!model.download) return false;
		return !['queued', 'resolving', 'downloading', 'downloaded', 'configured'].includes(
			downloadStatus(model)
		);
	}

	function canConfigureFit(model: FitAdvisorModel): boolean {
		return Boolean(
			model.installed ||
			model.downloaded ||
			model.configured ||
			downloadStatus(model) === 'downloaded'
		);
	}

	async function downloadModel(model: FitAdvisorModel) {
		error = '';
		try {
			const result = await FitAdvisorService.download(model);
			if (result.job) upsertDownload(result.job);
			catalogMessage = result.already_present
				? 'Model already present'
				: `Download queued: ${model.name}`;
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
			if (!selectedLocalIds.includes(result.model))
				selectedLocalIds = [...selectedLocalIds, result.model];
			activeTab = 'test-lab';
		} catch (e) {
			error = e instanceof Error ? e.message : String(e);
		}
	}

	async function openReport(report: CaliberReportSummary, switchTab = true) {
		error = '';
		try {
			selectedReport = await CaliberAdvisorService.report(report.id);
			selectedReportId = report.id;
			if (switchTab) activeTab = 'history';
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
		if (!canFitCaliberRow(row)) {
			error =
				'This row is synthetic or lacks complete context/memory evidence. Run a decision-grade streaming benchmark before FIT.';
			return;
		}
		const model = rowText(row, ['model', 'model_id']);
		if (!model) return;
		error = '';
		try {
			const result = await CaliberAdvisorService.configure({
				model,
				report_id: rowText(row, ['report_id'], selectedReportId),
				row_id: rowText(row, ['id']),
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
	<title>Local LLM Autopilot</title>
</svelte:head>

<main class="caliber-page">
	<header class="page-header">
		<div>
			<div class="header-kicker">
				<Gauge size={16} />
				private, offline-first, evidence-based
			</div>
			<h1>Local LLM Autopilot</h1>
			<p>
				Point it at your model folders. Autopilot discovers what is healthy, proves what fits,
				measures real responses, checks task quality and applies the best local model safely.
			</p>
		</div>
		<div class="hero-actions">
			<button type="button" onclick={() => (activeTab = 'library')} class="primary">
				Find my model
				<ChevronRight size={16} />
			</button>
			<button type="button" onclick={() => (activeTab = 'recommendations')}>
				<FileJson size={16} />
				View reports
			</button>
		</div>
	</header>

	{#if error}
		<div class="error">{error}</div>
	{/if}
	{#if message && !error}
		<div class="status-message">{message}</div>
	{/if}

	<section class="answer-strip">
		<div>
			<span>Best answer</span>
			<strong title={bestWinner ? modelFullName(rowText(bestWinner, ['model'], '-')) : ''}
				>{bestWinner
					? modelDisplayName(rowText(bestWinner, ['model'], '-'))
					: 'No winner yet'}</strong
			>
			<p>
				{bestWinner
					? rowText(
							bestWinner,
							['selection_reason'],
							`${fmtNumber(rowNum(bestWinner, ['eval_tps', 'tps']), 1)} tok/s at ${rowNum(bestWinner, ['ctx_size'], contextSize)} ctx`
						)
					: 'Run a campaign to populate this.'}
			</p>
		</div>
		<div>
			<span>Hardware</span>
			<strong>{fitSystem?.gpu_name ?? 'Detecting GPU'}</strong>
			<p>
				{fitSystem
					? `${fitSystem.gpu_count} GPU(s), ${fmtGb(fitSystem.total_gpu_vram_gb)} aggregate VRAM`
					: 'Fit Advisor system scan pending.'}
			</p>
		</div>
		<div>
			<span>Next step</span>
			<strong
				>{running
					? 'Running'
					: selectedLocalIds.length
						? 'Review and run'
						: 'Choose models'}</strong
			>
			<p>{nextAction}</p>
		</div>
	</section>

	<nav class="tabs" aria-label="Local LLM Autopilot sections">
		{#each tabs as tab (tab.id)}
			<button
				type="button"
				class:active={activeTab === tab.id}
				onclick={() => (activeTab = tab.id)}
			>
				{#if tab.id === 'test-lab'}<Gauge size={16} />{/if}
				{#if tab.id === 'library'}<Download size={16} />{/if}
				{#if tab.id === 'recommendations' || tab.id === 'history'}<FileJson size={16} />{/if}
				{#if tab.id === 'router'}<Route size={16} />{/if}
				{#if tab.id === 'doctor'}<Wrench size={16} />{/if}
				{tab.label}
			</button>
		{/each}
	</nav>

	{#if activeTab === 'test-lab'}
		<section class="wizard-grid">
			<div class="panel">
				<div class="panel-head">
					<div>
						<h2>1. What will you use it for?</h2>
						<p>The quality pack and eligible model features follow this choice.</p>
					</div>
				</div>
				<div class="choice-grid">
					{#each useCases as item (item.id)}
						<button
							type="button"
							class="choice compact-choice"
							class:active={useCase === item.id}
							onclick={() => (useCase = item.id)}
						>
							<strong>{item.label}</strong>
							<span>{item.help}</span>
						</button>
					{/each}
				</div>
			</div>

			<div class="panel">
				<div class="panel-head">
					<div>
						<h2>2. What matters most?</h2>
						<p>The backend applies this policy after fit and quality gates.</p>
					</div>
				</div>
				<div class="choice-grid">
					{#each Object.entries(profileLabels) as [id, item] (id)}
						<button
							type="button"
							class="choice"
							class:active={profile === id}
							onclick={() => (profile = id as ProfileId)}
						>
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
					{#each Object.entries(scopeOptions) as [id, item] (id)}
						<button
							type="button"
							class="choice"
							class:active={runScope === id}
							onclick={() => (runScope = id as RunScope)}
						>
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
						<p>
							{selectedLocalIds.length} selected · {pendingSelectedIds.length} need benchmarking · {selectedLocalIds.length -
								pendingSelectedIds.length} already archived.
						</p>
					</div>
					<div class="row-actions">
						<button type="button" onclick={selectRecommendedLocal}>Hardware shortlist</button>
						<button type="button" onclick={selectAllLocal}>All available</button>
						<button type="button" onclick={clearSelection}>Clear</button>
					</div>
				</div>
				<div class="model-list">
					{#each selectableModels as model (model.id)}
						<button
							type="button"
							class="model-option"
							class:active={selectedLocalIds.includes(model.id)}
							onclick={() => toggleLocalModel(model.id)}
						>
							<span class="checkbox">{selectedLocalIds.includes(model.id) ? '✓' : ''}</span>
							<div>
								<strong title={modelFullName(model.name || model.id)}
									>{modelDisplayName(model.name || model.id)}</strong
								>
								<span>{modelFullName(model.name || model.id)} · {modelParamLabel(model)}</span>
								<div class="tag-list">
									{#each modelTags(model) as tag (tag)}<b>{tag}</b>{/each}
								</div>
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
					<div class="intent-controls">
						<label class="check"
							><input type="checkbox" bind:checked={installedOnly} /><span
								>Use installed models first</span
							></label
						>
						<label
							><span>Allowed test time</span><select bind:value={allowedTestMinutes}
								><option value={10}>About 10 minutes</option><option value={20}
									>About 20 minutes</option
								><option value={45}>Up to 45 minutes</option><option value={90}
									>Deep, up to 90 minutes</option
								></select
							></label
						>
					</div>
					<dl>
						<div>
							<dt>Selected models</dt>
							<dd>{selectedLocalIds.length}</dd>
						</div>
						<div>
							<dt>Target context</dt>
							<dd>{targetContext?.label ?? contextSize}</dd>
						</div>
						<div>
							<dt>Benchmark scope</dt>
							<dd>{scopeOptions[runScope].title}</dd>
						</div>
						<div>
							<dt>Planned configs</dt>
							<dd>{plan.length || '-'}</dd>
						</div>
					</dl>
					<label>
						<span>Context target</span>
						<select bind:value={contextSize}>
							{#each contextOptions as option (option.value)}
								<option value={option.value}>{option.label} - {option.hint}</option>
							{/each}
						</select>
					</label>
					<label class="check">
						<input type="checkbox" bind:checked={loadAfterConfigure} />
						<span>Load winner immediately after FIT</span>
					</label>
					<div class="button-row">
						<button
							type="button"
							onclick={previewPlan}
							disabled={selectedLocalIds.length === 0 || loading}
						>
							<Settings2 size={16} />
							Review plan
						</button>
						<button type="button" class="primary" onclick={startSweep} disabled={!readyToRun}>
							<Play size={16} />
							Start benchmark
						</button>
						{#if sweepIsLive(status)}
							<button
								type="button"
								class="danger"
								onclick={stopSweep}
								disabled={status?.cancel_requested}
							>
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
				{#each workflow as item, index (item[0])}
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
						{#each plan.slice(0, 120) as row (row.id)}
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
							<button
								type="button"
								class="icon-action danger"
								onclick={stopSweep}
								disabled={status?.cancel_requested}
								aria-label="Stop benchmark"
							>
								<Square size={16} />
							</button>
						{:else}
							<Activity size={18} />
						{/if}
					</div>
					<div class="job">
						<div class="progress">
							<span
								style={`width:${status?.total ? Math.min(100, ((status.current ?? 0) / status.total) * 100) : running ? 12 : 0}%`}
							></span>
						</div>
						<dl>
							<div>
								<dt>Done</dt>
								<dd>{status?.current ?? 0}</dd>
							</div>
							<div>
								<dt>Total</dt>
								<dd>{status?.total ?? 0}</dd>
							</div>
							<div>
								<dt>Current</dt>
								<dd>{status?.current_item ?? '-'}</dd>
							</div>
							<div>
								<dt>Report</dt>
								<dd>{status?.report_id ?? '-'}</dd>
							</div>
						</dl>
						<div class="event-log">
							{#each eventLog as line, index (`${index}-${line}`)}
								<code>{line}</code>
							{/each}
						</div>
					</div>
				</div>
			</section>
		{/if}
	{/if}

	{#if activeTab === 'library'}
		<section class="panel explain">
			<Info size={18} />
			<div>
				<h2>Need more models?</h2>
				<p>
					Download candidates here, then press FIT to add them to local models. They will appear in
					Test Lab and can be selected for the benchmark campaign.
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
					{#each strategies as strategy (strategy.id)}
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
				{#each catalogModels as model (model.id)}
					{@const job = downloadFor(model)}
					<div class="table-row">
						<span class={`fit ${model.fit_level}`}>{model.fit_level}</span>
						<strong>{fmtNumber(model.score, 1)}</strong>
						<div class="model-cell">
							<strong title={modelFullName(model.name)}>{modelDisplayName(model.name)}</strong>
							<span>{modelFullName(model.name)}</span>
							<div class="tag-list">
								{#each uniqueStrings( [model.provider, model.download?.provider ? `GGUF: ${model.download.provider}` : '', ...(model.tags ?? [])] ).slice(0, 8) as tag (tag)}<b
										>{tag}</b
									>{/each}
							</div>
						</div>
						<span>{fmtGb(model.memory_required_gb)}</span>
						<span>{fmtNumber(model.estimated_tps, 1)} tok/s</span>
						<div class="status-cell">
							<span>{downloadStatus(model)}</span>
							{#if job}
								<div class="mini-progress">
									<span style={`width:${Math.min(100, job.percent || 0)}%`}></span>
								</div>
							{/if}
							{#if job?.error}<small class="download-error">{job.error}</small>{/if}
							{#if !model.download}<small>No verified GGUF repository; download disabled.</small
								>{/if}
						</div>
						<div class="row-actions">
							<button
								type="button"
								onclick={() => downloadModel(model)}
								disabled={!canStartDownload(model)}
								title={job?.error ?? (!model.download ? 'No verified GGUF source' : '')}
							>
								<Download size={15} />
								{downloadActionLabel(model)}
							</button>
							<button
								type="button"
								onclick={() => configureFitModel(model)}
								disabled={!canConfigureFit(model)}
							>
								<CheckCircle2 size={15} />
								FIT
							</button>
						</div>
					</div>
				{/each}
			</div>
		</section>
	{/if}

	{#if activeTab === 'recommendations' || activeTab === 'history'}
		<section class="reports-layout">
			{#if activeTab === 'history'}
				<div class="panel">
					<div class="panel-head">
						<div>
							<h2>Saved reports</h2>
							<p>
								Completed reports stay available for future comparisons. Pending reports can be
								deleted.
							</p>
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
						{#each reports as report (report.id)}
							<div class="table-row" class:active={selectedReportId === report.id}>
								<button type="button" class="linkish" onclick={() => openReport(report)}
									>{reportStatusLabel(report)}</button
								>
								<span>{report.rows}</span>
								<strong>{report.model || report.id}</strong>
								<span>{report.created_at}</span>
								<button
									type="button"
									onclick={() => deletePendingReport(report)}
									disabled={!canDeleteReport(report)}
								>
									<Trash2 size={15} />
								</button>
							</div>
						{/each}
					</div>
				</div>
			{/if}

			<div class="panel report-detail-panel">
				<div class="panel-head">
					<div>
						<h2>{activeTab === 'history' ? 'Report detail' : 'Decision center'}</h2>
						<p>
							{selectedReportId ||
								(activeTab === 'history' ? 'Select a report' : 'Latest compatible evidence')}
						</p>
					</div>
					{#if selectedReport}
						<button
							type="button"
							onclick={() => {
								selectedReport = null;
								selectedReportId = '';
							}}
						>
							Compare archive
						</button>
					{/if}
				</div>
				{#if selectedReport}
					<div class="detail report-summary-strip">
						<dl>
							<div>
								<dt>Status</dt>
								<dd>{String(selectedReport.status ?? '-')}</dd>
							</div>
							<div>
								<dt>Measured rows</dt>
								<dd>{reportRows.length}</dd>
							</div>
							<div>
								<dt>Planned configs</dt>
								<dd>{reportPlan.length}</dd>
							</div>
							<div>
								<dt>Created</dt>
								<dd>{String(selectedReport.created_at ?? '-')}</dd>
							</div>
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
								<p>
									{selectedReport
										? `Selected ${String(selectedReport.created_at ?? selectedReportId)}`
										: 'Historical archive across completed runs'}
								</p>
							</div>
							<strong
								>{okAnalyticsRows.length}/{scopedAnalyticsRows.length ||
									resultRows.length ||
									reportRows.length} successful configs</strong
							>
						</div>

						{#if bestWinner}
							<section class="recommendation-hero">
								<div class="winner-answer">
									<span class="eyebrow">Recommended on this hardware</span>
									<h3 title={modelFullName(rowText(bestWinner, ['model'], '-'))}>
										{modelDisplayName(rowText(bestWinner, ['model'], '-'))}
									</h3>
									<small class="full-model-name"
										>{modelFullName(rowText(bestWinner, ['model'], '-'))}</small
									>
									<p>
										{rowText(
											bestWinner,
											['selection_reason'],
											'Selected by the backend recommendation policy.'
										)}
									</p>
									<div class="evidence-badges">
										<b>{rowText(bestWinner, ['evidence_level'], 'unverified')}</b>
										<b class:pass={rowBool(bestWinner, 'quality_gate_passed')}
											>{rowBool(bestWinner, 'quality_gate_passed')
												? `quality ${Math.round(qualityScore(bestWinner) * 100)}%`
												: 'quality gate missing'}</b
										>
										<b>{rowText(bestWinner, ['measurement_confidence'], 'unknown confidence')}</b>
									</div>
									<button
										type="button"
										class="primary"
										onclick={() => configureCaliberRow(bestWinner)}
										disabled={!canFitCaliberRow(bestWinner)}
										><CheckCircle2 size={15} />Apply known-good preset</button
									>
									{#if !canFitCaliberRow(bestWinner)}
										<div class="fit-blocked">
											<span>{fitBlockReason(bestWinner)}</span>
											<button type="button" onclick={() => selectRowForTest(bestWinner)}
												>Test for FIT</button
											>
										</div>
									{/if}
								</div>
								<div class="answer-metrics">
									<div>
										<span>Decode</span><strong
											>{fmtNumber(rowNum(bestWinner, ['eval_tps', 'tps']), 1)} t/s</strong
										>
									</div>
									<div>
										<span>First token</span><strong
											>{fmtNumber(rowNum(bestWinner, ['e2e_ttft_ms']), 0)} ms</strong
										>
									</div>
									<div>
										<span>Context proved</span><strong
											>{Math.round(rowNum(bestWinner, ['ctx_size']) / 1024)}k</strong
										>
									</div>
									<div>
										<span>Peak memory</span><strong>{fmtMib(reportMemoryMib(bestWinner))}</strong>
									</div>
								</div>
							</section>
							{#if bestAlternatives.length > 0}
								<div class="alternative-grid">
									{#each bestAlternatives as alternative, index (rowIdentity(alternative))}
										<div>
											<span>Alternative {index + 1}</span><strong
												>{modelDisplayName(rowText(alternative, ['model'], '-'))}</strong
											>
											<div class="tag-list">
												{#each reportRowTags(alternative) as tag (tag)}<b>{tag}</b>{/each}
											</div>
											<p>
												{rowText(
													alternative,
													['selection_reason'],
													`${fmtNumber(rowNum(alternative, ['eval_tps']), 1)} t/s`
												)}
											</p>
										</div>
									{/each}
								</div>
							{/if}
						{:else}
							<div class="methodology-warning">
								<strong>No production winner yet</strong><span
									>A winner needs a healthy artifact, streaming measurements, verified context and a
									passing {useCase} quality pack. Run the missing quality evidence in
									<a href="#/ds4-eval">DS4 expert evaluator</a>.</span
								>
							</div>
						{/if}

						<div class="hardware-strip">
							<strong>Hardware:</strong>
							<span>{fitSystem?.gpu_name ?? 'GPU scan pending'}</span>
							<span
								>{fitSystem
									? `${fmtGb(fitSystem.total_gpu_vram_gb)} aggregate VRAM`
									: 'VRAM unavailable'}</span
							>
							<span
								>{fitSystem
									? `${fitSystem.cpu_name} / ${fitSystem.cpu_cores} threads`
									: 'CPU unavailable'}</span
							>
							<span>llama-server: native router</span>
						</div>

						<div class="filter-bar">
							<span>Data scope:</span>
							<div class="segmented">
								{#if selectedReport}
									<button type="button" class:active={true}>Selected report</button>
								{:else}
									<button
										type="button"
										class:active={reportScope === 'latest'}
										onclick={() => (reportScope = 'latest')}>Latest campaign</button
									>
									<button
										type="button"
										class:active={reportScope === 'all'}
										onclick={() => (reportScope = 'all')}>Compatible history</button
									>
								{/if}
							</div>
							<em>{reportGroups.length} models, {okAnalyticsRows.length} configs in view</em>
						</div>

						<div class="filter-bar">
							<span>Winner criterion:</span>
							<div class="segmented">
								{#each Object.entries(profileLabels) as [id, item] (id)}
									<button
										type="button"
										class:active={profile === id}
										onclick={() => (profile = id as ProfileId)}>{item.title}</button
									>
								{/each}
							</div>
							<em>{String(activeDecision?.reason ?? profileLabels[profile].help)}</em>
						</div>

						{#if syntheticRows > 0}
							<div class="methodology-warning">
								<strong>Synthetic benchmark</strong>
								<span
									>{syntheticRows} row(s) measured with llama-bench. Eval speed uses the generation row;
									full streaming timeline requires the server-runner telemetry backend.</span
								>
							</div>
						{/if}

						<section class="report-section throughput-section">
							<div class="section-heading">
								<h3>Throughput & memory</h3>
								<p>Compare each model winner before opening the detailed memory/latency plot.</p>
							</div>
							<div class="segmented metric-tabs">
								{#each reportMetrics as metric (metric)}
									<button
										type="button"
										class:active={reportMetric === metric}
										onclick={() => (reportMetric = metric)}>{reportMetricLabel(metric)}</button
									>
								{/each}
							</div>
							<div class="metric-legend">
								<span><i></i>Bar length is normalized within the selected metric.</span>
								<strong
									>{metricHigherIsBetter(reportMetric)
										? 'Longer is better for throughput.'
										: 'Longer means lower cost for memory/latency.'}</strong
								>
							</div>
							<div class="throughput-bars">
								{#each reportLeaderboard as row (rowIdentity(row))}
									<div class="throughput-row">
										<span title={modelFullName(rowText(row, ['model'], '-'))}>
											<b class={`rank-tag ${reportFitClass(row)}`}>{reportFitLabel(row)}</b>
											{modelDisplayName(rowText(row, ['model'], '-'))}
										</span>
										<div class="throughput-track">
											<i style={`width:${reportBarWidth(row)}%`}></i>
										</div>
										<strong
											>{reportMetricUnit(
												reportMetric,
												reportMetricValue(row, reportMetric)
											)}</strong
										>
									</div>
								{/each}
							</div>
						</section>

						<section class="report-section scatter-section">
							<div class="section-heading">
								<h3>Memory vs latency</h3>
								<p>
									One dot per successful config. X is total prompt + generation time; Y is peak VRAM
									plus observed spill. Hover a point for model details.
								</p>
							</div>
							<div class="memory-latency-grid">
								<div class="chart-shell">
									<svg
										class="report-scatter"
										viewBox="0 0 980 360"
										role="img"
										aria-label="Memory versus latency"
									>
										<line class="axis" x1="70" y1="312" x2="930" y2="312" />
										<line class="axis" x1="70" y1="54" x2="70" y2="312" />
										{#each scatterTickRatios as ratio (ratio)}
											{@const x = 70 + ratio * 860}
											{@const y = 312 - ratio * 258}
											<line class="grid-line" x1={x} y1="54" x2={x} y2="312" />
											<line class="grid-line" x1="70" y1={y} x2="930" y2={y} />
											<text class="tick-label" {x} y="330" text-anchor="middle"
												>{fmtNumber(
													scatterTimeTick(ratio),
													scatterTimeTick(ratio) < 10 ? 1 : 0
												)}s</text
											>
											<text class="tick-label" x="64" y={y + 4} text-anchor="end"
												>{ratio === 0 ? '0' : fmtMib(scatterMemoryTick(ratio))}</text
											>
										{/each}
										{#if reportVramBudgetMib > 0 && reportVramBudgetMib <= reportMaxMemory * 1.05}
											<line
												class="budget"
												x1="70"
												y1={reportBudgetY()}
												x2="930"
												y2={reportBudgetY()}
											/>
											<text class="budget-label" x="924" y={reportBudgetY() - 6} text-anchor="end"
												>VRAM budget {fmtMib(reportVramBudgetMib)}</text
											>
										{/if}
										<text x="420" y="352">Total request time (log scale)</text>
										<text x="16" y="46">Peak memory</text>
										{#each reportScatterRows.slice(0, 360) as row, index (`${index}-${rowIdentity(row)}`)}
											<circle
												cx={reportScatterX(row)}
												cy={reportScatterY(row)}
												r={isReportCandidate(row) ? 5.8 : 4}
												class={`dot-${index % 5}`}
												class:candidate={isReportCandidate(row)}
											>
												<title
													>{modelFullName(rowText(row, ['model'], '-'))} / {fmtNumber(
														rowNum(row, ['eval_tps', 'tps']),
														1
													)} t/s / {fmtMib(reportMemoryMib(row))} / ctx {rowNum(row, [
														'ctx_size'
													]) || '-'}</title
												>
											</circle>
										{/each}
									</svg>
								</div>

								<div class="metric-panel">
									<div class="analytics-cards">
										<div><span>Models</span><strong>{reportGroups.length}</strong></div>
										<div>
											<span>Measured configs</span><strong>{okAnalyticsRows.length}</strong>
										</div>
										<div>
											<span>Winner rule</span><strong>{profileLabels[profile].title}</strong>
										</div>
										<div><span>Metric</span><strong>{reportMetricLabel(reportMetric)}</strong></div>
									</div>
									<div class="leader-bars compact">
										{#each reportLeaderboard.slice(0, 8) as row (rowIdentity(row))}
											<div class="leader-row">
												<span title={modelFullName(rowText(row, ['model'], '-'))}
													>{modelDisplayName(rowText(row, ['model'], '-'))}</span
												>
												<div><i style={`width:${reportBarWidth(row)}%`}></i></div>
												<strong
													>{reportMetricUnit(
														reportMetric,
														reportMetricValue(row, reportMetric)
													)}</strong
												>
											</div>
										{/each}
									</div>
								</div>
							</div>
						</section>

						{#if bestWinner}
							<section class="report-section report-visual-grid">
								<div>
									<div class="section-heading">
										<h3>Tuned vs vanilla</h3>
										<p>
											Normalized speed, prompt, context, memory safety and task quality. Larger is
											better.
										</p>
									</div>
									<svg
										class="radar-chart"
										viewBox="0 0 220 210"
										role="img"
										aria-label="Tuned versus vanilla radar chart"
									>
										<polygon class="radar-grid" points="110,27 184,81 156,168 64,168 36,81" />
										{#if matchedVanilla(bestWinner)}<polygon
												class="radar-vanilla"
												points={radarPoints(matchedVanilla(bestWinner)!)}
											/>{/if}
										<polygon class="radar-winner" points={radarPoints(bestWinner)} />
										<text x="94" y="16">decode</text><text x="180" y="74">prompt</text><text
											x="157"
											y="191">context</text
										><text x="9" y="191">memory</text><text x="2" y="74">quality</text>
									</svg>
									<div class="chart-legend">
										<span class="winner-line">winner</span><span class="vanilla-line"
											>matched vanilla</span
										>
									</div>
								</div>
								<div>
									<div class="section-heading">
										<h3>Streaming timeline</h3>
										<p>
											Process-scoped VRAM, GPU utilization and RAM samples from the isolated server.
										</p>
									</div>
									{#if bestTimeline.length > 1}
										<svg
											class="timeline-chart"
											viewBox="0 0 760 280"
											role="img"
											aria-label="Streaming telemetry timeline"
										>
											<line x1="44" y1="250" x2="720" y2="250" /><line
												x1="44"
												y1="60"
												x2="44"
												y2="250"
											/>
											<polyline
												class="vram-line"
												points={bestTimeline
													.map((sample) => `${timelineX(sample)},${timelineY(sample, 1)}`)
													.join(' ')}
											/>
											<polyline
												class="util-line"
												points={bestTimeline
													.map((sample) => `${timelineX(sample)},${timelineY(sample, 2)}`)
													.join(' ')}
											/>
											<polyline
												class="ram-line"
												points={bestTimeline
													.map((sample) => `${timelineX(sample)},${timelineY(sample, 3)}`)
													.join(' ')}
											/>
											<text x="46" y="272"
												>0 → {fmtNumber((bestTimeline.at(-1)?.[0] ?? 0) / 1000, 1)} seconds</text
											>
										</svg>
										<div class="chart-legend">
											<span class="vram-line">VRAM</span><span class="util-line"
												>GPU utilization</span
											><span class="ram-line">process RAM</span>
										</div>
									{:else}<p class="empty">No compact streaming timeline in this report.</p>{/if}
								</div>
							</section>
						{/if}

						<section class="report-section">
							<div class="section-heading">
								<h3>Prefill & KV-depth load curve</h3>
								<p>
									Prefill measures prompt ingestion. KV-fill measures decode after the cache was
									filled to the stated depth. Repeated points at the same depth are shown as their
									median.
								</p>
							</div>
							{#if loadCurveRows.length > 0}
								<div class="curve-legend">
									<span><i class="healthy"></i>≥80% of this model's fastest point</span>
									<span><i class="degraded"></i>55–79% degradation</span>
									<span><i class="collapse"></i>&lt;55% possible collapse</span>
									<strong>Bar = speed retention; number = absolute prompt throughput.</strong>
								</div>
								<div class="load-curves">
									{#each loadCurveRows as row, index (`${index}-${rowIdentity(row)}`)}<div>
											<span title={modelFullName(rowText(row, ['model'], '-'))}
												>{modelDisplayName(rowText(row, ['model'], '-'))} · {rowText(row, [
													'workload_kind'
												])} · {fmtTokens(loadTarget(row))}{rowNum(row, ['curve_samples']) > 1
													? ` · median of ${rowNum(row, ['curve_samples'])}`
													: ''}</span
											>
											<div>
												<i class={loadCurveBand(row)} style={`width:${loadCurveWidth(row)}%`}></i>
											</div>
											<strong>{fmtNumber(loadCurveSpeed(row), 1)} t/s</strong>
										</div>{/each}
								</div>
							{:else}<p class="empty">
									Run a Decision or Deep campaign to collect prefill and KV-depth points.
								</p>{/if}
						</section>

						<section class="report-section">
							<div class="section-heading">
								<h3>Preset actions by model</h3>
								<p>
									Expand a model to inspect every config. FIT is enabled only for its own streaming,
									context-verified and quality-qualified row; otherwise use Test for FIT.
								</p>
							</div>
							<div class="analytics-models">
								{#each reportGroups as group, index (group.model)}
									<details class="analytics-model" open={index === 0}>
										<summary>
											<div class="summary-main">
												<span
													class={`rank-tag ${group.winner ? reportFitClass(group.winner) : 'low'}`}
													>{group.winner ? reportFitLabel(group.winner) : 'n/a'}</span
												>
												<strong title={modelFullName(group.model)}
													>{modelDisplayName(group.model)}</strong
												>
												{#if group.winner}
													<code
														>{rowText(
															group.winner,
															['variant', 'quant', 'kv_cache'],
															rowText(group.winner, ['row_role'], 'candidate')
														)}</code
													>
												{/if}
											</div>
											{#if group.winner}
												<div class="summary-metrics">
													<span>{fmtNumber(rowNum(group.winner, ['eval_tps', 'tps']), 1)} t/s</span>
													<span>{fmtMib(reportMemoryMib(group.winner))}</span>
													<span>{rowNum(group.winner, ['ctx_size']) || '-'} ctx</span>
													<button
														type="button"
														onclick={() => configureCaliberRow(group.winner as CaliberRow)}
														disabled={!canFitCaliberRow(group.winner as CaliberRow)}
														title={canFitCaliberRow(group.winner as CaliberRow)
															? 'Apply measured winner'
															: fitBlockReason(group.winner as CaliberRow)}
													>
														<CheckCircle2 size={14} />
														FIT winner
													</button>
													{#if !canFitCaliberRow(group.winner as CaliberRow)}
														<button
															type="button"
															onclick={() => selectRowForTest(group.winner as CaliberRow)}
															>Test for FIT</button
														>
													{/if}
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
											{#each group.rows as row, rowIndex (`${rowIndex}-${rowIdentity(row)}`)}
												<div
													class="table-row"
													class:winner={Boolean(group.winner) &&
														rowIdentity(group.winner as CaliberRow) === rowIdentity(row)}
												>
													<span>{rowText(row, ['row_role'], '-')}</span>
													<span>{rowText(row, ['workload_kind'], '-')}</span>
													<span>{rowNum(row, ['ctx_size']) || '-'}</span>
													<span>{fmtNumber(rowNum(row, ['prompt_tps']), 1)} t/s</span>
													<span>{fmtNumber(rowNum(row, ['eval_tps', 'tps']), 1)} t/s</span>
													<span>{fmtMib(reportMemoryMib(row))}</span>
													<span
														>{rowText(
															row,
															['decode_usability', 'residency', 'memory_state', 'fit_status'],
															'-'
														)}</span
													>
												</div>
											{/each}
										</div>
									</details>
								{/each}
							</div>
						</section>

						<section class="report-section">
							<div class="section-heading">
								<h3>Metric glossary</h3>
								<p>What the decision-grade measurements mean.</p>
							</div>
							<div class="glossary">
								<div><b>TTFT</b><span>Time from request to first streamed token.</span></div>
								<div>
									<b>TPOT / ITL</b><span>Average / percentile delay between generated tokens.</span>
								</div>
								<div><b>Prompt t/s</b><span>Prompt ingestion or prefill throughput.</span></div>
								<div><b>Decode t/s</b><span>Generated tokens per second after prefill.</span></div>
								<div>
									<b>Quality floor</b><span
										>Minimum measured task score before cost optimization.</span
									>
								</div>
								<div>
									<b>FIT eligible</b><span
										>Streaming, context, memory and quality evidence all pass policy.</span
									>
								</div>
							</div>
							<details class="raw-table">
								<summary>All technical columns ({scopedAnalyticsRows.length} rows)</summary>
								<div class="raw-scroll">
									<table>
										<thead
											><tr
												>{#each technicalColumns as column (column)}<th>{column}</th>{/each}</tr
											></thead
										><tbody
											>{#each scopedAnalyticsRows as row, rowIndex (`${rowIndex}-${rowIdentity(row)}`)}<tr
													>{#each technicalColumns as column (column)}<td
															>{String(row[column] ?? '-')}</td
														>{/each}</tr
												>{/each}</tbody
										>
									</table>
								</div>
							</details>
						</section>
					</div>
				{:else}
					<p class="empty">No historical Caliber rows yet.</p>
				{/if}
			</div>
		</section>
	{/if}

	{#if activeTab === 'router'}
		<section class="router-hero panel">
			<div>
				<span class="eyebrow">Stable virtual models</span>
				<h2>Ask for an outcome, not a filename</h2>
				<p>
					The local router filters by context, quality and features, then accounts for load cost and
					keeps a suitable resident model when possible.
				</p>
			</div>
			<div class="alias-grid">
				{#each aliasCards as alias (alias.id)}
					{@const decision = aliasDecision(alias.id)}
					{@const evidence = asRecord(decision?.evidence)}
					<div>
						<code>{alias.id}</code><span>{alias.description}</span>
						<div class="tag-list">
							{#each uniqueStrings( [...alias.tags, rowText( evidence ?? {}, ['variant'] ), rowText( evidence ?? {}, ['evidence_level'] ), decision?.quality_pack ? `${String(decision.quality_pack)} pack` : ''] ).filter(Boolean) as tag (tag)}<b
									>{tag}</b
								>{/each}
						</div>
						{#if decision?.selected_model}
							<small>Current winner</small>
							<strong title={modelFullName(String(decision.selected_model))}
								>{modelDisplayName(String(decision.selected_model))}</strong
							>
							<span
								>{fmtNumber(Number(decision.quality ?? 0) * 100, 0)}% quality · {fmtTokens(
									Number(decision.required_context ?? 0)
								)} required</span
							>
						{:else}<small>No qualified routing decision recorded yet.</small>{/if}
						<a href="#/">Use in chat</a>
					</div>
				{/each}
			</div>
			<div class="route-log">
				<div class="panel-head">
					<div>
						<h3>Recent route decisions</h3>
						<p>Inspectable local policy evidence; prompt content is not stored.</p>
					</div>
					<button type="button" onclick={refreshRouteEvents}><RefreshCw size={15} />Refresh</button>
				</div>
				{#if routeEvents.filter((event) => event.event_type === 'decision').length > 0}
					{#each routeEvents.filter((event) => event.event_type === 'decision') as event (event.object_id)}
						<div class="route-row">
							<code>{String(event.payload.alias ?? '-')}</code><strong
								>{String(event.payload.selected_model ?? '-')}</strong
							><span>{String(event.payload.reason ?? '-')}</span><small>{event.created_at}</small>
						</div>
					{/each}
				{:else}
					<p class="empty">No virtual-alias traffic has been routed yet.</p>
				{/if}
			</div>
		</section>
	{/if}

	{#if activeTab === 'doctor'}
		<section class="doctor-grid">
			<div class:doctor-pass={Boolean(doctorData?.state_writable)}>
				<span>Local state</span><strong
					>{doctorData?.state_writable ? 'Ready' : 'Needs attention'}</strong
				>
				<p>SQLite uses the XDG data directory, not the source tree.</p>
			</div>
			<div class:doctor-pass={Boolean(doctorData?.streaming_profiler_available)}>
				<span>Streaming profiler</span><strong
					>{doctorData?.streaming_profiler_available ? 'Available' : 'Missing binary'}</strong
				>
				<p>Required for final decision-grade evidence.</p>
			</div>
			<div class:doctor-warn={Number(doctorData?.unhealthy_artifacts ?? 0) > 0}>
				<span>Model library</span><strong
					>{String(doctorData?.ready_artifacts ?? 0)} ready · {String(
						doctorData?.unhealthy_artifacts ?? 0
					)} unhealthy</strong
				>
				<p>{String(doctorData?.duplicate_artifacts ?? 0)} duplicate artifacts detected.</p>
			</div>
			<div class:doctor-warn={Number(doctorData?.stale_reports ?? 0) > 0}>
				<span>Benchmark freshness</span><strong
					>{String(doctorData?.stale_reports ?? 0)} stale · {String(
						doctorData?.legacy_reports ?? 0
					)} legacy</strong
				>
				<p>Rerun after llama.cpp build or GPU-driver changes.</p>
			</div>
		</section>
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
						<div>
							<dt>CPU</dt>
							<dd>{fitSystem?.cpu_name ?? '-'}</dd>
						</div>
						<div>
							<dt>Threads</dt>
							<dd>{fitSystem?.cpu_cores ?? '-'}</dd>
						</div>
						<div>
							<dt>RAM total</dt>
							<dd>{fitSystem ? fmtGb(fitSystem.total_ram_gb) : '-'}</dd>
						</div>
						<div>
							<dt>GPU</dt>
							<dd>{fitSystem?.gpu_name ?? '-'}</dd>
						</div>
						<div>
							<dt>GPU count</dt>
							<dd>{fitSystem?.gpu_count ?? '-'}</dd>
						</div>
						<div>
							<dt>Aggregate VRAM</dt>
							<dd>{fitSystem ? fmtGb(fitSystem.total_gpu_vram_gb) : '-'}</dd>
						</div>
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
						<li>
							Finalists use an isolated streaming llama-server; llama-bench is retained only for
							fast synthetic racing.
						</li>
						<li>FIT is blocked until context, memory and the selected quality pack pass.</li>
						<li>
							Use the expert <a href="#/ds4-eval">quality evaluator</a> or
							<a href="#/ds4-bench">context bench</a> when Doctor reports missing evidence.
						</li>
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

	.compact-choice {
		min-height: 78px;
	}

	.intent-controls {
		display: grid;
		grid-template-columns: 1fr 1fr;
		gap: 10px;
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

	.tag-list {
		display: flex !important;
		flex-wrap: wrap;
		gap: 4px !important;
	}

	.tag-list b {
		width: fit-content;
		border: 1px solid color-mix(in oklch, var(--caliber-accent) 45%, var(--border));
		border-radius: 999px;
		background: color-mix(in oklch, var(--caliber-accent) 10%, transparent);
		padding: 2px 6px;
		color: var(--muted-foreground);
		font-size: 10px;
		font-weight: 650;
		line-height: 1.25;
	}

	.download-error {
		max-width: 260px;
		color: #fca5a5;
		line-height: 1.25;
	}

	.full-model-name {
		color: #aab4c3;
		line-height: 1.35;
	}

	.fit-blocked {
		display: grid;
		grid-template-columns: minmax(0, 1fr) max-content;
		gap: 8px;
		align-items: center;
		border-left: 3px solid #f59e0b;
		background: rgba(245, 158, 11, 0.08);
		padding: 8px 10px;
	}

	.fit-blocked span {
		color: #fde68a;
		font-size: 12px;
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

	.status-message {
		border-left: 3px solid var(--primary);
		background: color-mix(in oklch, var(--muted) 45%, transparent);
		padding: 10px 12px;
		color: var(--muted-foreground);
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

	.methodology-warning a,
	.router-hero a {
		color: #c4b5fd;
		text-decoration: underline;
	}

	.recommendation-hero {
		display: grid;
		grid-template-columns: minmax(0, 1.3fr) minmax(320px, 0.7fr);
		gap: 18px;
		border: 1px solid #6d5ce7;
		border-radius: 8px;
		background: linear-gradient(135deg, rgba(76, 29, 149, 0.62), rgba(30, 30, 30, 0.96));
		padding: 18px;
	}

	.winner-answer,
	.answer-metrics,
	.alternative-grid,
	.evidence-badges {
		display: grid;
		gap: 10px;
	}

	.winner-answer h3 {
		margin: 0;
		color: #fff;
		font-size: 26px;
	}

	.eyebrow {
		color: #c4b5fd !important;
		font-size: 11px;
		font-weight: 800;
		letter-spacing: 0.12em;
		text-transform: uppercase;
	}

	.evidence-badges {
		grid-template-columns: repeat(3, max-content);
	}

	.evidence-badges b {
		border: 1px solid #64748b;
		border-radius: 999px;
		padding: 4px 8px;
		color: #cbd5e1;
		font-size: 11px;
	}

	.evidence-badges b.pass {
		border-color: #22c55e;
		color: #86efac;
	}

	.answer-metrics {
		grid-template-columns: repeat(2, 1fr);
	}

	.answer-metrics > div,
	.alternative-grid > div {
		display: grid;
		gap: 4px;
		border: 1px solid #4f4f4f;
		border-radius: 6px;
		background: rgba(17, 17, 17, 0.72);
		padding: 11px;
	}

	.answer-metrics strong,
	.alternative-grid strong {
		color: #fff;
	}

	.alternative-grid {
		grid-template-columns: repeat(3, 1fr);
	}

	.report-visual-grid {
		grid-template-columns: minmax(280px, 0.55fr) minmax(480px, 1.45fr);
	}

	.radar-chart,
	.timeline-chart {
		width: 100%;
		min-height: 250px;
		background: #202020;
	}

	.radar-grid {
		fill: transparent;
		stroke: #52525b;
	}

	.radar-winner {
		fill: rgba(139, 92, 246, 0.35);
		stroke: #a78bfa;
		stroke-width: 2;
	}

	.radar-vanilla {
		fill: rgba(148, 163, 184, 0.12);
		stroke: #94a3b8;
		stroke-dasharray: 4 3;
	}

	.radar-chart text,
	.timeline-chart text {
		fill: #cbd5e1;
		font-size: 10px;
	}

	.timeline-chart line {
		stroke: #52525b;
	}

	.timeline-chart polyline {
		fill: none;
		stroke-width: 2;
	}

	.vram-line {
		stroke: #a78bfa;
		color: #a78bfa;
	}
	.util-line {
		stroke: #22c55e;
		color: #22c55e;
	}
	.ram-line {
		stroke: #38bdf8;
		color: #38bdf8;
	}

	.chart-legend {
		display: flex;
		gap: 14px;
		padding: 8px 0;
	}

	.chart-legend span::before {
		content: '';
		display: inline-block;
		width: 18px;
		height: 2px;
		margin-right: 5px;
		background: currentColor;
		vertical-align: middle;
	}

	.winner-line {
		color: #a78bfa !important;
	}
	.vanilla-line {
		color: #94a3b8 !important;
	}

	.load-curves {
		display: grid;
		gap: 7px;
	}

	.load-curves > div {
		display: grid;
		grid-template-columns: minmax(240px, 1fr) minmax(240px, 1.5fr) 90px;
		gap: 10px;
		align-items: center;
	}

	.load-curves > div > div {
		height: 12px;
		background: #111;
	}

	.load-curves i {
		display: block;
		height: 100%;
		background: linear-gradient(90deg, #8b5cf6, #22c55e);
	}

	.glossary {
		display: grid;
		grid-template-columns: repeat(3, 1fr);
		gap: 8px;
	}

	.glossary > div,
	.alias-grid > div {
		display: grid;
		gap: 4px;
		border: 1px solid #4f4f4f;
		border-radius: 6px;
		padding: 10px;
	}

	.glossary b,
	.alias-grid code {
		color: #f8fafc;
	}

	.raw-table summary {
		cursor: pointer;
		color: #f8fafc;
		font-weight: 700;
	}

	.raw-scroll {
		margin-top: 10px;
		overflow: auto;
	}

	.raw-scroll table {
		border-collapse: collapse;
		font-size: 11px;
	}

	.raw-scroll th,
	.raw-scroll td {
		border: 1px solid #4f4f4f;
		padding: 6px;
		white-space: nowrap;
		color: #cbd5e1;
	}

	.router-hero {
		display: grid;
		gap: 18px;
		padding: 18px;
	}

	.alias-grid {
		display: grid;
		grid-template-columns: repeat(3, 1fr);
		gap: 10px;
	}

	.route-log {
		border: 1px solid var(--border);
		border-radius: var(--radius);
	}

	.route-row {
		display: grid;
		grid-template-columns: 110px minmax(180px, 0.6fr) minmax(280px, 1fr) 160px;
		gap: 12px;
		align-items: center;
		border-top: 1px solid var(--border);
		padding: 10px 12px;
	}

	.route-row small {
		color: var(--muted-foreground);
	}

	.doctor-grid {
		display: grid;
		grid-template-columns: repeat(4, minmax(0, 1fr));
		gap: 10px;
	}

	.doctor-grid > div {
		display: grid;
		gap: 6px;
		border: 1px solid var(--border);
		border-top: 3px solid var(--muted-foreground);
		border-radius: var(--radius);
		background: var(--card);
		padding: 13px;
	}

	.doctor-grid > div.doctor-pass {
		border-top-color: #22c55e;
	}
	.doctor-grid > div.doctor-warn {
		border-top-color: #f59e0b;
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

	.report-scatter .grid-line {
		stroke: #3f3f46;
		stroke-width: 0.7;
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

	.report-scatter .tick-label {
		fill: #94a3b8;
		font-size: 10px;
	}

	.report-scatter .budget-label {
		fill: #fca5a5;
		font-size: 10px;
	}

	.metric-legend,
	.curve-legend {
		display: flex;
		flex-wrap: wrap;
		gap: 10px 18px;
		align-items: center;
		border: 1px solid #454545;
		background: #202020;
		padding: 8px 10px;
		font-size: 11px;
	}

	.metric-legend span,
	.curve-legend span {
		display: inline-flex;
		align-items: center;
		gap: 6px;
	}

	.metric-legend i {
		display: inline-block;
		width: 32px;
		height: 7px;
		background: linear-gradient(90deg, #8b5cf6, #22c55e);
	}

	.curve-legend i {
		display: inline-block;
		width: 18px;
		height: 7px;
	}

	.curve-legend i.healthy,
	.load-curves i.healthy {
		background: #22c55e;
	}

	.curve-legend i.degraded,
	.load-curves i.degraded {
		background: #f59e0b;
	}

	.curve-legend i.collapse,
	.load-curves i.collapse {
		background: #ef4444;
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
		.recommendation-hero,
		.report-visual-grid,
		.alternative-grid,
		.glossary,
		.alias-grid,
		.doctor-grid,
		.analytics-cards,
		.workflow {
			display: flex;
			flex-direction: column;
		}

		.choice-grid {
			grid-template-columns: 1fr;
		}

		.intent-controls,
		.answer-metrics,
		.evidence-badges,
		.load-curves > div,
		.route-row {
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
