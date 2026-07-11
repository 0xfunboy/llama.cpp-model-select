import { expect, test, type Page } from '@playwright/test';

const winner = {
	id: 'winner',
	model: 'Qwen Local 14B',
	artifact_id: 'artifact:qwen',
	variant: 'Q4_K_M',
	row_role: 'candidate',
	workload_kind: 'baseline',
	ok: true,
	benchmark_backend: 'llama-server-streaming',
	evidence_level: 'streaming-measured',
	quality_evidence_level: 'quality-tested',
	quality_gate_required: true,
	quality_gate_passed: true,
	quality_evidence: { score: 0.88, samples: 16 },
	measurement_confidence: 'reliable',
	fit_eligible: true,
	context_target_met: true,
	ctx_size: 32768,
	prompt_tps: 820,
	eval_tps: 48,
	e2e_ttft_ms: 280,
	vram_peak_mib: 11800,
	process_working_set_peak_mib: 1400,
	selection_reason: 'Best qualified daily-driver balance on this hardware.',
	timeline: {
		encoding: 'delta-columns-v1',
		rows: [
			[0, 2000, 10, 900],
			[200, 8000, 70, 1200],
			[200, 11800, 90, 1400]
		]
	}
};

const alternatives = ['Mistral Local 12B', 'Code Local 9B', 'Reason Local 20B'].map(
	(model, index) => ({
		...winner,
		id: `alternative-${index}`,
		model,
		eval_tps: 42 - index * 3,
		selection_reason:
			index === 0 ? 'Lower memory alternative.' : 'Qualified specialist alternative.'
	})
);

const diagnosticRows = [
	{
		...winner,
		id: 'prefill-2k',
		row_role: 'diagnostic',
		workload_kind: 'prefill',
		prefill_target_tokens: 2048,
		prompt_tps: 900
	},
	{
		...winner,
		id: 'kv-32k',
		row_role: 'diagnostic',
		workload_kind: 'kv-fill',
		prefill_target_tokens: 0,
		kv_fill_target_tokens: 32768,
		prompt_tps: 720
	},
	{
		...winner,
		id: 'kv-64k',
		row_role: 'diagnostic',
		workload_kind: 'kv-fill',
		prefill_target_tokens: 0,
		kv_fill_target_tokens: 65536,
		prompt_tps: 400
	}
];

async function mockProductApi(page: Page) {
	await page.route('**/*', async (route) => {
		const url = new URL(route.request().url());
		if (url.pathname === '/api/caliber-advisor/models')
			return route.fulfill({
				json: {
					data: [
						{
							id: 'qwen',
							name: 'Qwen Local 14B',
							loadable: true,
							path: '/models/qwen.gguf',
							plan_meta: { size_mib: 9000, gguf_context_length: 131072 }
						}
					]
				}
			});
		if (url.pathname === '/api/caliber-advisor/reports')
			return route.fulfill({ json: { data: [], total: 0 } });
		if (url.pathname === '/api/caliber-advisor/results')
			return route.fulfill({
				json: {
					rows: [winner, ...alternatives, ...diagnosticRows],
					recommendations: {
						overall: {
							winner,
							alternatives,
							reason: winner.selection_reason,
							best_by_model: { [winner.model]: winner }
						}
					}
				}
			});
		if (url.pathname === '/api/caliber-advisor/sweep/status')
			return route.fulfill({ json: { status: 'idle', finished: true } });
		if (url.pathname === '/api/caliber-advisor/system')
			return route.fulfill({
				json: {
					doctor: {
						state_writable: true,
						streaming_profiler_available: true,
						ready_artifacts: 1,
						unhealthy_artifacts: 0,
						duplicate_artifacts: 0,
						stale_reports: 0,
						legacy_reports: 0
					}
				}
			});
		if (url.pathname === '/api/fit-advisor/system')
			return route.fulfill({
				json: {
					cpu_name: 'Test CPU',
					cpu_cores: 16,
					total_ram_gb: 64,
					available_ram_gb: 48,
					gpu_name: 'RTX Test',
					gpu_count: 1,
					total_gpu_vram_gb: 24,
					backend: 'cuda',
					gpus: [{ name: 'RTX Test', backend: 'cuda', vram_gb: 24 }]
				}
			});
		if (url.pathname === '/api/fit-advisor/models')
			return route.fulfill({ json: { data: [], total: 0 } });
		if (url.pathname === '/api/fit-advisor/downloads') return route.fulfill({ json: { data: [] } });
		if (url.pathname === '/api/fit-advisor/downloads/sse')
			return route.fulfill({ status: 200, contentType: 'text/event-stream', body: '' });
		if (url.pathname === '/api/router/decisions')
			return route.fulfill({
				json: {
					object: 'route-event-list',
					data: [
						{
							event_type: 'decision',
							object_id: 'route-1',
							created_at: '2026-07-10T20:00:00Z',
							payload: {
								ok: true,
								alias: 'local-auto',
								selected_model: winner.model,
								quality: 0.88,
								quality_pack: 'overall',
								required_context: 4096,
								evidence: winner,
								reason: 'Qualified resident model avoided a switch.'
							}
						}
					]
				}
			});
		if (url.pathname === '/props')
			return route.fulfill({ json: { model_path: '', modalities: {} } });
		if (url.pathname === '/models') return route.fulfill({ json: { data: [], object: 'list' } });
		if (url.pathname === '/tools') return route.fulfill({ json: { tools: [] } });
		return route.continue();
	});
}

