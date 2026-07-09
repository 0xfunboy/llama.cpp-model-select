import { apiFetch, apiPost } from '$lib/utils';

export interface CaliberModel {
	id: string;
	name: string;
	source: string;
	status: string;
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
	path: string;
}

export interface CaliberReportsResponse {
	object: 'list';
	data: CaliberReportSummary[];
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
	report_id?: string;
	finished?: boolean;
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
		return apiPost<CaliberPlanResponse, Record<string, unknown>>('/api/caliber-advisor/plan', payload);
	}

	static sweep(payload: Record<string, unknown>): Promise<CaliberSweepResponse> {
		return apiPost<CaliberSweepResponse, Record<string, unknown>>('/api/caliber-advisor/sweep', payload);
	}

	static sweepStatus(jobId?: string): Promise<CaliberSweepStatus> {
		const suffix = jobId ? `?id=${encodeURIComponent(jobId)}` : '';
		return apiFetch<CaliberSweepStatus>(`/api/caliber-advisor/sweep/status${suffix}`, {
			authOnly: true
		});
	}

	static reports(): Promise<CaliberReportsResponse> {
		return apiFetch<CaliberReportsResponse>('/api/caliber-advisor/reports', { authOnly: true });
	}

	static report(id: string): Promise<Record<string, unknown>> {
		return apiFetch<Record<string, unknown>>(
			`/api/caliber-advisor/report?id=${encodeURIComponent(id)}`,
			{ authOnly: true }
		);
	}

	static results(): Promise<Record<string, unknown>> {
		return apiFetch<Record<string, unknown>>('/api/caliber-advisor/results', { authOnly: true });
	}
}
