import type { Element, Root, RootContent } from 'hast';

/**
 * Remove rendered image nodes while preserving every surrounding text node.
 *
 * This is deliberately a HAST transform rather than a Markdown regexp: image
 * syntax can be nested inside links and may contain escaped brackets. The
 * generated-media renderer already owns the canonical local asset, so a later
 * assistant acknowledgement must not render a second, usually hallucinated,
 * image URL below it.
 */
export function rehypeSuppressImages() {
	return (tree: Root) => {
		removeImages(tree);
	};
}

function removeImages(parent: Root | Element): void {
	const nextChildren: RootContent[] = [];

	for (const child of parent.children) {
		if (child.type === 'element' && child.tagName === 'img') continue;

		if (child.type === 'element') {
			removeImages(child);

			// Markdown images are sometimes wrapped in a link. Do not leave an
			// empty clickable target after removing the image.
			if (child.tagName === 'a' && child.children.length === 0) continue;
		}

		nextChildren.push(child);
	}

	parent.children = nextChildren;
}
