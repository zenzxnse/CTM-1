<script lang="ts">
  import Reading from './Reading.svelte';
  import { humanize, type Telemetry, type View } from '$lib/api';

  let {
    view,
    telemetry,
    fresh,
    differentContext,
    inspectBindings = $bindable(false),
    element = $bindable()
  }: {
    view: View;
    telemetry: Telemetry | null;
    fresh: boolean;
    differentContext: boolean;
    inspectBindings: boolean;
    element: HTMLElement | undefined;
  } = $props();

  let assetCount = $derived(new Set(view.components.map((item) => item.asset_id)).size);
  let statusClass = $derived(
    view.status === 'ready' ? '' : view.status === 'conflict' ? 'status-conflict' : 'status-review'
  );
</script>

<section class="view-shell" bind:this={element} aria-labelledby="view-title">
  <div class="view-heading">
    <div>
      <span class="eyebrow">Resolved view</span>
      <h2 id="view-title">{view.title}</h2>
    </div>
    <span class="status-tag {statusClass}"
      >{view.status === 'conflict'
        ? '▲ Conflict'
        : view.status === 'ready'
          ? '● Ready'
          : '▲ Needs review'}</span
    >
  </div>
  <div class="view-meta">
    <span>{view.components.length} components</span>
    <span>{assetCount} equipment assets</span>
    <span>Revision {view.model_revision}</span>
    <span class="provenance"><code>{view.view_id}</code></span>
    <button
      class="text-button"
      onclick={() => {
        inspectBindings = !inspectBindings;
      }}>{inspectBindings ? 'Hide bindings' : 'Inspect bindings'}</button
    >
  </div>
  {#if view.issues.length}
    <div class="view-issues" role="status">
      <strong>Context needs review</strong>
      <ul>
        {#each view.issues as issue (issue.code + issue.message)}
          <li>
            {issue.message}{#if issue.code}
              <code>{issue.code}</code>{/if}
          </li>
        {/each}
      </ul>
    </div>
  {/if}
  {#if !fresh}
    <div class="stale-banner">
      <span class="stale-icon" aria-hidden="true">▲</span>
      {differentContext
        ? 'The runtime session or configuration has changed. Make a fresh request to use the current context.'
        : 'Current readings are unavailable. Values stay unknown until matching telemetry arrives.'}
    </div>
  {/if}
  {#if view.components.length}
    <div class="readings-grid">
      {#each view.components as component (component.id)}
        <Reading {component} {telemetry} {fresh} />
      {/each}
    </div>
  {:else}
    <div class="empty-view">
      <h3>No components resolved</h3>
      <p>Check the missing context above, or make a new request against the current model.</p>
    </div>
  {/if}
  {#if inspectBindings}
    <div class="table-scroll binding-table">
      <table>
        <thead>
          <tr><th>Component</th><th>Equipment</th><th>Tag and role</th><th>State</th></tr>
        </thead>
        <tbody>
          {#each view.components as component (component.id)}
            <tr>
              <td>{component.label}</td>
              <td><code>{component.asset_id}</code></td>
              <td
                ><code>{component.tag_id}</code><small
                  >{humanize(component.role)}{component.unit ? ` · ${component.unit}` : ''}</small
                ></td
              >
              <td>{humanize(component.status ?? 'ready')}</td>
            </tr>
          {/each}
        </tbody>
      </table>
    </div>
  {/if}
  {#if view.changes?.length}
    <div class="change-record">
      <h3>Reconciliation record</h3>
      {#each view.changes as change (change.kind + change.message)}
        <p>
          <span class="inline-tag">{humanize(change.kind)}</span>
          {change.message}{#if change.tag_id}
            <code>{change.tag_id}</code>{/if}
        </p>
      {/each}
    </div>
  {/if}
</section>
