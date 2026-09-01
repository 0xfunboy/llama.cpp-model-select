import { AgenticSectionType, AttachmentType, BuiltInTool } from '$lib/enums';
import type { MediaJobState } from '$lib/services/media.service';
import { mediaStore } from '$lib/stores';
import type {
	AgenticSection,
	DatabaseMessageExtraGeneratedMediaCompleted,
	DatabaseMessageExtraImageFile
} from '$lib/types';
import { render } from 'svelte/server';
import { afterAll, beforeAll, describe, expect, it, vi } from 'vitest';

let GeneratedMediaRenderer: typeof import('$lib/components/app/chat/ChatMessages/ChatMessage/ChatMessageToolCall/ChatMessageToolCallBlockGenerateMedia.svelte').default;

beforeAll(async () => {
	// The component reaches the app barrel, which also exposes PDF preview helpers.
	// pdf.js reads DOMMatrix at module load even though this renderer never uses it.
	vi.stubGlobal('DOMMatrix', class DOMMatrix {});
	GeneratedMediaRenderer = (
		await import('$lib/components/app/chat/ChatMessages/ChatMessage/ChatMessageToolCall/ChatMessageToolCallBlockGenerateMedia.svelte')
	).default;
}, 30_000);

afterAll(() => {
	vi.unstubAllGlobals();
});

function makeGeneratedImage(): DatabaseMessageExtraGeneratedMediaCompleted {
	return {
		assetId: 'asset-inline-1',
		assetUrl: '/api/media/assets/asset-inline-1',
		conversationId: 'conv-inline',
		height: 1024,
		jobId: 'media_inline_1',
		kind: 'image',
		mimeType: 'image/png',
		model: 'pony-v6-xl',
		name: 'asset-inline-1.png',
		ownerMessageId: 'tool-inline-1',
		prompt: 'a marmot wrapping a chocolate bar',
		size: 4096,
		status: 'completed',
		type: AttachmentType.GENERATED_MEDIA,
		width: 1024
	};
}

describe('generated-media tool renderer', () => {
	it('labels the LLM tool-argument phase as prompt generation', () => {
		const imageSection: AgenticSection = {
			content: '',
			toolCallId: 'call-prompt-image',
			toolName: BuiltInTool.BROWSER_GENERATE_IMAGE,
			type: AgenticSectionType.TOOL_CALL_STREAMING
		};
		const videoSection: AgenticSection = {
			...imageSection,
			toolCallId: 'call-prompt-video',
			toolName: BuiltInTool.BROWSER_GENERATE_VIDEO
		};
		const image = render(GeneratedMediaRenderer, {
			props: { isStreaming: true, open: false, section: imageSection }
		});
		const video = render(GeneratedMediaRenderer, {
			props: { isStreaming: true, open: false, section: videoSection }
		});

		expect(image.body).toContain('Generating prompt');
		expect(image.body).not.toContain('>Generate image<');
		expect(video.body).toContain('Generating video prompt');
	});

	it('keeps detailed backend progress visible while the tool block is collapsed', () => {
		const generated = makeGeneratedImage();
		const pending = { ...generated, status: 'pending' as const };
		const section: AgenticSection = {
			content: 'Generating image locally.',
			toolCallId: 'call-live-media-1',
			toolName: BuiltInTool.BROWSER_GENERATE_IMAGE,
			toolResultExtras: [pending],
			type: AgenticSectionType.TOOL_CALL
		};
		const state: MediaJobState = {
			average_step_seconds: 4.1,
			can_cancel: true,
			elapsed_seconds: 12,
			error: null,
			estimated_total_seconds: 30,
			eta_seconds: 18,
			generation_elapsed_seconds: 9,
			height: 688,
			id: 'media-live-1',
			kind: 'image',
			message: 'Il modello sta eseguendo la diffusione.',
			model: 'krea2-snofs-turbo-fp8',
			model_label: 'Krea 2 Turbo + SNOFS v1.3D',
			phase: 'generating',
			preview_height: 256,
			preview_step: 3,
			preview_url: '/api/media/previews/preview-capability',
			preview_width: 448,
			progress: 37.5,
			progress_source: 'measured',
			queue_position: 2,
			result: null,
			sample_step: 3,
			sample_steps: 8,
			status: 'generating',
			step_seconds: 4.3,
			width: 1216
		};

		mediaStore.set(section.toolCallId!, state);
		const { body } = render(GeneratedMediaRenderer, {
			props: { isStreaming: true, open: false, section }
		});

		expect(body).toContain('Generating image');
		expect(body).toContain('data-testid="media-live-card"');
		expect(body).toContain('Krea 2 Turbo + SNOFS v1.3D');
		expect(body).toContain('Diffusione');
		expect(body).toContain('12s elapsed');
		expect(body).toContain('circa 18s rimanenti');
		expect(body).toContain('~30s total');
		expect(body).toContain('step 3/8');
		expect(body).toContain('4.3s/step');
		expect(body).toContain('1216x688');
		expect(body).toContain('queue 2');
		expect(body).toContain('src="/api/media/previews/preview-capability?preview_step=3"');
		expect(body).toContain('Cancel');
	});

	it('renders the tool-result asset inline and does not use fallback assistant attachments', () => {
		const generated = makeGeneratedImage();
		const fallback: DatabaseMessageExtraImageFile = {
			base64Url: 'data:image/png;base64,c2hvdWxkLW5vdC1yZW5kZXI=',
			name: 'wrong-assistant-extra.png',
			type: AttachmentType.IMAGE
		};
		const section: AgenticSection = {
			content: 'Image generated locally.',
			toolCallId: 'call-inline-1',
			toolName: BuiltInTool.BROWSER_GENERATE_IMAGE,
			toolResult: 'Image generated locally.',
			toolResultExtras: [generated],
			type: AgenticSectionType.TOOL_CALL
		};
		const { body } = render(GeneratedMediaRenderer, {
			props: {
				attachments: [fallback],
				isStreaming: false,
				open: false,
				section
			}
		});

		expect(body).toContain('<figure');
		expect(body).toContain('Image generated');
		expect(body).toContain('aria-label="Apri immagine generata a piena risoluzione"');
		expect(body).toContain('cursor-zoom-in');
		expect(body).toContain('src="/api/media/assets/asset-inline-1"');
		expect(body).toContain('a marmot wrapping a chocolate bar');
		expect(body).not.toContain('wrong-assistant-extra.png');
		expect(body).not.toContain('c2hvdWxkLW5vdC1yZW5kZXI=');
		expect(body).not.toContain('target="_blank"');
	});

	it('renders completed video as an inline player', () => {
		const video: DatabaseMessageExtraGeneratedMediaCompleted = {
			...makeGeneratedImage(),
			assetId: 'asset-video-1',
			assetUrl: '/api/media/assets/asset-video-1',
			jobId: 'media_video_1',
			kind: 'video',
			mimeType: 'video/webm',
			name: 'asset-video-1.webm',
			prompt: 'the marmot waves at the camera'
		};
		const section: AgenticSection = {
			content: 'Video generated locally.',
			toolCallId: 'call-video-1',
			toolName: BuiltInTool.BROWSER_GENERATE_VIDEO,
			toolResultExtras: [video],
			type: AgenticSectionType.TOOL_CALL
		};
		const { body } = render(GeneratedMediaRenderer, {
			props: { isStreaming: false, open: false, section }
		});

		expect(body).toContain('<video');
		expect(body).toContain('controls');
		expect(body).toContain('playsinline');
		expect(body).toContain('src="/api/media/assets/asset-video-1"');
		expect(body).not.toContain('target="_blank"');
	});
});
