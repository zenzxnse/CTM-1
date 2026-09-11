<script lang="ts">
  import {
    displayValue,
    humanize,
    qualityLabel,
    type Telemetry,
    type ViewComponent
  } from '$lib/api';

  let {
    component,
    telemetry,
    fresh
  }: {
    component: ViewComponent;
    telemetry: Telemetry | null;
    fresh: boolean;
  } = $props();

  let supported = $derived(
    component.kind === 'gauge' ||
      component.kind === 'value' ||
      component.kind === 'status' ||
      component.kind === 'alarm'
  );
  let blocked = $derived(component.status !== undefined && component.status !== 'ready');
  let sample = $derived(telemetry?.values[component.tag_id]);
  let paused = $derived(sample !== undefined && sample.quality === 'paused' && fresh);
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
  let alarmActive = $derived(usable && alarm?.active === true);
</script>

{#if !supported}
  <article class="reading reading-unsupported">
    <div class="reading-heading">
      <h3>{component.label}</h3>
      <span class="component-kind">Unsupported</span>
    </div>
    <p class="unsupported-note">
      The service returned a component of kind <code>{component.kind}</code>. This workbench renders
      gauge, value, status and alarm components only.
    </p>
    <details class="binding-detail">
      <summary>Binding evidence</summary>
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
{:else}
  <article
    class:reading-blocked={blocked}
    class:reading-alarm={alarmActive}
    class:reading-paused={paused}
    class="reading"
  >
    <div class="reading-heading">
      <h3>{component.label}</h3>
      <span class="component-kind">{humanize(component.kind)}</span>
    </div>
    {#if component.kind === 'alarm'}
      <div class="alarm-value" class:alarm-clear={usable && alarm?.active === false}>
        {!usable || !alarm ? 'Unknown' : alarm.active ? '▲ Active' : '● Clear'}
      </div>
      <p class="reading-subtitle">
        {alarm?.severity ? `${humanize(alarm.severity)} priority` : 'Declared alarm'}
        {#if usable && alarm}
          {displayValue(alarm.value)} against threshold {displayValue(alarm.threshold)}
        {/if}
      </p>
    {:else}
      <div class:unknown-value={!usable} class="reading-value">
        {displayValue(value)}
        {#if usable && component.unit}<span>{component.unit}</span>{/if}
      </div>
      {#if component.kind === 'gauge' && percent !== null}
        <meter
          min="0"
          max="100"
          value={percent}
          aria-label={`${component.label}: ${percent} percent`}>{percent}%</meter
        >
        <div class="gauge-scale"><span>0%</span><span>100%</span></div>
      {:else}
        <p class="reading-subtitle">
          {blocked
            ? humanize(component.status ?? 'unavailable')
            : paused
              ? `${qualityLabel(sample?.quality ?? 'unknown')} feed, value held`
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
        {#if component.unit}<dt>Unit</dt>
          <dd>{component.unit}</dd>{/if}
        {#if component.alarm_id}<dt>Alarm</dt>
          <dd><code>{component.alarm_id}</code></dd>{/if}
        {#if component.relationship_id}
          <dt>Relationship</dt>
          <dd><code>{component.relationship_id}</code></dd>
        {/if}
      </dl>
    </details>
  </article>
{/if}
