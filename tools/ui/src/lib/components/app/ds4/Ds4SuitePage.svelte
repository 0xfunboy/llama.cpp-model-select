<script lang="ts">
	import { onMount, tick } from 'svelte';
	import { Activity, BarChart3, ClipboardCheck, RefreshCw } from '@lucide/svelte';
	import {
		Ds4Service,
		type Ds4Event,
		type Ds4Model,
		type Ds4Report,
		type Ds4ReportSummary
	} from '$lib/services/ds4.service';

	interface Props {
		mode: 'eval' | 'bench';
	}

	interface EvalRow {
		id?: string;
		model?: string;
		expected?: string;
		got?: string;
		pass?: boolean;
	}

	interface BenchRow {
		ctx?: number;
		model?: string;
		prompt_tokens_per_second?: number;
		decode_tokens_per_second?: number;
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
	let selectedModel = $state('ALL');
	let isLoadingModels = $state(false);
	let isRunning = $state(false);
	let error = $state('');
	let jobId = $state('');
	let jobStatus = $state('idle');
	let jobCurrent = $state(0);
	let jobTotal = $state(0);
	let terminalText = $state('');
	let terminalRef = $state<HTMLDivElement | null>(null);
	let activeReport = $state<Ds4Report | null>(null);
	let selectedReportId = $state('');
	let evalRows = $state<EvalRow[]>([]);
	let benchRows = $state<BenchRow[]>([]);
	let streamController: AbortController | null = null;

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

	onMount(() => {
		void refreshModels();
		void refreshReports();
		return () => {
			streamController?.abort();
		};
	});

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
		const pattern = /\x1b\[([0-9;]*)m/g;
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
					output += `<span class="${klass}">`;
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

	function basename(path?: string): string {
		if (!path) return '';
		const normalized = path.replaceAll('\\', '/');
		return normalized.slice(normalized.lastIndexOf('/') + 1);
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

	async function refreshModels(reload = false) {
		isLoadingModels = true;
		try {
			const response = await Ds4Service.listModels(reload);
			models = response.data;
			if (!models.some((model) => model.id === selectedModel)) {
				selectedModel = 'ALL';
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
		} catch {
			reports = [];
		}
	}

	function handleEvent(event: Ds4Event) {
		jobStatus = event.data.status ?? jobStatus;
		jobCurrent = event.data.current ?? jobCurrent;
		jobTotal = event.data.total ?? jobTotal;

		if (event.event === 'log' && typeof event.data.text === 'string') {
			appendTerminal(event.data.text);
		}
		if (event.event === 'case') {
			evalRows = [event.data as EvalRow, ...evalRows].slice(0, 24);
		}
		if (event.event === 'bench-row') {
			benchRows = [event.data as BenchRow, ...benchRows].slice(0, 32);
		}
		if (event.event === 'done' && event.data.error) {
			error = String(event.data.error);
		}
	}

	async function startTest() {
		streamController?.abort();
		streamController = new AbortController();
		isRunning = true;
		error = '';
		jobId = '';
		jobStatus = 'queued';
		jobCurrent = 0;
		jobTotal = 0;
		terminalText = '';
		activeReport = null;
		evalRows = [];
		benchRows = [];

		try {
			const start = isEval
				? await Ds4Service.runEval({
						model: selectedModel,
						max_tokens: maxTokens,
						thinking_budget_tokens: thinkingBudget,
						thinking,
						temperature,
						limit: evalLimit > 0 ? evalLimit : undefined
					})
				: await Ds4Service.runBench({
						model: selectedModel,
						ctx_start: ctxStart,
						ctx_max: ctxMax,
						ctx_step: ctxStep,
						gen_tokens: genTokens
					});

			jobId = start.id;
			appendTerminal(`Started ${title} job ${start.id}\n`);
			await Ds4Service.streamJob(start.id, handleEvent, streamController.signal);
			const snapshot = await Ds4Service.getJob(start.id);
			jobStatus = snapshot.status;
			jobCurrent = snapshot.current;
			jobTotal = snapshot.total;
			activeReport = snapshot.report ?? null;
			await refreshReports();
		} catch (e) {
			if (!(e instanceof DOMException && e.name === 'AbortError')) {
				error = e instanceof Error ? e.message : String(e);
				appendTerminal(`\nERROR: ${error}\n`);
			}
		} finally {
			isRunning = false;
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

<main class="mx-auto flex min-h-dvh w-full max-w-7xl flex-col gap-5 px-4 py-5 md:px-8">
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
			<p class="mt-1 max-w-2xl text-sm text-muted-foreground">{subtitle}</p>
		</div>

		<button
			class="inline-flex h-9 items-center justify-center gap-2 rounded-md border px-3 text-sm hover:bg-muted disabled:opacity-50"
			disabled={isLoadingModels || isRunning}
			onclick={() => refreshModels(true)}
		>
			<RefreshCw class="h-4 w-4" />
			Refresh models
		</button>
	</header>

	{#if error}
		<div class="rounded-md border border-destructive/50 bg-destructive/10 px-3 py-2 text-sm text-destructive">
			{error}
		</div>
	{/if}

	<section class="grid gap-4 lg:grid-cols-[22rem_minmax(0,1fr)]">
		<form class="space-y-4 rounded-lg border bg-background p-4" onsubmit={(event) => event.preventDefault()}>
			<div class="space-y-2">
				<label class="text-sm font-medium" for="ds4-model">Model</label>
				<select
					id="ds4-model"
					bind:value={selectedModel}
					disabled={isRunning}
					class="h-10 w-full rounded-md border bg-background px-3 text-sm"
				>
					{#each models as model (model.id)}
						<option value={model.id}>
							{model.id}{model.id === 'ALL'
								? ''
								: ` · ${model.status.value}${model.path ? ` · ${basename(model.path)}` : ''}`}
						</option>
					{/each}
				</select>
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

			<button
				class="h-10 w-full rounded-md bg-primary px-4 text-sm font-medium text-primary-foreground hover:bg-primary/90 disabled:opacity-50"
				disabled={isRunning || !selectedModel}
				onclick={startTest}
			>
				{isRunning ? 'Running...' : 'Start Test'}
			</button>

			<div class="space-y-2">
				<div class="flex items-center justify-between text-xs text-muted-foreground">
					<span>{jobStatus}</span>
					<span>{jobCurrent}/{jobTotal}</span>
				</div>
				<div class="h-2 overflow-hidden rounded-full bg-muted">
					<div
						class="h-full rounded-full bg-primary transition-[width]"
						style={`width: ${progressPercent}%`}
					></div>
				</div>
			</div>
		</form>

		<section class="min-w-0 space-y-4">
			<div class="rounded-lg border bg-[#070707]">
				<div class="flex items-center justify-between border-b border-white/10 px-3 py-2">
					<div class="font-mono text-xs text-white/70">
						{jobId || `${title.toLowerCase()} terminal`}
					</div>
					<div class="text-xs text-white/45">{progressPercent}%</div>
				</div>
				<div
					bind:this={terminalRef}
					class="h-[28rem] overflow-auto p-3 font-mono text-xs leading-relaxed whitespace-pre-wrap text-white"
				>
					{@html terminalHtml || 'Ready.\n'}
				</div>
			</div>

			<div class="grid gap-4 xl:grid-cols-[minmax(0,1fr)_18rem]">
				<section class="rounded-lg border bg-background p-4">
					<div class="mb-3 flex items-center gap-2">
						<BarChart3 class="h-4 w-4" />
						<h2 class="text-sm font-semibold">Current Report</h2>
					</div>

					{#if activeReport}
						{#if activeReport.kind === 'eval'}
							{@const pass = summaryNumber('pass')}
							{@const fail = summaryNumber('fail')}
							{@const total = Math.max(1, pass + fail)}
							<div class="grid gap-3 md:grid-cols-3">
								<div class="rounded-md border p-3">
									<div class="text-xs text-muted-foreground">Pass</div>
									<div class="mt-1 text-2xl font-semibold text-emerald-500">{pass}</div>
								</div>
								<div class="rounded-md border p-3">
									<div class="text-xs text-muted-foreground">Fail</div>
									<div class="mt-1 text-2xl font-semibold text-red-500">{fail}</div>
								</div>
								<div class="rounded-md border p-3">
									<div class="text-xs text-muted-foreground">Score</div>
									<div class="mt-1 text-2xl font-semibold">
										{Math.round((pass / total) * 100)}%
									</div>
								</div>
							</div>
							<div class="mt-4 flex h-4 overflow-hidden rounded-full bg-red-500/25">
								<div class="bg-emerald-500" style={`width: ${(pass / total) * 100}%`}></div>
							</div>
						{:else}
							<div class="grid gap-3 md:grid-cols-2">
								<div class="rounded-md border p-3">
									<div class="text-xs text-muted-foreground">Best prompt tok/s</div>
									<div class="mt-1 text-2xl font-semibold">
										{summaryNumber('best_prompt_tokens_per_second').toFixed(1)}
									</div>
								</div>
								<div class="rounded-md border p-3">
									<div class="text-xs text-muted-foreground">Best decode tok/s</div>
									<div class="mt-1 text-2xl font-semibold">
										{summaryNumber('best_decode_tokens_per_second').toFixed(1)}
									</div>
								</div>
							</div>
						{/if}
					{:else}
						<p class="text-sm text-muted-foreground">No completed report selected yet.</p>
					{/if}

					{#if isEval && evalRows.length}
						<div class="mt-4 overflow-x-auto">
							<table class="w-full text-left text-xs">
								<thead class="text-muted-foreground">
									<tr>
										<th class="py-2">Case</th>
										<th>Model</th>
										<th>Expected</th>
										<th>Got</th>
										<th>Status</th>
									</tr>
								</thead>
								<tbody>
									{#each evalRows as row}
										<tr class="border-t">
											<td class="py-2">{row.id}</td>
											<td>{row.model}</td>
											<td>{row.expected}</td>
											<td>{row.got}</td>
											<td class={row.pass ? 'text-emerald-500' : 'text-red-500'}>
												{row.pass ? 'PASS' : 'FAIL'}
											</td>
										</tr>
									{/each}
								</tbody>
							</table>
						</div>
					{/if}

					{#if !isEval && benchRows.length}
						<div class="mt-4 overflow-x-auto">
							<table class="w-full text-left text-xs">
								<thead class="text-muted-foreground">
									<tr>
										<th class="py-2">Ctx</th>
										<th>Model</th>
										<th>Prompt tok/s</th>
										<th>Decode tok/s</th>
									</tr>
								</thead>
								<tbody>
									{#each benchRows as row}
										<tr class="border-t">
											<td class="py-2">{row.ctx}</td>
											<td>{row.model}</td>
											<td>{num(row.prompt_tokens_per_second).toFixed(1)}</td>
											<td>{num(row.decode_tokens_per_second).toFixed(1)}</td>
										</tr>
									{/each}
								</tbody>
							</table>
						</div>
					{/if}
				</section>

				<aside class="rounded-lg border bg-background p-4">
					<div class="mb-3 flex items-center justify-between">
						<h2 class="text-sm font-semibold">Report History</h2>
						<button class="text-xs text-muted-foreground hover:text-foreground" onclick={refreshReports}>
							Refresh
						</button>
					</div>
					<select
						bind:value={selectedReportId}
						onchange={() => loadReport(selectedReportId)}
						class="h-10 w-full rounded-md border bg-background px-3 text-sm"
					>
						<option value="">Select report</option>
						{#each reports as report (report.id)}
							<option value={report.id}>{report.created_at} · {report.model_selector}</option>
						{/each}
					</select>
					{#if reports.length === 0}
						<p class="mt-3 text-xs text-muted-foreground">No saved {title} reports yet.</p>
					{:else}
						<div class="mt-3 space-y-2">
							{#each reports.slice(0, 6) as report (report.id)}
								<button
									class="block w-full rounded-md border px-2 py-2 text-left text-xs hover:bg-muted"
									onclick={() => {
										selectedReportId = report.id;
										loadReport(report.id);
									}}
								>
									<div class="font-medium">{report.model_selector}</div>
									<div class="text-muted-foreground">{report.created_at}</div>
								</button>
							{/each}
						</div>
					{/if}
				</aside>
			</div>
		</section>
	</section>
</main>
