<script lang="ts">
	import { onMount, tick } from 'svelte';
	import { SvelteMap, SvelteSet } from 'svelte/reactivity';
	import {
		Activity,
		BarChart3,
		ClipboardCheck,
		RefreshCw,
		Square,
		SquareCheck,
		Trash2,
		Trophy,
		X
	} from '@lucide/svelte';
	import {
		Ds4Service,
		type Ds4Event,
		type Ds4JobSnapshot,
		type Ds4Model,
		type Ds4Report,
		type Ds4ReportSummary
	} from '$lib/services/ds4.service';

	interface Props {
		mode: 'eval' | 'bench';
	}

	interface EvalRow {
		case_index?: number;
		source?: string;
		id?: string;
		domain?: string;
		title?: string;
		model?: string;
		expected?: string;
		got?: string;
		pass?: boolean;
		reasoning_tokens?: number;
		content_tokens?: number;
		tokens_per_second?: number;
		elapsed_ms?: number;
	}

	interface BenchRow {
		ctx?: number;
		model?: string;
		prompt_tokens_per_second?: number;
		decode_tokens_per_second?: number;
		prompt_seconds?: number;
		decode_seconds?: number;
		gen_tokens?: number;
	}

	interface EvalModelSummary {
		model: string;
		pass: number;
		fail: number;
		total: number;
		score: number;
		avg_tokens_per_second: number;
	}

	interface EvalSectorSummary extends EvalModelSummary {
		sector: string;
	}

	interface EvalRaceSectorSummary {
		sector: string;
		left?: EvalSectorSummary;
		right?: EvalSectorSummary;
		delta: number;
	}

	let { mode }: Props = $props();

	const isEval = $derived(mode === 'eval');
	const title = $derived(isEval ? 'DS4-Eval' : 'DS4-Bench');
	const subtitle = $derived(
		isEval
			? 'Regression suite: 92 logic, reasoning and cybersecurity cases.'
			: 'Performance suite: prompt processing, decode throughput and context scaling.'
	);

	let models = $state<Ds4Model[]>([]);
	let reports = $state<Ds4ReportSummary[]>([]);
	let selectedModels = $state<string[]>(['ALL']);
	let isLoadingModels = $state(false);
	let isRunning = $state(false);
	let isResuming = $state(false);
	let isStopping = $state(false);
	let error = $state('');
	let jobId = $state('');
	let jobStatus = $state('idle');
	let jobCurrent = $state(0);
	let jobTotal = $state(0);
	let lastSeq = $state(0);
	let terminalText = $state('');
	let terminalRef = $state<HTMLDivElement | null>(null);
	let activeReport = $state<Ds4Report | null>(null);
	let selectedReportId = $state('');
	let evalRows = $state<EvalRow[]>([]);
	let benchRows = $state<BenchRow[]>([]);
	let archiveEvalRows = $state<EvalRow[]>([]);
	let archiveEvalReportCount = $state(0);
	let otherActiveJob = $state<Ds4JobSnapshot | null>(null);
	let streamController: AbortController | null = null;
	let reportMode = $state<'single' | 'race'>('single');
	let singleReportModel = $state('');
	let raceModelA = $state('');
	let raceModelB = $state('');

	let maxTokens = $state(16000);
	let thinkingBudget = $state(16000);
	let evalLimit = $state(0);
	let thinking = $state(true);
	let temperature = $state(0);

	let ctxStart = $state(2048);
	let ctxMax = $state(131072);
	let ctxStep = $state(4096);
	let genTokens = $state(64);

	const progressPercent = $derived(jobTotal > 0 ? Math.round((jobCurrent / jobTotal) * 100) : 0);
	const terminalHtml = $derived(ansiToHtml(terminalText));
	const isAllSelected = $derived(selectedModels.includes('ALL'));
	const selectedModelLabel = $derived(isAllSelected ? 'ALL' : selectedModels.join(', '));
	const concreteModelCount = $derived(models.filter((model) => model.id !== 'ALL').length);
	const selectedModelCount = $derived(isAllSelected ? concreteModelCount : selectedModels.length);
	const canStart = $derived(!isRunning && selectedModels.length > 0 && !otherActiveJob);
	const displayEvalRows = $derived.by(() => {
		if (activeReport?.kind === 'eval' && Array.isArray(activeReport.results)) {
			return activeReport.results as EvalRow[];
		}
		return evalRows;
	});
	const displayBenchRows = $derived.by(() => {
		if (activeReport?.kind === 'bench' && Array.isArray(activeReport.results)) {
			return activeReport.results as BenchRow[];
		}
		return benchRows;
	});
	const comparisonEvalRows = $derived.by(() => archiveEvalRows);
	const reportModels = $derived.by(() => uniqueModels(comparisonEvalRows));
	const modelReportRows = $derived.by(() => buildModelSummaries(comparisonEvalRows));
	const sectorReportRows = $derived.by(() => buildSectorSummaries(comparisonEvalRows));
	const singleModelSummary = $derived.by(() =>
		modelReportRows.find((row) => row.model === singleReportModel)
	);
	const singleSectorRows = $derived.by(() =>
		sectorReportRows.filter((row) => row.model === singleReportModel)
	);
	const raceModelSummaryA = $derived.by(() =>
		modelReportRows.find((row) => row.model === raceModelA)
	);
	const raceModelSummaryB = $derived.by(() =>
		modelReportRows.find((row) => row.model === raceModelB)
	);
	const raceSectorRows = $derived.by(() =>
		buildRaceSectorRows(sectorReportRows, raceModelA, raceModelB)
	);
	const evalPass = $derived.by(() => {
		if (activeReport?.kind === 'eval') return summaryNumber('pass');
		return evalRows.filter((row) => row.pass).length;
	});
	const evalFail = $derived.by(() => {
		if (activeReport?.kind === 'eval') return summaryNumber('fail');
		return evalRows.filter((row) => row.pass === false).length;
	});
	const evalTotal = $derived(Math.max(1, evalPass + evalFail));
	const bestPromptTps = $derived.by(() => {
		if (activeReport?.kind === 'bench') return summaryNumber('best_prompt_tokens_per_second');
		return Math.max(0, ...benchRows.map((row) => num(row.prompt_tokens_per_second)));
	});
	const bestDecodeTps = $derived.by(() => {
		if (activeReport?.kind === 'bench') return summaryNumber('best_decode_tokens_per_second');
		return Math.max(0, ...benchRows.map((row) => num(row.decode_tokens_per_second)));
	});
	const canResumeSelectedReport = $derived.by(() => {
		if (!activeReport || activeReport.kind !== mode || isRunning || otherActiveJob) return false;
		return activeReport.resumable === true || activeReport.status === 'paused';
	});
	const canDeleteSelectedReport = $derived.by(() => {
		if (!activeReport || activeReport.kind !== mode || isRunning) return false;
		return activeReport.resumable === true || activeReport.status === 'paused';
	});

	onMount(() => {
		void initialize();
		return () => {
			streamController?.abort();
		};
	});

	$effect(() => {
		const ids = reportModels;
		if (ids.length === 0) {
			singleReportModel = '';
			raceModelA = '';
			raceModelB = '';
			reportMode = 'single';
			return;
		}

		if (!ids.includes(singleReportModel)) {
			singleReportModel = ids[0];
		}
		if (!ids.includes(raceModelA)) {
			raceModelA = ids[0];
		}
		if (ids.length > 1 && (!ids.includes(raceModelB) || raceModelB === raceModelA)) {
			raceModelB = ids.find((id) => id !== raceModelA) ?? ids[1];
		}
		if (ids.length < 2) {
			raceModelB = '';
			reportMode = 'single';
		}
	});

	async function initialize() {
		await refreshModels();
		await refreshReports();
		await resumeActiveJob();
	}

	function storageKey(): string {
		return 'llama-ds4-' + mode + '-job-id';
	}

	function escapeHtml(text: string): string {
		return text
			.replaceAll('&', '&amp;')
			.replaceAll('<', '&lt;')
			.replaceAll('>', '&gt;')
			.replaceAll('"', '&quot;')
			.replaceAll("'", '&#039;');
	}

	function ansiClass(codes: string): string {
		const parts = codes.split(';');
		const classes: string[] = [];
		if (parts.includes('1')) classes.push('font-bold');
		if (parts.includes('2')) classes.push('text-foreground/55');
		if (parts.includes('31')) classes.push('text-red-400');
		if (parts.includes('32')) classes.push('text-emerald-400');
		if (parts.includes('33')) classes.push('text-yellow-300');
		if (parts.includes('36')) classes.push('text-cyan-300');
		return classes.join(' ');
	}

	function ansiToHtml(text: string): string {
		let output = '';
		let open = false;
		const pattern = new RegExp(`${String.fromCharCode(27)}\\[([0-9;]*)m`, 'g');
		let lastIndex = 0;
		let match: RegExpExecArray | null;

		while ((match = pattern.exec(text)) !== null) {
			output += escapeHtml(text.slice(lastIndex, match.index));
			if (open) {
				output += '</span>';
				open = false;
			}
			const codes = match[1] || '0';
			if (codes !== '0') {
				const klass = ansiClass(codes);
				if (klass) {
					output += '<span class="' + klass + '">';
					open = true;
				}
			}
			lastIndex = pattern.lastIndex;
		}

		output += escapeHtml(text.slice(lastIndex));
		if (open) output += '</span>';
		return output;
	}

	function num(value: unknown): number {
		return typeof value === 'number' && Number.isFinite(value) ? value : 0;
	}

	function scorePercent(pass: number, total: number): number {
		return total > 0 ? Math.round((pass / total) * 100) : 0;
	}

	function formatDelta(value: number): string {
		if (!Number.isFinite(value)) return '0%';
		return (value > 0 ? '+' : '') + Math.round(value) + '%';
	}

	function uniqueModels(rows: EvalRow[]): string[] {
		const seen = new SvelteSet<string>();
		for (const row of rows) {
			const model = String(row.model || '').trim();
			if (model) seen.add(model);
		}
		return [...seen].sort((a, b) => a.localeCompare(b));
	}

	function sectorsForRow(row: EvalRow): string[] {
		const source = String(row.source || '')
			.trim()
			.toLowerCase();
		if (source === 'compsec') return ['Cybersecurity'];

		const raw = String(row.domain || '').trim();
		if (!raw) return ['General'];
		const sectors = raw
			.split(',')
			.map((item) => item.trim())
			.filter(Boolean);
		return sectors.length > 0 ? sectors : ['General'];
	}

	function addSummary(
		map: Map<string, EvalSectorSummary>,
		key: string,
		model: string,
		pass: boolean,
		tokensPerSecond: number,
		sector = ''
	) {
		const row =
			map.get(key) ??
			({
				model,
				sector,
				pass: 0,
				fail: 0,
				total: 0,
				score: 0,
				avg_tokens_per_second: 0
			} satisfies EvalSectorSummary);
		if (pass) row.pass += 1;
		else row.fail += 1;
		row.total += 1;
		row.avg_tokens_per_second += tokensPerSecond;
		map.set(key, row);
	}

	function finalizeSummary<T extends EvalModelSummary>(row: T): T {
		row.score = scorePercent(row.pass, row.total);
		row.avg_tokens_per_second = row.total > 0 ? row.avg_tokens_per_second / row.total : 0;
		return row;
	}

	function buildModelSummaries(rows: EvalRow[]): EvalModelSummary[] {
		const byModel = new Map<string, EvalSectorSummary>();
		for (const row of rows) {
			const model = String(row.model || '').trim();
			if (!model) continue;
			addSummary(byModel, model, model, row.pass === true, num(row.tokens_per_second));
		}
		return [...byModel.values()]
			.map(finalizeSummary)
			.sort((a, b) => b.score - a.score || b.total - a.total || a.model.localeCompare(b.model));
	}

	function buildSectorSummaries(rows: EvalRow[]): EvalSectorSummary[] {
		const bySector = new Map<string, EvalSectorSummary>();
		for (const row of rows) {
			const model = String(row.model || '').trim();
			if (!model) continue;
			for (const sector of sectorsForRow(row)) {
				addSummary(
					bySector,
					model + '\t' + sector,
					model,
					row.pass === true,
					num(row.tokens_per_second),
					sector
				);
			}
		}
		return [...bySector.values()]
			.map(finalizeSummary)
			.sort((a, b) => a.sector.localeCompare(b.sector) || a.model.localeCompare(b.model));
	}

	function buildRaceSectorRows(
		rows: EvalSectorSummary[],
		leftModel: string,
		rightModel: string
	): EvalRaceSectorSummary[] {
		if (!leftModel || !rightModel || leftModel === rightModel) return [];
		const sectors = new SvelteSet<string>();
		for (const row of rows) {
			if (row.model === leftModel || row.model === rightModel) sectors.add(row.sector);
		}
		return [...sectors]
			.sort((a, b) => a.localeCompare(b))
			.map((sector) => {
				const left = rows.find((row) => row.model === leftModel && row.sector === sector);
				const right = rows.find((row) => row.model === rightModel && row.sector === sector);
				return {
					sector,
					left,
					right,
					delta: (left?.score ?? 0) - (right?.score ?? 0)
				};
			});
	}

	function bestEvalRowsFromReports(completedReports: Ds4Report[]): EvalRow[] {
		const bestByModel = new SvelteMap<
			string,
			{ score: number; total: number; updated: string; rows: EvalRow[] }
		>();

		for (const report of completedReports) {
			if (report.kind !== 'eval' || report.status !== 'completed' || !Array.isArray(report.results))
				continue;
			const rowsByModel = new SvelteMap<string, EvalRow[]>();
			for (const row of report.results as EvalRow[]) {
				const model = String(row.model || '').trim();
				if (!model) continue;
				const rows = rowsByModel.get(model) ?? [];
				rows.push(row);
				rowsByModel.set(model, rows);
			}

			for (const [model, rows] of rowsByModel) {
				const pass = rows.filter((row) => row.pass === true).length;
				const total = rows.filter((row) => row.pass === true || row.pass === false).length;
				const score = total > 0 ? pass / total : 0;
				const updated = String(report.updated_at || report.created_at || '');
				const current = bestByModel.get(model);
				if (
					!current ||
					score > current.score ||
					(score === current.score && total > current.total) ||
					(score === current.score && total === current.total && updated > current.updated)
				) {
					bestByModel.set(model, { score, total, updated, rows });
				}
			}
		}

		return [...bestByModel.values()].flatMap((entry) => entry.rows);
	}

	function basename(path?: string): string {
		if (!path) return '';
		const normalized = path.replaceAll('\\', '/');
		return normalized.slice(normalized.lastIndexOf('/') + 1);
	}

	function modelLabel(model: Ds4Model): string {
		if (model.id === 'ALL') return 'ALL models';
		const file = basename(model.path);
		const tags = model.tags?.length ? ' · ' + model.tags.join(', ') : '';
		return model.id + (file ? ' · ' + file : '') + tags;
	}

	function reportLabel(report: Ds4ReportSummary): string {
		const status =
			report.resumable || report.status === 'paused'
				? '[resume] '
				: report.status === 'completed'
					? '[archive] '
					: report.status
						? '[' + report.status + '] '
						: '';
		return status + report.created_at + ' · ' + report.model_selector;
	}

	function summaryNumber(key: string): number {
		return activeReport?.summary ? num(activeReport.summary[key]) : 0;
	}

	function appendTerminal(text: string) {
		terminalText += text;
		void tick().then(() => {
			if (terminalRef) {
				terminalRef.scrollTop = terminalRef.scrollHeight;
			}
		});
	}

	function normalizeSelection(): string[] {
		if (selectedModels.includes('ALL')) return ['ALL'];
		const available = new Set(models.map((model) => model.id));
		return selectedModels.filter((id) => available.has(id));
	}

	function isModelSelected(id: string): boolean {
		return selectedModels.includes(id);
	}

	function toggleModel(id: string) {
		if (isRunning) return;
		if (id === 'ALL') {
			selectedModels = ['ALL'];
			return;
		}
		const withoutAll = selectedModels.filter((value) => value !== 'ALL');
		if (withoutAll.includes(id)) {
			selectedModels = withoutAll.filter((value) => value !== id);
		} else {
			selectedModels = [...withoutAll, id];
		}
	}

	function selectAllModels() {
		if (!isRunning) selectedModels = ['ALL'];
	}

	function clearModelSelection() {
		if (!isRunning) selectedModels = [];
	}

	async function refreshModels(reload = false) {
		isLoadingModels = true;
		try {
			const response = await Ds4Service.listModels(reload);
			models = response.data;
			const available = new Set(models.map((model) => model.id));
			if (selectedModels.includes('ALL')) {
				selectedModels = ['ALL'];
			} else {
				selectedModels = selectedModels.filter((id) => available.has(id));
				if (selectedModels.length === 0 && available.has('ALL')) selectedModels = ['ALL'];
			}
		} catch (e) {
			error = e instanceof Error ? e.message : String(e);
		} finally {
			isLoadingModels = false;
		}
	}

	async function refreshReports() {
		try {
			const response = await Ds4Service.listReports();
			reports = response.data
				.filter((report) => report.kind === mode)
				.sort((a, b) => b.created_at.localeCompare(a.created_at));
			await refreshArchiveReports();
		} catch {
			reports = [];
			archiveEvalRows = [];
			archiveEvalReportCount = 0;
		}
	}

	async function refreshArchiveReports() {
		if (!isEval) {
			archiveEvalRows = [];
			archiveEvalReportCount = 0;
			return;
		}
		const completed = reports.filter(
			(report) =>
				report.kind === 'eval' && report.status === 'completed' && report.resumable !== true
		);
		const loaded = await Promise.all(
			completed.map(async (report) => {
				try {
					return await Ds4Service.getReport(report.id);
				} catch {
					return null;
				}
			})
		);
		const validReports = loaded.filter((report): report is Ds4Report => Boolean(report));
		archiveEvalRows = bestEvalRowsFromReports(validReports);
		archiveEvalReportCount = validReports.length;
	}

	function setSnapshot(snapshot: Ds4JobSnapshot) {
		jobId = snapshot.id;
		jobStatus = snapshot.status;
		jobCurrent = snapshot.current;
		jobTotal = snapshot.total;
		lastSeq = Math.max(lastSeq, (snapshot.next_seq ?? 1) - 1);
		if (snapshot.error) error = snapshot.error;
		if (snapshot.report && Object.keys(snapshot.report).length > 0) {
			activeReport = snapshot.report;
		}
	}

	function resetLiveState() {
		jobId = '';
		jobStatus = 'queued';
		jobCurrent = 0;
		jobTotal = 0;
		lastSeq = 0;
		terminalText = '';
		activeReport = null;
		selectedReportId = '';
		evalRows = [];
		benchRows = [];
		error = '';
	}

	function handleEvent(event: Ds4Event) {
		lastSeq = Math.max(lastSeq, event.data.seq ?? 0);
		jobStatus = event.data.status ?? jobStatus;
		jobCurrent = event.data.current ?? jobCurrent;
		jobTotal = event.data.total ?? jobTotal;

		if (event.event === 'log' && typeof event.data.text === 'string') {
			appendTerminal(event.data.text);
		}
		if (event.event === 'case') {
			evalRows = [...evalRows, event.data as EvalRow].slice(-500);
		}
		if (event.event === 'bench-row') {
			benchRows = [...benchRows, event.data as BenchRow].slice(-500);
		}
		if (event.event === 'done' && event.data.error) {
			error = String(event.data.error);
		}
	}

	async function resumeActiveJob() {
		isResuming = true;
		try {
			const active = await Ds4Service.getActiveJob(mode);
			if (active.active && active.job) {
				if (active.job.kind === mode) {
					void attachJob(active.job.id, true, true);
				} else {
					otherActiveJob = active.job;
				}
				return;
			}

			const stored = localStorage.getItem(storageKey());
			if (!stored) return;
			const snapshot = await Ds4Service.getJob(stored);
			if (snapshot.kind === mode && !snapshot.finished) {
				void attachJob(snapshot.id, true, true);
			} else {
				localStorage.removeItem(storageKey());
			}
		} catch {
			localStorage.removeItem(storageKey());
		} finally {
			isResuming = false;
		}
	}

	async function attachJob(id: string, replay: boolean, resumed: boolean) {
		streamController?.abort();
		const controller = new AbortController();
		streamController = controller;
		isRunning = true;
		otherActiveJob = null;
		resetLiveState();
		jobId = id;
		localStorage.setItem(storageKey(), id);
		if (resumed) appendTerminal('Resumed ' + title + ' job ' + id + '\n');

		let finalSnapshot: Ds4JobSnapshot | null = null;
		try {
			const snapshot = await Ds4Service.getJob(id);
			setSnapshot(snapshot);
			await Ds4Service.streamJob(id, handleEvent, controller.signal, replay ? 0 : lastSeq);
			finalSnapshot = await Ds4Service.getJob(id);
			setSnapshot(finalSnapshot);
			await refreshReports();
		} catch (e) {
			if (!(e instanceof DOMException && e.name === 'AbortError')) {
				error = e instanceof Error ? e.message : String(e);
				appendTerminal('\nERROR: ' + error + '\n');
			}
		} finally {
			if (streamController === controller && !controller.signal.aborted) {
				isRunning = finalSnapshot ? !finalSnapshot.finished : false;
				if (!isRunning) localStorage.removeItem(storageKey());
			}
		}
	}

	async function startTest() {
		const selected = normalizeSelection();
		if (selected.length === 0) return;
		streamController?.abort();
		resetLiveState();
		isRunning = true;
		const model = selected.includes('ALL') ? 'ALL' : selected.join(', ');

		try {
			const start = isEval
				? await Ds4Service.runEval({
						model,
						models: selected,
						max_tokens: maxTokens,
						thinking_budget_tokens: thinkingBudget,
						thinking,
						temperature,
						limit: evalLimit > 0 ? evalLimit : undefined
					})
				: await Ds4Service.runBench({
						model,
						models: selected,
						ctx_start: ctxStart,
						ctx_max: ctxMax,
						ctx_step: ctxStep,
						gen_tokens: genTokens
					});

			appendTerminal('Started ' + title + ' job ' + start.id + '\n');
			await attachJob(start.id, true, false);
		} catch (e) {
			error = e instanceof Error ? e.message : String(e);
			appendTerminal('\nERROR: ' + error + '\n');
			isRunning = false;
		}
	}

	async function stopTest() {
		if (!jobId && !isRunning) return;
		isStopping = true;
		try {
			const snapshot = await Ds4Service.stopJob(jobId || undefined);
			setSnapshot(snapshot);
			await refreshReports();
			appendTerminal(
				'\nStop requested. Partial report saved; waiting for the server to pause safely...\n'
			);
		} catch (e) {
			error = e instanceof Error ? e.message : String(e);
		} finally {
			isStopping = false;
		}
	}

	async function resumeSelectedReport() {
		if (!activeReport || !canResumeSelectedReport) return;
		const report = activeReport;
		streamController?.abort();
		resetLiveState();
		isRunning = true;
		try {
			const start =
				report.kind === 'eval'
					? await Ds4Service.runEval({ resume_report_id: report.id })
					: await Ds4Service.runBench({ resume_report_id: report.id });
			appendTerminal('Resuming ' + title + ' from report ' + report.id + '\n');
			await attachJob(start.id, true, false);
		} catch (e) {
			error = e instanceof Error ? e.message : String(e);
			appendTerminal('\nERROR: ' + error + '\n');
			isRunning = false;
		}
	}

	async function deleteSelectedReport() {
		if (!activeReport || !canDeleteSelectedReport) return;
		const id = activeReport.id;
		try {
			await Ds4Service.deleteReport(id);
			if (selectedReportId === id) selectedReportId = '';
			activeReport = null;
			await refreshReports();
		} catch (e) {
			error = e instanceof Error ? e.message : String(e);
		}
	}

	async function loadReport(id: string) {
		if (!id) {
			activeReport = null;
			return;
		}
		try {
			activeReport = await Ds4Service.getReport(id);
		} catch (e) {
			error = e instanceof Error ? e.message : String(e);
		}
	}
