<script lang="ts">
	import { Check, Info, Lightbulb, LightbulbOff } from '@lucide/svelte';
	import * as DropdownMenu from '$lib/components/ui/dropdown-menu';
	import * as Tooltip from '$lib/components/ui/tooltip';
	import { ICON_CLASS_DEFAULT } from '$lib/constants';
	import { useReasoningMenu } from '$lib/hooks/use-reasoning-menu.svelte';

	const reasoning = useReasoningMenu();

	let isOpen = $state(false);
	let stateLabel = $derived(
		reasoning.currentEffort === 'default'
			? 'Model default'
			: reasoning.currentEffort.charAt(0).toUpperCase() + reasoning.currentEffort.slice(1)
	);
</script>

<DropdownMenu.Root bind:open={isOpen}>
	<Tooltip.Root ignoreNonKeyboardFocus>
		<Tooltip.Trigger>
			{#snippet child({ props })}
				<DropdownMenu.Trigger
					{...props}
					aria-label={`Reasoning: ${stateLabel}. Click to configure.`}
					class={[
						'flex h-7 w-7 shrink-0 cursor-pointer items-center justify-center rounded-full p-0 transition-colors focus:outline-none focus-visible:ring-2 focus-visible:ring-ring focus-visible:ring-offset-2',
						reasoning.isReasoningActive
							? 'bg-amber-400/10 hover:bg-amber-400/20'
							: 'bg-muted hover:bg-muted-foreground/20'
					]}
				>
					{#if reasoning.isReasoningActive}
						<Lightbulb class="h-3.5 w-3.5 text-amber-400" />
					{:else if reasoning.isOff}
						<LightbulbOff class="h-3.5 w-3.5 text-muted-foreground" />
					{:else}
						<Lightbulb class="h-3.5 w-3.5 text-muted-foreground" />
					{/if}
				</DropdownMenu.Trigger>
			{/snippet}
		</Tooltip.Trigger>

		<Tooltip.Content>
			<p>Reasoning: {stateLabel}</p>
		</Tooltip.Content>
	</Tooltip.Root>

	<DropdownMenu.Content
		align="end"
		class="w-60 bg-popover p-1.5 text-popover-foreground shadow-md outline-none"
	>
		<div class="px-2 py-1.5 text-xs font-medium text-muted-foreground">Reasoning effort</div>

		{#each reasoning.levels as level (level.value)}
			{@const tokenLabel = reasoning.tokenLabel(level)}
			<DropdownMenu.Item
				class={[
					'flex w-full cursor-pointer items-center gap-3 rounded-md px-2 py-1.75 text-left text-sm transition-colors hover:bg-accent',
					reasoning.isSelected(level) && 'bg-accent'
				]}
				onclick={() => reasoning.select(level)}
			>
				{#if reasoning.isSelected(level)}
					<Check class="{ICON_CLASS_DEFAULT} shrink-0 text-foreground" />
				{:else}
					<div class="{ICON_CLASS_DEFAULT} shrink-0"></div>
				{/if}

				<span class="flex-1">{level.label}</span>

				{#if tokenLabel}
					<span class="text-[11px] text-muted-foreground opacity-60">{tokenLabel}</span>
				{/if}

				{#if level.hasInfo}
					<Tooltip.Root>
						<Tooltip.Trigger>
							<Info class="h-3.5 w-3.5 shrink-0 text-muted-foreground" />
						</Tooltip.Trigger>

						<Tooltip.Content side="left">
							<p>Maximum reasoning effort with extended context usage</p>
						</Tooltip.Content>
					</Tooltip.Root>
				{/if}
			</DropdownMenu.Item>
		{/each}
	</DropdownMenu.Content>
</DropdownMenu.Root>
