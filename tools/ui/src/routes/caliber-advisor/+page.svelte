<script lang="ts">
	import { onMount } from 'svelte';
	import { Play, RefreshCw, Trash2 } from '@lucide/svelte';
	import {
		CaliberAdvisorService,
		type CaliberModel,
		type CaliberPlanItem,
		type CaliberReportSummary,
		type CaliberSweepStatus
	} from '$lib/services/caliber-advisor.service';

	let models = $state<CaliberModel[]>([]);
	let selectedModel = $state('');
	let plan = $state<CaliberPlanItem[]>([]);
	let reports = $state<CaliberReportSummary[]>([]);
	let selectedReport = $state<Record<string, unknown> | null>(null);
	let status = $state<CaliberSweepStatus | null>(null);
	let loading = $state(false);
	let running = $state(false);
	let error = $state('');
	let message = $state('');
	let contextSize = $state(131072);
	let workloadSweep = $state<'baseline' | 'prefill' | 'kv-fill' | 'all'>('baseline');

	const selected = $derived(models.find((model) => model.id === selectedModel) ?? models[0] ?? null);
	const visiblePlan = $derived(plan.slice(0, 200));

	onMount(() => {
		void refreshAll();
	});

	async function refreshAll() {
		loading = true;
		error = '';
		try {
			const [modelsResult, reportsResult] = await Promise.all([
				CaliberAdvisorService.models(),
				CaliberAdvisorService.reports()
			]);
			models = modelsResult.data;
			reports = reportsResult.data;
			selectedModel = selectedModel || models[0]?.id || '';
			message = `${models.length} models, ${reports.length} reports`;
		} catch (e) {
			error = e instanceof Error ? e.message : String(e);
		} finally {
			loading = false;
		}
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

	async function previewPlan() {
		if (!selected) return;
		loading = true;
		error = '';
		try {
			const result = await CaliberAdvisorService.plan(payload());
			plan = result.plan;
			message = `${result.plan_count} planned rows`;
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
		try {
			const started = await CaliberAdvisorService.sweep(payload());
			status = { job_id: started.job_id, status: started.status };
			message = `Job ${started.job_id}`;
			for (let i = 0; i < 20; i += 1) {
				await new Promise((resolve) => setTimeout(resolve, 500));
				status = await CaliberAdvisorService.sweepStatus(started.job_id);
				if (status.finished) break;
			}
			await refreshAll();
		} catch (e) {
			error = e instanceof Error ? e.message : String(e);
		} finally {
			running = false;
		}
	}

	async function openReport(report: CaliberReportSummary) {
		error = '';
		try {
			selectedReport = await CaliberAdvisorService.report(report.id);
		} catch (e) {
			error = e instanceof Error ? e.message : String(e);
		}
	}
</script>

<svelte:head>
	<title>Caliber Advisor</title>
</svelte:head>

<main class="caliber-page">
	<header class="topbar">
		<div>
			<h1>Caliber Advisor</h1>
			<p>{message}</p>
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
				<option value={8192}>8k</option>
				<option value={32768}>32k</option>
				<option value={65536}>64k</option>
				<option value={131072}>131k</option>
				<option value={262144}>262k</option>
			</select>
		</label>
		<label>
			<span>Workload</span>
			<select bind:value={workloadSweep}>
				<option value="baseline">Baseline</option>
				<option value="prefill">Prefill</option>
				<option value="kv-fill">KV fill</option>
				<option value="all">All</option>
			</select>
		</label>
		<button type="button" onclick={previewPlan} disabled={!selected || loading}>Plan</button>
	</section>

	<section class="grid">
		<div class="panel">
			<div class="panel-head">
				<h2>Plan</h2>
				<span>{plan.length}</span>
			</div>
			<div class="table">
				{#each visiblePlan as row}
					<button type="button" class="row">
						<span>{row.sweep}</span>
						<strong>{row.label}</strong>
						<code>{row.extra_args}</code>
					</button>
				{/each}
			</div>
		</div>

		<div class="panel">
			<div class="panel-head">
				<h2>Reports</h2>
				<span>{reports.length}</span>
			</div>
			<div class="table">
				{#each reports as report}
					<button type="button" class="row" onclick={() => openReport(report)}>
						<span>{report.status}</span>
						<strong>{report.model || report.id}</strong>
						<code>{report.created_at}</code>
					</button>
				{/each}
			</div>
		</div>
	</section>

	{#if status}
		<section class="panel">
			<div class="panel-head">
				<h2>Job</h2>
				<span>{status.status}</span>
			</div>
			<pre>{JSON.stringify(status, null, 2)}</pre>
		</section>
	{/if}

	{#if selectedReport}
		<section class="panel">
			<div class="panel-head">
				<h2>Report</h2>
				<button type="button" title="Delete pending report" disabled>
					<Trash2 size={16} />
				</button>
			</div>
			<pre>{JSON.stringify(selectedReport, null, 2)}</pre>
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
	}

	.topbar,
	.controls,
	.panel {
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
	p {
		margin: 0;
	}

	h1 {
		font-size: 24px;
		line-height: 1.2;
	}

	h2 {
		font-size: 15px;
	}

	p,
	span,
	code {
		color: rgba(255, 255, 255, 0.68);
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

	select,
	button {
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

	button.primary {
		background: rgba(255, 255, 255, 0.88);
		color: #111;
	}

	button:disabled {
		cursor: not-allowed;
		opacity: 0.55;
	}

	.grid {
		display: grid;
		grid-template-columns: minmax(0, 1.4fr) minmax(280px, 0.6fr);
		gap: 16px;
	}

	.panel {
		overflow: hidden;
	}

	.panel-head {
		display: flex;
		align-items: center;
		justify-content: space-between;
		border-bottom: 1px solid rgba(255, 255, 255, 0.14);
		padding: 12px;
	}

	.table {
		max-height: 520px;
		overflow: auto;
	}

	.row {
		display: grid;
		width: 100%;
		grid-template-columns: 90px minmax(180px, 0.7fr) minmax(220px, 1fr);
		justify-content: stretch;
		border-width: 0 0 1px 0;
		text-align: left;
	}

	.row strong,
	.row code {
		overflow: hidden;
		text-overflow: ellipsis;
		white-space: nowrap;
	}

	pre {
		max-height: 420px;
		margin: 0;
		overflow: auto;
		padding: 12px;
		font-size: 12px;
	}

	.error {
		border: 1px solid rgba(255, 174, 0, 0.6);
		background: rgba(255, 174, 0, 0.1);
		padding: 12px;
	}

	@media (max-width: 900px) {
		.topbar,
		.grid {
			display: flex;
			flex-direction: column;
		}

		.row {
			grid-template-columns: 1fr;
		}
	}
</style>
