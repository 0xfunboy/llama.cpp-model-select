import { type MediaModelState, MediaService } from '$lib/services/media.service';
import { mediaStore } from '$lib/stores/media.svelte';
import { afterEach, describe, expect, it, vi } from 'vitest';

function model(overrides: Partial<MediaModelState> = {}): MediaModelState {
	return {
		available: true,
		chat_enabled: true,
		default_for_chat: false,
		defaults: {},
		description: 'Fast local image model',
		download_progress: 100,
		id: 'image-ready',
		kind: 'image',
		label: 'Image Ready',
		tool: { routing: 'Use for the default image workflow.' },
		...overrides
	};
}

afterEach(() => {
	vi.restoreAllMocks();
});

describe('media model catalog cache', () => {
	it('caches the backend catalog reactively and exposes only usable chat models to tools', async () => {
		const response = [
			model(),
			model({ available: false, id: 'image-downloading' }),
			model({ chat_enabled: false, id: 'image-manual' }),
			model({ id: 'image-without-routing', tool: { routing: '' } }),
			model({ id: 'video-ready', kind: 'video', tool: { routing: 'Use for local video.' } })
		];
		const request = vi.spyOn(MediaService, 'getModels').mockResolvedValue(response);

		await expect(mediaStore.models()).resolves.toEqual(response);

		expect(request).toHaveBeenCalledOnce();
		expect(mediaStore.catalogReady).toBe(true);
		expect(mediaStore.catalog).toEqual(response);
		expect(mediaStore.toolModels('image').map(({ id }) => id)).toEqual(['image-ready']);
		expect(mediaStore.toolModels('video').map(({ id }) => id)).toEqual(['video-ready']);
	});

	it('orders the declared chat default first', async () => {
		vi.spyOn(MediaService, 'getModels').mockResolvedValue([
			model({ id: 'legacy-first' }),
			model({ default_for_chat: true, id: 'preferred' })
		]);

		await mediaStore.models();

		expect(mediaStore.toolModels('image').map(({ id }) => id)).toEqual([
			'preferred',
			'legacy-first'
		]);
	});

	it('binds a dialog selection to one matching tool call and then consumes it', async () => {
		vi.spyOn(MediaService, 'getModels').mockResolvedValue([
			model({ default_for_chat: true, id: 'preferred' })
		]);
		await mediaStore.models();

		mediaStore.armModelSelection('conversation-one-shot', 'image', 'preferred');

		expect(mediaStore.consumeModelSelection('conversation-one-shot', 'image')).toBe('preferred');
		expect(mediaStore.consumeModelSelection('conversation-one-shot', 'image')).toBeNull();
	});

	it('keeps a kind-mismatched selection for the correct tool but expires stale locks', async () => {
		vi.spyOn(MediaService, 'getModels').mockResolvedValue([
			model({ default_for_chat: true, id: 'preferred' })
		]);
		await mediaStore.models();

		const now = 1_800_000_000_000;

		vi.spyOn(Date, 'now').mockReturnValue(now);
		mediaStore.armModelSelection('conversation-kind', 'image', 'preferred');

		expect(() => mediaStore.consumeModelSelection('conversation-kind', 'video')).toThrow(
			'selected an image model'
		);
		expect(mediaStore.consumeModelSelection('conversation-kind', 'image')).toBe('preferred');

		mediaStore.armModelSelection('conversation-expired', 'image', 'preferred', 1_000);
		vi.mocked(Date.now).mockReturnValue(now + 1_001);

		expect(mediaStore.consumeModelSelection('conversation-expired', 'image')).toBeNull();
	});
});
