<script lang="ts">
	interface Props {
		class?: string;
	}

	let { class: className = '' }: Props = $props();
</script>

<svg
	aria-hidden="true"
	class={className}
	fill="none"
	height="24"
	stroke="currentColor"
	stroke-linecap="round"
	stroke-linejoin="round"
	stroke-width="2"
	viewBox="0 0 24 24"
	width="24"
	xmlns="http://www.w3.org/2000/svg"
>
	<path d="M3.34 19a10 10 0 1 1 17.32 0" />

	<g transform="translate(12 12.5) scale(0.42) translate(-12 -12)">
		<path
			d="M22 12h-2.48a2 2 0 0 0-1.93 1.46l-2.35 8.36a.25.25 0 0 1-.48 0L9.24 2.18a.25.25 0 0 0-.48 0l-2.35 8.36A2 2 0 0 1 4.49 12H2"
			vector-effect="non-scaling-stroke"
		/>
	</g>
</svg>
