<script lang="ts">
  let {
    steps
  }: {
    steps: { label: string; state: string; detail: string }[];
  } = $props();
</script>

<section class="workflow-strip" aria-labelledby="workflow-title">
  <h2 id="workflow-title">End-to-end workflow</h2>
  <ol class="workflow-list">
    {#each steps as step, index (step.label)}
      <li class="workflow-step" data-state={step.state.toLowerCase()}>
        <span class="workflow-index" aria-hidden="true">{index + 1}</span>
        <span class="workflow-label">{step.label}</span>
        <span class="workflow-state">{step.state}</span>
        <small>{step.detail}</small>
      </li>
    {/each}
  </ol>
</section>
