import { base } from '$app/paths';
import { apiFetch, apiPost } from '$lib/utils';
import { getAuthHeaders } from '$lib/utils/api-headers';

export interface ArchiveStatus {
	object: 'llm-model-select.archive.status';
	database_path: string;
	reports: number;
	results: number;
	best_results: number;
	downloads: number;
	fit_recommendations: number;
	configurations: number;
	events: number;
}

export interface ArchiveImportResponse {
	success: boolean;
	database_path: string;
	reports: number;
	results: number;
	downloads: number;
	fit_recommendations: number;
}

export class ArchiveService {
	static status(): Promise<ArchiveStatus> {
		return apiFetch<ArchiveStatus>('/api/archive/status', { authOnly: true });
	}

	static async exportArchive(): Promise<void> {
		const response = await fetch(base + '/api/archive/export', {
			headers: getAuthHeaders()
		});
		if (!response.ok) {
			throw new Error('Archive export failed: ' + response.status + ' ' + response.statusText);
		}
		const blob = await response.blob();
		const url = URL.createObjectURL(blob);
		const a = document.createElement('a');
		a.href = url;
		a.download = `llm-model-select-archive-${new Date().toISOString().slice(0, 10)}.json`;
		document.body.appendChild(a);
		a.click();
		document.body.removeChild(a);
		URL.revokeObjectURL(url);
	}

	static importArchive(archive: Record<string, unknown>): Promise<ArchiveImportResponse> {
		return apiPost<ArchiveImportResponse, Record<string, unknown>>('/api/archive/import', archive);
	}
}
