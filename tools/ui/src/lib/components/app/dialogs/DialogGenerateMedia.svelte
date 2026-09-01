<script lang="ts">
	import { Image, Loader2, Video } from '@lucide/svelte';
	import { Button } from '$lib/components/ui/button';
	import * as Dialog from '$lib/components/ui/dialog';
	import type { MediaModelState } from '$lib/services/media.service';
	import { chatStore, mediaStore } from '$lib/stores';

	interface Props {
		initialKind?: 'image' | 'video';
		open?: boolean;
		onOpenChange?: (open: boolean) => void;
	}

	let { initialKind = 'image', onOpenChange, open = $bindable(false) }: Props = $props();

	let error = $state('');
	let kind = $state<'image' | 'video'>('image');
	let loading = $state(false);
	let models = $state<MediaModelState[]>([]);
	let prompt = $state('');
	let quality = $state<'preview' | 'balanced' | 'quality'>('balanced');
	let selectedModel = $state('');
	let submitting = $state(false);

	let kindModels = $derived(models.filter((model) => model.kind === kind && model.chat_enabled));
	let availableModels = $derived(kindModels.filter((model) => model.available));
	let downloadingModels = $derived(kindModels.filter((model) => !model.available));
	let selected = $derived(availableModels.find((model) => model.id === selectedModel));

	$effect(() => {
		if (!open) return;

		kind = initialKind;
		error = '';
		void loadModels();

		const timer = setInterval(() => void loadModels(true), 3000);

		return () => clearInterval(timer);
	});

	$effect(() => {
		if (!availableModels.some((model) => model.id === selectedModel)) {
			selectedModel =
				availableModels.find((model) => model.default_for_chat === true)?.id ??
				availableModels[0]?.id ??
				'';
		}
	});

	async function loadModels(silent = false) {
		if (!silent) loading = true;

		try {
			models = await mediaStore.models();
		} catch (reason) {
			error = reason instanceof Error ? reason.message : String(reason);
		} finally {
			if (!silent) loading = false;
		}
	}

	function handleOpenChange(value: boolean) {
		open = value;
		onOpenChange?.(value);
	}

	async function generate() {
		const cleanPrompt = prompt.trim();

		if (!cleanPrompt || !selectedModel || submitting) return;

		error = '';
		submitting = true;

		try {
			const modelLabel = selected?.label ?? selectedModel;
			const qualityLabel =
				quality === 'preview' ? 'breve' : quality === 'quality' ? 'lungo' : 'di durata normale';
			const request =
				kind === 'image'
					? `Genera un'immagine usando il modello media ${modelLabel} (${selectedModel}): ${cleanPrompt}`
					: `Genera un video ${qualityLabel} usando il modello media ${modelLabel} (${selectedModel}): ${cleanPrompt}`;

			prompt = '';
			handleOpenChange(false);
			await chatStore.sendMessage(request, undefined, {
				mediaModelSelection: { kind, model: selectedModel }
			});
		} catch (reason) {
			error = reason instanceof Error ? reason.message : String(reason);
		} finally {
			submitting = false;
		}
	}
</script>

