import {
	buildGenerateImageToolDefinition,
	buildGenerateVideoToolDefinition
} from '$lib/constants/generate-media';
import type { MediaModelState } from '$lib/services/media.service';
import { describe, expect, it } from 'vitest';

function mediaModel(
	id: string,
	kind: 'image' | 'video',
	routing: string,
	overrides: Partial<MediaModelState> = {}
): MediaModelState {
	return {
		available: true,
		chat_enabled: true,
		default_for_chat: false,
		defaults: {},
		description: `description for ${id}`,
		download_progress: 100,
		id,
		kind,
		label: id,
		tool: { routing },
		...overrides
	};
}

function modelEnum(definition: ReturnType<typeof buildGenerateImageToolDefinition>): string[] {
	const parameters = definition.function.parameters as {
		properties: { model: { enum: string[] } };
	};

	return parameters.properties.model.enum;
}

describe('generated-media tool routing contract', () => {
	it('builds the image enum and routing instructions from live eligible models', () => {
		const models = [
			mediaModel('photo-fast', 'image', 'Use by default for fast realistic photography.'),
			mediaModel('illustration', 'image', 'Use for anime and stylized illustration.'),
			mediaModel('still-downloading', 'image', 'Never exposed while unavailable.', {
				available: false
			}),
			mediaModel('manual-only', 'image', 'Never exposed to chat.', { chat_enabled: false }),
			mediaModel('missing-contract', 'image', '   '),
			mediaModel('video-model', 'video', 'Use for video.'),
			mediaModel('photo-fast', 'image', 'Duplicate ids are ignored.')
		];
		const definition = buildGenerateImageToolDefinition(models);

		expect(modelEnum(definition)).toEqual(['photo-fast', 'illustration']);
		expect(definition.function.description).toContain(
			'photo-fast: Use by default for fast realistic photography.'
		);
		expect(definition.function.description).toContain(
			'illustration: Use for anime and stylized illustration.'
		);
		expect(definition.function.description).not.toContain('Never exposed while unavailable.');
		expect(definition.function.description).not.toContain('Never exposed to chat.');
		expect(definition.function.description).not.toContain('Duplicate ids are ignored.');
		expect(definition.function.description).toContain(
			'answer with useful text only and never invent a Markdown image, link or attachment URL'
		);
		expect(definition.function.description).toContain(
			'latest user message explicitly asks to create, modify or regenerate an image'
		);
	});

	it('uses a fail-closed sentinel while the image catalog is unavailable', () => {
		const definition = buildGenerateImageToolDefinition();

		expect(modelEnum(definition)).toEqual(['__media_catalog_unavailable__']);
		expect(definition.function.description).toContain(
			'Do not call this tool until an available model is advertised.'
		);
	});

	it('prefers the declared chat default and lets the backend derive image dimensions', () => {
		const definition = buildGenerateImageToolDefinition([
			mediaModel('legacy-first', 'image', 'Legacy fallback.'),
			mediaModel('preferred', 'image', 'Default local image route.', {
				default_for_chat: true
			})
		]);
		const parameters = definition.function.parameters as {
			properties: {
				scene: { properties: { aspect_ratio: { description: string } } };
				width?: unknown;
				height?: unknown;
			};
		};

		expect(modelEnum(definition)).toEqual(['preferred', 'legacy-first']);
		expect(parameters.properties).not.toHaveProperty('width');
		expect(parameters.properties).not.toHaveProperty('height');
		expect(parameters.properties.scene.properties.aspect_ratio.description).toContain(
			'Use 1:1 unless the user explicitly requests'
		);
	});

	it('requires action-aware human staging in the image scene contract', () => {
		const definition = buildGenerateImageToolDefinition([
			mediaModel('photo-fast', 'image', 'Use for local photography.')
		]);
		const parameters = definition.function.parameters as {
			properties: {
				scene: {
					properties: {
						focus: { description: string };
						interaction: { description: string };
						shot: { description: string };
						subjects: {
							items: {
								properties: {
									action: { description: string };
									position: { description: string };
								};
							};
						};
					};
				};
			};
		};
		const scene = parameters.properties.scene.properties;

		expect(definition.function.description).toContain('Compose human actions causally');
		expect(definition.function.description).toContain(
			'gripping hand, hammer head, separate target hand'
		);
		expect(scene.subjects.items.properties.action.description).toContain('exact target');
		expect(scene.subjects.items.properties.position.description).toContain('unobstructed');
		expect(scene.interaction.description).toContain('contact topology');
		expect(scene.shot.description).toContain('contextual medium close-up/action shot');
		expect(scene.focus.description).toContain('target/contact point');
	});

	it('builds video routing dynamically without assuming a checkpoint family', () => {
		const definition = buildGenerateVideoToolDefinition([
			mediaModel('local-video-a', 'video', 'Use for the fastest local preview.'),
			mediaModel('local-video-b', 'video', 'Use when the user explicitly asks for quality.'),
			mediaModel('image-model', 'image', 'Do not expose to the video tool.')
		]);

		expect(modelEnum(definition)).toEqual(['local-video-a', 'local-video-b']);
		expect(definition.function.description).toContain(
			'local-video-a: Use for the fastest local preview.'
		);
		expect(definition.function.description).toContain(
			'local-video-b: Use when the user explicitly asks for quality.'
		);
		expect(definition.function.description).not.toContain('Do not expose to the video tool.');
		expect(definition.function.description).not.toContain('FastWan');
		expect(definition.function.description).toContain(
			'latest user message explicitly asks to create, modify or regenerate a video'
		);
	});
});
