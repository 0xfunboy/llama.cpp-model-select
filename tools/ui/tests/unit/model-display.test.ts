import { describe, expect, it } from 'vitest';
import { compactModelName, normalizeModelName, uniqueModelTags } from '$lib/utils/model-display';

describe('model display normalization', () => {
	it('normalizes separators and provider noise', () => {
		expect(normalizeModelName('deepseek-ai_deepseek-coder-33b-base')).toBe(
			'DeepSeek Coder 33b base'
		);
	});

	it('keeps a stable family and size label for long finetune names', () => {
		expect(
			compactModelName('Qwen3.5-21B-Claude-4.6-Opus-Deckard-Heretic-Uncensored-Thinking')
		).toBe('Qwen3.5 · 21B');
	});

	it('deduplicates presentation tags', () => {
		expect(uniqueModelTags(['Q4_K_M', 'coding', 'Q4_K_M', '', undefined])).toEqual([
			'Q4_K_M',
			'coding'
		]);
	});
});