</script>

<svelte:head>
	<title>{title}</title>
</svelte:head>

<main class="flex min-h-dvh w-full flex-col gap-5 px-4 py-5 md:px-8">
	<header class="flex flex-col gap-3 md:flex-row md:items-center md:justify-between">
		<div>
			<div class="flex items-center gap-2 text-sm text-muted-foreground">
				{#if isEval}
					<ClipboardCheck class="h-4 w-4" />
				{:else}
					<Activity class="h-4 w-4" />
				{/if}
				<span>llama.cpp integrated suite</span>
			</div>
			<h1 class="mt-1 text-2xl font-semibold tracking-normal">{title}</h1>
			<p class="mt-1 max-w-3xl text-sm text-muted-foreground">{subtitle}</p>
		</div>

		<button
			type="button"
			class="inline-flex h-9 items-center justify-center gap-2 rounded-md border px-3 text-sm hover:bg-muted disabled:opacity-50"
			disabled={isLoadingModels || isRunning}
			onclick={() => refreshModels(true)}
		>
			<RefreshCw class="h-4 w-4" />
			Refresh models
		</button>
	</header>

	{#if error}
		<div
			class="rounded-md border border-destructive/50 bg-destructive/10 px-3 py-2 text-sm text-destructive"
		>
			{error}
		</div>
	{/if}

	{#if otherActiveJob}
		<div
			class="rounded-md border border-yellow-500/40 bg-yellow-500/10 px-3 py-2 text-sm text-yellow-700 dark:text-yellow-300"
		>
			A {otherActiveJob.kind} job is already running: {otherActiveJob.id}. Open the matching DS4
			page to monitor it.
		</div>
	{/if}

	<section class="grid gap-4 lg:grid-cols-[26rem_minmax(0,1fr)]">
		<form
			class="space-y-4 rounded-lg border bg-background p-4"
			onsubmit={(event) => event.preventDefault()}
		>
			<div class="space-y-2">
				<div class="flex items-center justify-between gap-3">
					<div class="text-sm font-medium">Models</div>
					<div class="flex items-center gap-2 text-xs">
						<button
							type="button"
							class="text-muted-foreground hover:text-foreground"
							onclick={selectAllModels}
							disabled={isRunning}>ALL</button
						>
						<button
							type="button"
							class="text-muted-foreground hover:text-foreground"
							onclick={clearModelSelection}
							disabled={isRunning}>Clear</button
						>
					</div>
				</div>
				<div class="max-h-72 space-y-1 overflow-auto rounded-md border p-2">
					{#each models as model (model.id)}
						<label
							class="flex cursor-pointer items-start gap-2 rounded-md px-2 py-2 text-sm hover:bg-muted"
						>
							<input
								class="mt-1"
								type="checkbox"
								checked={isModelSelected(model.id)}
								disabled={isRunning}
								onchange={() => toggleModel(model.id)}
							/>
							<span class="min-w-0 flex-1">
								<span class="block truncate font-medium"
									>{model.id === 'ALL' ? 'ALL' : model.id}</span
								>
								<span class="block truncate text-xs text-muted-foreground">{modelLabel(model)}</span
								>
							</span>
							{#if isModelSelected(model.id)}
								<SquareCheck class="mt-0.5 h-4 w-4 text-primary" />
							{:else}
								<Square class="mt-0.5 h-4 w-4 text-muted-foreground" />
							{/if}
						</label>
					{/each}
				</div>
				<div class="text-xs text-muted-foreground">
					Selected: {selectedModelLabel || 'none'} ({selectedModelCount} model{selectedModelCount ===
					1
						? ''
						: 's'})
				</div>
			</div>

			{#if isEval}
				<div class="grid grid-cols-2 gap-3">
					<label class="space-y-2 text-sm">
						<span class="font-medium">Max tokens</span>
						<input
							class="h-10 w-full rounded-md border bg-background px-3"
							type="number"
							min="1"
							bind:value={maxTokens}
							disabled={isRunning}
						/>
					</label>
					<label class="space-y-2 text-sm">
						<span class="font-medium">Think budget</span>
						<input
							class="h-10 w-full rounded-md border bg-background px-3"
							type="number"
							min="1"
							bind:value={thinkingBudget}
							disabled={isRunning}
						/>
					</label>
					<label class="space-y-2 text-sm">
						<span class="font-medium">Limit</span>
						<input
							class="h-10 w-full rounded-md border bg-background px-3"
							type="number"
							min="0"
							bind:value={evalLimit}
							disabled={isRunning}
						/>
					</label>
					<label class="space-y-2 text-sm">
						<span class="font-medium">Temperature</span>
						<input
							class="h-10 w-full rounded-md border bg-background px-3"
							type="number"
							step="0.05"
							min="0"
							bind:value={temperature}
							disabled={isRunning}
						/>
					</label>
				</div>
				<label class="flex items-center gap-2 text-sm">
					<input type="checkbox" bind:checked={thinking} disabled={isRunning} />
					<span>Enable thinking/reasoning template controls</span>
				</label>
			{:else}
				<div class="grid grid-cols-2 gap-3">
					<label class="space-y-2 text-sm">
						<span class="font-medium">Ctx start</span>
						<input
							class="h-10 w-full rounded-md border bg-background px-3"
							type="number"
							min="128"
							bind:value={ctxStart}
							disabled={isRunning}
						/>
					</label>
					<label class="space-y-2 text-sm">
						<span class="font-medium">Ctx max</span>
						<input
							class="h-10 w-full rounded-md border bg-background px-3"
							type="number"
							min="128"
							bind:value={ctxMax}
							disabled={isRunning}
						/>
					</label>
					<label class="space-y-2 text-sm">
						<span class="font-medium">Ctx step</span>
						<input
							class="h-10 w-full rounded-md border bg-background px-3"
							type="number"
							min="128"
							bind:value={ctxStep}
							disabled={isRunning}
						/>
					</label>
					<label class="space-y-2 text-sm">
						<span class="font-medium">Gen tokens</span>
						<input
							class="h-10 w-full rounded-md border bg-background px-3"
							type="number"
							min="1"
							bind:value={genTokens}
							disabled={isRunning}
						/>
					</label>
				</div>
			{/if}

			<div class="grid grid-cols-[minmax(0,1fr)_auto] gap-2">
				<button
					type="button"
					class="h-10 rounded-md bg-primary px-4 text-sm font-medium text-primary-foreground hover:bg-primary/90 disabled:opacity-50"
					disabled={!canStart}
					onclick={startTest}
				>
					{isResuming ? 'Resuming...' : isRunning ? 'Running...' : 'Start Test'}
				</button>
				<button
					type="button"
					class="inline-flex h-10 items-center justify-center gap-2 rounded-md border px-3 text-sm hover:bg-muted disabled:opacity-50"
					disabled={!isRunning || isStopping}
					onclick={stopTest}
				>
					<X class="h-4 w-4" />
					{isStopping ? 'Stopping' : 'Stop'}
				</button>
			</div>

			<div class="space-y-2">
				<div class="flex items-center justify-between text-xs text-muted-foreground">
					<span>{jobStatus}</span>
					<span>{jobCurrent}/{jobTotal}</span>
				</div>
				<div class="h-2 overflow-hidden rounded-full bg-muted">
					<div
						class="h-full rounded-full bg-primary transition-[width]"
						style={'width: ' + progressPercent + '%'}
					></div>
				</div>
			</div>
		</form>

		<section class="min-w-0 rounded-lg border bg-[#070707]">
			<div class="flex items-center justify-between border-b border-white/10 px-3 py-2">
				<div class="font-mono text-xs text-white/70">
					{jobId || title.toLowerCase() + ' terminal'}
				</div>
				<div class="text-xs text-white/45">{progressPercent}% · seq {lastSeq}</div>
			</div>
			<div
				bind:this={terminalRef}
				class="h-[34rem] overflow-auto p-3 font-mono text-xs leading-relaxed whitespace-pre-wrap text-white"
			>
				{@html terminalHtml || 'Ready.\n'}
			</div>
		</section>
	</section>

	<section class="rounded-lg border bg-background p-4">
		<div class="mb-4 flex flex-col gap-3 lg:flex-row lg:items-center lg:justify-between">
			<div class="flex items-center gap-2">
				<BarChart3 class="h-4 w-4" />
				<div>
					<h2 class="text-sm font-semibold">Current Report</h2>
					<p class="text-xs text-muted-foreground">
						{activeReport
							? 'Showing saved report ' + activeReport.id
							: isRunning
								? 'Live run, restored from server-side job state.'
								: 'Live results or selected history report.'}
					</p>
				</div>
			</div>

			<div class="flex flex-col gap-2 sm:flex-row sm:items-center">
				<select
					bind:value={selectedReportId}
					onchange={() => loadReport(selectedReportId)}
					class="h-10 min-w-72 rounded-md border bg-background px-3 text-sm"
				>
					<option value="">Live / no saved report</option>
					{#each reports as report (report.id)}
						<option value={report.id}>{reportLabel(report)}</option>
					{/each}
				</select>
				<button
					type="button"
					class="h-10 rounded-md border px-3 text-sm hover:bg-muted disabled:opacity-50"
					disabled={!canResumeSelectedReport}
					onclick={resumeSelectedReport}
				>
					Resume interrupted
				</button>
				<button
					type="button"
					class="inline-flex h-10 items-center gap-2 rounded-md border px-3 text-sm hover:bg-muted disabled:opacity-50"
					disabled={!canDeleteSelectedReport}
					onclick={deleteSelectedReport}
				>
					<Trash2 class="h-4 w-4" />
					Delete pending
				</button>
				<button
					type="button"
					class="h-10 rounded-md border px-3 text-sm hover:bg-muted"
					onclick={refreshReports}>Refresh history</button
				>
			</div>
		</div>

		{#if isEval}
			<div class="grid gap-3 md:grid-cols-4">
				<div class="rounded-md border p-3">
					<div class="text-xs text-muted-foreground">Pass</div>
					<div class="mt-1 text-2xl font-semibold text-emerald-500">{evalPass}</div>
				</div>
				<div class="rounded-md border p-3">
					<div class="text-xs text-muted-foreground">Fail</div>
					<div class="mt-1 text-2xl font-semibold text-red-500">{evalFail}</div>
				</div>
				<div class="rounded-md border p-3">
					<div class="text-xs text-muted-foreground">Score</div>
					<div class="mt-1 text-2xl font-semibold">{Math.round((evalPass / evalTotal) * 100)}%</div>
				</div>
				<div class="rounded-md border p-3">
					<div class="text-xs text-muted-foreground">Rows shown</div>
					<div class="mt-1 text-2xl font-semibold">{displayEvalRows.length}</div>
				</div>
			</div>
			<div class="mt-4 flex h-4 overflow-hidden rounded-full bg-red-500/25">
				<div class="bg-emerald-500" style={'width: ' + (evalPass / evalTotal) * 100 + '%'}></div>
			</div>

			<div class="mt-4 max-h-[42rem] overflow-auto rounded-md border">
				<table class="w-full min-w-[78rem] text-left text-xs">
					<thead class="sticky top-0 bg-background text-muted-foreground">
						<tr>
							<th class="px-3 py-2">Test</th>
							<th class="px-3 py-2">Model</th>
							<th class="px-3 py-2">Expected</th>
							<th class="px-3 py-2">Got</th>
							<th class="px-3 py-2">Think</th>
							<th class="px-3 py-2">Answer</th>
							<th class="px-3 py-2">Tok/s</th>
							<th class="px-3 py-2">Status</th>
						</tr>
					</thead>
					<tbody>
						{#each displayEvalRows as row, index (`${row.model}-${row.id}-${index}`)}
							<tr class="border-t align-top">
								<td class="px-3 py-2">
									<div class="font-medium">{row.title || row.id || 'case ' + (index + 1)}</div>
									<div class="text-muted-foreground">
										#{row.case_index ?? index + 1} · {row.source || 'DS4'} · {row.domain ||
											'reasoning'} · {row.id}
									</div>
								</td>
								<td class="px-3 py-2">{row.model}</td>
								<td class="px-3 py-2">{row.expected}</td>
								<td class="px-3 py-2">{row.got}</td>
								<td class="px-3 py-2">{num(row.reasoning_tokens)}</td>
								<td class="px-3 py-2">{num(row.content_tokens)}</td>
								<td class="px-3 py-2">{num(row.tokens_per_second).toFixed(1)}</td>
								<td
									class={row.pass
										? 'px-3 py-2 font-medium text-emerald-500'
										: 'px-3 py-2 font-medium text-red-500'}>{row.pass ? 'PASS' : 'FAIL'}</td
								>
							</tr>
						{/each}
						{#if displayEvalRows.length === 0}
							<tr><td class="px-3 py-6 text-muted-foreground" colspan="8">No eval rows yet.</td></tr
							>
						{/if}
					</tbody>
				</table>
			</div>

			<div class="mt-4 rounded-md border bg-muted/20 p-4">
				<div class="flex flex-col gap-3 lg:flex-row lg:items-center lg:justify-between">
					<div class="flex items-center gap-2">
						<Trophy class="h-4 w-4" />
						<div>
							<h3 class="text-sm font-semibold">Model Report</h3>
							<p class="text-xs text-muted-foreground">
								Completed archive only · {archiveEvalReportCount} report{archiveEvalReportCount ===
								1
									? ''
									: 's'} · best run per model.
							</p>
						</div>
					</div>

					<div class="flex flex-col gap-2 sm:flex-row sm:items-center">
						<div
							class="inline-grid h-10 grid-cols-2 overflow-hidden rounded-md border bg-background text-sm"
						>
							<button
								type="button"
								class={reportMode === 'single'
									? 'bg-primary px-3 text-primary-foreground'
									: 'px-3 hover:bg-muted'}
								onclick={() => (reportMode = 'single')}
							>
								Single
							</button>
							<button
								type="button"
								class={reportMode === 'race'
									? 'bg-primary px-3 text-primary-foreground disabled:opacity-50'
									: 'px-3 hover:bg-muted disabled:opacity-50'}
								disabled={reportModels.length < 2}
								onclick={() => (reportMode = 'race')}
							>
								Race
							</button>
						</div>

						{#if reportMode === 'single'}
							<select
								bind:value={singleReportModel}
								class="h-10 min-w-64 rounded-md border bg-background px-3 text-sm"
								disabled={reportModels.length === 0}
							>
								{#each reportModels as model (model)}
									<option value={model}>{model}</option>
								{/each}
							</select>
						{:else}
							<select
								bind:value={raceModelA}
								class="h-10 min-w-56 rounded-md border bg-background px-3 text-sm"
							>
								{#each reportModels as model (model)}
									<option value={model} disabled={model === raceModelB}>{model}</option>
								{/each}
							</select>
							<select
								bind:value={raceModelB}
								class="h-10 min-w-56 rounded-md border bg-background px-3 text-sm"
							>
								{#each reportModels as model (model)}
									<option value={model} disabled={model === raceModelA}>{model}</option>
								{/each}
							</select>
						{/if}
					</div>
				</div>

				{#if reportModels.length === 0}
					<div class="mt-4 rounded-md border bg-background px-3 py-6 text-sm text-muted-foreground">
						No model report yet.
					</div>
				{:else if reportMode === 'single'}
					<div class="mt-4 grid gap-3 md:grid-cols-4">
						<div class="rounded-md border bg-background p-3">
							<div class="text-xs text-muted-foreground">Model</div>
							<div class="mt-1 truncate text-lg font-semibold">{singleReportModel}</div>
						</div>
						<div class="rounded-md border bg-background p-3">
							<div class="text-xs text-muted-foreground">Score</div>
							<div class="mt-1 text-2xl font-semibold">{singleModelSummary?.score ?? 0}%</div>
						</div>
						<div class="rounded-md border bg-background p-3">
							<div class="text-xs text-muted-foreground">Pass / total</div>
							<div class="mt-1 text-2xl font-semibold">
								{singleModelSummary?.pass ?? 0}/{singleModelSummary?.total ?? 0}
							</div>
						</div>
						<div class="rounded-md border bg-background p-3">
							<div class="text-xs text-muted-foreground">Avg tok/s</div>
							<div class="mt-1 text-2xl font-semibold">
								{num(singleModelSummary?.avg_tokens_per_second).toFixed(1)}
							</div>
						</div>
					</div>

					<div class="mt-4 overflow-auto rounded-md border bg-background">
						<table class="w-full min-w-[46rem] text-left text-xs">
							<thead class="bg-background text-muted-foreground">
								<tr>
									<th class="px-3 py-2">Sector</th>
									<th class="px-3 py-2">Score</th>
									<th class="px-3 py-2">Pass</th>
									<th class="px-3 py-2">Fail</th>
									<th class="px-3 py-2">Cases</th>
									<th class="px-3 py-2">Avg tok/s</th>
								</tr>
							</thead>
							<tbody>
								{#each singleSectorRows as row (`${row.model}-${row.sector}`)}
									<tr class="border-t">
										<td class="px-3 py-2 font-medium">{row.sector}</td>
										<td class="px-3 py-2">
											<div class="flex items-center gap-2">
												<div class="h-2 w-28 overflow-hidden rounded-full bg-red-500/20">
													<div
														class="h-full bg-emerald-500"
														style={'width: ' + row.score + '%'}
													></div>
												</div>
												<span>{row.score}%</span>
											</div>
										</td>
										<td class="px-3 py-2 text-emerald-500">{row.pass}</td>
										<td class="px-3 py-2 text-red-500">{row.fail}</td>
										<td class="px-3 py-2">{row.total}</td>
										<td class="px-3 py-2">{row.avg_tokens_per_second.toFixed(1)}</td>
									</tr>
								{/each}
							</tbody>
						</table>
					</div>
				{:else}
					<div class="mt-4 grid gap-3 md:grid-cols-3">
						<div class="rounded-md border bg-background p-3">
							<div class="text-xs text-muted-foreground">{raceModelA}</div>
							<div class="mt-1 text-2xl font-semibold">{raceModelSummaryA?.score ?? 0}%</div>
							<div class="text-xs text-muted-foreground">
								{raceModelSummaryA?.pass ?? 0}/{raceModelSummaryA?.total ?? 0} pass
							</div>
						</div>
						<div class="rounded-md border bg-background p-3">
							<div class="text-xs text-muted-foreground">Leader</div>
							<div class="mt-1 truncate text-2xl font-semibold">
								{#if (raceModelSummaryA?.score ?? 0) === (raceModelSummaryB?.score ?? 0)}
									Tie
								{:else if (raceModelSummaryA?.score ?? 0) > (raceModelSummaryB?.score ?? 0)}
									{raceModelA}
								{:else}
									{raceModelB}
								{/if}
							</div>
							<div class="text-xs text-muted-foreground">
								Delta {formatDelta(
									(raceModelSummaryA?.score ?? 0) - (raceModelSummaryB?.score ?? 0)
								)}
							</div>
						</div>
						<div class="rounded-md border bg-background p-3">
							<div class="text-xs text-muted-foreground">{raceModelB}</div>
							<div class="mt-1 text-2xl font-semibold">{raceModelSummaryB?.score ?? 0}%</div>
							<div class="text-xs text-muted-foreground">
								{raceModelSummaryB?.pass ?? 0}/{raceModelSummaryB?.total ?? 0} pass
							</div>
						</div>
					</div>

					<div class="mt-4 overflow-auto rounded-md border bg-background">
						<table class="w-full min-w-[64rem] text-left text-xs">
							<thead class="bg-background text-muted-foreground">
								<tr>
									<th class="px-3 py-2">Sector</th>
									<th class="px-3 py-2">{raceModelA}</th>
									<th class="px-3 py-2">{raceModelB}</th>
									<th class="px-3 py-2">Delta</th>
									<th class="px-3 py-2">Cases</th>
								</tr>
							</thead>
							<tbody>
								{#each raceSectorRows as row (row.sector)}
									<tr class="border-t">
										<td class="px-3 py-2 font-medium">{row.sector}</td>
										<td class="px-3 py-2"
											>{row.left?.score ?? 0}% · {row.left?.pass ?? 0}/{row.left?.total ?? 0}</td
										>
										<td class="px-3 py-2"
											>{row.right?.score ?? 0}% · {row.right?.pass ?? 0}/{row.right?.total ?? 0}</td
										>
										<td
											class={row.delta >= 0
												? 'px-3 py-2 font-medium text-emerald-500'
												: 'px-3 py-2 font-medium text-red-500'}>{formatDelta(row.delta)}</td
										>
										<td class="px-3 py-2">{row.left?.total ?? 0} / {row.right?.total ?? 0}</td>
									</tr>
								{/each}
							</tbody>
						</table>
					</div>
				{/if}
			</div>
		{:else}
			<div class="grid gap-3 md:grid-cols-3">
				<div class="rounded-md border p-3">
					<div class="text-xs text-muted-foreground">Best prompt tok/s</div>
					<div class="mt-1 text-2xl font-semibold">{bestPromptTps.toFixed(1)}</div>
				</div>
				<div class="rounded-md border p-3">
					<div class="text-xs text-muted-foreground">Best decode tok/s</div>
					<div class="mt-1 text-2xl font-semibold">{bestDecodeTps.toFixed(1)}</div>
				</div>
				<div class="rounded-md border p-3">
					<div class="text-xs text-muted-foreground">Rows shown</div>
					<div class="mt-1 text-2xl font-semibold">{displayBenchRows.length}</div>
				</div>
			</div>

			<div class="mt-4 max-h-[42rem] overflow-auto rounded-md border">
				<table class="w-full min-w-[58rem] text-left text-xs">
					<thead class="sticky top-0 bg-background text-muted-foreground">
						<tr>
							<th class="px-3 py-2">Ctx</th>
							<th class="px-3 py-2">Model</th>
							<th class="px-3 py-2">Prompt tok/s</th>
							<th class="px-3 py-2">Decode tok/s</th>
							<th class="px-3 py-2">Prompt sec</th>
							<th class="px-3 py-2">Decode sec</th>
							<th class="px-3 py-2">Gen tokens</th>
						</tr>
					</thead>
					<tbody>
						{#each displayBenchRows as row (`${row.model}-${row.ctx}`)}
							<tr class="border-t">
								<td class="px-3 py-2">{row.ctx}</td>
								<td class="px-3 py-2">{row.model}</td>
								<td class="px-3 py-2">{num(row.prompt_tokens_per_second).toFixed(1)}</td>
								<td class="px-3 py-2">{num(row.decode_tokens_per_second).toFixed(1)}</td>
								<td class="px-3 py-2">{num(row.prompt_seconds).toFixed(2)}</td>
								<td class="px-3 py-2">{num(row.decode_seconds).toFixed(2)}</td>
								<td class="px-3 py-2">{num(row.gen_tokens)}</td>
							</tr>
						{/each}
						{#if displayBenchRows.length === 0}
							<tr
								><td class="px-3 py-6 text-muted-foreground" colspan="7">No bench rows yet.</td></tr
							>
						{/if}
					</tbody>
				</table>
			</div>
		{/if}
	</section>
</main>
