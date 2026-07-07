import { apiFetch, apiPost } from '$lib/utils';

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
	effective_context_length: number;
	use_case: string;
	fit_level: 'perfect' | 'good' | 'marginal' | 'too_tight';
	score: number;
	score_components: {
		quality: number;
		speed: number;
		fit: number;
		context: number;
	};
	estimated_tps: number;
	memory_required_gb: number;
	memory_available_gb: number;
	weights_gb: number;
	kv_cache_gb: number;
	overhead_gb: number;
	utilization_pct: number;
	gpu_mode: string;
	runtime: string;
	installed: boolean;
	installed_model_id?: string;
	local_path?: string | null;
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
		return apiPost<FitAdvisorDownloadResponse, Record<string, unknown>>('/api/fit-advisor/download', {
			model_id: model.id,
			hf_ref: model.download?.hf_ref,
			repo: model.download?.repo,
			quant: model.quant
		});
	}

	static configure(
		model: FitAdvisorModel,
		loadNow = false
	): Promise<FitAdvisorConfigureResponse> {
		return apiPost<FitAdvisorConfigureResponse, FitAdvisorConfigureRequest>(
			'/api/fit-advisor/configure',
			{
				model_id: model.id,
				local_path: model.local_path,
				hf_ref: model.download?.hf_ref,
				repo: model.download?.repo,
				quant: model.quant,
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
