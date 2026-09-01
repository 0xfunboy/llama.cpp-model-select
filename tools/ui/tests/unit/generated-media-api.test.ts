import { AttachmentType } from '$lib/enums';
import { type MediaJobState, MediaService } from '$lib/services/media.service';
import { afterEach, describe, expect, it, vi } from 'vitest';

function successfulJsonResponse(body: unknown = {}): Response {
	return new Response(JSON.stringify(body), {
		headers: { 'Content-Type': 'application/json' },
		status: 200
	});
}

afterEach(() => {
	vi.useRealTimers();
	vi.unstubAllGlobals();
	vi.restoreAllMocks();
});

describe('MediaService ownership API', () => {
	it('posts the canonical owner binding payload', async () => {
		const fetchMock = vi.fn().mockResolvedValue(successfulJsonResponse({ added: 2 }));

		vi.stubGlobal('fetch', fetchMock);

		await MediaService.addAssetOwners('conversation 1', [
			{ assetId: 'asset-1', messageId: 'message-1' },
			{ assetId: 'asset-2' }
		]);

		expect(fetchMock).toHaveBeenCalledOnce();
		const [url, init] = fetchMock.mock.calls[0] as [string, RequestInit];

		expect(url).toBe('/api/media/assets/owners');
		expect(init.method).toBe('POST');
		expect(new Headers(init.headers).get('Content-Type')).toBe('application/json');
		expect(JSON.parse(String(init.body))).toEqual({
			assets: [{ asset_id: 'asset-1', message_id: 'message-1' }, { asset_id: 'asset-2' }],
			conversation_id: 'conversation 1'
		});
	});

	it('deletes conversation and message assets through encoded owner routes', async () => {
		const fetchMock = vi
			.fn()
			.mockImplementation(async () => successfulJsonResponse({ deleted: 1, released: 1 }));

		vi.stubGlobal('fetch', fetchMock);

		await MediaService.cleanupConversationAssets('conversation/with spaces');
		await MediaService.cleanupMessageAssets('message/with spaces');

		expect(fetchMock).toHaveBeenNthCalledWith(
			1,
			'/api/media/conversations/conversation%2Fwith%20spaces/assets',
			expect.objectContaining({ method: 'DELETE' })
		);
		expect(fetchMock).toHaveBeenNthCalledWith(
			2,
			'/api/media/messages/message%2Fwith%20spaces/assets',
			expect.objectContaining({ method: 'DELETE' })
		);
	});

	it('does not call the backend for an empty owner list', async () => {
		const fetchMock = vi.fn();

		vi.stubGlobal('fetch', fetchMock);

		await MediaService.addAssetOwners('conversation-1', []);

		expect(fetchMock).not.toHaveBeenCalled();
	});
});

describe('MediaService job lifecycle', () => {
	it('posts a real cancellation request for a live media job', async () => {
		const cancelled = {
			elapsed_seconds: 12,
			error: null,
			eta_seconds: 0,
			id: 'media-cancel-1',
			kind: 'image',
			message: 'Annullamento richiesto.',
			model: 'krea2-snofs-turbo-fp8',
			phase: 'cancelled',
			progress: 37,
			queue_position: 0,
			result: null,
			status: 'cancelled'
		} satisfies MediaJobState;
		const fetchMock = vi.fn().mockResolvedValue(successfulJsonResponse(cancelled));

		vi.stubGlobal('fetch', fetchMock);

		await expect(MediaService.cancelJob(cancelled.id)).resolves.toEqual(cancelled);
		expect(fetchMock).toHaveBeenCalledOnce();
		expect(fetchMock.mock.calls[0][0]).toBe(`/api/media/jobs/${cancelled.id}/cancel`);
		expect(fetchMock.mock.calls[0][1]).toEqual(
			expect.objectContaining({ body: '{}', method: 'POST' })
		);
	});

	it('cancels by owner when an aborted submit was accepted without returning a job id', async () => {
		const controller = new AbortController();
		const fetchMock = vi
			.fn()
			.mockImplementationOnce(async () => {
				controller.abort();

				throw new DOMException('Aborted', 'AbortError');
			})
			.mockResolvedValueOnce(successfulJsonResponse({ cancelled_jobs: ['job-1'] }));

		vi.stubGlobal('fetch', fetchMock);

		await expect(
			MediaService.runJob(
				{ kind: 'image', model: 'pony-v6-xl', prompt: 'a marmot' },
				{
					conversationId: 'conv-1',
					ownerMessageId: 'tool-owner-1',
					signal: controller.signal
				}
			)
		).rejects.toThrow('Aborted');

		expect(fetchMock).toHaveBeenCalledTimes(2);
		expect(fetchMock.mock.calls[1][0]).toBe('/api/media/messages/tool-owner-1/assets');
		expect(fetchMock.mock.calls[1][1]).toEqual(expect.objectContaining({ method: 'DELETE' }));
	});

	it('publishes a terminal failed reference instead of leaving the pending ref behind', async () => {
		vi.useFakeTimers();
		const queued: MediaJobState = {
			elapsed_seconds: 0,
			error: null,
			eta_seconds: null,
			id: 'job-failed-1',
			kind: 'image',
			message: 'Queued',
			model: 'pony-v6-xl',
			phase: 'queued',
			progress: 0,
			queue_position: 0,
			result: null,
			status: 'queued'
		};
		const failed: MediaJobState = {
			...queued,
			elapsed_seconds: 2,
			error: 'CUDA worker exited',
			message: 'Generation failed',
			phase: 'failed',
			status: 'failed'
		};
		const fetchMock = vi
			.fn()
			.mockResolvedValueOnce(successfulJsonResponse(queued))
			.mockResolvedValueOnce(successfulJsonResponse(failed));
		const onSubmitted = vi.fn();
		const onTerminal = vi.fn();

		vi.stubGlobal('fetch', fetchMock);
		const run = MediaService.runJob(
			{ kind: 'image', model: 'pony-v6-xl', prompt: 'a marmot' },
			{
				conversationId: 'conv-1',
				onSubmitted,
				onTerminal,
				ownerMessageId: 'tool-1'
			}
		);
		const rejection = expect(run).rejects.toThrow('CUDA worker exited');

		await vi.advanceTimersByTimeAsync(1000);
		await rejection;
		expect(onSubmitted).toHaveBeenCalledWith(
			queued,
			expect.objectContaining({
				jobId: queued.id,
				status: 'pending',
				type: AttachmentType.GENERATED_MEDIA
			})
		);
		expect(onTerminal).toHaveBeenCalledOnce();
		expect(onTerminal.mock.calls[0][1]).toEqual(
			expect.objectContaining({
				error: 'CUDA worker exited',
				jobId: queued.id,
				status: 'failed',
				type: AttachmentType.GENERATED_MEDIA
			})
		);
	});
});
