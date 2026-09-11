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
        ? { asset_id: candidate, name: candidate }
        : {
            asset_id: candidate.asset_id ?? 'Unknown asset',
            name: candidate.name ?? candidate.asset_id ?? 'Unknown asset'
          }
    )
  );
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
    {/if}
  </dl>
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
