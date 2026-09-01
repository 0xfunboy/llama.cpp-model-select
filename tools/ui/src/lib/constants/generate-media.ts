import { BuiltInTool, JsonSchemaType, ToolCallType } from '$lib/enums';
import type { MediaModelState } from '$lib/services/media.service';
import type { OpenAIToolDefinition } from '$lib/types';

const STRING_ARRAY = {
	items: { type: JsonSchemaType.STRING },
	type: 'array'
} as const;
const IMAGE_SUBJECT = {
	additionalProperties: false,
	properties: {
		action: {
			description:
				'Visible action in active English. Name the acting limb or hand, every manipulated tool/object, the exact target, and their direction/contact geometry; never use a vague verb alone.',
			type: JsonSchemaType.STRING
		},
		count: {
			description: 'Exact number of this subject type, from 1 to 12',
			maximum: 12,
			minimum: 1,
			type: JsonSchemaType.NUMBER
		},
		description: {
			description:
				'Positive appearance, identity, clothing and distinguishing attributes; exclusions do not belong here',
			type: JsonSchemaType.STRING
		},
		kind: {
			description:
				'Specific subject type in English, for example adult woman, robot, dog or landscape',
			type: JsonSchemaType.STRING
		},
		position: {
			description:
				'Position and body orientation in final-image coordinates, such as image-left foreground facing image-right. Stage the pose so the acting limb, object and target are separated and unobstructed; never use the subject viewpoint.',
			type: JsonSchemaType.STRING
		}
	},
	required: ['count', 'kind', 'description', 'action', 'position'],
	type: JsonSchemaType.OBJECT
} as const;
const IMAGE_SCENE = {
	additionalProperties: false,
	description:
		"Provider-neutral visual contract. The local backend deterministically compiles it into the selected checkpoint family's exact prompt contract.",
	properties: {
		angle: { description: 'Camera angle in English', type: JsonSchemaType.STRING },
		aspect_ratio: {
			description:
				'Composition aspect ratio. Use 1:1 unless the user explicitly requests a landscape/wide (16:9) or portrait/vertical (9:16) image.',
			enum: ['1:1', '16:9', '9:16'],
			type: JsonSchemaType.STRING
		},
		content_rating: {
			description:
				'safe, suggestive, or explicit. Every depicted person must always be described as an adult.',
			enum: ['safe', 'suggestive', 'explicit'],
			type: JsonSchemaType.STRING
		},
		exact_text: {
			description: 'Exact visible wording requested by the user, otherwise an empty string',
			type: JsonSchemaType.STRING
		},
		focus: {
			description:
				'Explicit visual focus hierarchy. For an action, prioritize the target/contact point, then acting limb and object, then the actor identity; keep each unobstructed.',
			type: JsonSchemaType.STRING
		},
		interaction: {
			description:
				'Exact spatial and contact topology: who or what acts on which target, which limb holds or touches which object, the object orientation, and the visible contact point. Use an empty string only when nothing interacts.',
			type: JsonSchemaType.STRING
		},
		lens: { description: 'Lens or perspective in English', type: JsonSchemaType.STRING },
		lighting: { description: 'Lighting design in English', type: JsonSchemaType.STRING },
		medium: {
			description: 'Requested visual medium',
			enum: [
				'photo',
				'anime',
				'manga',
				'digital_illustration',
				'comic',
				'watercolor',
				'oil_painting',
				'pixel_art',
				'three_d'
			],
			type: JsonSchemaType.STRING
		},
		mood: { description: 'Mood in English', type: JsonSchemaType.STRING },
		must_avoid: {
			...STRING_ARRAY,
			description:
				'Only unwanted visible elements or failures. Never include a requested subject or attribute.'
		},
		must_include: {
			...STRING_ARRAY,
			description:
				'Every indispensable visible detail, one concise English item per entry. For actions include the acting limb, tool/object, target and unobstructed contact or manipulation point.'
		},
		objective: {
			description: 'Literal one-sentence visual objective in English',
			type: JsonSchemaType.STRING
		},
		palette: { description: 'Color palette in English', type: JsonSchemaType.STRING },
		setting: { description: 'Environment and background in English', type: JsonSchemaType.STRING },
		shot: {
			description:
				'Action-aware literal framing. Choose the nearest framing that shows both actor identity/body context and a large readable action zone. For hand or body-part actions prefer a contextual medium close-up/action shot, not a face-only close-up, distant full body, or detached macro crop, unless the user explicitly requested otherwise.',
			type: JsonSchemaType.STRING
		},
		subjects: {
			description:
				'One entry per distinct subject identity/type. Never merge different people, animals or characters.',
			items: IMAGE_SUBJECT,
			maxItems: 8,
			minItems: 1,
			type: 'array'
		}
	},
	required: [
		'objective',
		'medium',
		'content_rating',
		'aspect_ratio',
		'subjects',
		'interaction',
		'shot',
		'angle',
		'lens',
		'focus',
		'setting',
		'lighting',
		'palette',
		'mood',
		'must_include',
		'must_avoid',
		'exact_text'
	],
	type: JsonSchemaType.OBJECT
} as const;
const VIDEO_SCENE = {
	additionalProperties: false,
	description:
		'Temporal scene contract compiled by the backend for FastWan. Keep one coherent clip and one camera strategy.',
	properties: {
		action: { description: 'Main visible action in English', type: JsonSchemaType.STRING },
		appearance: {
			description: 'Stable identity, appearance and wardrobe of every subject',
			type: JsonSchemaType.STRING
		},
		aspect_ratio: {
			enum: ['16:9', '9:16', '1:1'],
			type: JsonSchemaType.STRING
		},
		beats: {
			description: 'Chronological visible changes, from opening state to final state',
			items: { type: JsonSchemaType.STRING },
			maxItems: 5,
			minItems: 1,
			type: 'array'
		},
		camera_motion: {
			description: 'One feasible camera motion/direction for the whole short clip',
			type: JsonSchemaType.STRING
		},
		composition: { description: 'Framing, angle and lens', type: JsonSchemaType.STRING },
		continuity_locks: {
			...STRING_ARRAY,
			description: 'Identity, wardrobe, layout and lighting facts that must never drift'
		},
		final_state: {
			description: 'What must be visibly true in the last frame',
			type: JsonSchemaType.STRING
		},
		lighting: { description: 'Stable lighting and palette', type: JsonSchemaType.STRING },
		must_avoid: {
			...STRING_ARRAY,
			description: 'Scene-specific failures or unwanted visible elements'
		},
		setting: { description: 'Stable environment in English', type: JsonSchemaType.STRING },
		subject: {
			description: 'Exact subject count and identities in English',
			type: JsonSchemaType.STRING
		}
	},
	required: [
		'subject',
		'appearance',
		'action',
		'setting',
		'composition',
		'camera_motion',
		'lighting',
		'beats',
		'continuity_locks',
		'final_state',
		'must_avoid',
		'aspect_ratio'
	],
	type: JsonSchemaType.OBJECT
} as const;
const MEDIA_CATALOG_UNAVAILABLE_MODEL = '__media_catalog_unavailable__';

