<script lang="ts">
  import { onMount, tick } from 'svelte';
  import { gsap } from 'gsap';
  import Reading from '$lib/Reading.svelte';
  import { registerWorkbenchTools } from '$lib/agent-tools';
  import {
    api,
    humanize,
    type Health,
    type MachineModel,
    type Scenario,
    type Interpretation,
    type Telemetry,
    type View
  } from '$lib/api';

  let section = $state<'runtime' | 'context' | 'guide'>('runtime');
  let health = $state<Health | null>(null);
  let model = $state<MachineModel | null>(null);
  let scenarios = $state<Scenario[]>([]);
  let currentScenario = $state('');
  let prompt = $state('Show the filling view for Tank 3');
  let interpreter = $state('rules');
  let viewInterpreter = $state('rules');
  let view = $state<View | null>(null);
  let telemetry = $state<Telemetry | null>(null);
  let interpretation = $state<Interpretation | null>(null);
  let error = $state('');
  let notice = $state('');
  let busy = $state(false);
  let booting = $state(true);
  let receiving = $state(false);
  let running = $state(true);
  let lastReceived = $state(0);
  let now = $state(Date.now());
  let requestMilliseconds = $state<number | null>(null);
  let inspectBindings = $state(false);
  let contextFilter = $state('');
  let viewElement: HTMLElement | undefined = $state();
  let generation = 0;

  let fresh = $derived(
    receiving &&
      now - lastReceived < 3000 &&
      telemetry !== null &&
      telemetry.model_revision === view?.model_revision &&
      telemetry.model_id === view.model_id &&
      telemetry.session_id === view.session_id &&
      telemetry.context_generation === view.context_generation
  );
  let differentContext = $derived(
    view !== null &&
      telemetry !== null &&
      (view.model_id !== telemetry.model_id ||
        view.model_revision !== telemetry.model_revision ||
        view.session_id !== telemetry.session_id ||
        view.context_generation !== telemetry.context_generation)
  );
  let assetCount = $derived(new Set(view?.components.map((item) => item.asset_id) ?? []).size);
  let filteredTags = $derived(
    model?.tags.filter((tag) =>
      `${tag.id} ${tag.asset_id} ${tag.role} ${tag.name}`
        .toLowerCase()
        .includes(contextFilter.toLowerCase())
    ) ?? []
  );
  let statusLabel = $derived(
    !receiving
      ? 'Disconnected'
      : !running
        ? 'Simulation paused'
        : now - lastReceived >= 3000
          ? 'Feed delayed'
          : 'Receiving simulated data'
  );

  async function animateView() {
    await tick();
    if (viewElement && !window.matchMedia('(prefers-reduced-motion: reduce)').matches) {
      gsap.fromTo(
        viewElement,
        { opacity: 0.65, y: 5 },
        { opacity: 1, y: 0, duration: 0.2, overwrite: true }
      );
    }
  }

  async function compose() {
    if (!prompt.trim() || busy) return;
    busy = true;
    error = '';
    notice = '';
    interpretation = null;
    const requestGeneration = ++generation;
    const started = performance.now();
    try {
      const result = await api<Interpretation>('interpret', { prompt, interpreter });
      if (requestGeneration !== generation) return;
      interpretation = result;
      requestMilliseconds = Math.round(performance.now() - started);
      if (result.view) {
        view = result.view;
        viewInterpreter = result.interpreter;
        if (
          model?.session_id !== view.session_id ||
          model?.context_generation !== view.context_generation
        ) {
          const [currentModel, currentHealth] = await Promise.all([
            api<MachineModel>('model'),
            api<Health>('health')
          ]);
          model = currentModel;
          health = currentHealth;
          currentScenario = currentHealth.active_scenario ?? currentScenario;
        }
        await animateView();
      } else {
        notice =
          result.message ?? 'The request needs clarification. The existing view has been kept.';
      }
    } catch (failure) {
      if (requestGeneration === generation)
        error = failure instanceof Error ? failure.message : 'The request failed.';
    } finally {
      if (requestGeneration === generation) busy = false;
    }
  }

  async function switchScenario() {
    if (busy) return;
    busy = true;
    error = '';
    notice = '';
    generation++;
    telemetry = null;
    try {
      const result = await api<MachineModel | { model: MachineModel }>('scenario', {
        id: currentScenario
      });
      model = 'model' in result ? result.model : result;
      if (view) {
        view = await api<View>('reconcile', { view_id: view.view_id });
        notice = `Checked the saved task against revision ${model.revision}.`;
        await animateView();
      }
    } catch (failure) {
      error = failure instanceof Error ? failure.message : 'Could not change the configuration.';
    } finally {
      busy = false;
    }
  }

  async function toggleSimulation() {
    try {
      await api('simulation', { running: !running });
      running = !running;
    } catch (failure) {
      error = failure instanceof Error ? failure.message : 'Could not change simulation state.';
    }
  }

  async function bootstrap() {
    error = '';
    booting = true;
    try {
      const [serverHealth, machineModel, availableScenarios] = await Promise.all([
        api<Health>('health'),
        api<MachineModel>('model'),
        api<{ scenarios: Scenario[] }>('scenarios')
      ]);
      health = serverHealth;
      model = machineModel;
      scenarios = availableScenarios.scenarios;
      currentScenario =
        serverHealth.active_scenario ??
        scenarios.find((scenario) => scenario.id === 'pump-station')?.id ??
        scenarios[0]?.id ??
        '';
      running = serverHealth.simulation_running ?? true;
      await compose();
    } catch (failure) {
      error = failure instanceof Error ? failure.message : 'The local runtime is not available.';
    } finally {
      booting = false;
    }
  }

  onMount(() => {
    void bootstrap();
    const unregisterTools = registerWorkbenchTools(
      async (request) => {
        if (busy || !health)
          throw new Error('The runtime is unavailable or a request is already in progress.');
        section = 'runtime';
        prompt = request;
        await compose();
        if (error) throw new Error(error);
        return { interpretation, view };
      },
      () => ({ model_id: model?.model_id, revision: model?.revision, view })
    );
    const events = new EventSource('/api/v1/events');
    const handleTelemetry = (event: MessageEvent) => {
      try {
        const value = JSON.parse(event.data) as Telemetry;
        if (
          !value ||
          typeof value.model_revision !== 'number' ||
          typeof value.model_id !== 'string' ||
          typeof value.session_id !== 'string' ||
          !Number.isSafeInteger(value.context_generation) ||
          !value.values ||
          typeof value.values !== 'object' ||
          Array.isArray(value.values) ||
          !Array.isArray(value.alarms)
        )
          return;
        telemetry = value;
        running = Object.values(value.values).some((sample) => sample.quality === 'good');
        receiving = true;
        lastReceived = Date.now();
        now = lastReceived;
      } catch {
        receiving = false;
      }
    };
    events.onmessage = handleTelemetry;
    events.addEventListener('telemetry', handleTelemetry as EventListener);
    events.onerror = () => {
      receiving = false;
    };
    const timer = setInterval(() => {
      now = Date.now();
    }, 1000);
    return () => {
      events.close();
      clearInterval(timer);
      generation++;
      unregisterTools();
      if (viewElement) gsap.killTweensOf(viewElement);
    };
  });
