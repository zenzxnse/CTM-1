<script lang="ts">
  import { displayValue, humanize, type ViewComponent, type Telemetry } from './api';
  let {
    component,
    telemetry,
    fresh
  }: {
    component: ViewComponent;
    telemetry: Telemetry | null;
    fresh: boolean;
  } = $props();
  let blocked = $derived(component.status !== undefined && component.status !== 'ready');
  let sample = $derived(telemetry?.values[component.tag_id]);
  let usable = $derived(fresh && !blocked && sample !== undefined && sample.quality === 'good');
  let value = $derived(usable ? sample?.value : undefined);
  let numeric = $derived(typeof value === 'number' ? value : null);
  let percent = $derived(
    component.unit === '%' && numeric !== null ? Math.min(100, Math.max(0, numeric)) : null
  );
  let alarm = $derived(
    component.alarm_id
      ? telemetry?.alarms.find((item) => item.id === component.alarm_id)
      : undefined
  );
</script>

<article
  class:reading-blocked={blocked}
  class:reading-alarm={component.kind === 'alarm' && usable && alarm?.active}
  class="reading"
>
  <div class="reading-heading">
    <h3>{component.label}</h3>
    <span class="component-kind">{humanize(component.kind)}</span>
  </div>
  {#if component.kind === 'alarm'}
    <div class="alarm-value">
      {!usable || !alarm ? 'Unknown' : alarm.active ? 'Active' : 'Clear'}
    </div>
    <p class="reading-subtitle">
      {alarm?.severity ? `${humanize(alarm.severity)} priority` : 'Declared alarm'}
    </p>
  {:else}
    <div class:unknown-value={!usable} class="reading-value">
      {displayValue(value)}
      {#if usable && component.unit}<span>{component.unit}</span>{/if}
    </div>
    {#if component.kind === 'gauge' && percent !== null}
      <meter min="0" max="100" value={percent} aria-label={`${component.label}: ${percent}%`}
        >{percent}%</meter
      >
      <div class="gauge-scale"><span>0%</span><span>100%</span></div>
    {:else}
      <p class="reading-subtitle">
        {blocked
          ? humanize(component.status ?? 'unavailable')
          : usable
            ? humanize(component.role)
            : 'Waiting for valid data'}
      </p>
    {/if}
  {/if}
  <details class="binding-detail">
    <summary>Why this reading?</summary>
    <p>{component.reason}</p>
    <dl>
      <dt>Equipment</dt>
      <dd><code>{component.asset_id}</code></dd>
      <dt>Tag</dt>
      <dd><code>{component.tag_id}</code></dd>
      <dt>Role</dt>
      <dd>{humanize(component.role)}</dd>
    </dl>
  </details>
</article>