function modelsForTool(
	models: readonly MediaModelState[],
	kind: 'image' | 'video'
): MediaModelState[] {
	const seen = new Set<string>();

	return models
		.filter((model) => {
			const routing = model.tool?.routing;
			const eligible =
				model.kind === kind &&
				model.available === true &&
				model.chat_enabled === true &&
				typeof routing === 'string' &&
				routing.trim().length > 0 &&
				typeof model.id === 'string' &&
				model.id.trim().length > 0 &&
				!seen.has(model.id);

			if (eligible) seen.add(model.id);

			return eligible;
		})
		.sort(
			(left, right) =>
				Number(right.default_for_chat === true) - Number(left.default_for_chat === true)
		);
}

function modelEnum(models: readonly MediaModelState[]): string[] {
	return models.length > 0 ? models.map((model) => model.id) : [MEDIA_CATALOG_UNAVAILABLE_MODEL];
}

function routingInstructions(models: readonly MediaModelState[], kind: 'image' | 'video'): string {
	if (models.length === 0) {
		return `The local ${kind} model catalog is not ready. Do not call this tool until an available model is advertised.`;
	}

	return [
		`Choose only among these currently available local ${kind} models and follow their routing contracts:`,
		...models.map((model) => `${model.id}: ${model.tool.routing.trim()}`)
	].join('\n');
}

