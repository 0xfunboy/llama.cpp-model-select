import { rehypeSuppressImages } from '$lib/components/app/content/MarkdownContent/plugins/rehype/suppress-images';
import rehypeStringify from 'rehype-stringify';
import { remark } from 'remark';
import remarkRehype from 'remark-rehype';
import { describe, expect, it } from 'vitest';

async function renderSuppressed(markdown: string): Promise<string> {
	const processor = remark().use(remarkRehype).use(rehypeSuppressImages).use(rehypeStringify);

	return String(await processor.process(markdown));
}

describe('generated-media Markdown suppression', () => {
	it('removes duplicate images and their empty wrappers but preserves useful prose and links', async () => {
		const html = await renderSuppressed(
			'Here is the useful answer before ![duplicate](broken-generated-url) and after.\n\n' +
				'[![linked duplicate](broken-linked-url)](https://example.com/duplicate)\n\n' +
				'Read the [normal documentation](https://example.com/docs).'
		);

		expect(html).not.toContain('<img');
		expect(html).not.toContain('broken-generated-url');
		expect(html).not.toContain('broken-linked-url');
		expect(html).not.toContain('example.com/duplicate');
		expect(html).toContain('Here is the useful answer before  and after.');
		expect(html).toContain('href="https://example.com/docs"');
		expect(html).toContain('normal documentation');
	});
});
