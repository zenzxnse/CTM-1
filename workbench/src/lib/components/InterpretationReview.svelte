<script lang="ts">
  import { humanize, type Interpretation } from '$lib/api';

  let {
    interpretation,
    busy,
    onpick
  }: {
    interpretation: Interpretation;
    busy: boolean;
    onpick: (name: string) => void;
  } = $props();

  let ready = $derived(interpretation.status === 'ready');
  let interpreterLabel = $derived(
    interpretation.interpreter === 'llama' || interpretation.interpreter === 'llama.cpp'
      ? 'Local AI, llama.cpp'
      : 'Rules, offline deterministic'
  );
  let candidates = $derived(
    (interpretation.candidates ?? []).map((candidate) =>
      typeof candidate === 'string'
        ? {
            asset_id: candidate,
            name: candidate,
            kind: undefined,
            score: undefined,
            eligible: false
          }
        : {
            asset_id: candidate.asset_id ?? 'Unknown asset',
            name: candidate.name ?? candidate.asset_id ?? 'Unknown asset',
            kind: candidate.kind,
            score: candidate.score,
            eligible: candidate.eligible
          }
    )
  );
  let context = $derived(interpretation.execution?.context);
  let taskRoles = $derived(interpretation.task?.measurement_roles ?? []);
</script>

<section class="review-panel" aria-labelledby="review-title">
  <div class="review-heading">
    <h2 id="review-title">Interpretation review</h2>
    <span class="status-tag" class:status-review={!ready}
      >{ready ? 'Interpreted' : 'Needs clarification'}</span
    >
  </div>
  <dl class="task-facts review-facts">
    <dt>Interpreter</dt>
    <dd>{interpreterLabel}</dd>
    <dt>Request</dt>
    <dd>{interpretation.prompt ?? 'Not echoed by the service'}</dd>
    {#if interpretation.task}
      <dt>Task</dt>
      <dd>{humanize(interpretation.task.kind)}</dd>
      <dt>Equipment</dt>
      <dd><code>{interpretation.task.anchor_asset_id || 'All declared equipment'}</code></dd>
      <dt>Model revision</dt>
      <dd>{interpretation.task.model_revision}</dd>
      <dt>Validation</dt>
      <dd>Native scope and binding checks required</dd>
    {/if}
  </dl>
  {#if interpretation.task || context}
    <div class="scope-summary" aria-label="Interpreted scope evidence">
      <div class="scope-summary-heading">
        <span class="eyebrow">Interpreted scope</span>
        <span class="scope-summary-status">Server evidence</span>
      </div>
      <div class="scope-summary-grid">
        {#if interpretation.task}
          <div>
            <span>Requested task</span>
            <strong>{humanize(interpretation.task.kind)}</strong>
          </div>
          <div>
            <span>Anchor</span>
            <strong
              ><code>{interpretation.task.anchor_asset_id ?? 'All eligible assets'}</code></strong
            >
          </div>
          <div>
            <span>Measurement roles</span>
            <strong
              >{taskRoles.length ? taskRoles.map(humanize).join(', ') : 'Declared defaults'}</strong
            >
          </div>
        {/if}
        {#if context}
          <div>
            <span>Retrieved context</span>
            <strong>{context.assets ?? '?'} assets · {context.tags ?? '?'} tags</strong>
          </div>
          <div>
            <span>Candidate set</span>
            <strong>{context.candidates ?? candidates.length} considered</strong>
          </div>
          <div>
            <span>Context bound</span>
            <strong>{context.bytes ?? '?'} bytes · {context.estimated_tokens ?? '?'} tokens</strong>
          </div>
        {/if}
      </div>
      {#if context?.truncated}
        <p class="scope-warning">
          ▲ Retrieval was bounded and truncated. Review the candidate evidence before relying on
          this view.
        </p>
      {:else}
        <p class="scope-note">
          Only this retrieved, revision-matched scope is eligible for native resolution.
        </p>
      {/if}
    </div>
  {/if}
  {#if !ready}
    {#if interpretation.message}
      <p class="review-message">{interpretation.message}</p>
    {/if}
    {#if interpretation.supported_tasks?.length}
      <p class="review-note">
        Supported tasks: {interpretation.supported_tasks.map(humanize).join(', ')}.
      </p>
    {/if}
    {#if candidates.length}
      <div class="candidate-list">
        <span class="candidate-label">Candidate equipment considered</span>
        {#each candidates as candidate (candidate.asset_id)}
          <button
            type="button"
            class="candidate-button"
            disabled={busy}
            onclick={() => {
              onpick(candidate.name);
            }}>Use {candidate.name} <code>{candidate.asset_id}</code></button
          >
        {/each}
      </div>
      <p class="review-note">
        Unmatched equipment does not create a binding. Pick one candidate to resolve the request
        against the declared model.
      </p>
    {/if}
  {:else if !interpretation.view}
    <p class="review-note">
      The service accepted the request. Compose the view to resolve it against the current model.
    </p>
  {/if}
</section>