export function buildGenerateImageToolDefinition(
	models: readonly MediaModelState[] = []
): OpenAIToolDefinition {
	const availableModels = modelsForTool(models, 'image');

	return {
		function: {
			description: [
				'Call this tool only when the latest user message explicitly asks to create, modify or regenerate an image. Prior media, earlier requests or text-only follow-ups never authorize an image tool call.',
				'Generate an image locally. First resolve the user request against the conversation, then call this tool with a literal structured scene. Carry prior visual details only when the user refers to them with pronouns, names, "same as before", "again", or an explicit continuation; otherwise do not contaminate the new scene with unrelated chat. Preserve exact subject counts and keep every identity, appearance, action and image-left/image-right position separate.',
				'Compose human actions causally, not as a list of keywords. Before calling the tool, identify the actor and acting limb plus, when present, the manipulated object or tool, exact target and contact/manipulation point. Select camera distance and body orientation from that action: the action-driving parts must be large, inside frame, separate and unobstructed, while the face or enough connected torso/body remains visible so the person and posture are not lost. Show plausible joints, grip, weight and the complete kinematic chain. For a hammer striking a hand, for example, show the gripping hand, hammer head, separate target hand and their exact contact point in one contextual action framing.',
				routingInstructions(availableModels, 'image'),
				'For explicit requests, write literal consenting-adult anatomy and actions in the structured scene instead of euphemisms. The backend knows each checkpoint and injects its exact prompt dialect and runtime parameters: do not add keyword sludge or exclusions to positive fields and do not merely describe the image instead of calling the tool. The UI inserts the generated asset automatically; after this tool, answer with useful text only and never invent a Markdown image, link or attachment URL.'
			].join('\n'),
			name: BuiltInTool.BROWSER_GENERATE_IMAGE,
			parameters: {
				additionalProperties: false,
				properties: {
					model: {
						description: 'Local image checkpoint selected for the requested medium',
						enum: modelEnum(availableModels),
						type: JsonSchemaType.STRING
					},
					negative_prompt: {
						description: 'Optional extra exclusions not already represented by scene.must_avoid',
						type: JsonSchemaType.STRING
					},
					prompt: {
						description:
							'Short literal English fallback brief. Do not add quality-tag sludge or Pony score/source/rating triggers; the backend compiles the selected model contract.',
						type: JsonSchemaType.STRING
					},
					qa_correction: {
						description:
							'Only for the single vision-QA retry: concise visible defects that this revised scene fixes',
						type: JsonSchemaType.STRING
					},
					qa_retry_of_job_id: {
						description:
							'Only after inspecting a generated image and finding an objective mismatch; copy its Job ID. At most one retry is allowed.',
						type: JsonSchemaType.STRING
					},
					scene: IMAGE_SCENE,
					seed: { description: '-1 for random or a fixed integer', type: JsonSchemaType.NUMBER }
				},
				required: ['prompt', 'model', 'scene'],
				type: JsonSchemaType.OBJECT
			}
		},
		type: ToolCallType.FUNCTION
	};
}

export function buildGenerateVideoToolDefinition(
	models: readonly MediaModelState[] = []
): OpenAIToolDefinition {
	const availableModels = modelsForTool(models, 'video');

	return {
		function: {
			description: [
				'Call this tool only when the latest user message explicitly asks to create, modify or regenerate a video. Prior media, earlier requests or text-only follow-ups never authorize a video tool call.',
				'Generate a local video. Resolve referenced identities and prior visual details from the conversation, but ignore unrelated chat. Plan a short chronological clip: exact subject and wardrobe, a clear opening action, up to five feasible visible beats, one consistent camera motion, stable setting/lighting and an explicit last-frame state.',
				routingInstructions(availableModels, 'video'),
				"The backend compiles the scene and injects the selected model's exact prompt dialect, runtime parameters and continuity negatives. Select preview for a brief result, balanced by default, and quality only for an explicitly longer clip. The UI inserts the generated asset automatically; after this tool, answer with useful text only and never invent a Markdown video, image, link or attachment URL."
			].join('\n'),
			name: BuiltInTool.BROWSER_GENERATE_VIDEO,
			parameters: {
				additionalProperties: false,
				properties: {
					height: {
						description: 'Optional height; omit for the native FastWan profile',
						type: JsonSchemaType.NUMBER
					},
					model: {
						description: 'Local video model selected for the requested result',
						enum: modelEnum(availableModels),
						type: JsonSchemaType.STRING
					},
					negative_prompt: {
						description: 'Optional extra exclusions',
						type: JsonSchemaType.STRING
					},
					prompt: {
						description: 'Short English fallback description; the scene contract is authoritative',
						type: JsonSchemaType.STRING
					},
					quality: {
						description:
							'Generation profile: preview is brief, balanced is the default, quality is longer',
						enum: ['preview', 'balanced', 'quality'],
						type: JsonSchemaType.STRING
					},
					scene: VIDEO_SCENE,
					seed: { description: '-1 for random or a fixed integer', type: JsonSchemaType.NUMBER },
					video_frames: {
						description: 'Optional explicit frame count, normalized to 4n+1',
						type: JsonSchemaType.NUMBER
					},
					width: {
						description: 'Optional width; omit for the native FastWan profile',
						type: JsonSchemaType.NUMBER
					}
				},
				required: ['prompt', 'model', 'quality', 'scene'],
				type: JsonSchemaType.OBJECT
			}
		},
		type: ToolCallType.FUNCTION
	};
}
