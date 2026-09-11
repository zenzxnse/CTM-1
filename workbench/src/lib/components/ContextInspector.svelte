<script lang="ts">
  import { humanize, type MachineModel, type Telemetry } from '$lib/api';

  let {
    model,
    telemetry,
    fresh
  }: { model: MachineModel | null; telemetry: Telemetry | null; fresh: boolean } = $props();

  let tagFilter = $state('');
  let filteredTags = $derived(
    model?.tags.filter((tag) =>
      `${tag.id} ${tag.asset_id} ${tag.role} ${tag.name}`
        .toLowerCase()
        .includes(tagFilter.toLowerCase())
    ) ?? []
  );
  let alarmStates = $derived(
    new Map((telemetry?.alarms ?? []).map((item) => [item.alarm_id, item]))
  );
  let provenance = $derived(model?.metadata?.provenance);
  let artifacts = $derived(provenance?.artifacts ?? []);
</script>

{#if model}
  <div class="context-counts">
    <div><strong>{model.assets.length}</strong><span>Equipment assets</span></div>
    <div><strong>{model.tags.length}</strong><span>Declared tags</span></div>
    <div><strong>{model.relationships.length}</strong><span>Connections</span></div>
    <div><strong>{model.alarms.length}</strong><span>Alarm definitions</span></div>
  </div>
  <section class="document-section">
    <h2>Model provenance</h2>
    <dl class="task-facts provenance-facts">
      <dt>Model</dt>
      <dd><code>{model.model_id}</code></dd>
      <dt>Revision</dt>
      <dd>{model.revision}</dd>
      <dt>Session</dt>
      <dd><code>{model.session_id ?? 'Unknown'}</code></dd>
      <dt>Generation</dt>
      <dd>{model.context_generation ?? 'Unknown'}</dd>
      <dt>Context schema</dt>
      <dd>{provenance?.schema ?? 'Native machine model'}</dd>
      <dt>Engineering sources</dt>
      <dd>{artifacts.length || 'Not reported'}</dd>
    </dl>
    <p class="section-note">
      Identity and source reference are separate. A name or namespace index alone is not proof of
      ownership. Every binding used by a view is resolved by the service from this declared model.
    </p>
    {#if artifacts.length}
      <div class="table-scroll provenance-table">
        <table>
          <thead>
            <tr><th>Engineering input</th><th>Format</th><th>Records</th><th>Evidence</th></tr>
          </thead>
          <tbody>
            {#each artifacts as artifact (artifact.id)}
              <tr>
                <td>{humanize(artifact.kind)}<small><code>{artifact.path}</code></small></td>
                <td>{artifact.format}</td>
                <td>{artifact.records}</td>
                <td><code>{artifact.fingerprint}</code><small>{artifact.bytes} bytes</small></td>
              </tr>
            {/each}
          </tbody>
        </table>
      </div>
      <p class="section-note">
        These fingerprints identify the reviewed source files used to create this context. They are
        provenance evidence, not security signatures.
      </p>
    {:else}
      <p class="section-note">
        This model does not report an engineering-bundle provenance record. Its declared identities
        and bindings are still validated, but the workbench cannot attribute them to source
        artifacts.
      </p>
    {/if}
  </section>
  <section class="document-section">
    <h2>Equipment and connections</h2>
    <div class="table-scroll">
      <table>
        <thead>
          <tr><th>Equipment</th><th>Identity</th><th>Type</th><th>Declared connection</th></tr>
        </thead>
        <tbody>
          {#each model.assets as asset (asset.id)}
            <tr>
              <td>{asset.name}</td>
              <td><code>{asset.id}</code></td>
              <td>{asset.kind}</td>
              <td>
                {model.relationships
                  .filter((relationship) => relationship.from === asset.id)
                  .map((relationship) => `${relationship.kind} ${relationship.to}`)
                  .join(', ') || 'None declared'}
              </td>
            </tr>
          {/each}
        </tbody>
      </table>
    </div>
  </section>
  <section class="document-section">
    <div class="section-heading">
      <h2>Tag registry</h2>
      <label class="search-label"
        >Filter tags<input
          bind:value={tagFilter}
          placeholder="Name, owner or role"
          type="search"
        /></label
      >
    </div>
    <div class="table-scroll">
      <table>
        <thead>
          <tr
            ><th>Tag</th><th>Owner</th><th>Role</th><th>Type / unit</th><th>Source reference</th
            ></tr
          >
        </thead>
        <tbody>
          {#each filteredTags as tag (tag.id)}
            <tr>
              <td><code>{tag.id}</code><small>{tag.name}</small></td>
              <td><code>{tag.asset_id}</code></td>
              <td>{humanize(tag.role)}</td>
              <td>{tag.data_type}{tag.unit ? ` / ${tag.unit}` : ''}</td>
              <td
                ><code>{tag.source.identifier}</code><small>{tag.source.namespace_uri}</small><small
                  >{tag.source.server}</small
                ></td
              >
            </tr>
          {/each}
        </tbody>
      </table>
    </div>
    {#if !filteredTags.length}<p class="empty-results">No tags match this filter.</p>{/if}
  </section>
  <section class="document-section">
    <h2>Alarm registry</h2>
    <div class="table-scroll">
      <table>
        <thead>
          <tr
            ><th>Alarm</th><th>Equipment</th><th>Tag</th><th>Condition</th><th>Priority</th><th
              >State</th
            ></tr
          >
        </thead>
        <tbody>
          {#each model.alarms as alarm (alarm.id)}
            <tr>
              <td><code>{alarm.id}</code><small>{alarm.name}</small></td>
              <td><code>{alarm.asset_id}</code></td>
              <td><code>{alarm.tag_id}</code></td>
              <td>{alarm.operator ?? '?'} {alarm.threshold ?? '?'}</td>
              <td>{humanize(alarm.severity)}</td>
              <td>
                {#if !fresh}
                  Unknown, feed not current
                {:else if alarmStates.has(alarm.id)}
                  {alarmStates.get(alarm.id)?.active ? 'Active' : 'Clear'}
                {:else}
                  Not evaluated
                {/if}
              </td>
            </tr>
          {/each}
        </tbody>
      </table>
    </div>
    {#if !model.alarms.length}<p class="empty-results">This model declares no alarms.</p>{/if}
  </section>
{:else}
  <p class="intro">The machine model is not loaded yet.</p>
{/if}
