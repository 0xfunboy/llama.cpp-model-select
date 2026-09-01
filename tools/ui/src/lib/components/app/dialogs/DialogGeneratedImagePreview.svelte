<script lang="ts">
	import { RotateCcw, X, ZoomIn, ZoomOut } from '@lucide/svelte';
	import * as DialogUI from '$lib/components/ui/dialog';
	import { Dialog } from 'bits-ui';

	interface Props {
		alt: string;
		open: boolean;
		src: string;
		onOpenChange?: (open: boolean) => void;
	}

	let { alt, onOpenChange, open = $bindable(false), src }: Props = $props();

	let dragging = $state(false);
	let scale = $state(1);
	let translateX = $state(0);
	let translateY = $state(0);
	let viewport = $state<HTMLDivElement | null>(null);

	let dragOriginX = 0;
	let dragOriginY = 0;
	let dragTranslateX = 0;
	let dragTranslateY = 0;

	const MIN_SCALE = 0.25;
	const MAX_SCALE = 8;
	const ZOOM_FACTOR = 1.2;

	function clampScale(value: number): number {
		return Math.min(MAX_SCALE, Math.max(MIN_SCALE, value));
	}

	function resetView() {
		scale = 1;
		translateX = 0;
		translateY = 0;
	}

	function setScale(nextScale: number, centerX = 0, centerY = 0) {
		const resolved = clampScale(nextScale);

		if (resolved === scale) return;

		const ratio = resolved / scale;

		translateX = centerX - (centerX - translateX) * ratio;
		translateY = centerY - (centerY - translateY) * ratio;
		scale = resolved;
	}

	function zoomIn() {
		setScale(scale * ZOOM_FACTOR);
	}

	function zoomOut() {
		setScale(scale / ZOOM_FACTOR);
	}

	function handlePointerDown(event: PointerEvent) {
		if (event.button !== 0 && event.pointerType === 'mouse') return;

		dragging = true;
		dragOriginX = event.clientX;
		dragOriginY = event.clientY;
		dragTranslateX = translateX;
		dragTranslateY = translateY;
		(event.currentTarget as HTMLElement).setPointerCapture(event.pointerId);
	}

	function handlePointerMove(event: PointerEvent) {
		if (!dragging) return;

		translateX = dragTranslateX + event.clientX - dragOriginX;
		translateY = dragTranslateY + event.clientY - dragOriginY;
	}

	function handlePointerUp(event: PointerEvent) {
		dragging = false;

		const target = event.currentTarget as HTMLElement;

		if (target.hasPointerCapture(event.pointerId)) target.releasePointerCapture(event.pointerId);
	}

	function handleDoubleClick() {
		if (scale > 1) resetView();
		else setScale(2);
	}

	$effect(() => {
		if (!open || !src) return;

		// Every image opens in a predictable fitted state, including when the same
		// dialog instance is reused for another generated asset.
		resetView();
	});

	$effect(() => {
		if (!open || !viewport) return;

		const element = viewport;
		const handleWheel = (event: WheelEvent) => {
			event.preventDefault();

			const rect = element.getBoundingClientRect();
			const centerX = event.clientX - rect.left - rect.width / 2;
			const centerY = event.clientY - rect.top - rect.height / 2;
			const factor = event.deltaY < 0 ? ZOOM_FACTOR : 1 / ZOOM_FACTOR;

			setScale(scale * factor, centerX, centerY);
		};

		element.addEventListener('wheel', handleWheel, { passive: false });

		return () => element.removeEventListener('wheel', handleWheel);
	});
</script>

<Dialog.Root bind:open {onOpenChange}>
	<Dialog.Portal>
		<DialogUI.Overlay class="z-[1000000] bg-black/90 backdrop-blur-sm" />

		<Dialog.Content
			aria-label="Anteprima immagine generata"
			class="fixed inset-0 z-[1000000] overflow-hidden bg-transparent outline-none"
		>
			<div
				bind:this={viewport}
				class="flex h-full w-full touch-none items-center justify-center overflow-hidden"
				ondblclick={handleDoubleClick}
				onpointercancel={handlePointerUp}
				onpointerdown={handlePointerDown}
				onpointermove={handlePointerMove}
				onpointerup={handlePointerUp}
				role="presentation"
			>
				<img
					{alt}
					class={[
						'max-h-[calc(100dvh-3rem)] max-w-[calc(100vw-3rem)] object-contain select-none will-change-transform',
						dragging ? 'cursor-grabbing' : scale > 1 ? 'cursor-grab' : 'cursor-zoom-in'
					]}
					draggable="false"
					{src}
					style="transform: translate3d({translateX}px, {translateY}px, 0) scale({scale});"
				/>
			</div>

			<div
				class="absolute bottom-5 left-1/2 flex -translate-x-1/2 items-center gap-1 rounded-full border border-white/15 bg-black/70 p-1.5 text-white shadow-2xl backdrop-blur"
			>
				<button
					aria-label="Riduci"
					class="rounded-full p-2 transition hover:bg-white/15 disabled:opacity-40"
					disabled={scale <= MIN_SCALE}
					onclick={zoomOut}
					type="button"
				>
					<ZoomOut class="size-5" />
				</button>

				<span class="min-w-14 text-center text-xs font-medium tabular-nums"
					>{Math.round(scale * 100)}%</span
				>

				<button
					aria-label="Ingrandisci"
					class="rounded-full p-2 transition hover:bg-white/15 disabled:opacity-40"
					disabled={scale >= MAX_SCALE}
					onclick={zoomIn}
					type="button"
				>
					<ZoomIn class="size-5" />
				</button>

				<button
					aria-label="Adatta allo schermo"
					class="rounded-full p-2 transition hover:bg-white/15"
					onclick={resetView}
					type="button"
				>
					<RotateCcw class="size-5" />
				</button>
			</div>

			<Dialog.Close
				aria-label="Chiudi anteprima"
				class="absolute top-5 right-5 rounded-full border border-white/15 bg-black/70 p-2.5 text-white shadow-2xl backdrop-blur transition hover:bg-white/15"
			>
				<X class="size-6" />
			</Dialog.Close>
		</Dialog.Content>
	</Dialog.Portal>
</Dialog.Root>
