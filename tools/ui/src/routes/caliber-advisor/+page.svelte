<script lang="ts">
	import {
		Activity,
		CheckCircle2,
		ChevronRight,
		ClipboardCheck,
		FileJson,
		Gauge,
		Play,
		RefreshCw,
		Route,
		Settings2,
		Square,
		Trash2,
		Wrench
	} from '@lucide/svelte';
	import { Ds4SuitePage } from '$lib/components/app/ds4';
	import {
		CaliberAdvisorService,
		type CaliberModel,
		type CaliberPlanItem,
		type CaliberReportSummary,
		type CaliberSweepStatus,
		isCaliberBenchmarkEligible
	} from '$lib/services/caliber-advisor.service';
	import {
		type Ds4QualityEvidence,
		type Ds4QualityProfile,
		type Ds4ReportSummary,
		Ds4Service
	} from '$lib/services/ds4.service';
	import { FitAdvisorService, type FitAdvisorSystem } from '$lib/services/fit-advisor.service';
	import { type LocalRouteEvent, RouterService } from '$lib/services/router.service';
	import { compactModelName, normalizeModelName } from '$lib/utils/model-display';
	import { onDestroy, onMount } from 'svelte';
	import { SvelteMap } from 'svelte/reactivity';

	type TabId = 'overview' | 'run-tests' | 'evidence' | 'archive';
	type EvidenceMode = 'quality' | 'performance';
	type UseCaseId = 'general' | 'chat' | 'coding' | 'reasoning' | 'rag' | 'tools' | 'long-context';
	type ProfileId = 'overall' | 'speed' | 'efficiency' | 'safety';
	type RunScope = 'quick' | 'standard' | 'evaluator';
	type QualitySectorId = 'daily' | 'coding' | 'research' | 'reasoning' | 'agents' | 'long';
	type ReportScope = 'latest' | 'all';
	type ReportMetric = 'eval' | 'prompt' | 'memory' | 'latency' | 'vram';
	type CaliberRow = Record<string, unknown>;
	type ReportModelGroup = { model: string; rows: CaliberRow[]; winner: CaliberRow | null };
	type RecommendationDecision = Record<string, unknown>;
	type ArchiveEntry = {
		id: string;
		source: 'caliber' | 'ds4';
		kind: 'performance' | 'quality';
		status: string;
		count: number;
		model: string;
		createdAt: string;
		caliber?: CaliberReportSummary;
		ds4?: Ds4ReportSummary;
	};

	const contextOptions = [
		{ hint: 'Short chat and smoke tests', label: '8k', value: 8192 },
		{ hint: 'Most coding and analysis sessions', label: '32k', value: 32768 },
		{ hint: 'Large files and longer conversations', label: '64k', value: 65536 },
		{ hint: 'Long-context models and serious repo work', label: '131k', value: 131072 },
		{ hint: 'Only when the model really supports it', label: '262k', value: 262144 }
	];
	const tabs: { id: TabId; label: string }[] = [
		{ id: 'overview', label: 'Overview' },
		{ id: 'run-tests', label: 'Run tests' },
		{ id: 'evidence', label: 'Evidence' },
		{ id: 'archive', label: 'Archive' }
	];
	const useCases: { id: UseCaseId; label: string; help: string }[] = [
		{
			help: 'Writing, questions and mixed daily work.',
			id: 'general',
			label: 'Everyday assistant'
		},
		{ help: 'Responsive conversation and instruction following.', id: 'chat', label: 'Fast chat' },
		{ help: 'Repository work, debugging and code generation.', id: 'coding', label: 'Coding' },
		{ help: 'Math, science and difficult decisions.', id: 'reasoning', label: 'Deep reasoning' },
		{ help: 'Grounded answers over your own documents.', id: 'rag', label: 'Documents / RAG' },
		{ help: 'Reliable structured output and tool calls.', id: 'tools', label: 'Agents & tools' },
		{
			help: 'Large files, long chats and needle retrieval.',
			id: 'long-context',
			label: 'Long context'
		}
	];
	const profileLabels: Record<ProfileId, { title: string; help: string }> = {
		efficiency: {
			help: 'Prefers strong throughput without wasting memory or power.',
			title: 'Best speed per watt/GB'
		},
		overall: {
			help: 'Balances speed, fit, context and KV quality. This is the default answer most users want.',
			title: 'Best daily driver'
		},
		safety: {
			help: 'Prefers configs that avoid memory pressure and risky spill.',
			title: 'Safest fit'
		},
		speed: {
			help: 'Ranks raw token speed first. Useful for autocomplete, simple chat and batch work.',
			title: 'Fastest response'
		}
	};
	const scopeOptions: Record<
		RunScope,
		{ title: string; help: string; workload: 'baseline' | 'all'; evaluator?: boolean }
	> = {
		evaluator: {
			evaluator: true,
			help: 'Runs DS4 plus product quality packs to discover where each selected model is strongest. This is a massive test and can take hours or several days.',
			title: 'Use-case Evaluator',
			workload: 'all'
		},
		quick: {
			help: 'Benchmarks normal generation only. Use this first to get a useful answer quickly.',
			title: 'Quick comparison',
			workload: 'baseline'
		},
		standard: {
			help: 'Adds long-prompt and KV-fill checks so the report explains context behavior.',
			title: 'Decision run',
			workload: 'all'
		}
	};
	const qualitySectors: Record<
		QualitySectorId,
		{ title: string; help: string; weights: Record<string, number> }
	> = {
		agents: {
			help: 'Structured tool use supported by coding, instruction following and reasoning.',
			title: 'Agents & tools',
			weights: { chat: 0.2, coding: 0.2, reasoning: 0.2, tools: 0.4 }
		},
		coding: {
			help: 'Programming accuracy reinforced by completion, tool use and problem solving.',
			title: 'Coding & engineering',
			weights: { coding: 0.4, fim: 0.15, general: 0.1, reasoning: 0.15, tools: 0.2 }
		},
		daily: {
			help: 'General knowledge reinforced by chat, reasoning, tools and grounded retrieval.',
			title: 'Everyday assistant',
			weights: { chat: 0.2, general: 0.35, rag: 0.1, reasoning: 0.2, tools: 0.15 }
		},
		long: {
			help: 'Needle retrieval and grounded documents supported by reasoning quality.',
			title: 'Long-context knowledge',
			weights: { 'long-context': 0.45, rag: 0.35, reasoning: 0.2 }
		},
		reasoning: {
			help: 'Logic and scientific problem solving, with supporting coding and knowledge evidence.',
			title: 'Deep reasoning',
			weights: { coding: 0.1, general: 0.2, 'long-context': 0.1, reasoning: 0.6 }
		},
		research: {
			help: 'Retrieval and long-context evidence combined with reasoning and general knowledge.',
			title: 'Research & documents',
			weights: { general: 0.15, 'long-context': 0.2, rag: 0.3, reasoning: 0.25, tools: 0.1 }
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
	const aliasCards = [
		{
			description: 'Balanced measured default',
			id: 'local-auto',
			tags: ['balanced', 'overall quality']
		},
		{
			description: 'Lowest interactive latency',
			id: 'local-fast',
			tags: ['latency', 'TTFT-aware']
		},
		{
			description: 'Highest qualified quality',
			id: 'local-best',
			tags: ['quality-first', 'quality-gated']
		},
		{ description: 'Coding and FIM qualified', id: 'local-code', tags: ['coding', 'FIM', 'tools'] },
		{
			description: 'Long-context retrieval qualified',
			id: 'local-long',
			tags: ['long context', 'retrieval']
		},
		{ description: 'Vision-capable artifact', id: 'local-vision', tags: ['vision', 'multimodal'] }
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

	let activeTab = $state<TabId>('overview');
	let evidenceMode = $state<EvidenceMode>('quality');
	let selectedDs4ReportId = $state('');
	let profile = $state<ProfileId>('overall');
	let useCase = $state<UseCaseId>('general');
	let shortlistSize = $state(4);
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
	let ds4QualityProfiles = $state<Record<string, Ds4QualityProfile>>({});
	let ds4Reports = $state<Ds4ReportSummary[]>([]);
	let qualitySector = $state<QualitySectorId>('daily');

	let fitSystem = $state<FitAdvisorSystem | null>(null);
	let reportScope = $state<ReportScope>('latest');
	let reportMetric = $state<ReportMetric>('eval');
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
	const pendingSelectedIds = $derived(selectedLocalIds.filter((id) => !hasHistoricModelResult(id)));
	const selectableModels = $derived(
		models.filter(
			(model) =>
				model.loadable !== false && Boolean(model.path) && isCaliberBenchmarkEligible(model)
		)
	);
	const planModels = $derived(uniqueStrings(plan.map((item) => item.model)).length);
	const targetContext = $derived(contextOptions.find((item) => item.value === contextSize));
	const selectedDs4ModelIds = $derived.by(() =>
		uniqueStrings(
			selectedLocalIds.map((id) => {
				const model = models.find((item) => item.id === id);

				return model?.configured_id ?? model?.configured_ids?.[0] ?? '';
			})
		)
	);
	const readyToRun = $derived(
		!running &&
			(runScope === 'evaluator' ? selectedDs4ModelIds.length > 0 : pendingSelectedIds.length > 0)
	);
	const qualityRankings = $derived.by(() =>
		rankQualityProfiles(ds4QualityProfiles, qualitySectors[qualitySector].weights)
	);
	const archiveEntries = $derived.by(buildArchiveEntries);
	const hasQualifiedEvidence = $derived(
		Object.values(ds4QualityProfiles).some((item) => Number(item.samples ?? 0) > 0) ||
			resultRows.some(canFitCaliberRow) ||
			reportRows.some(canFitCaliberRow)
	);
	const nextAction = $derived(nextActionText());
	const doctorData = $derived(asRecord(doctorSystem?.doctor));

	onMount(() => {
		void refreshAll();
		void loadFitSystem();
		void restoreActiveSweep();
		void refreshRouteEvents();
	});

	onDestroy(() => {
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

	function qualitySectorForUseCase(value: UseCaseId): QualitySectorId {
		if (value === 'coding') return 'coding';

		if (value === 'reasoning') return 'reasoning';

		if (value === 'rag') return 'research';

		if (value === 'tools') return 'agents';

		if (value === 'long-context') return 'long';

		return 'daily';
	}

	function rankQualityProfiles(
		profiles: Record<string, Ds4QualityProfile>,
		weights: Record<string, number>
	): Array<{
		artifactId: string;
		profile: Ds4QualityProfile;
		score: number;
		coverage: number;
		samples: number;
		mix: Array<{ pack: string; weight: number; evidence: Ds4QualityEvidence }>;
	}> {
		const totalWeight = Object.values(weights).reduce((sum, value) => sum + value, 0) || 1;

		return Object.entries(profiles)
			.map(([artifactId, profile]) => {
				const mix = Object.entries(weights)
					.map(([pack, weight]) => ({ evidence: profile.packs?.[pack], pack, weight }))
					.filter((row): row is { pack: string; weight: number; evidence: Ds4QualityEvidence } =>
						Boolean(row.evidence?.samples)
					);
				const coveredWeight = mix.reduce((sum, row) => sum + row.weight, 0);
				const weightedScore = mix.reduce((sum, row) => sum + row.evidence.score * row.weight, 0);
				const samples = mix.reduce((sum, row) => sum + row.evidence.samples, 0);
				const coverage = coveredWeight / totalWeight;
				const evidenceScore = coveredWeight > 0 ? weightedScore / coveredWeight : 0;
				const confidence = Math.min(1, samples / 12);
				const score = evidenceScore * (0.75 + coverage * 0.25) * (0.85 + confidence * 0.15);

				return { artifactId, coverage, mix, profile, samples, score };
			})
			.filter((row) => row.samples > 0)
			.sort(
				(a, b) =>
					b.score - a.score ||
					b.coverage - a.coverage ||
					b.samples - a.samples ||
					String(a.profile.name ?? a.artifactId).localeCompare(
						String(b.profile.name ?? b.artifactId)
					)
			);
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
			ctx: Math.max(1, ...okAnalyticsRows.map((item) => rowNum(item, ['ctx_size']))),
			eval: Math.max(1, ...okAnalyticsRows.map((item) => rowNum(item, ['eval_tps', 'tps']))),
			memory: Math.max(1, ...okAnalyticsRows.map(reportMemoryMib)),
			prompt: Math.max(1, ...okAnalyticsRows.map((item) => rowNum(item, ['prompt_tps'])))
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
				curve_samples: samples.length,
				eval_tps: medianValues(samples.map((row) => rowNum(row, ['eval_tps']))),
				prompt_tps: medianValues(samples.map((row) => rowNum(row, ['prompt_tps', 'eval_tps'])))
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
			return 'Needs a streaming Run tests campaign; synthetic llama-bench rows cannot become presets.';

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

		activeTab = 'run-tests';
		message = match
			? `${modelDisplayName(identity)} selected. Review scope and start a Decision run to unlock FIT.`
			: `Open Run tests and select ${modelDisplayName(identity)} for a streaming Decision run.`;
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
		const configuredId = model?.configured_id ?? model?.configured_ids?.[0] ?? '';
		const identities = new Set(
			[modelId, configuredId, configuredId ? '' : (model?.artifact_id ?? '')]
				.filter(Boolean)
				.map((value) => normalizeIdentity(String(value)))
		);
		const modelPath = (model?.path ?? '').toLowerCase().replaceAll('\\', '/');

		return resultRows.some((row) => {
			if (rowText(row, ['ok']) === 'false') return false;

			const rowIdentities = [
				rowText(row, ['model']),
				rowText(row, ['configured_id']),
				rowText(row, ['artifact_id']),
				rowText(row, ['id'])
			]
				.filter(Boolean)
				.map((value) => normalizeIdentity(String(value)));
			const rowPath = rowText(row, ['model_path', 'path']).toLowerCase().replaceAll('\\', '/');

			if (rowIdentities.some((identity) => identities.has(identity))) return true;

			return !configuredId && Boolean(modelPath) && rowPath === modelPath;
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

	function buildArchiveEntries(): ArchiveEntry[] {
		return [
			...reports.map(
				(report): ArchiveEntry => ({
					caliber: report,
					count: report.rows,
					createdAt: report.created_at,
					id: `caliber:${report.id}`,
					kind: 'performance',
					model: report.model || report.id,
					source: 'caliber',
					status: reportStatusLabel(report)
				})
			),
			...ds4Reports.map((report): ArchiveEntry => {
				const summary = report.summary ?? {};

				return {
					count: rowNum(summary, ['rows', 'cases', 'completed', 'total']),
					createdAt: report.updated_at || report.created_at,
					ds4: report,
					id: `ds4:${report.id}`,
					kind: report.kind === 'eval' ? 'quality' : 'performance',
					model: report.model_selector || report.id,
					source: 'ds4',
					status: report.status || (report.archive ? 'archived' : 'saved')
				};
			})
		].sort((a, b) => b.createdAt.localeCompare(a.createdAt));
	}

	async function openArchiveEntry(entry: ArchiveEntry) {
		if (entry.caliber) {
			await openReport(entry.caliber);

			return;
		}

		if (entry.ds4) {
			openEvidence(entry.ds4.kind === 'eval' ? 'quality' : 'performance', entry.ds4.id);
		}
	}

	function openEvidence(mode: EvidenceMode, reportId = '') {
		evidenceMode = mode;
		selectedDs4ReportId = reportId;
		activeTab = 'evidence';
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
			.slice(0, shortlistSize)
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

		if (runScope === 'evaluator' && selectedDs4ModelIds.length === 0)
			return 'The selected artifacts must be configured in Models before DS4 can load and evaluate them.';

		if (runScope === 'evaluator')
			return `Ready for ${selectedDs4ModelIds.length} model(s) x the complete DS4 and product quality suite. This can take hours or days.`;

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
			cfg: {
				context_candidates: [{ ctx: contextSize, kv: 'q8_0' }],
				hardware: fitSystem
					? {
							backend: fitSystem.backend,
							cpu_cores_physical: Math.max(1, Math.floor(fitSystem.cpu_cores / 2)),
							cpu_threads_logical: fitSystem.cpu_cores,
							gpus: fitSystem.gpus.map((gpu) => ({
								backend: gpu.backend,
								name: gpu.name,
								vram_driver_usable_mib: Math.round(gpu.vram_gb * 1024),
								vram_total_mib: Math.round(gpu.vram_gb * 1024)
							})),
							system_ram_available_mib: Math.round(fitSystem.available_ram_gb * 1024),
							unified_memory: fitSystem.unified_memory ?? false,
							vram_budget_mib: Math.round(fitSystem.total_gpu_vram_gb * 1024),
							vram_driver_usable_mib: Math.round(fitSystem.total_gpu_vram_gb * 1024)
						}
					: {},
				max_context_cap: contextSize,
				product_intent: {
					objective: profile,
					use_case: useCase
				},
				quality: {
					min_samples: 1,
					min_score: 0.5,
					pack: useCase === 'long-context' ? 'long-context' : useCase,
					required: true
				}
			},
			models: pendingSelectedIds,
			opts: { workloadSweep: scopeOptions[runScope].workload }
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

			activeTab = 'overview';
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
				activeTab = 'run-tests';
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
		const [modelsResult, reportsResult, resultsResult, systemResult, ds4Result] =
			await Promise.allSettled([
				CaliberAdvisorService.models(),
				CaliberAdvisorService.reports(),
				CaliberAdvisorService.results(),
				CaliberAdvisorService.system(),
				Ds4Service.listReports()
			]);
		const failures: string[] = [];

		if (modelsResult.status === 'fulfilled') models = modelsResult.value.data;
		else failures.push(`Models: ${String(modelsResult.reason)}`);

		if (reportsResult.status === 'fulfilled') {
			reports = reportsResult.value.data.sort((a, b) => b.created_at.localeCompare(a.created_at));
		} else failures.push(`Benchmark reports: ${String(reportsResult.reason)}`);

		if (resultsResult.status === 'fulfilled') results = resultsResult.value;
		else failures.push(`Recommendations: ${String(resultsResult.reason)}`);

		if (systemResult.status === 'fulfilled') doctorSystem = systemResult.value;
		else failures.push(`System diagnostics: ${String(systemResult.reason)}`);

		if (ds4Result.status === 'fulfilled') {
			ds4QualityProfiles = ds4Result.value.quality_profiles ?? {};
			ds4Reports = ds4Result.value.data;
		} else {
			ds4QualityProfiles = {};
			ds4Reports = [];
			failures.push(`Quality evidence: ${String(ds4Result.reason)}`);
		}

		error = failures.join(' | ');
		message = `${completedReports.length} completed benchmark reports, ${resultRows.length} measured rows`;
		loading = false;
	}

	async function previewPlan() {
		if (runScope === 'evaluator') {
			if (selectedDs4ModelIds.length === 0) {
				error =
					'No evaluator-ready model selected. Configure downloaded artifacts in Models first.';

				return;
			}

			plan = [];
			message = `${selectedDs4ModelIds.length} model(s) will run through the complete DS4 and product quality suite. Expect a long server-side run that can be resumed.`;

			return;
		}

		if (pendingSelectedIds.length === 0) {
			message = 'No benchmark needed: selected models are already in the historical archive.';
			activeTab = 'overview';

			return;
		}

		loading = true;
		error = '';
		try {
			const result = await CaliberAdvisorService.plan(payload());

			plan = result.plan;
			message = `${result.plan_count} configs planned across ${pendingSelectedIds.length} new model(s); ${selectedLocalIds.length - pendingSelectedIds.length} already archived.`;
			activeTab = 'run-tests';
		} catch (e) {
			error = e instanceof Error ? e.message : String(e);
		} finally {
			loading = false;
		}
	}

	async function startSweep() {
		if (!readyToRun) return;

		if (runScope === 'evaluator') {
			error = '';
			try {
				const started = await Ds4Service.runEval({
					max_tokens: 16000,
					model: selectedDs4ModelIds.join(', '),
					models: selectedDs4ModelIds,
					temperature: 0,
					thinking: true,
					thinking_budget_tokens: 16000,
					use_case: useCase
				});

				message = `Use-case evaluation ${started.id} started for ${selectedDs4ModelIds.length} model(s).`;
				openEvidence('quality');
			} catch (e) {
				error = e instanceof Error ? e.message : String(e);
			}

			return;
		}

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

	async function loadFitSystem() {
		try {
			fitSystem = await FitAdvisorService.system();
		} catch (e) {
			const detail = e instanceof Error ? e.message : String(e);

			error = [error, `Hardware fit: ${detail}`].filter(Boolean).join(' | ');
		}
	}
	async function openReport(report: CaliberReportSummary, switchTab = true) {
		error = '';
		try {
			selectedReport = await CaliberAdvisorService.report(report.id);
			selectedReportId = report.id;

			if (switchTab) activeTab = 'archive';
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
				load_now: loadAfterConfigure,
				model,
				report_id: rowText(row, ['report_id'], selectedReportId),
				row_id: rowText(row, ['id']),
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
			<a class="button-link primary" href="#/fit-advisor">
				Find models
				<ChevronRight size={16} />
			</a>

			<button onclick={() => (activeTab = 'archive')} type="button">
				<FileJson size={16} />
				Open archive
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

	<nav aria-label="Local LLM Autopilot sections" class="tabs">
		{#each tabs as tab (tab.id)}
			<button
				class:active={activeTab === tab.id}
				onclick={() => (tab.id === 'evidence' ? openEvidence(evidenceMode) : (activeTab = tab.id))}
				type="button"
			>
				{#if tab.id === 'overview'}<Activity size={16} />{/if}

				{#if tab.id === 'run-tests'}<Gauge size={16} />{/if}

				{#if tab.id === 'evidence'}<ClipboardCheck size={16} />{/if}

				{#if tab.id === 'archive'}<FileJson size={16} />{/if}
				{tab.label}
			</button>
		{/each}
	</nav>

	{#if activeTab === 'run-tests'}
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
							class:active={useCase === item.id}
							class="choice compact-choice"
							onclick={() => {
								useCase = item.id;
								qualitySector = qualitySectorForUseCase(item.id);
							}}
							type="button"
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
							class:active={profile === id}
							class="choice"
							onclick={() => (profile = id as ProfileId)}
							type="button"
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
						<h2>3. Pick benchmark scope</h2>

						<p>Start small; use deeper sweeps only when the winner needs diagnosis.</p>
					</div>
				</div>

				<div class="choice-grid">
					{#each Object.entries(scopeOptions) as [id, item] (id)}
						<button
							class:active={runScope === id}
							class="choice"
							onclick={() => (runScope = id as RunScope)}
							type="button"
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
						<h2>4. Select candidate models</h2>

						<p>
							{selectedLocalIds.length} selected · {pendingSelectedIds.length} need benchmarking · {selectedLocalIds.length -
								pendingSelectedIds.length} already archived.
						</p>
					</div>

					<div class="row-actions">
						<button onclick={selectRecommendedLocal} type="button">Hardware shortlist</button>

						<button onclick={selectAllLocal} type="button">All available</button>

						<button onclick={clearSelection} type="button">Clear</button>
					</div>
				</div>

				<div class="model-list">
					{#each selectableModels as model (model.id)}
						<button
							class:active={selectedLocalIds.includes(model.id)}
							class="model-option"
							onclick={() => toggleLocalModel(model.id)}
							type="button"
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
						<h2>5. Review and run</h2>

						<p>No command-line knowledge required. The report stores the technical details.</p>
					</div>
				</div>

				<div class="run-card">
					<div class="intent-controls">
						<label
							><span>Hardware shortlist size</span><select bind:value={shortlistSize}
								><option value={2}>2 models</option><option value={4}>4 models</option><option
									value={8}>8 models</option
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

							<dd>{runScope === 'evaluator' ? 'Full quality suite' : plan.length || '-'}</dd>
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
						<input bind:checked={loadAfterConfigure} type="checkbox" />

						<span>Load winner immediately after FIT</span>
					</label>

					<div class="button-row">
						<button
							disabled={selectedLocalIds.length === 0 || loading}
							onclick={previewPlan}
							type="button"
						>
							<Settings2 size={16} />
							{runScope === 'evaluator' ? 'Review evaluation' : 'Review plan'}
						</button>

						<button class="primary" disabled={!readyToRun} onclick={startSweep} type="button">
							<Play size={16} />
							{runScope === 'evaluator' ? 'Start use-case evaluation' : 'Start benchmark'}
						</button>

						{#if sweepIsLive(status)}
							<button
								class="danger"
								disabled={status?.cancel_requested}
								onclick={stopSweep}
								type="button"
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
								aria-label="Stop benchmark"
								class="icon-action danger"
								disabled={status?.cancel_requested}
								onclick={stopSweep}
								type="button"
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

	{#if activeTab === 'evidence'}
		<section class="panel evidence-header">
			<div>
				<span class="eyebrow">Measured local evidence</span>

				<h2>{evidenceMode === 'quality' ? 'Quality evaluation' : 'Performance and context'}</h2>

				<p>
					{evidenceMode === 'quality'
						? 'Reasoning, coding, knowledge, RAG, tool use and long-context task quality.'
						: 'Prompt processing, decode throughput, latency and context scaling on this hardware.'}
				</p>
			</div>

			<div aria-label="Evidence type" class="segmented evidence-tabs">
				<button
					class:active={evidenceMode === 'quality'}
					onclick={() => openEvidence('quality')}
					type="button">Quality</button
				>

				<button
					class:active={evidenceMode === 'performance'}
					onclick={() => openEvidence('performance')}
					type="button">Performance</button
				>
			</div>
		</section>

		<section class="panel evaluator-shell">
			{#if evidenceMode === 'quality'}
				<Ds4SuitePage
					embedded
					initialModels={selectedDs4ModelIds}
					initialReportId={selectedDs4ReportId}
					mode="eval"
				/>
			{:else}
				<Ds4SuitePage
					embedded
					initialModels={selectedDs4ModelIds}
					initialReportId={selectedDs4ReportId}
					mode="bench"
				/>
			{/if}
		</section>
	{/if}

	{#if activeTab === 'overview' || activeTab === 'archive'}
		<section class="reports-layout">
			{#if activeTab === 'archive'}
				<div class="panel">
					<div class="panel-head">
						<div>
							<h2>Evidence archive</h2>

							<p>Caliber campaigns and DS4 quality/performance reports, ordered on one timeline.</p>
						</div>

						<FileJson size={18} />
					</div>

					<div class="table reports-table">
						<div class="table-head">
							<span>Status</span>

							<span>Evidence</span>

							<span>Model/Campaign</span>

							<span>Date</span>

							<span></span>
						</div>

						{#each archiveEntries as entry (entry.id)}
							<div
								class:active={entry.caliber && selectedReportId === entry.caliber.id}
								class="table-row"
							>
								<button class="linkish" onclick={() => openArchiveEntry(entry)} type="button"
									>{entry.status}</button
								>

								<span>{entry.count || '-'}</span>

								<strong title={entry.model}
									>{modelDisplayName(entry.model)} - {entry.source === 'caliber'
										? 'Caliber performance'
										: `DS4 ${entry.kind}`}</strong
								>

								<span>{entry.createdAt}</span>

								<div class="row-actions">
									<button onclick={() => openArchiveEntry(entry)} type="button">Open</button>

									{#if entry.caliber}
										<button
											aria-label={`Delete ${entry.caliber.id}`}
											disabled={!canDeleteReport(entry.caliber)}
											onclick={() => deletePendingReport(entry.caliber!)}
											type="button"
										>
											<Trash2 size={15} />
										</button>
									{/if}
								</div>
							</div>
						{/each}
					</div>
				</div>
			{/if}

			{#if activeTab === 'overview'}
				<section class="panel quality-ranking-panel">
					<div class="panel-head">
						<div>
							<h2>Use-case capability ranking</h2>

							<p>
								DS4 evidence is combined across complementary skills. Coverage and sample count
								prevent a narrow high score from looking like a complete recommendation.
							</p>
						</div>

						<ClipboardCheck size={18} />
					</div>

					<div aria-label="Use-case quality category" class="quality-sector-picker">
						{#each Object.entries(qualitySectors) as [id, sector] (id)}
							<button
								class:active={qualitySector === id}
								onclick={() => (qualitySector = id as QualitySectorId)}
								type="button">{sector.title}</button
							>
						{/each}
					</div>

					<div class="quality-method">
						<strong>{qualitySectors[qualitySector].title}</strong>

						<span>{qualitySectors[qualitySector].help}</span>

						<small>
							Composite score = weighted measured packs, adjusted for evidence coverage and sample
							confidence. It is not a vendor or parameter-count estimate.
						</small>
					</div>

					{#if qualityRankings.length > 0}
						<div class="quality-ranking-list">
							{#each qualityRankings as row, index (row.artifactId)}
								<article>
									<b class="quality-rank">#{index + 1}</b>

									<div class="quality-model">
										<strong
											title={modelFullName(
												row.profile.name ?? row.profile.model_id ?? row.artifactId
											)}
											>{modelDisplayName(
												row.profile.name ?? row.profile.model_id ?? row.artifactId
											)}</strong
										>

										<span>{row.profile.variant || row.profile.artifact_id}</span>

										<div class="tag-list">
											{#each row.mix as item (item.pack)}
												<b
													>{item.pack}
													{Math.round(item.evidence.score * 100)}% · {item.evidence.samples} cases</b
												>
											{/each}
										</div>
									</div>

									<div class="quality-score">
										<strong>{Math.round(row.score * 100)}%</strong>

										<span>{Math.round(row.coverage * 100)}% skill coverage</span>

										<small>{row.samples} measured cases</small>
									</div>
								</article>
							{/each}
						</div>
					{:else}
						<div class="quality-empty">
							<div>
								<strong>No DS4 evidence for this capability mix yet.</strong>

								<span>Run the selected models through Use-case Evaluator to build the ranking.</span
								>
							</div>

							<button class="primary" onclick={() => openEvidence('quality')} type="button"
								>Open Evaluator</button
							>
						</div>
					{/if}
				</section>
			{/if}

			<div class="panel report-detail-panel">
				<div class="panel-head">
					<div>
						<h2>{activeTab === 'archive' ? 'Report detail' : 'Decision center'}</h2>

						<p>
							{selectedReportId ||
								(activeTab === 'archive' ? 'Select a report' : 'Latest compatible evidence')}
						</p>
					</div>

					{#if selectedReport}
						<button
							onclick={() => {
								selectedReport = null;
								selectedReportId = '';
							}}
							type="button"
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
							<button onclick={() => deletePendingReport(selectedReportSummary()!)} type="button">
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
										class="primary"
										disabled={!canFitCaliberRow(bestWinner)}
										onclick={() => configureCaliberRow(bestWinner)}
										type="button"><CheckCircle2 size={15} />Apply known-good preset</button
									>

									{#if !canFitCaliberRow(bestWinner)}
										<div class="fit-blocked">
											<span>{fitBlockReason(bestWinner)}</span>

											<button onclick={() => selectRowForTest(bestWinner)} type="button"
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
									<button class="inline-link" onclick={() => openEvidence('quality')} type="button"
										>Use-case Evaluator</button
									>.</span
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
									<button class:active={true} type="button">Selected report</button>
								{:else}
									<button
										class:active={reportScope === 'latest'}
										onclick={() => (reportScope = 'latest')}
										type="button">Latest campaign</button
									>

									<button
										class:active={reportScope === 'all'}
										onclick={() => (reportScope = 'all')}
										type="button">Compatible history</button
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
										class:active={profile === id}
										onclick={() => (profile = id as ProfileId)}
										type="button">{item.title}</button
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
										class:active={reportMetric === metric}
										onclick={() => (reportMetric = metric)}
										type="button">{reportMetricLabel(metric)}</button
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
										aria-label="Memory versus latency"
										class="report-scatter"
										role="img"
										viewBox="0 0 980 360"
									>
										<line class="axis" x1="70" x2="930" y1="312" y2="312" />

										<line class="axis" x1="70" x2="70" y1="54" y2="312" />

										{#each scatterTickRatios as ratio (ratio)}
											{@const x = 70 + ratio * 860}
											{@const y = 312 - ratio * 258}
											<line class="grid-line" x1={x} x2={x} y1="54" y2="312" />

											<line class="grid-line" x1="70" x2="930" y1={y} y2={y} />

											<text class="tick-label" text-anchor="middle" {x} y="330"
												>{fmtNumber(
													scatterTimeTick(ratio),
													scatterTimeTick(ratio) < 10 ? 1 : 0
												)}s</text
											>

											<text class="tick-label" text-anchor="end" x="64" y={y + 4}
												>{ratio === 0 ? '0' : fmtMib(scatterMemoryTick(ratio))}</text
											>
										{/each}

										{#if reportVramBudgetMib > 0 && reportVramBudgetMib <= reportMaxMemory * 1.05}
											<line
												class="budget"
												x1="70"
												x2="930"
												y1={reportBudgetY()}
												y2={reportBudgetY()}
											/>

											<text class="budget-label" text-anchor="end" x="924" y={reportBudgetY() - 6}
												>VRAM budget {fmtMib(reportVramBudgetMib)}</text
											>
										{/if}

										<text x="420" y="352">Total request time (log scale)</text>

										<text x="16" y="46">Peak memory</text>

										{#each reportScatterRows.slice(0, 360) as row, index (`${index}-${rowIdentity(row)}`)}
											<circle
												class:candidate={isReportCandidate(row)}
												class={`dot-${index % 5}`}
												cx={reportScatterX(row)}
												cy={reportScatterY(row)}
												r={isReportCandidate(row) ? 5.8 : 4}
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
										aria-label="Tuned versus vanilla radar chart"
										class="radar-chart"
										role="img"
										viewBox="0 0 220 210"
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
											aria-label="Streaming telemetry timeline"
											class="timeline-chart"
											role="img"
											viewBox="0 0 760 280"
										>
											<line x1="44" x2="720" y1="250" y2="250" /><line
												x1="44"
												x2="44"
												y1="60"
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
														disabled={!canFitCaliberRow(group.winner as CaliberRow)}
														onclick={() => configureCaliberRow(group.winner as CaliberRow)}
														title={canFitCaliberRow(group.winner as CaliberRow)
															? 'Apply measured winner'
															: fitBlockReason(group.winner as CaliberRow)}
														type="button"
													>
														<CheckCircle2 size={14} />
														FIT winner
													</button>

													{#if !canFitCaliberRow(group.winner as CaliberRow)}
														<button
															onclick={() => selectRowForTest(group.winner as CaliberRow)}
															type="button">Test for FIT</button
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
													class:winner={Boolean(group.winner) &&
														rowIdentity(group.winner as CaliberRow) === rowIdentity(row)}
													class="table-row"
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

	{#if activeTab === 'overview' && hasQualifiedEvidence}
		<details class="advanced-shell">
			<summary><Route size={16} />Advanced: qualified local router</summary>

			<section class="router-hero panel">
				<div>
					<span class="eyebrow">Stable virtual models</span>

					<h2>Ask for an outcome, not a filename</h2>

					<p>
						The local router filters by context, quality and features, then accounts for load cost
						and keeps a suitable resident model when possible.
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
									>{fmtNumber(Number(decision.quality ?? 0) * 100, 0)}% quality - {fmtTokens(
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

						<button onclick={refreshRouteEvents} type="button"
							><RefreshCw size={15} />Refresh</button
						>
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
		</details>
	{/if}

	{#if activeTab === 'overview'}
		<section class="panel diagnostics-header">
			<div>
				<span class="eyebrow">Diagnostics</span>

				<h2>System readiness</h2>

				<p>Health checks stay visible without competing with the primary workflow.</p>
			</div>

			<button disabled={loading} onclick={refreshAll} type="button"
				><RefreshCw size={15} />Refresh diagnostics</button
			>
		</section>

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
				<span>Models</span><strong
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
							Use the expert <button
								class="inline-link"
								onclick={() => openEvidence('quality')}
								type="button">quality evaluator</button
							>
							or
							<button class="inline-link" onclick={() => openEvidence('performance')} type="button"
								>context bench</button
							>
							when diagnostics report missing evidence.
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
	.button-link,
	input,
	select {
		min-height: 38px;
		border: 1px solid var(--border);
		border-radius: var(--radius);
		background: var(--background);
		color: inherit;
	}

	button,
	.button-link {
		display: inline-flex;
		align-items: center;
		justify-content: center;
		gap: 8px;
		padding: 0 12px;
		text-decoration: none;
		cursor: pointer;
	}

	button.primary,
	.button-link.primary {
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

	.check input[type='checkbox'] {
		flex: 0 0 18px;
		width: 18px;
		height: 18px;
		min-height: 18px;
		margin: 0;
		padding: 0;
		accent-color: var(--caliber-accent);
	}

	.tabs,
	.controls {
		align-items: center;
		padding: 8px;
	}

	.evidence-header,
	.diagnostics-header {
		display: flex;
		align-items: center;
		justify-content: space-between;
		gap: 16px;
		padding: 14px;
	}

	.evidence-header > div:first-child,
	.diagnostics-header > div:first-child {
		display: grid;
		gap: 5px;
	}

	.evidence-tabs {
		flex: 0 0 auto;
	}

	.advanced-shell {
		border: 1px solid var(--border);
		border-radius: var(--radius);
		background: var(--card);
	}

	.advanced-shell > summary {
		display: flex;
		align-items: center;
		gap: 8px;
		padding: 12px 14px;
		font-weight: 650;
		cursor: pointer;
	}

	.advanced-shell[open] > summary {
		border-bottom: 1px solid var(--border);
	}

	.advanced-shell .router-hero {
		border: 0;
		border-radius: 0 0 var(--radius) var(--radius);
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
		width: 100%;
		min-height: 98px;
		align-items: start;
		align-content: start;
		justify-content: stretch;
		justify-items: start;
		gap: 8px;
		padding: 12px;
		text-align: left;
	}

	.choice > strong,
	.choice > span {
		width: 100%;
		margin: 0;
		padding: 0;
		text-align: left;
	}

	.choice > span {
		line-height: 1.35;
	}

	.compact-choice {
		min-height: 78px;
	}

	.intent-controls {
		display: grid;
		grid-template-columns: 1fr;
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

	.reports-table .table-head,
	.reports-table .table-row {
		grid-template-columns: 90px 70px minmax(220px, 1fr) 180px minmax(110px, max-content);
	}

	.table-row.active {
		background: color-mix(in oklch, var(--muted) 60%, transparent);
	}

	.table-row strong {
		overflow: hidden;
		text-overflow: ellipsis;
		white-space: nowrap;
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

	.progress {
		width: 100%;
		height: 10px;
		overflow: hidden;
		border-radius: var(--radius);
		background: var(--muted);
	}

	.progress span {
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

	.evaluator-shell {
		overflow: visible;
		padding: 0 16px 16px;
	}

	.quality-sector-picker {
		display: flex;
		flex-wrap: wrap;
		gap: 8px;
		padding: 12px 12px 0;
	}

	.quality-method {
		display: grid;
		gap: 4px;
		margin: 12px;
		border-left: 3px solid var(--caliber-accent);
		background: color-mix(in oklch, var(--muted) 45%, transparent);
		padding: 10px 12px;
	}

	.quality-ranking-list {
		display: grid;
		padding: 0 12px 12px;
	}

	.quality-ranking-list article {
		display: grid;
		grid-template-columns: 46px minmax(0, 1fr) 150px;
		gap: 12px;
		align-items: center;
		border-top: 1px solid var(--border);
		padding: 12px 0;
	}

	.quality-rank {
		color: var(--caliber-accent);
		font-size: 16px;
	}

	.quality-model,
	.quality-score,
	.quality-empty > div {
		display: grid;
		gap: 5px;
		min-width: 0;
	}

	.quality-score {
		text-align: right;
	}

	.quality-score strong {
		font-size: 24px;
	}

	.quality-empty {
		display: flex;
		align-items: center;
		justify-content: space-between;
		gap: 16px;
		padding: 16px;
	}

	.inline-link {
		display: inline;
		min-height: 0;
		border: 0;
		background: transparent;
		padding: 0;
		color: var(--primary);
		text-decoration: underline;
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
		.route-row,
		.quality-ranking-list article {
			grid-template-columns: 1fr;
		}

		.quality-score {
			text-align: left;
		}

		.table-head {
			display: none;
		}

		.plan-table .table-row,
		.reports-table .table-row,
		.config-matrix .table-row,
		.leader-row,
		.throughput-row,
		.analytics-model summary {
			grid-template-columns: 1fr;
		}
	}
</style>