test('guided flow exposes one qualified answer and three alternatives', async ({ page }) => {
	await mockProductApi(page);
	await page.goto('/#/caliber-advisor');
	const dismiss = page.getByRole('button', { name: 'Not now' });
	if (await dismiss.count()) await dismiss.click();

	await expect(page.getByRole('heading', { name: 'Local LLM Autopilot' })).toBeVisible();
	for (const label of ['Library', 'Test Lab', 'Recommendations', 'Router', 'History', 'Doctor']) {
		await expect(page.getByRole('button', { name: label, exact: true })).toBeVisible();
	}
	await expect(page.getByRole('button', { name: 'Home', exact: true })).toHaveCount(0);
	await expect(page.getByText('Downloadable recommendations')).toBeVisible();
	await expect(page.getByText('Pick first 4')).toHaveCount(0);
	await page.getByRole('button', { name: 'Test Lab', exact: true }).click();
	await expect(page.getByLabel('Context target')).toHaveValue('32768');

	await page.getByRole('button', { name: 'Recommendations', exact: true }).click();
	await expect(page.getByText('Recommended on this hardware')).toBeVisible();
	await expect(page.getByRole('heading', { name: 'Qwen Local · 14B' })).toBeVisible();
	await expect(page.getByText(/quality 88%/).first()).toBeVisible();
	await expect(page.getByText('Alternative 3')).toBeVisible();
	await expect(page.getByText('Streaming timeline')).toBeVisible();
	await expect(page.getByText('Qwen Local · 14B · kv-fill · 32k')).toBeVisible();
	await expect(page.getByText('Qwen Local · 14B · kv-fill · 64k')).toBeVisible();
	await expect(page.getByText(/kv-fill · 0k/)).toHaveCount(0);
	const throughput = page.getByRole('heading', { name: 'Throughput & memory' });
	const scatter = page.getByRole('heading', { name: 'Memory vs latency' });
	await expect(throughput).toBeVisible();
	await expect(scatter).toBeVisible();
	expect((await throughput.boundingBox())!.y).toBeLessThan((await scatter.boundingBox())!.y);
	await expect(page.getByText('Total request time (log scale)')).toBeVisible();
	await expect(page.getByText('Metric glossary')).toBeVisible();
	await page.getByRole('button', { name: 'Router', exact: true }).click();
	await expect(page.getByText('Current winner')).toBeVisible();
	await expect(page.getByText('Qualified resident model avoided a switch.')).toBeVisible();
	await page.getByRole('button', { name: 'Doctor', exact: true }).click();
	await expect(page.getByText('1 ready · 0 unhealthy')).toBeVisible();
});

test('mobile product navigation wraps without horizontal overflow', async ({ page }) => {
	await page.setViewportSize({ width: 390, height: 844 });
	await mockProductApi(page);
	await page.goto('/#/caliber-advisor');
	const dismiss = page.getByRole('button', { name: 'Not now' });
	if (await dismiss.count()) await dismiss.click();
	const tabs = page.locator('nav.tabs');
	await expect(tabs).toBeVisible();
	const dimensions = await tabs.evaluate((element) => ({
		scrollWidth: element.scrollWidth,
		clientWidth: element.clientWidth
	}));
	expect(dimensions.scrollWidth).toBeLessThanOrEqual(dimensions.clientWidth);
});
