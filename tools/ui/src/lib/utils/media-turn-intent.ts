import { BuiltInTool, ContentPartType, MessageRole } from '$lib/enums';
import type { ApiChatMessageData, OpenAIToolDefinition } from '$lib/types';

export type MediaTurnKind = 'image' | 'video';

const IMAGE_NOUN = String.raw`(?:image|images|picture|pictures|photo|photos|photograph|illustration|drawing|artwork|immagin(?:e|i)|foto|fotografia|illustrazion(?:e|i)|disegno)`;
const VIDEO_NOUN = String.raw`(?:video|videos|clip|movie|animation|filmato|filmati|animazion(?:e|i))`;
const CREATE_VERB = String.raw`(?:generate|create|make|draw|render|illustrate|edit|modify|regenerate|redo|produce|animate|genera(?:re)?|crea(?:re)?|disegna(?:re)?|illustra(?:re)?|renderizza(?:re)?|modifica(?:re)?|rigenera(?:re)?|rifai|produci|anima(?:re)?)`;
const WANT_VERB = String.raw`(?:i\s+(?:want|would\s+like|need)|vorrei|voglio|mi\s+serve)`;
const TEXT_OBJECT =
	/\b(?:text|prose|story|description|caption|explanation|analysis|summary|words|prompt|testo|prosa|racconto|descrizione|didascalia|spiegazione|analisi|riassunto|parole)\b/iu;
const MEDIA_METADATA_TAIL =
	/^\s+(?:prompt|model|checkpoint|generator|settings|parameters|workflow|modello|generatore|impostazioni|parametri)\b/iu;
