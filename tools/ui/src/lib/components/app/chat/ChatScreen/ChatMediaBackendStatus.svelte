<script lang="ts">
	import { mediaStore } from '$lib/stores';
	import { onDestroy, onMount } from 'svelte';

	let stopMonitoring: (() => void) | null = null;

	onMount(() => {
		stopMonitoring = mediaStore.monitor();
	});

	onDestroy(() => {
		stopMonitoring?.();
	});
</script>