</script>

<svelte:head>
  <title>Context HMI | Task workbench</title>
  <meta
    name="description"
    content="Inspect machine context, compose task views and review configuration changes in a local HMI prototype."
  />
</svelte:head>

<a class="skip-link" href="#main">Skip to workbench</a>
<header class="topbar">
  <a href="/" class="wordmark" aria-label="Context HMI home"
    ><span class="brand-symbol" aria-hidden="true">[·]</span><strong>context</strong><span>hmi</span
    ></a
  >
  <span class="topbar-divider"></span><span class="topbar-label">Engineering workbench</span>
  <span class="prototype-label">Prototype {health?.version ?? '0.1.0'}</span>
</header>

<div class="application">
  <aside class="sidebar" aria-label="Workbench navigation">
    <p class="sidebar-heading">Workspace</p>
    <nav>
      <button
        class:active={section === 'runtime'}
        onclick={() => {
          section = 'runtime';
        }}><span aria-hidden="true">▦</span> Task view</button
      >
      <button
        class:active={section === 'context'}
        onclick={() => {
          section = 'context';
        }}><span aria-hidden="true">≡</span> Machine context</button
      >
      <button
        class:active={section === 'guide'}
        onclick={() => {
          section = 'guide';
        }}><span aria-hidden="true">?</span> How it works</button
      >
    </nav>
    <div class="sidebar-model">
      <p class="sidebar-heading">Active model</p>
      <strong>{model?.name ?? 'Connecting…'}</strong>
      <code>{model?.model_id ?? 'local runtime'}</code>
      {#if model}<span>Revision {model.revision}</span>{/if}
    </div>
    <div class="sidebar-bottom">
      <span class="outline-label">Simulator</span>
      <p>All readings come from simulated equipment.</p>
    </div>
  </aside>

  <main id="main">
    <div class="page-heading">
      <div>
        <p class="breadcrumb">
          Workspace / {section === 'runtime'
            ? 'Task view'
            : section === 'context'
              ? 'Machine context'
              : 'Documentation'}
        </p>
        <h1>
          {section === 'runtime'
            ? 'Task view'
            : section === 'context'
              ? 'Machine context'
              : 'How it works'}
        </h1>
      </div>
      <span class:connection-lost={!receiving} class="connection"
        ><span class="connection-marker"></span>{statusLabel}</span
      >
    </div>

    {#if error}
      <div class="message error" role="alert">
        <strong>Could not complete the request</strong>
        <p>{error}</p>
        {#if !health}<p>Start the local C++ service, then reconnect.</p>
          <button class="small-button" onclick={bootstrap}>Reconnect</button>{/if}
      </div>
    {/if}

    {#if section === 'runtime'}
      <div class="runtime-layout">
        <div class="runtime-main">
          <section class="request-panel" aria-labelledby="request-title">
            <h2 id="request-title">What do you need to see?</h2>
            <p>Describe a supported task and the equipment involved.</p>
            <form
              onsubmit={(event) => {
                event.preventDefault();
                void compose();
              }}
            >
              <label for="request" class="sr-only">Operator request</label>
              <textarea
                id="request"
                bind:value={prompt}
                maxlength="2048"
                rows="2"
                placeholder="Show the filling view for Tank 3"
                disabled={busy}></textarea>
              <div class="request-actions">
                <div class="interpreter-select">
                  <label for="interpreter">Interpret with</label><select
                    id="interpreter"
                    bind:value={interpreter}
                    disabled={busy}
                  >
                    <option value="rules">Rules · offline</option><option
                      value="llama"
                      disabled={!health?.llama_configured}>Local AI · llama.cpp</option
                    >
                  </select>
                </div>
                <button
                  class="primary-button"
                  type="submit"
                  disabled={busy || !health || !prompt.trim()}
                  >{busy ? 'Resolving…' : 'Compose view'}<span aria-hidden="true">↗</span></button
                >
              </div>
            </form>
            <div class="examples">
              <span>Try</span
              >{#each ['Show the filling view for Tank 3', 'Show an overview of Pump 1', 'Show alarms for Tank 3'] as example}
                <button
                  disabled={busy}
                  onclick={() => {
                    prompt = example;
                  }}>{example}</button
                >{/each}
            </div>
          </section>

          {#if notice}<div class="message notice" role="status">
              <p>{notice}</p>
              {#if interpretation?.candidates?.length}<p>
                  Matches: {interpretation.candidates
                    .map((candidate) =>
                      typeof candidate === 'string' ? candidate : candidate.name
                    )
                    .join(', ')}.
                </p>{/if}
              {#if interpretation?.status === 'clarification' && view}<p>
                  The previous view is still shown below.
                </p>{/if}
            </div>{/if}

          {#if view}
            <section class="view-shell" bind:this={viewElement} aria-labelledby="view-title">
              <div class="view-heading">
                <div>
                  <span class="eyebrow">Resolved view</span>
                  <h2 id="view-title">{view.title}</h2>
                </div>
                <span class:status-review={view.status !== 'ready'} class="status-tag"
                  >{humanize(view.status)}</span
                >
              </div>
              <div class="view-meta">
                <span>{view.components.length} components</span><span
                  >{assetCount} equipment assets</span
                ><span>Revision {view.model_revision}</span>
                <button
                  class="text-button"
                  onclick={() => {
                    inspectBindings = !inspectBindings;
                  }}>{inspectBindings ? 'Hide bindings' : 'Inspect bindings'}</button
                >
              </div>
              {#if view.issues.length}<div class="view-issues" role="status">
                  <strong>Context needs review</strong>
                  <ul>
                    {#each view.issues as issue}<li>{issue.message}</li>{/each}
                  </ul>
                </div>{/if}
              {#if !fresh}<div class="stale-banner">
                  {differentContext
                    ? 'The runtime session or configuration has changed. Make a fresh request to use the current context.'
                    : 'Current readings are unavailable. Values stay unknown until matching telemetry arrives.'}
                </div>{/if}
              {#if view.components.length}
                <div class="readings-grid">
                  {#each view.components as component (component.id)}<Reading
                      {component}
                      {telemetry}
                      {fresh}
                    />{/each}
                </div>
              {:else}<div class="empty-view">
                  <h3>No components resolved</h3>
                  <p>
                    Check the missing context above, or make a new request against the current
                    model.
                  </p>
                </div>{/if}
              {#if inspectBindings}<div class="table-scroll binding-table">
                  <table>
                    <thead
                      ><tr
                        ><th>Component</th><th>Equipment</th><th>Tag and role</th><th>State</th></tr
                      ></thead
                    ><tbody>
                      {#each view.components as component}<tr
                          ><td>{component.label}</td><td><code>{component.asset_id}</code></td><td
                            ><code>{component.tag_id}</code><small
                              >{humanize(component.role)}{component.unit
                                ? ` · ${component.unit}`
                                : ''}</small
                            ></td
                          ><td>{humanize(component.status ?? 'ready')}</td></tr
                        >{/each}
                    </tbody>
                  </table>
                </div>{/if}
              {#if view.changes?.length}<div class="change-record">
                  <h3>Reconciliation record</h3>
                  {#each view.changes as change}<p>
                      <span class="inline-tag">{humanize(change.kind)}</span>
                      {change.message}{#if change.tag_id}
                        <code>{change.tag_id}</code>{/if}
                    </p>{/each}
                </div>{/if}
            </section>
          {:else}<div class="empty-view">
              <span class="empty-icon" aria-hidden="true">[ ]</span>
              <h2>{booting ? 'Connecting to the runtime' : 'Your task view will appear here'}</h2>
              <p>
                {booting
                  ? 'Loading the machine model and its declared relationships.'
                  : 'The runtime composes components after resolving your request.'}
              </p>
            </div>{/if}
        </div>

        <aside class="context-rail" aria-label="Task and configuration details">
          <section class="rail-section">
            <h2>Interpreted task</h2>
            {#if view}<dl class="task-facts">
                <dt>Task</dt>
                <dd>{humanize(view.task.kind)}</dd>
                <dt>Equipment</dt>
                <dd><code>{view.task.anchor_asset_id || 'All equipment'}</code></dd>
                <dt>Interpreter</dt>
                <dd>{viewInterpreter.startsWith('llama') ? 'Local AI' : 'Rules'}</dd>
                <dt>Data source</dt>
                <dd>Simulator</dd>
              </dl>
              <p class="rail-note">
                Open “Why this reading?” to see the declared reason for each component.
              </p>
            {:else}<p class="rail-note">
                A resolved request will show its equipment scope here.
              </p>{/if}
          </section>
          <section class="rail-section">
            <h2>Configuration changes</h2>
            <p>Change the model to check the saved task against another configuration.</p>
            <label for="scenario">Machine configuration</label><select
              id="scenario"
              bind:value={currentScenario}
              disabled={busy || !scenarios.length}
              onchange={switchScenario}
            >
              {#each scenarios as scenario}<option value={scenario.id}
                  >{scenario.name}{scenario.revision ? ` · r${scenario.revision}` : ''}</option
                >{/each}</select
            >
            <p class="rail-note">
              {scenarios.find((scenario) => scenario.id === currentScenario)?.description ??
                'Loading configurations…'}
            </p>
          </section>
          <section class="rail-section">
            <h2>Live feed</h2>
            <dl class="task-facts">
              <dt>Source</dt>
              <dd>Simulated readings</dd>
              <dt>Last message</dt>
              <dd>
                {lastReceived
                  ? `${Math.max(0, Math.floor((now - lastReceived) / 1000))}s ago`
                  : 'Waiting'}
              </dd>
              <dt>Snapshot</dt>
              <dd>{telemetry?.tick ?? 'Unknown'}</dd>
            </dl>
            <button class="small-button" onclick={toggleSimulation} disabled={!health}
              >{running ? 'Pause simulation' : 'Resume simulation'}</button
            >
          </section>
          <section class="rail-section timing">
            <h2>Last request</h2>
            <p class="timing-value">
              {requestMilliseconds === null ? 'No measurement' : `${requestMilliseconds} ms`}
            </p>
            <p class="rail-note">
              Observed browser round trip for this request. Includes interpretation and response,
              excludes rendering.
            </p>
          </section>
        </aside>
      </div>
    {:else if section === 'context'}
      <p class="intro">
        Equipment identities, tag ownership and connections used to resolve a task.
      </p>
      {#if model}
        <div class="context-counts">
          <div><strong>{model.assets.length}</strong><span>Equipment assets</span></div>
          <div><strong>{model.tags.length}</strong><span>Declared tags</span></div>
          <div><strong>{model.relationships.length}</strong><span>Connections</span></div>
          <div><strong>{model.alarms.length}</strong><span>Alarm definitions</span></div>
        </div>
        <section class="document-section">
          <h2>Equipment and connections</h2>
          <div class="table-scroll">
            <table>
              <thead
                ><tr
                  ><th>Equipment</th><th>Identity</th><th>Type</th><th>Declared connection</th></tr
                ></thead
              ><tbody
                >{#each model.assets as asset}<tr
                    ><td>{asset.name}</td><td><code>{asset.id}</code></td><td>{asset.kind}</td><td
                      >{model.relationships
                        .filter((relationship) => relationship.from === asset.id)
                        .map((relationship) => `${relationship.kind} ${relationship.to}`)
                        .join(', ') || 'None declared'}</td
                    ></tr
                  >{/each}</tbody
              >
            </table>
          </div>
        </section>
        <section class="document-section">
          <div class="section-heading">
            <h2>Tag registry</h2>
            <label class="search-label"
              >Filter tags<input
                bind:value={contextFilter}
                placeholder="Name, owner or role"
                type="search"
              /></label
            >
          </div>
          <div class="table-scroll">
            <table>
              <thead
                ><tr
                  ><th>Tag</th><th>Owner</th><th>Role</th><th>Type / unit</th><th
                    >Source reference</th
                  ></tr
                ></thead
              ><tbody
                >{#each filteredTags as tag}<tr
                    ><td><code>{tag.id}</code><small>{tag.name}</small></td><td
                      ><code>{tag.asset_id}</code></td
                    ><td>{humanize(tag.role)}</td><td
                      >{tag.data_type}{tag.unit ? ` / ${tag.unit}` : ''}</td
                    ><td
                      ><code>{tag.source.identifier}</code><small>{tag.source.namespace_uri}</small
                      ><small>{tag.source.server}</small></td
                    ></tr
                  >{/each}</tbody
              >
            </table>
          </div>
          {#if !filteredTags.length}<p class="empty-results">No tags match this filter.</p>{/if}
        </section>
      {/if}
    {:else}
      <article class="guide">
        <p class="intro">
          This prototype composes views for a small set of tasks using declared machine context.
        </p>
        <h2>1. Interpret the request</h2>
        <p>
          The interpreter identifies a task and an equipment scope. The offline rule mode supports
          filling, overview and alarms. A configured llama.cpp server can interpret the request
          through the same typed task interface.
        </p>
        <h2>2. Resolve the machine context</h2>
        <p>
          The C++ engine checks equipment identity, measurement roles, engineering units and
          declared connections. It selects the data that the supported task needs. It returns
          missing or ambiguous context for review.
        </p>
        <h2>3. Compose the view</h2>
        <p>
          The engine returns a JSON screen description. The workbench renders gauges, values, states
          and alarms from its component catalog. Each component includes its binding and the reason
          it was selected.
        </p>
        <h2>4. Reevaluate after changes</h2>
        <p>
          The saved task is checked against the revised machine model. Binding dependencies retain
          equipment ownership, measurement role, unit and source reference. Changed meaning is
          reported for review.
        </p>
        <h2>Try the demonstration</h2>
        <ol>
          <li>Request the filling view for Tank 3.</li>
          <li>Inspect why the level and feed measurements were included.</li>
          <li>Choose a revised configuration and inspect the changes.</li>
          <li>Make a fresh request to inspect the current model.</li>
          <li>Pause the simulator or stop the service to inspect the unavailable-data state.</li>
        </ol>
        <h2>What the prototype does not establish</h2>
        <p>
          The readings are simulated. This workbench does not control physical equipment. Schema and
          binding checks reject specific errors; they do not establish general interpretation
          accuracy or process safety. The request timing shown in the interface is a local
          observation, not a comparative benchmark.
        </p>
        <h2>Implementation references</h2>
        <p>
          <a href="https://github.com/nlohmann/json">nlohmann JSON</a> ·
          <a href="https://github.com/yhirose/cpp-httplib">cpp-httplib</a>
          · <a href="https://github.com/ggml-org/llama.cpp">llama.cpp</a> ·
          <a href="https://svelte.dev/docs/kit">SvelteKit</a>
          · <a href="https://gsap.com/docs/v3/">GSAP</a>
        </p>
        <p class="guide-note">
          Architecture decisions, API investigation and verification commands are maintained in the
          repository’s AsciiDoc documents.
        </p>
      </article>
    {/if}
    <footer class="page-footer">
      <span>Context HMI</span><span>Local PS2 prototype · Simulated equipment</span>
    </footer>
  </main>
</div>
