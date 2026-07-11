import { base } from '$app/paths';
import { apiFetch, apiPost } from '$lib/utils';
import { getAuthHeaders } from '$lib/utils/api-headers';

export interface CaliberModel {
	id: string;
	name: string;
	source: string;
	status: string;
	loadable?: boolean;
	configured?: boolean;
	path?: string | null;
	plan_meta?: Record<string, unknown>;
}

export interface CaliberModelsResponse {
	object: 'list';
	data: CaliberModel[];
}

export interface CaliberPlanItem {
	id: string;
	model: string;
	variant: string;
	sweep: 'context' | 'moe-cpu' | 'offload';
	workload_kind: 'baseline' | 'prefill' | 'kv-fill';
	control_kind?: string | null;
	row_role: string;
	label: string;
	extra_args: string;
}

export interface CaliberPlanResponse {
	object: 'caliber.plan';
	cfg: Record<string, unknown>;
	models: Record<string, unknown>[];
	plan: CaliberPlanItem[];
	plan_count: number;
}

export interface CaliberReportSummary {
	id: string;
	created_at: string;
	status: string;
	model: string;
	plan_items: number;
	rows: number;
	path?: string;
}

export interface CaliberReportsResponse {
	object: 'list';
	data: CaliberReportSummary[];
	offset: number;
	limit: number;
	total: number;
	has_more: boolean;
}

export interface CaliberDeleteReportResponse {
	success: boolean;
	id: string;
}

export interface CaliberSweepResponse {
	success: boolean;
	job_id: string;
	status: string;
}

export interface CaliberSweepStatus {
	job_id?: string;
	status: string;
	error?: string;
	current?: number;
	total?: number;
	current_item?: string | null;
	report_id?: string | null;
	finished?: boolean;
	cancel_requested?: boolean;
}

export interface CaliberSweepEvent {
	event: string;
	data: CaliberSweepStatus & Record<string, unknown>;
}

export interface CaliberConfigureResponse {
	success: boolean;
	model: string;
	loaded: boolean;
	entry?: Record<string, unknown>;
	models_preset?: string;
}

function parseSseBlock(block: string): CaliberSweepEvent | null {
	let event = 'message';
	let data = '';
	for (const line of block.split('\n')) {
		if (line.startsWith('event:')) {
			event = line.slice(6).trim();
		} else if (line.startsWith('data:')) {
			data += line.slice(5).trim();
		}
	}
	if (!data) return null;
	return { event, data: JSON.parse(data) as CaliberSweepStatus & Record<string, unknown> };
}

export class CaliberAdvisorService {
	static system(): Promise<Record<string, unknown>> {
		return apiFetch<Record<string, unknown>>('/api/caliber-advisor/system', { authOnly: true });
	}

	static models(reload = false): Promise<CaliberModelsResponse> {
		return apiFetch<CaliberModelsResponse>(
			`/api/caliber-advisor/models${reload ? '?reload=1' : ''}`,
			{ authOnly: true }
		);
	}

	static plan(payload: Record<string, unknown>): Promise<CaliberPlanResponse> {
		return apiPost<CaliberPlanResponse, Record<string, unknown>>(
			'/api/caliber-advisor/plan',
			payload
		);
	}

	static sweep(payload: Record<string, unknown>): Promise<CaliberSweepResponse> {
		return apiPost<CaliberSweepResponse, Record<string, unknown>>(
			'/api/caliber-advisor/sweep',
			payload
		);
	}

	static sweepStatus(jobId?: string): Promise<CaliberSweepStatus> {
		const suffix = jobId ? `?id=${encodeURIComponent(jobId)}` : '';
		return apiFetch<CaliberSweepStatus>(`/api/caliber-advisor/sweep/status${suffix}`, {
			authOnly: true
		});
	}

	static stopSweep(jobId?: string): Promise<CaliberSweepStatus> {
		return apiPost<CaliberSweepStatus, Record<string, unknown>>(
			'/api/caliber-advisor/sweep/stop',
			jobId ? { id: jobId } : {}
		);
	}

	static async streamSweepEvents(
		jobId: string,
		onEvent: (event: CaliberSweepEvent) => void,
		signal?: AbortSignal,
		since = 0
	): Promise<void> {
		const params = new URLSearchParams({ id: jobId });
		if (since > 0) params.set('since', String(since));
		const response = await fetch(base + `/api/caliber-advisor/sweep/events?${params.toString()}`, {
			headers: getAuthHeaders(),
			signal
		});

		if (!response.ok) {
			throw new Error(
				'Caliber Advisor event stream failed: ' + response.status + ' ' + response.statusText
			);
		}
		if (!response.body) throw new Error('Caliber Advisor event stream is empty');

		const reader = response.body.getReader();
		const decoder = new TextDecoder();
		let buffer = '';
		for (;;) {
			const { done, value } = await reader.read();
			if (done) break;
			buffer += decoder.decode(value, { stream: true });
			for (;;) {
				const index = buffer.indexOf('\n\n');
				if (index === -1) break;
				const parsed = parseSseBlock(buffer.slice(0, index));
				buffer = buffer.slice(index + 2);
				if (parsed) onEvent(parsed);
			}
		}
		const tail = buffer.trim();
		if (tail) {
			const parsed = parseSseBlock(tail);
			if (parsed) onEvent(parsed);
		}
	}

	static reports(limit = 50, offset = 0): Promise<CaliberReportsResponse> {
		return apiFetch<CaliberReportsResponse>(
			`/api/caliber-advisor/reports?limit=${limit}&offset=${offset}`,
			{ authOnly: true }
		);
	}

	static report(id: string): Promise<Record<string, unknown>> {
		return apiFetch<Record<string, unknown>>(
			`/api/caliber-advisor/report?id=${encodeURIComponent(id)}`,
			{ authOnly: true }
		);
	}

	static deleteReport(id: string): Promise<CaliberDeleteReportResponse> {
		return apiFetch<CaliberDeleteReportResponse>(
			`/api/caliber-advisor/reports/${encodeURIComponent(id)}`,
			{
				authOnly: true,
				method: 'DELETE'
			}
		);
	}

	static results(): Promise<Record<string, unknown>> {
		return apiFetch<Record<string, unknown>>('/api/caliber-advisor/results', { authOnly: true });
	}

	static configure(payload: Record<string, unknown>): Promise<CaliberConfigureResponse> {
		return apiPost<CaliberConfigureResponse, Record<string, unknown>>(
			'/api/caliber-advisor/configure',
			payload
		);
	}
}
