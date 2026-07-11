import { base } from '$app/paths';
import { apiFetch, apiPost } from '$lib/utils';
import { getAuthHeaders } from '$lib/utils/api-headers';

export interface FitAdvisorGpu {
	name: string;
	vram_gb: number;
	backend: string;
	unified_memory?: boolean;
}

export interface FitAdvisorSystem {
	cpu_name: string;
	cpu_cores: number;
	total_ram_gb: number;
	available_ram_gb: number;
	fit_ram_capacity_gb?: number;
	has_gpu: boolean;
	gpu_name: string;
	gpu_count: number;
	gpu_vram_gb: number;
	total_gpu_vram_gb: number;
	backend: string;
	unified_memory?: boolean;
	gpus: FitAdvisorGpu[];
}

export interface FitAdvisorCatalogStatus {
	source: string;
	url: string;
	cache_path: string;
	updated_at?: string;
	from_cache: boolean;
	model_count?: number;
	error?: string;
}

export interface FitAdvisorDownload {
	repo: string;
	quant: string;
	hf_ref: string;
	provider?: string;
	url: string;
	target_dir?: string;
}

export interface FitAdvisorDownloadFile {
	filename: string;
	url: string;
	local_path: string;
	downloaded_bytes: number;
	total_bytes: number;
}

export interface FitAdvisorDownloadJob {
	id: string;
	model_id: string;
	repo: string;
	quant: string;
	hf_ref: string;
	status: 'queued' | 'resolving' | 'downloading' | 'downloaded' | 'partial' | 'failed';
	error?: string | null;
	target_dir: string;
	requested_filename?: string | null;
	local_path?: string | null;
	downloaded_bytes: number;
	total_bytes: number;
	speed_bps: number;
	percent: number;
	exit_code?: number;
	started_at?: string | null;
	updated_at?: string | null;
	finished_at?: string | null;
	active_file_index?: number;
	files?: FitAdvisorDownloadFile[];
	seq?: number;
}

export interface FitAdvisorDownloadsResponse {
	object: 'list';
	data: FitAdvisorDownloadJob[];
}

export interface FitAdvisorDownloadEvent {
	event: string;
	data: FitAdvisorDownloadJob;
}

export interface FitAdvisorModel {
	id: string;
	name: string;
	provider: string;
	params_b: number;
	parameter_count?: string;
	is_moe: boolean;
	quant: string;
	format: string;
	context_length: number;
	requested_context_length?: number;
	effective_context_length: number;
	use_case: string;
	fit_level: 'perfect' | 'good' | 'marginal' | 'too_tight';
	score: number;
	score_components: {
		quality: number;
		speed: number;
		fit: number;
		context: number;
		capacity?: number;
	};
	estimated_tps: number;
	memory_required_gb: number;
	full_memory_required_gb?: number;
	memory_available_gb: number;
	ram_available_now_gb?: number;
	ram_capacity_gb?: number;
	weights_gb: number;
	kv_cache_gb: number;
	overhead_gb: number;
	moe_offloaded_gb?: number;
	utilization_pct: number;
	gpu_mode: string;
	fit_strategy?: string;
	runtime: string;
	installed: boolean;
	configured?: boolean;
	downloaded?: boolean;
	partial?: boolean;
	download_status?:
		| 'available'
		| 'downloading'
		| 'downloaded'
		| 'configured'
		| 'partial'
		| 'failed';
	installed_model_id?: string;
	local_path?: string | null;
	target_dir?: string;
	download_progress?: FitAdvisorDownloadJob | null;
	notes: string[];
	download?: FitAdvisorDownload | null;
	recommended_args: string[];
	preset: Record<string, unknown>;
}

export interface FitAdvisorModelsResponse {
	object: 'list';
	system: FitAdvisorSystem;
	catalog: FitAdvisorCatalogStatus;
	total_catalog_models: number;
	returned_models: number;
	installed: Record<string, unknown>[];
	models: FitAdvisorModel[];
}

export interface FitAdvisorModelsQuery {
	refresh?: boolean;
	use_case?: string;
	min_fit?: string;
	quant?: string;
	search?: string;
	strategy?: string;
	context?: number;
	limit?: number;
	include_too_tight?: boolean;
}

