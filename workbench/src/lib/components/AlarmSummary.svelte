<script lang="ts">
  import { displayValue, humanize, severityRank, type Telemetry } from '$lib/api';

  let { telemetry, fresh }: { telemetry: Telemetry | null; fresh: boolean } = $props();

  let ranked = $derived(
    (telemetry?.alarms ?? [])
      .slice()
      .sort(
        (a, b) =>
          Number(b.active) - Number(a.active) ||
          severityRank(b.severity) - severityRank(a.severity) ||
          a.alarm_id.localeCompare(b.alarm_id)
      )
  );
  let activeCount = $derived(ranked.filter((alarm) => alarm.active).length);
</script>

<section class="alarm-panel" aria-labelledby="alarm-title">
  <div class="alarm-heading">
    <h2 id="alarm-title">Alarms</h2>
    <span class="status-tag" class:status-conflict={activeCount > 0}
      >{activeCount > 0 ? `▲ ${activeCount} active` : '● No active alarms'}</span
    >
  </div>
  {#if telemetry === null}
    <p class="alarm-empty">
      Waiting for telemetry. Alarm states are unknown until the feed is live.
    </p>
  {:else if !ranked.length}
    <p class="alarm-empty">The active model declares no alarms.</p>
  {:else}
    <ul class="alarm-list">
      {#each ranked as alarm (alarm.alarm_id)}
        <li class:alarm-row-active={alarm.active}>
          <span class="alarm-state" aria-hidden="true">{alarm.active ? '▲' : '●'}</span>
          <div class="alarm-facts">
            <strong>{alarm.name}</strong>
            <small>
              <code>{alarm.asset_id}</code> · {displayValue(alarm.value)} against
              {displayValue(alarm.threshold)} · {humanize(alarm.severity)} priority
            </small>
          </div>
          <span class="alarm-state-word">{alarm.active ? 'Active' : 'Clear'}</span>
        </li>
      {/each}
    </ul>
    {#if !fresh}
      <p class="alarm-note">▲ Alarm states may be stale. The feed is not current.</p>
    {/if}
  {/if}
</section>