<Dialog.Root onOpenChange={handleOpenChange} {open}>
	<Dialog.Content class="sm:max-w-xl">
		<Dialog.Header>
			<Dialog.Title>Genera contenuto locale</Dialog.Title>

			<Dialog.Description>
				Il modello linguistico attivo usa tutta la conversazione per costruire il prompt, poi avvia
				il modello media locale su Vulkan nella stessa sessione chat.
			</Dialog.Description>
		</Dialog.Header>

		<form
			class="space-y-5"
			onsubmit={(event) => {
				event.preventDefault();
				void generate();
			}}
		>
			<div class="grid grid-cols-2 gap-2 rounded-xl bg-muted p-1">
				<button
					aria-pressed={kind === 'image'}
					class={[
						'flex items-center justify-center gap-2 rounded-lg px-3 py-2 text-sm font-medium transition',
						kind === 'image'
							? 'bg-background shadow-sm'
							: 'text-muted-foreground hover:text-foreground'
					]}
					onclick={() => (kind = 'image')}
					type="button"
				>
					<Image class="h-4 w-4" />
					Immagine
				</button>

				<button
					aria-pressed={kind === 'video'}
					class={[
						'flex items-center justify-center gap-2 rounded-lg px-3 py-2 text-sm font-medium transition',
						kind === 'video'
							? 'bg-background shadow-sm'
							: 'text-muted-foreground hover:text-foreground'
					]}
					onclick={() => (kind = 'video')}
					type="button"
				>
					<Video class="h-4 w-4" />
					Video
				</button>
			</div>

			<label class="block space-y-2">
				<span class="text-sm font-medium">Modello media</span>

				<select
					bind:value={selectedModel}
					class="h-10 w-full rounded-md border border-input bg-background px-3 text-sm outline-none focus:ring-2 focus:ring-ring"
					disabled={loading || submitting}
				>
					{#if loading}
						<option value="">Caricamento modelli...</option>
					{:else if kindModels.length === 0}
						<option value="">Nessun modello disponibile</option>
					{:else}
						{#if availableModels.length === 0}
							<option value="">Download dei modelli in corso...</option>
						{/if}

						{#each kindModels as model (model.id)}
							<option disabled={!model.available} value={model.id}>
								{model.label}{model.available
									? ''
									: ` - download ${model.download_progress.toFixed(1)}%`}
							</option>
						{/each}
					{/if}
				</select>

				{#if selected}
					<span class="block text-xs text-muted-foreground">{selected.description}</span>
				{/if}

				{#if downloadingModels.length > 0}
					<div class="space-y-2 rounded-md border border-border/70 bg-muted/40 p-3">
						{#each downloadingModels as model (model.id)}
							<div class="space-y-1">
								<div class="flex items-center justify-between gap-3 text-xs">
									<span class="truncate font-medium">{model.label}</span>

									<span class="tabular-nums text-muted-foreground">
										{model.download_progress.toFixed(1)}%
									</span>
								</div>

								<div class="h-1.5 overflow-hidden rounded-full bg-muted">
									<div
										class="h-full rounded-full bg-primary transition-[width] duration-500"
										style={`width: ${Math.max(0, Math.min(100, model.download_progress))}%`}
									></div>
								</div>
							</div>
						{/each}
					</div>
				{/if}
			</label>

			{#if kind === 'video'}
				<label class="block space-y-2">
					<span class="text-sm font-medium">Durata</span>

					<select
						bind:value={quality}
						class="h-10 w-full rounded-md border border-input bg-background px-3 text-sm outline-none focus:ring-2 focus:ring-ring"
						disabled={submitting}
					>
						<option value="preview">Breve - 17 frame - circa 1 minuto</option>

						<option value="balanced">Normale - 33 frame - circa 2 minuti</option>

						<option value="quality">Lunga - 49 frame - circa 3 minuti</option>
					</select>
				</label>
			{/if}

			<label class="block space-y-2">
				<span class="text-sm font-medium">Descrizione</span>

				<textarea
					bind:value={prompt}
					class="min-h-32 w-full resize-y rounded-md border border-input bg-background px-3 py-2 text-sm outline-none placeholder:text-muted-foreground focus:ring-2 focus:ring-ring"
					disabled={submitting}
					placeholder={kind === 'image'
						? 'Descrivi soggetto, stile, inquadratura e illuminazione...'
						: 'Descrivi soggetto, movimento, camera, scena e illuminazione...'}
				></textarea>
			</label>

			{#if error}
				<p
					class="rounded-md border border-destructive/40 bg-destructive/10 p-3 text-sm text-destructive"
				>
					{error}
				</p>
			{/if}

			<Dialog.Footer>
				<Button onclick={() => handleOpenChange(false)} type="button" variant="outline">
					Annulla
				</Button>

				<Button disabled={!prompt.trim() || !selectedModel || loading || submitting} type="submit">
					{#if submitting}<Loader2 class="mr-2 h-4 w-4 animate-spin" />{/if}
					Genera {kind === 'image' ? 'immagine' : 'video'}
				</Button>
			</Dialog.Footer>
		</form>
	</Dialog.Content>
</Dialog.Root>
