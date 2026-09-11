<script lang="ts">
  import {
    humanize,
    type ClientSurface,
    type ExecutionInfo,
    type Health,
    type Interpretation,
    type MachineModel,
    type Scenario,
    type Telemetry,
    type View
  } from '$lib/api';
  import AlarmSummary from './AlarmSummary.svelte';
  import ExecutionPanel from './ExecutionPanel.svelte';
  import InterpretationReview from './InterpretationReview.svelte';
  import PromptPanel from './PromptPanel.svelte';
  import TaskView from './TaskView.svelte';
  import WorkflowTimeline from './WorkflowTimeline.svelte';

  type WorkflowStep = { label: string; state: string; detail: string };

  let {
    prompt = $bindable(),
    interpreter = $bindable(),
    busy,
    health,
    notice,
    interpretation,
    execution,
    telemetry,
    feedFresh,
    view,
    fresh,
    differentContext,
    viewInterpreter,
    inspectBindings = $bindable(false),
    model,
    scenarios,
    currentScenario = $bindable(),
    viewport,
    feedAge,
    requestMilliseconds,
    workflowSteps,
    booting,
    viewElement = $bindable(),
    oncompose,
    onpick,
    onswitchscenario
  }: {
    prompt: string;
    interpreter: string;
    busy: boolean;
    health: Health | null;
    notice: string;
    interpretation: Interpretation | null;
    execution: ExecutionInfo | null;
    telemetry: Telemetry | null;
    feedFresh: boolean;
    view: View | null;
    fresh: boolean;
    differentContext: boolean;
    viewInterpreter: string;
    inspectBindings: boolean;
    model: MachineModel | null;
    scenarios: Scenario[];
    currentScenario: string;
    viewport: ClientSurface;
    feedAge: string;
    requestMilliseconds: number | null;
    workflowSteps: WorkflowStep[];
    booting: boolean;
    viewElement: HTMLElement | undefined;
    oncompose: () => void;
    onpick: (name: string) => void;
    onswitchscenario: () => void;
  } = $props();

  let scenarioDescription = $derived(
    scenarios.find((scenario) => scenario.id === currentScenario)?.description ??
      'No alternate configuration loaded.'
  );
</script>

<div class="runtime-layout">
  <div class="runtime-main">
    <PromptPanel bind:prompt bind:interpreter {busy} {health} {oncompose} />
    {#if notice}<div class="message notice" role="status"><p>{notice}</p></div>{/if}
    {#if interpretation}
      <InterpretationReview {interpretation} {busy} onpick={(name) => onpick(name)} />
    {/if}
    <ExecutionPanel {execution} />
    <AlarmSummary {telemetry} fresh={feedFresh} />
    {#if view}
      <TaskView
        {view}
        {telemetry}
        {fresh}
        {differentContext}
        bind:inspectBindings
        bind:element={viewElement}
      />
    {:else}
      <div class="empty-view">
        <span class="empty-icon" aria-hidden="true">[ ]</span>
        <h2>{booting ? 'Connecting to the service' : 'Your task view will appear here'}</h2>
        <p>
          {booting
            ? 'Loading declared context and provider state.'
            : 'Ask for a view to resolve components against the current model.'}
        </p>
      </div>
    {/if}
  </div>
  <aside class="context-rail" aria-label="Request and data details">
    <section class="rail-section">
      <h2>Interpreted task</h2>
      {#if view}
        <dl class="task-facts">
          <dt>Task</dt>
          <dd>{humanize(view.task.kind)}</dd>
          <dt>Equipment</dt>
          <dd><code>{view.task.anchor_asset_id || 'All declared equipment'}</code></dd>
          <dt>Interpreter</dt>
          <dd>
            {viewInterpreter === 'rules'
              ? 'Rules'
              : viewInterpreter === 'openai-compatible'
                ? 'Remote AI'
                : 'Local AI'}
          </dd>
          <dt>Model revision</dt>
          <dd>{view.model_revision}</dd>
        </dl>
        <p class="rail-note">
          Every component shows its declared binding reason and source identity.
        </p>
      {:else}
        <p class="rail-note">A resolved request will show its equipment scope here.</p>
      {/if}
    </section>
    <section class="rail-section">
      <h2>Model revision</h2>
      <p>
        Switch between declared configurations to revalidate a saved view and inspect binding
        changes.
      </p>
      <label for="scenario">Machine configuration</label>
      <select
        id="scenario"
        bind:value={currentScenario}
        disabled={busy || !scenarios.length}
        onchange={onswitchscenario}
      >
        {#each scenarios as scenario (scenario.id)}
          <option value={scenario.id}
            >{scenario.name}{scenario.revision ? ` · r${scenario.revision}` : ''}</option
          >
        {/each}
      </select>
      <p class="rail-note">{scenarioDescription}</p>
    </section>
    <section class="rail-section">
      <h2>Live telemetry</h2>
      <dl class="task-facts">
        <dt>Source</dt>
        <dd>{health?.mode ?? 'Unknown'}</dd>
        <dt>Last message</dt>
        <dd>{feedAge}</dd>
        <dt>Sequence</dt>
        <dd>{telemetry?.sequence ?? 'Unknown'}</dd>
        <dt>Quality</dt>
        <dd>{telemetry ? humanize(telemetry.quality) : 'Unknown'}</dd>
        <dt>Values</dt>
        <dd>{telemetry ? Object.keys(telemetry.values).length : 'Unknown'}</dd>
      </dl>
      <p class="rail-note">
        Current values are server-owned. Stale or unknown data is never presented as valid.
      </p>
    </section>
    <section class="rail-section">
      <h2>Client surface</h2>
      <dl class="task-facts">
        <dt>Viewport</dt>
        <dd>{viewport.width_px} × {viewport.height_px}</dd>
        <dt>Size class</dt>
        <dd>{viewport.size_class}</dd>
      </dl>
      <p class="rail-note">
        The current surface is sent as optional request metadata so the composer can select a
        suitable layout.
      </p>
    </section>
    <section class="rail-section timing">
      <h2>Browser request time</h2>
      <p class="timing-value">
        {requestMilliseconds === null ? 'No measurement' : `${requestMilliseconds} ms`}
      </p>
      <p class="rail-note">
        Round trip only. Inspect execution details for context size, provider usage, and cache
        state.
      </p>
    </section>
  </aside>
</div>
<WorkflowTimeline steps={workflowSteps} />
