import { base } from '$app/paths';
import { apiFetch, apiPost } from '$lib/utils';
import { getAuthHeaders } from '$lib/utils/api-headers';

export interface Ds4Model {
	id: string;
	source: string;
	path?: string;
	loadable?: boolean;
	status: {
		value: string;
		loaded: boolean;
		running: boolean;
	};
	tags?: string[];
	aliases?: string[];
}

export interface Ds4ModelsResponse {
	object: 'list';
	data: Ds4Model[];
}

export interface Ds4JobStartResponse {
	id: string;
	kind: 'eval' | 'bench';
	status: string;
	model: string;
}

export interface Ds4JobSnapshot {
	id: string;
	kind: 'eval' | 'bench';
	model: string;
	status: string;
	error?: string;
	current: number;
	total: number;
	finished: boolean;
	cancel_requested?: boolean;
	next_seq?: number;
	report?: Ds4Report;
}

export interface Ds4ActiveJobResponse {
	active: boolean;
	matches: boolean;
	job?: Ds4JobSnapshot;
}

export interface Ds4ReportSummary {
	id: string;
	kind: 'eval' | 'bench';
	status?: string;
	resumable?: boolean;
	archive?: boolean;
	created_at: string;
	updated_at?: string;
	model_selector: string;
	summary: Record<string, unknown>;
}

export interface Ds4ReportsResponse {
	object: 'list';
	data: Ds4ReportSummary[];
}

export interface Ds4DeleteReportResponse {
	deleted: boolean;
	id: string;
}

export interface Ds4Report {
	id: string;
	kind: 'eval' | 'bench';
	status?: string;
	resumable?: boolean;
	archive?: boolean;
	created_at: string;
	updated_at?: string;
	model_selector: string;
	summary?: Record<string, unknown>;
	results?: Record<string, unknown>[];
	[key: string]: unknown;
}

export interface Ds4Event<T = Record<string, unknown>> {
	event: string;
	data: T & {
		seq?: number;
		job_id?: string;
		kind?: string;
		status?: string;
		current?: number;
		total?: number;
		text?: string;
		error?: string;
	};
}

export interface Ds4EvalRequest {
	model?: string;
	models?: string[];
	resume_report_id?: string;
	max_tokens?: number;
	thinking_budget_tokens?: number;
	thinking?: boolean;
	temperature?: number;
	limit?: number;
}

export interface Ds4BenchRequest {
	model?: string;
	models?: string[];
	resume_report_id?: string;
	ctx_start?: number;
	ctx_max?: number;
	ctx_step?: number;
	gen_tokens?: number;
}

function parseSseBlock(block: string): Ds4Event | null {
	let event = 'message';
	const dataLines: string[] = [];

	for (const line of block.split(/\r?\n/)) {
		if (line.startsWith('event:')) {
			event = line.slice(6).trim();
		} else if (line.startsWith('data:')) {
			dataLines.push(line.slice(5).trimStart());
		}
	}

	if (dataLines.length === 0) {
		return null;
	}

	try {
		return {
			event,
			data: JSON.parse(dataLines.join('\n')) as Ds4Event['data']
		};
	} catch {
		return {
			event,
			data: { text: dataLines.join('\n') }
		};
	}
}

export class Ds4Service {
	static listModels(_reload = false): Promise<Ds4ModelsResponse> {
		return apiFetch<Ds4ModelsResponse>('/api/ds4/models', {
			authOnly: true
		});
	}

	static runEval(body: Ds4EvalRequest): Promise<Ds4JobStartResponse> {
		return apiPost<Ds4JobStartResponse, Ds4EvalRequest>('/api/ds4/run-eval', body);
	}

	static runBench(body: Ds4BenchRequest): Promise<Ds4JobStartResponse> {
		return apiPost<Ds4JobStartResponse, Ds4BenchRequest>('/api/ds4/run-bench', body);
	}

	static getActiveJob(kind?: 'eval' | 'bench'): Promise<Ds4ActiveJobResponse> {
		return apiFetch<Ds4ActiveJobResponse>('/api/ds4/report', {
			authOnly: true,
			headers: {
				'X-Cmd': 'active',
				...(kind ? { 'X-Kind': kind } : {})
			}
		});
	}

	static getJob(id: string): Promise<Ds4JobSnapshot> {
		return apiFetch<Ds4JobSnapshot>('/api/ds4/report', {
			authOnly: true,
			headers: {
				'X-Cmd': 'status',
				'X-Job-Id': id
			}
		});
	}

	static stopJob(id?: string): Promise<Ds4JobSnapshot> {
		return apiPost<Ds4JobSnapshot, { cmd: string; id?: string }>('/api/ds4/run-eval', {
			cmd: 'stop',
			...(id ? { id } : {})
		});
	}

	static listReports(): Promise<Ds4ReportsResponse> {
		return apiFetch<Ds4ReportsResponse>('/api/ds4/reports', { authOnly: true });
	}

	static getReport(id: string): Promise<Ds4Report> {
		return apiFetch<Ds4Report>('/api/ds4/report', {
			authOnly: true,
			headers: {
				'X-Cmd': 'report',
				'X-Report-Id': id
			}
		});
	}

	static deleteReport(id: string): Promise<Ds4DeleteReportResponse> {
		return apiFetch<Ds4DeleteReportResponse>(`/api/ds4/reports/${encodeURIComponent(id)}`, {
			authOnly: true,
			method: 'DELETE'
		});
	}

	static async streamJob(
		id: string,
		onEvent: (event: Ds4Event) => void,
		signal?: AbortSignal,
		since = 0
	): Promise<void> {
		const response = await fetch(base + '/api/ds4/report', {
			headers: {
				...getAuthHeaders(),
				'X-Cmd': 'events',
				'X-Job-Id': id,
				'X-Since': String(Math.max(0, since))
			},
			signal
		});

		if (!response.ok) {
			throw new Error('DS4 event stream failed: ' + response.status + ' ' + response.statusText);
		}
		if (!response.body) {
			throw new Error('DS4 event stream is empty');
		}

		const reader = response.body.getReader();
		const decoder = new TextDecoder();
		let buffer = '';

		for (;;) {
			const { done, value } = await reader.read();
			if (done) {
				break;
			}
			buffer += decoder.decode(value, { stream: true });

			for (;;) {
				const index = buffer.indexOf('\n\n');
				if (index === -1) {
					break;
				}
				const block = buffer.slice(0, index);
				buffer = buffer.slice(index + 2);
				const parsed = parseSseBlock(block);
				if (parsed) {
					onEvent(parsed);
				}
			}
		}

		const tail = buffer.trim();
		if (tail) {
			const parsed = parseSseBlock(tail);
			if (parsed) {
				onEvent(parsed);
			}
		}
	}
}
