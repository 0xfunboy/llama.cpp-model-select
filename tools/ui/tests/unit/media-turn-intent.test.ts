import { BuiltInTool, ContentPartType, MessageRole } from '$lib/enums';
import type { OpenAIToolDefinition } from '$lib/types';
import {
	classifyMediaTurn,
	filterMediaToolsForTurn,
	isEmptyAgenticTurn,
	latestUserTurnText
} from '$lib/utils/media-turn-intent';
import { describe, expect, it } from 'vitest';

function names(value: ReadonlySet<'image' | 'video'>): string[] {
	return [...value].sort();
}

function tool(name: string): OpenAIToolDefinition {
	return {
		function: {
			description: `${name} test tool`,
			name,
			parameters: { properties: {}, type: 'object' }
		},
		type: 'function'
	} as OpenAIToolDefinition;
}

describe('per-turn media intent', () => {
	it.each([
		["Genera un'immagine di una marmotta", ['image']],
		['Ok, mostramela generando una immagine', ['image']],
		['Fammi vedere una immagine di quella scena', ['image']],
		['Illustrala con luce naturale', ['image']],
		['I want a photo of that scene', ['image']],
		['Create a short video of the same scene', ['video']],
		['Animala e falla muovere', ['video']],
		['Crea una foto e poi genera un video', ['image', 'video']]
	])('recognizes an explicit current-turn request: %s', (text, expected) => {
		expect(names(classifyMediaTurn(text))).toEqual(expected);
	});

	it.each([
		"Genera un testo dopo l'immagine",
		"Scrivi una descrizione di cio che vedi nell'immagine",
		'Continua il racconto, solo testo',
		'Non generare immagini: spiegamelo a parole',
		'Tell me what you think about image generation models',
		'Generate an image prompt for Z-Image',
		'Edit the image model settings',
		'Describe the image in prose without creating another one'
	])('does not inherit or infer media authorization: %s', (text) => {
		expect(names(classifyMediaTurn(text))).toEqual([]);
	});

	it('lets a specific denial coexist with a request for the other modality', () => {
		expect(names(classifyMediaTurn("Non creare un'immagine; genera invece un video"))).toEqual([
			'video'
		]);
	});

	it('reads only the latest user turn, including text content parts', () => {
		const messages = [
			{ content: "Genera un'immagine", role: MessageRole.USER },
			{ content: 'Fatto.', role: MessageRole.ASSISTANT },
			{
				content: [
					{ text: 'Ora scrivi soltanto un testo.', type: ContentPartType.TEXT },
					{ image_url: { url: 'data:image/png;base64,AA==' }, type: ContentPartType.IMAGE_URL }
				],
				role: MessageRole.USER
			}
		];

		expect(latestUserTurnText(messages)).toBe('Ora scrivi soltanto un testo.');
		expect(names(classifyMediaTurn(latestUserTurnText(messages)))).toEqual([]);
	});

	it('filters only media tools and preserves unrelated capabilities', () => {
		const tools = [
			tool(BuiltInTool.BROWSER_GENERATE_IMAGE),
			tool(BuiltInTool.BROWSER_GENERATE_VIDEO),
			tool(BuiltInTool.BROWSER_GET_DATETIME),
			tool('mcp_search')
		];
		const filtered = filterMediaToolsForTurn(tools, new Set(['image']));

		expect(filtered.map((definition) => definition.function.name)).toEqual([
			BuiltInTool.BROWSER_GENERATE_IMAGE,
			BuiltInTool.BROWSER_GET_DATETIME,
			'mcp_search'
		]);
	});

	it('recognizes only a completely empty non-tool turn as recoverable', () => {
		expect(isEmptyAgenticTurn('', '', 0)).toBe(true);
		expect(isEmptyAgenticTurn('  \n', '\t', 0)).toBe(true);
		expect(isEmptyAgenticTurn('Si.', '', 0)).toBe(false);
		expect(isEmptyAgenticTurn('', 'reasoning', 0)).toBe(false);
		expect(isEmptyAgenticTurn('', '', 1)).toBe(false);
	});
});
