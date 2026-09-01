import { expect, type Page, test } from '@playwright/test';

const winner = {
	artifact_id: 'artifact:qwen',
	benchmark_backend: 'llama-server-streaming',
	context_target_met: true,
	ctx_size: 32768,
	e2e_ttft_ms: 280,
	eval_tps: 48,
	evidence_level: 'streaming-measured',
	fit_eligible: true,
	id: 'winner',
	measurement_confidence: 'reliable',
	model: 'qwen-runtime',
	ok: true,
	process_working_set_peak_mib: 1400,
	prompt_tps: 820,
	quality_evidence: { samples: 16, score: 0.88 },
	quality_evidence_level: 'quality-tested',
	quality_gate_passed: true,
	quality_gate_required: true,
	row_role: 'candidate',
	selection_reason: 'Best qualified daily-driver balance on this hardware.',
	timeline: {
		encoding: 'delta-columns-v1',
		rows: [
			[0, 2000, 10, 900],
			[200, 8000, 70, 1200],
			[200, 11800, 90, 1400]
		]
	},
	variant: 'Q4_K_M',
	vram_peak_mib: 11800,
	workload_kind: 'baseline'
};
const alternatives = ['Mistral Local 12B', 'Code Local 9B', 'Reason Local 20B'].map(
	(model, index) => ({
		...winner,
		eval_tps: 42 - index * 3,
		id: `alternative-${index}`,
		model,
		selection_reason:
			index === 0 ? 'Lower memory alternative.' : 'Qualified specialist alternative.'
	})
);
const diagnosticRows = [
	{
		...winner,
		id: 'prefill-2k',
		prefill_target_tokens: 2048,
		prompt_tps: 900,
		row_role: 'diagnostic',
		workload_kind: 'prefill'
	},
	{
		...winner,
		id: 'kv-32k',
		kv_fill_target_tokens: 32768,
		prefill_target_tokens: 0,
		prompt_tps: 720,
		row_role: 'diagnostic',
		workload_kind: 'kv-fill'
	},
	{
		...winner,
		id: 'kv-64k',
		kv_fill_target_tokens: 65536,
		prefill_target_tokens: 0,
		prompt_tps: 400,
		row_role: 'diagnostic',
		workload_kind: 'kv-fill'
	}
];

