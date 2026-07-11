export function normalizeModelName(value: string): string {
	return (value || 'Unknown model')
		.replace(/[_-]+/g, ' ')
		.replace(/\s+/g, ' ')
		.replace(/^deepseek ai\s+/i, 'DeepSeek ')
		.replace(/^DeepSeek deepseek\s+/i, 'DeepSeek ')
		.replace(/\bcoder\b/gi, 'Coder')
		.trim();
}

export function compactModelName(value: string): string {
	const full = normalizeModelName(value);
	const size = full.match(/\b\d+(?:\.\d+)?B(?:[- ]A\d+(?:\.\d+)?B)?\b/i);
	if (!size || size.index === undefined) return full.length > 54 ? `${full.slice(0, 51)}…` : full;
	const family = full
		.slice(0, size.index)
		.replace(/[\s._-]+$/g, '')
		.split(/\s+/)
		.slice(0, 4)
		.join(' ');
	return `${family || full.split(/\s+/)[0]} · ${size[0].toUpperCase().replace(' ', '-')}`;
}

export function uniqueModelTags(values: Array<string | null | undefined>, limit = 8): string[] {
	return [...new Set(values.map((value) => value?.trim() ?? '').filter(Boolean))].slice(0, limit);
}
