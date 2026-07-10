import { ROUTES } from '$lib/constants/routes';
import { apiFetch } from '$lib/utils';

export interface LocalRouteEvent {
	event_type: 'decision' | 'feedback';
	object_id: string;
	created_at: string;
	payload: Record<string, unknown>;
}

export class RouterService {
	static chat(id: string): string {
		return `${ROUTES.CHAT}/${id}`;
	}

	static settings(section: string): string {
		return `${ROUTES.SETTINGS}/${section}`;
	}

	static localRouteEvents(limit = 50): Promise<{ object: string; data: LocalRouteEvent[] }> {
		return apiFetch(`/api/router/decisions?limit=${limit}`, { authOnly: true });
	}
}