async function mockProductApi(page: Page, options: { failDs4Reports?: boolean } = {}) {
	await page.route('**/*', async (route) => {
		const url = new URL(route.request().url());

		if (url.pathname === '/api/caliber-advisor/models')
			return route.fulfill({
				json: {
					data: [
						{
							artifact_id: 'artifact:qwen',
							benchmark_eligible: true,
							configured_id: 'qwen-runtime',
							configured_ids: ['qwen-runtime'],
							id: 'qwen-runtime',
							loadable: true,
							name: 'Qwen Local 14B',
							path: '/models/qwen.gguf',
							plan_meta: { gguf_context_length: 131072, size_mib: 9000 }
						},
						{
							artifact_id: 'artifact:qwen',
							benchmark_eligible: true,
							configured_id: 'qwen-runtime-alt',
							configured_ids: ['qwen-runtime-alt'],
							id: 'qwen-runtime-alt',
							loadable: true,
							name: 'Qwen Alternate 14B',
							path: '/models/qwen.gguf',
							plan_meta: { gguf_context_length: 131072, size_mib: 9000 }
						},
						{
							benchmark_eligible: false,
							eligibility_reason: 'Media generation artifact',
							id: 'media-authoritative',
							loadable: true,
							name: 'Z-Image Media',
							path: '/models/zimage.gguf'
						},
						{
							id: 'media-legacy',
							loadable: true,
							name: 'Legacy Media Encoder',
							path: '/models/media/image/encoder.gguf'
						}
					]
				}
			});

		if (url.pathname === '/api/caliber-advisor/reports')
			return route.fulfill({
				json: {
					data: [
						{
							created_at: '2026-07-10T19:00:00Z',
							id: 'caliber-report-1',
							model: winner.model,
							plan_items: 7,
							rows: 7,
							status: 'complete'
						}
					],
					total: 1
				}
			});

		if (url.pathname === '/api/caliber-advisor/results')
			return route.fulfill({
				json: {
					recommendations: {
						overall: {
							alternatives,
							best_by_model: { [winner.model]: winner },
							reason: winner.selection_reason,
							winner
						}
					},
					rows: [winner, ...alternatives, ...diagnosticRows]
				}
			});

		if (url.pathname === '/api/caliber-advisor/sweep/status')
			return route.fulfill({ json: { finished: true, status: 'idle' } });

		if (url.pathname === '/api/caliber-advisor/system')
			return route.fulfill({
				json: {
					doctor: {
						duplicate_artifacts: 0,
						legacy_reports: 0,
						ready_artifacts: 1,
						stale_reports: 0,
						state_writable: true,
						streaming_profiler_available: true,
						unhealthy_artifacts: 0
					}
				}
			});

		if (url.pathname === '/api/fit-advisor/system')
			return route.fulfill({
				json: {
					available_ram_gb: 48,
					backend: 'cuda',
					cpu_cores: 16,
					cpu_name: 'Test CPU',
					gpu_count: 1,
					gpu_name: 'RTX Test',
					gpus: [{ backend: 'cuda', name: 'RTX Test', vram_gb: 24 }],
					total_gpu_vram_gb: 24,
					total_ram_gb: 64
				}
			});

		if (url.pathname === '/api/fit-advisor/models')
			return route.fulfill({ json: { data: [], total: 0 } });

		if (url.pathname === '/api/fit-advisor/downloads') return route.fulfill({ json: { data: [] } });

		if (url.pathname === '/api/fit-advisor/downloads/sse')
			return route.fulfill({ body: '', contentType: 'text/event-stream', status: 200 });

		if (url.pathname === '/api/ds4/reports' && options.failDs4Reports)
			return route.fulfill({
				body: JSON.stringify({ error: 'DS4 unavailable' }),
				contentType: 'application/json',
				status: 500
			});

		if (url.pathname === '/api/ds4/reports')
			return route.fulfill({
				json: {
					data: [
						{
							created_at: '2026-07-10T20:00:00Z',
							id: 'ds4-eval-1',
							kind: 'eval',
							model_selector: winner.model,
							status: 'completed',
							summary: { cases: 40 }
						}
					],
					quality_profiles: {
						'artifact:qwen': {
							artifact_id: 'artifact:qwen',
							name: 'Qwen Local 14B',
							packs: {
								chat: { pass: 8, samples: 10, score: 0.8 },
								general: { pass: 9, samples: 10, score: 0.9 },
								reasoning: { pass: 17, samples: 20, score: 0.85 }
							},
							pass: 34,
							samples: 40,
							score: 0.84,
							variant: 'Q4_K_M'
						}
					}
				}
			});

		if (url.pathname === '/api/ds4/models')
			return route.fulfill({
				json: {
					data: [
						{
							evaluator_eligible: true,
							id: 'ALL',
							name: 'All configured models',
							source: 'virtual',
							status: { loaded: false, running: false, value: 'virtual' }
						},
						{
							artifact_id: 'artifact:qwen',
							configured: true,
							evaluated: true,
							evaluation_cases: 40,
							evaluator_eligible: true,
							id: 'qwen-runtime',
							name: 'Qwen Local 14B',
							source: 'preset',
							status: { loaded: true, running: true, value: 'loaded' },
							variant: 'Q4_K_M'
						},
						{
							artifact_id: 'artifact:mistral-new',
							configured: true,
							evaluated: false,
							evaluation_cases: 0,
							evaluator_eligible: true,
							id: 'mistral-new-runtime',
							name: 'Mistral New 12B',
							source: 'preset',
							status: { loaded: false, running: false, value: 'unloaded' },
							variant: 'Q4_K_M'
						},
						{
							configured: false,
							evaluated: false,
							evaluation_cases: 0,
							evaluator_eligible: false,
							id: 'downloaded-only',
							name: 'Downloaded Only 7B',
							source: 'registry',
							status: { loaded: false, running: false, value: 'ready' }
						}
					],
					object: 'list'
				}
			});

		if (url.pathname === '/api/ds4/report') {
			const command = route.request().headers()['x-cmd'];

			if (command === 'report')
				return route.fulfill({
					json: {
						created_at: '2026-07-10T20:00:00Z',
						id: route.request().headers()['x-report-id'] || 'ds4-eval-1',
						kind: 'eval',
						model_selector: winner.model,
						results: [],
						resumable: false,
						status: 'completed',
						summary: { fail: 6, pass: 34, total: 40 }
					}
				});

			return route.fulfill({ json: { active: false, job: {}, matches: false } });
		}

		if (url.pathname === '/api/router/decisions')
			return route.fulfill({
				json: {
					data: [
						{
							created_at: '2026-07-10T20:00:00Z',
							event_type: 'decision',
							object_id: 'route-1',
							payload: {
								alias: 'local-auto',
								evidence: winner,
								ok: true,
								quality: 0.88,
								quality_pack: 'overall',
								reason: 'Qualified resident model avoided a switch.',
								required_context: 4096,
								selected_model: winner.model
							}
						}
					],
					object: 'route-event-list'
				}
			});

		if (url.pathname === '/props')
			return route.fulfill({ json: { modalities: {}, model_path: '' } });

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
	for (const label of ['Overview', 'Run tests', 'Evidence', 'Archive']) {
		await expect(page.getByRole('button', { exact: true, name: label })).toBeVisible();
	}
	await expect(page.getByRole('button', { exact: true, name: 'Home' })).toHaveCount(0);
	await expect(page.getByRole('link', { name: 'Find models' })).toHaveAttribute(
		'href',
		'#/fit-advisor'
	);
	await expect(page.getByText('Downloadable recommendations')).toHaveCount(0);
	await expect(page.getByText('Pick first 4')).toHaveCount(0);
	await page.getByRole('button', { exact: true, name: 'Run tests' }).click();
	await expect(page.getByLabel('Context target')).toHaveValue('32768');
	await expect(page.getByRole('button', { name: /Use-case Evaluator/ })).toBeVisible();
	const shortlistSize = page.getByLabel('Hardware shortlist size');
	const loadWinner = page.getByLabel('Load winner immediately after FIT');

	await expect(shortlistSize).toHaveValue('4');
	await expect(loadWinner).not.toBeChecked();
	await loadWinner.check();
	await expect(shortlistSize).toHaveValue('4');
	await expect(loadWinner).toBeChecked();
	await expect(page.getByText('Z-Image Media')).toHaveCount(0);
	await expect(page.getByText('Legacy Media Encoder')).toHaveCount(0);
	const historicPreset = page.locator('.model-option').filter({ hasText: 'Qwen Local 14B' });
	const siblingPreset = page.locator('.model-option').filter({ hasText: 'Qwen Alternate 14B' });

	await historicPreset.click();
	await siblingPreset.click();
	await expect(page.getByText(/2 selected.*1 need benchmarking.*1 already archived/)).toBeVisible();
	await historicPreset.click();
	await siblingPreset.click();
	const firstChoice = page.locator('.compact-choice').first();
	const choiceAlignment = await firstChoice.evaluate((element) => {
		const strong = element.querySelector('strong')!.getBoundingClientRect();
		const span = element.querySelector('span')!.getBoundingClientRect();

		return Math.abs(strong.x - span.x);
	});

	expect(choiceAlignment).toBeLessThan(1);

	await page.getByRole('button', { exact: true, name: 'Overview' }).click();
	await expect(page.getByRole('heading', { name: 'Use-case capability ranking' })).toBeVisible();
	await expect(page.getByText('Qwen Local · 14B').first()).toBeVisible();
	await expect(page.getByText('skill coverage')).toBeVisible();
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
	await page.getByRole('button', { exact: true, name: 'Evidence' }).click();
	await expect(page.getByRole('button', { exact: true, name: 'Quality' })).toBeVisible();
	await expect(page.getByRole('button', { exact: true, name: 'Performance' })).toBeVisible();
	await expect(page.getByRole('heading', { name: 'Use-case Evaluator' })).toBeVisible();
	await expect(page.getByText('DS4 evidence engine')).toBeVisible();
	await expect(page.getByText('Qwen Local · 14B').first()).toBeVisible();
	await expect(page.getByText(/Selected: none \(0 models\)/)).toBeVisible();
	await expect(page.getByText(/configure in Models first/)).toBeVisible();
	await page.getByRole('button', { exact: true, name: 'All new' }).click();
	await expect(page.getByText('Mistral New · 12B').first()).toBeVisible();
	await expect(
		page.locator('label').filter({ hasText: 'Qwen Local · 14B' }).getByRole('checkbox')
	).not.toBeChecked();
	await expect(
		page.locator('label').filter({ hasText: 'Mistral New · 12B' }).getByRole('checkbox')
	).toBeChecked();
	await expect(page.getByText(/All new selects configured models/)).toBeVisible();
	await expect(page.getByText(/evaluated: select manually to retest/)).toBeVisible();
	await expect(
		page
			.locator('label')
			.filter({ hasText: 'Mistral New' })
			.getByText(/not evaluated yet/)
	).toBeVisible();
	await page.getByRole('button', { exact: true, name: 'Performance' }).click();
	await expect(page.getByRole('heading', { name: 'Context Benchmark' })).toBeVisible();
	await page.getByRole('button', { exact: true, name: 'Archive' }).click();
	const caliberArchiveRow = page
		.locator('.reports-table .table-row')
		.filter({ hasText: 'Caliber performance' });
	const ds4ArchiveRow = page
		.locator('.reports-table .table-row')
		.filter({ hasText: 'DS4 quality' });

	await expect(caliberArchiveRow).toBeVisible();
	await expect(ds4ArchiveRow).toBeVisible();
	await ds4ArchiveRow.getByRole('button', { exact: true, name: 'Open' }).click();
	await expect(page.getByRole('heading', { name: 'Use-case Evaluator' })).toBeVisible();
	await expect(
		page.locator('select').filter({ has: page.locator('option[value="ds4-eval-1"]') })
	).toHaveValue('ds4-eval-1');
	await expect(page.getByText('Showing saved report ds4-eval-1')).toBeVisible();
	await page.getByRole('button', { exact: true, name: 'Overview' }).click();
	await page.getByText('Advanced: qualified local router').click();
	await expect(page.getByText('Current winner')).toBeVisible();
	await expect(page.getByText('Qualified resident model avoided a switch.')).toBeVisible();
	await expect(page.getByText('1 ready · 0 unhealthy')).toBeVisible();
});

test('overview survives an unavailable DS4 report service', async ({ page }) => {
	await mockProductApi(page, { failDs4Reports: true });
	await page.goto('/#/caliber-advisor');
	const dismiss = page.getByRole('button', { name: 'Not now' });

	if (await dismiss.count()) await dismiss.click();

	await expect(page.getByRole('button', { exact: true, name: 'Overview' })).toBeVisible();
	await expect(page.getByText('Recommended on this hardware')).toBeVisible();
	await expect(page.getByText(/Quality evidence:/)).toBeVisible();
	await page.getByRole('button', { exact: true, name: 'Evidence' }).click();
	await expect(page.getByText(/DS4 reports:/)).toBeVisible();
});

test('mobile product navigation wraps without horizontal overflow', async ({ page }) => {
	await page.setViewportSize({ height: 844, width: 390 });
	await mockProductApi(page);
	await page.goto('/#/caliber-advisor');
	const dismiss = page.getByRole('button', { name: 'Not now' });

	if (await dismiss.count()) await dismiss.click();

	const tabs = page.locator('nav.tabs');

	await expect(tabs).toBeVisible();
	const dimensions = await tabs.evaluate((element) => ({
		clientWidth: element.clientWidth,
		scrollWidth: element.scrollWidth
	}));

	expect(dimensions.scrollWidth).toBeLessThanOrEqual(dimensions.clientWidth);
});