const IMAGE_DENIAL_PATTERNS = [
	new RegExp(
		String.raw`\b(?:do\s+not|don't|dont|without|no|non|senza)\b[^,;.!?\n]{0,45}\b(?:generate|create|make|genera(?:re)?|crea(?:re)?|fare|produrre)?\s*${IMAGE_NOUN}\b`,
		'iu'
	),
	/\b(?:text\s+only|only\s+text|solo\s+testo|soltanto\s+testo|non\s+un'?immagine)\b/iu
];
const VIDEO_DENIAL_PATTERNS = [
	new RegExp(
		String.raw`\b(?:do\s+not|don't|dont|without|no|non|senza)\b[^,;.!?\n]{0,45}\b(?:generate|create|make|genera(?:re)?|crea(?:re)?|fare|produrre)?\s*${VIDEO_NOUN}\b`,
		'iu'
	),
	/\b(?:text\s+only|only\s+text|solo\s+testo|soltanto\s+testo|non\s+un\s+video)\b/iu
];

function contentText(content: ApiChatMessageData['content']): string {
	if (typeof content === 'string') return content;

	return content
		.map((part) =>
			part.type === ContentPartType.TEXT && typeof part.text === 'string' ? part.text : ''
		)
		.filter(Boolean)
		.join('\n');
}

function hasDeniedKind(text: string, patterns: readonly RegExp[]): boolean {
	return patterns.some((pattern) => pattern.test(text));
}

function hasCreateRequest(text: string, noun: string): boolean {
	const afterVerb = new RegExp(
		String.raw`\b${CREATE_VERB}\b(?<bridge>[^.!?\n]{0,80})\b${noun}\b(?<tail>[^.!?\n]{0,24})`,
		'iu'
	).exec(text);

	// "Generate text after the image" refers to the image but asks for text.
	if (
		afterVerb &&
		!TEXT_OBJECT.test(afterVerb.groups?.bridge ?? '') &&
		!MEDIA_METADATA_TAIL.test(afterVerb.groups?.tail ?? '')
	) {
		return true;
	}

	const beforeVerb = new RegExp(
		String.raw`\b${noun}\b[^.!?\n]{0,50}\b(?:edit|modify|regenerate|redo|animate|modifica(?:re)?|rigenera(?:re)?|rifai|anima(?:re)?)\b`,
		'iu'
	);

	return beforeVerb.test(text);
}

function hasWantedMedia(text: string, noun: string): boolean {
	return new RegExp(String.raw`\b${WANT_VERB}\b[^.!?\n]{0,50}\b${noun}\b`, 'iu').test(text);
}

function hasImageRequest(text: string): boolean {
	return (
		hasCreateRequest(text, IMAGE_NOUN) ||
		hasWantedMedia(text, IMAGE_NOUN) ||
		/\b(?:make|draw|create|generate|show)\s+me\s+(?:an?\s+)?(?:image|picture|photo|illustration)\b/iu.test(
			text
		) ||
		/\b(?:fammi|fai)\s+(?:una?\s+)?(?:immagine|foto|fotografia|illustrazione|disegno)\b/iu.test(
			text
		) ||
		/\bfammi\s+vedere\s+(?:una?\s+)?(?:immagine|foto|fotografia|illustrazione|disegno)\b/iu.test(
			text
		) ||
		/\b(?:and|e)\s+(?:an?|una?)\s+(?:image|picture|photo|illustration|immagine|foto|illustrazione)\b/iu.test(
			text
		) ||
		/\b(?:show\s+it\s+to\s+me|show\s+me\s+that|visualize\s+it|turn\s+it\s+into\s+an?\s+image)\b/iu.test(
			text
		) ||
		/\b(?:mostramela|fammela\s+vedere|illustrala|disegnala|visualizzala|rigenerala|rifalla|modificala)\b/iu.test(
			text
		)
	);
}

function hasVideoRequest(text: string): boolean {
	return (
		hasCreateRequest(text, VIDEO_NOUN) ||
		hasWantedMedia(text, VIDEO_NOUN) ||
		/\b(?:make|create|generate|show)\s+me\s+(?:a\s+)?(?:video|clip|animation)\b/iu.test(text) ||
		/\b(?:fammi|fai)\s+(?:un\s+)?(?:video|filmato|clip|animazione)\b/iu.test(text) ||
		/\bfammi\s+vedere\s+(?:un\s+)?(?:video|filmato|clip|animazione)\b/iu.test(text) ||
		/\b(?:and|e)\s+(?:a|un)\s+(?:video|clip|animation|filmato|animazione)\b/iu.test(text) ||
		/\b(?:animate\s+it|turn\s+it\s+into\s+(?:a\s+)?video|make\s+it\s+move)\b/iu.test(text) ||
		/\b(?:animala|fall[oa]\s+muovere|trasformal[oa]\s+in\s+(?:un\s+)?video|rigenera(?:lo|re)\s+come\s+video)\b/iu.test(
			text
		)
	);
}

/** Only the current user turn can authorize a new local media generation. */
export function latestUserTurnText(messages: readonly ApiChatMessageData[]): string {
	for (let index = messages.length - 1; index >= 0; index -= 1) {
		const message = messages[index];

		if (message.role === MessageRole.USER) return contentText(message.content).trim();
	}

	return '';
}

export function classifyMediaTurn(text: string): ReadonlySet<MediaTurnKind> {
	const requested = new Set<MediaTurnKind>();
	const clean = text.trim();

	if (!clean) return requested;

	if (!hasDeniedKind(clean, IMAGE_DENIAL_PATTERNS) && hasImageRequest(clean)) {
		requested.add('image');
	}

	if (!hasDeniedKind(clean, VIDEO_DENIAL_PATTERNS) && hasVideoRequest(clean)) {
		requested.add('video');
	}

	return requested;
}

export function mediaToolKind(tool: OpenAIToolDefinition): MediaTurnKind | null {
	if (tool.function.name === BuiltInTool.BROWSER_GENERATE_IMAGE) return 'image';

	if (tool.function.name === BuiltInTool.BROWSER_GENERATE_VIDEO) return 'video';

	return null;
}

/** Keep every non-media tool; media is a per-turn capability, not ambient state. */
export function filterMediaToolsForTurn(
	tools: readonly OpenAIToolDefinition[],
	requestedKinds: ReadonlySet<MediaTurnKind>
): OpenAIToolDefinition[] {
	return tools.filter((tool) => {
		const kind = mediaToolKind(tool);

		return kind === null || requestedKinds.has(kind);
	});
}

/** An entirely blank model turn is never a useful final answer. Retry at most once. */
export function isEmptyAgenticTurn(
	content: string,
	reasoningContent: string,
	toolCallCount: number
): boolean {
	return toolCallCount === 0 && content.trim().length === 0 && reasoningContent.trim().length === 0;
}