export interface FitAdvisorConfigureRequest {
	model_id: string;
	preset_id?: string;
	local_path?: string | null;
	hf_ref?: string;
	repo?: string;
	quant?: string;
	target_dir?: string;
	gpu_mode?: string;
	ctx_size?: number;
	alias?: string;
	tags?: string[];
	preset?: Record<string, unknown>;
	load_now?: boolean;
}

export interface FitAdvisorConfigureResponse {
	success: boolean;
	model: string;
	models_preset: string;
	entry: Record<string, unknown>;
	loaded: boolean;
}

export interface FitAdvisorDownloadResponse {
	success: boolean;
	model: string;
	status?: string;
	already_present?: boolean;
	local_path?: string;
	target_dir?: string;
	job?: FitAdvisorDownloadJob;
}

function parseSseBlock(block: string): FitAdvisorDownloadEvent | null {
	let event = 'message';
	let data = '';
	for (const line of block.split('\n')) {
		if (line.startsWith('event:')) {
			event = line.slice(6).trim();
		} else if (line.startsWith('data:')) {
			data += line.slice(5).trim();
		}
	}
	if (!data) {
		return null;
	}
	return {
		event,
		data: JSON.parse(data) as FitAdvisorDownloadJob
	};
}

function queryString(query: FitAdvisorModelsQuery): string {
	const params = new URLSearchParams();
	for (const [key, value] of Object.entries(query)) {
		if (value === undefined || value === null || value === '') continue;
		params.set(key, String(value));
	}
	const text = params.toString();
	return text ? `?${text}` : '';
}

export class FitAdvisorService {
	static system(): Promise<FitAdvisorSystem> {
		return apiFetch<FitAdvisorSystem>('/api/fit-advisor/system', { authOnly: true });
	}

	static models(query: FitAdvisorModelsQuery = {}): Promise<FitAdvisorModelsResponse> {
		return apiFetch<FitAdvisorModelsResponse>(`/api/fit-advisor/models${queryString(query)}`, {
			authOnly: true
		});
	}

	static refreshCatalog(): Promise<FitAdvisorCatalogStatus> {
		return apiPost<FitAdvisorCatalogStatus, Record<string, never>>(
			'/api/fit-advisor/catalog/refresh',
			{}
		);
	}

	static download(model: FitAdvisorModel): Promise<FitAdvisorDownloadResponse> {
		return apiPost<FitAdvisorDownloadResponse, Record<string, unknown>>(
			'/api/fit-advisor/download',
			{
				model_id: model.id,
				hf_ref: model.download?.hf_ref,
				repo: model.download?.repo,
				quant: model.quant,
				target_dir: model.download?.target_dir ?? model.target_dir
			}
		);
	}

	static listDownloads(): Promise<FitAdvisorDownloadsResponse> {
		return apiFetch<FitAdvisorDownloadsResponse>('/api/fit-advisor/downloads', { authOnly: true });
	}

	static async streamDownloads(
		onEvent: (event: FitAdvisorDownloadEvent) => void,
		signal?: AbortSignal,
		since = 0
	): Promise<void> {
		const response = await fetch(
			base + `/api/fit-advisor/downloads/sse${since > 0 ? `?since=${since}` : ''}`,
			{
				headers: getAuthHeaders(),
				signal
			}
		);

		if (!response.ok) {
			throw new Error(
				'Fit Advisor download stream failed: ' + response.status + ' ' + response.statusText
			);
		}
		if (!response.body) {
			throw new Error('Fit Advisor download stream is empty');
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

	static configure(model: FitAdvisorModel, loadNow = false): Promise<FitAdvisorConfigureResponse> {
		return apiPost<FitAdvisorConfigureResponse, FitAdvisorConfigureRequest>(
			'/api/fit-advisor/configure',
			{
				model_id: model.id,
				local_path: model.local_path ?? model.download_progress?.local_path ?? null,
				hf_ref: model.download?.hf_ref,
				repo: model.download?.repo,
				quant: model.quant,
				target_dir: model.download?.target_dir ?? model.target_dir,
				gpu_mode: model.gpu_mode,
				ctx_size: model.effective_context_length,
				alias: model.name,
				tags: [
					model.parameter_count || `${model.params_b.toFixed(1)}B`,
					model.quant,
					model.fit_level,
					model.use_case
				],
				preset: model.preset,
				load_now: loadNow
			}
		);
	}
}
