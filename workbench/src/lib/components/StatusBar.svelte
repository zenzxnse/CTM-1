<script lang="ts">
  import type { Health, MachineModel, Readiness } from '$lib/api';

  let {
    health,
    model,
    sessionRole,
    scenarioName,
    reachable,
    readiness,
    feedLabel,
    feedStale,
    feedAge,
    feedDisconnected,
    onReconnect
  }: {
    health: Health | null;
    model: MachineModel | null;
    sessionRole: string;
    scenarioName: string;
    reachable: boolean | null;
    readiness: Readiness | null;
    feedLabel: string;
    feedStale: boolean;
    feedAge: string;
    feedDisconnected: boolean;
    onReconnect: () => void;
  } = $props();

  let serviceState = $derived(
    reachable === null ? 'Unknown' : reachable ? 'Reachable' : 'Unreachable'
  );
  let sourceLabel = $derived(health === null ? 'Unknown' : `${health.mode}, ingested telemetry`);
  let inferenceLabel = $derived(
    health === null
      ? 'Unknown'
      : health.interpreter === 'llama'
        ? 'Local AI, llama.cpp'
        : 'Rules, offline deterministic'
  );
  let providerState = $derived(
    readiness === null
      ? '● Unknown'
      : readiness.inference?.ready
        ? `● ${readiness.inference.provider ?? 'Provider'} ready`
        : `▲ ${readiness.inference?.provider ?? 'Provider'} not ready`
  );
  let providerReady = $derived(readiness?.inference.ready === true);
  let providerUnknown = $derived(readiness === null);
</script>

<section class="status-strip" aria-label="Runtime status">
  <div class="status-chip">
    <span class="chip-label">Service</span>
    <span
      class="chip-value"
      class:state-good={reachable === true}
      class:state-bad={reachable === false}>● {serviceState}</span
    >
  </div>
  <div class="status-chip">
    <span class="chip-label">Source</span>
    <span class="chip-value">{sourceLabel}</span>
  </div>
  <div class="status-chip">
    <span class="chip-label">Inference</span>
    <span class="chip-value">{inferenceLabel}</span>
  </div>
  <div class="status-chip">
    <span class="chip-label">Provider</span>
    <span
      class="chip-value"
      class:state-good={providerReady}
      class:state-bad={!providerReady && !providerUnknown}
      class:state-warn={!providerReady}
      title={readiness?.inference.reason}>{providerState}</span
    >
  </div>
  <div class="status-chip">
    <span class="chip-label">Session</span>
    <span class="chip-value">{sessionRole || 'Unknown'}</span>
  </div>
  <div class="status-chip">
    <span class="chip-label">Generation</span>
    <span class="chip-value"
      >{model?.context_generation ?? health?.context_generation ?? 'Unknown'}</span
    >
  </div>
  <div class="status-chip">
    <span class="chip-label">Configuration</span>
    <span class="chip-value">{scenarioName || 'Unknown'}</span>
  </div>
  <div class="status-chip">
    <span class="chip-label">Feed</span>
    <span
      class="chip-value"
      class:state-good={!feedStale && !feedDisconnected}
      class:state-bad={feedDisconnected}
      class:state-warn={feedStale && !feedDisconnected}
      >{feedDisconnected ? '■ Disconnected' : feedStale ? '▲ Delayed' : '● Live'},
      {feedAge}</span
    >
    {#if feedDisconnected}
      <button class="chip-button" onclick={onReconnect}>Reconnect feed</button>
    {/if}
  </div>
</section>
