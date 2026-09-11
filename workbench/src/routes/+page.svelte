<script lang="ts">
  import { onMount, tick } from 'svelte';
  import { gsap } from 'gsap';
  import {
    api,
    ApiError,
    errorHint,
    fetchReady,
    humanize,
    type ClientSurface,
    type ExecutionInfo,
    type Health,
    type Interpretation,
    type MachineModel,
    type Readiness,
    type Scenario,
    type SessionInfo,
    type Telemetry,
    type View
  } from '$lib/api';
  import { registerWorkbenchTools } from '$lib/agent-tools';
  import StatusBar from '$lib/components/StatusBar.svelte';
  import PromptPanel from '$lib/components/PromptPanel.svelte';
  import InterpretationReview from '$lib/components/InterpretationReview.svelte';
  import TaskView from '$lib/components/TaskView.svelte';
  import AlarmSummary from '$lib/components/AlarmSummary.svelte';
  import WorkflowTimeline from '$lib/components/WorkflowTimeline.svelte';
  import ContextInspector from '$lib/components/ContextInspector.svelte';
  import DiagnosticsPanel from '$lib/components/DiagnosticsPanel.svelte';
  import ExecutionPanel from '$lib/components/ExecutionPanel.svelte';
  import RoleFilter from '$lib/components/RoleFilter.svelte';

  type Section = 'overview' | 'context' | 'diagnostics' | 'guide';
  type Role = 'operator' | 'engineer' | 'supervisor';
  type LogEntry = { time: string; code: string; message: string };

  let section = $state<Section>('overview');
  let role = $state<Role>('operator');
  const sectionTitles: Record<Section, string> = {
    overview: 'Operator workspace',
    context: 'Machine context',
    diagnostics: 'Diagnostics',
    guide: 'Operating guide'
  };
  let health = $state<Health | null>(null);
  let model = $state<MachineModel | null>(null);
  let scenarios = $state<Scenario[]>([]);
  let currentScenario = $state('');
  let prompt = $state('Show an overview of the assembly line');
  let interpreter = $state('rules');
  let view = $state<View | null>(null);
  let viewInterpreter = $state('rules');
  let interpretation = $state<Interpretation | null>(null);
  let telemetry = $state<Telemetry | null>(null);
  let errorState = $state<{ code: string; message: string } | null>(null);
  let notice = $state('');
  let errorLog = $state<LogEntry[]>([]);
  let busy = $state(false);
  let booting = $state(true);
  let receiving = $state(false);
  let reachable = $state<boolean | null>(null);
  let lastReceived = $state(0);
  let now = $state(Date.now());
  let requestMilliseconds = $state<number | null>(null);
  let inspectBindings = $state(false);
  let viewElement: HTMLElement | undefined = $state();
  let headingElement: HTMLElement | undefined = $state();
  let sessionInfo = $state<SessionInfo | null>(null);
  let readiness = $state<Readiness | null>(null);
  let viewport = $state<ClientSurface>({ width_px: 240, height_px: 160, size_class: 'large' });
  let generation = 0;
  let events: EventSource | null = null;

  let execution = $derived<ExecutionInfo | null>(interpretation?.execution ?? null);
  let feedAgeMilliseconds = $derived(lastReceived ? Math.max(0, now - lastReceived) : null);
  let feedStale = $derived(receiving && (feedAgeMilliseconds ?? Infinity) >= 3000);
  let feedFresh = $derived(receiving && !feedStale);
  let fresh = $derived(
    feedFresh &&
      telemetry !== null &&
      view !== null &&
      telemetry.model_revision === view.model_revision &&
      telemetry.model_id === view.model_id &&
      telemetry.session_id === view.session_id &&
      telemetry.context_generation === view.context_generation
  );
  let differentContext = $derived(
    view !== null &&
      telemetry !== null &&
      (view.model_id !== telemetry.model_id ||
        view.model_revision !== telemetry.model_revision ||
        view.context_generation !== telemetry.context_generation)
  );
  let activeAlarmCount = $derived((telemetry?.alarms ?? []).filter((alarm) => alarm.active).length);
  let feedLabel = $derived(!receiving ? 'Disconnected' : feedStale ? 'Delayed' : 'Live');
  let feedAge = $derived(
    feedAgeMilliseconds === null ? 'No message' : `${Math.floor(feedAgeMilliseconds / 1000)}s ago`
  );
  let scenarioName = $derived(
    scenarios.find((scenario) => scenario.id === currentScenario)?.name ??
      health?.active_scenario ??
      ''
  );
  let statusLabel = $derived(
    !receiving ? 'Waiting for telemetry' : feedStale ? 'Telemetry delayed' : 'Telemetry current'
  );
  let announcement = $derived(view !== null ? `View resolved: ${view.title}` : 'No resolved view.');
  let alarmAnnouncement = $derived(
    telemetry === null ? 'Alarm states unknown.' : `${activeAlarmCount} active alarms.`
  );
  let workflowSteps = $derived([
    {
      label: 'Request',
      state: interpretation ? 'Done' : busy ? 'Sending' : 'Pending',
      detail: prompt.trim() ? 'Operator intent ready' : 'Waiting for a request'
    },
    {
      label: 'Interpretation',
      state: interpretation
        ? interpretation.status === 'ready'
          ? 'Done'
          : 'Clarification'
        : 'Pending',
      detail: interpretation
        ? interpretation.interpreter === 'rules'
          ? 'Deterministic local rules'
          : 'llama.cpp provider'
        : 'Bounded context is selected first'
    },
    {
      label: 'Resolution',
      state: view ? 'Done' : 'Pending',
      detail: view ? `${view.components.length} bound components` : 'No resolved view'
    },
    {
      label: 'Telemetry',
      state: !receiving ? 'Waiting' : feedStale ? 'Delayed' : 'Live',
      detail: 'Ingestion remains independent of inference'
    },
    {
      label: 'Reconciliation',
      state: view?.reconciled ? 'Done' : 'Ready',
      detail: view?.reconciled
        ? `${view.changes?.length ?? 0} recorded changes`
        : 'Revalidate after a model revision'
    }
  ]);

  $effect(() => {
    inspectBindings = role === 'engineer';
  });

  async function animateView() {
    await tick();
    if (viewElement && !window.matchMedia('(prefers-reduced-motion: reduce)').matches) {
      gsap.fromTo(viewElement, { opacity: 0.65, y: 5 }, { opacity: 1, y: 0, duration: 0.2 });
    }
  }

  function recordError(failure: unknown) {
    const code =
      failure instanceof ApiError
        ? failure.code
        : failure instanceof Error
          ? 'client_error'
          : 'unknown';
    const message = failure instanceof Error ? failure.message : 'The request failed.';
    if (failure instanceof ApiError && (failure.code === 'network' || failure.code === 'timeout'))
      reachable = false;
    errorLog = [{ time: new Date().toLocaleTimeString(), code, message }, ...errorLog].slice(0, 8);
  }

  async function loadSession() {
    try {
      sessionInfo = await api<SessionInfo>('session');
    } catch {
      sessionInfo = null;
    }
  }

  async function refreshContext() {
    const [currentModel, currentHealth] = await Promise.all([
      api<MachineModel>('model'),
      api<Health>('health')
    ]);
    model = currentModel;
    health = currentHealth;
    currentScenario = currentHealth.active_scenario ?? currentScenario;
    await loadSession();
  }

  async function compose() {
    if (!prompt.trim() || busy) return;
    busy = true;
    errorState = null;
    notice = '';
    interpretation = null;
    const requestGeneration = ++generation;
    const started = performance.now();
    try {
      const result = await api<Interpretation>('interpret', {
        prompt,
        interpreter,
        client: viewport
      });
      if (requestGeneration !== generation) return;
      reachable = true;
      interpretation = result;
      requestMilliseconds = Math.round(performance.now() - started);
      if (result.view) {
        view = result.view;
        viewInterpreter = result.interpreter;
        await animateView();
      } else {
        notice = result.message ?? 'The request needs clarification. The existing view is kept.';
      }
    } catch (failure) {
      if (requestGeneration !== generation) return;
      recordError(failure);
      const code = failure instanceof ApiError ? failure.code : 'client_error';
      errorState = {
        code,
        message: failure instanceof Error ? failure.message : 'The request failed.'
      };
    } finally {
      if (requestGeneration === generation) busy = false;
    }
  }

  async function pickCandidate(name: string) {
    if (!name || busy) return;
    prompt = `${interpretation?.prompt ?? prompt} for ${name}`;
    await compose();
  }

  async function switchScenario() {
    if (busy || !currentScenario) return;
    busy = true;
    errorState = null;
    notice = '';
    generation++;
    telemetry = null;
    try {
      const result = await api<MachineModel | { model: MachineModel }>('scenario', {
        id: currentScenario
      });
      reachable = true;
      model = 'model' in result ? result.model : result;
      await refreshContext();
      if (view) {
        try {
          view = await api<View>('reconcile', { view_id: view.view_id });
          notice = `Saved view revalidated against revision ${model.revision}.`;
          await animateView();
        } catch (failure) {
          recordError(failure);
          const code = failure instanceof ApiError ? failure.code : 'client_error';
          errorState = {
            code,
            message: failure instanceof Error ? failure.message : 'Reconciliation failed.'
          };
        }
      }
    } catch (failure) {
      recordError(failure);
      const code = failure instanceof ApiError ? failure.code : 'client_error';
      errorState = {
        code,
        message: failure instanceof Error ? failure.message : 'Could not change the model.'
      };
    } finally {
      busy = false;
    }
  }

  async function loadReadiness() {
    try {
      readiness = await fetchReady();
    } catch {
      readiness = null;
    }
  }

  async function recheckDiagnostics() {
    await Promise.all([loadReadiness(), loadSession()]);
  }

  function connectFeed() {
    events?.close();
    receiving = false;
    events = new EventSource('/api/v1/events');
    events.addEventListener('telemetry', (event) => {
      try {
        const value = JSON.parse((event as MessageEvent).data) as Telemetry;
        if (
          !value ||
          typeof value.model_revision !== 'number' ||
          typeof value.model_id !== 'string' ||
          !value.values ||
          typeof value.values !== 'object' ||
          Array.isArray(value.values) ||
          !Array.isArray(value.alarms)
        )
          return;
        telemetry = value;
        receiving = true;
        lastReceived = Date.now();
        now = lastReceived;
      } catch {
        receiving = false;
      }
    });
    events.onerror = () => {
      receiving = false;
    };
  }

  async function bootstrap() {
    errorState = null;
    booting = true;
    try {
      const [serverHealth, machineModel, availableScenarios] = await Promise.all([
        api<Health>('health'),
        api<MachineModel>('model'),
        api<{ scenarios: Scenario[] }>('scenarios')
      ]);
      reachable = true;
      health = serverHealth;
      model = machineModel;
      scenarios = Array.isArray(availableScenarios.scenarios) ? availableScenarios.scenarios : [];
      currentScenario = serverHealth.active_scenario ?? scenarios[0]?.id ?? '';
      connectFeed();
      void loadSession();
      void loadReadiness();
      await compose();
    } catch (failure) {
      recordError(failure);
      errorState = {
        code: failure instanceof ApiError ? failure.code : 'client_error',
        message: failure instanceof Error ? failure.message : 'The local service is not available.'
      };
    } finally {
      booting = false;
    }
  }

  async function setSection(next: Section) {
    section = next;
    await tick();
    headingElement?.focus();
  }

  onMount(() => {
    function updateViewport() {
      const width = window.innerWidth;
      const height = window.innerHeight;
      viewport = {
        width_px: Math.max(240, Math.min(7680, width)),
        height_px: Math.max(160, Math.min(4320, height)),
        size_class: width <= 640 ? 'small' : width <= 1024 ? 'medium' : 'large'
      };
    }
    updateViewport();
    window.addEventListener('resize', updateViewport, { passive: true });
    void bootstrap();
    const unregisterTools = registerWorkbenchTools(
      async (request) => {
        if (busy || !health) throw new Error('The runtime is unavailable or busy.');
        section = 'overview';
        prompt = request;
        await compose();
        if (errorState) throw new Error(errorState.message);
        return { interpretation, view };
      },
      () => ({ model_id: model?.model_id, revision: model?.revision, view })
    );
    const timer = setInterval(() => {
      now = Date.now();
    }, 1000);
    return () => {
      events?.close();
      window.removeEventListener('resize', updateViewport);
      clearInterval(timer);
      generation++;
      unregisterTools();
      if (viewElement) gsap.killTweensOf(viewElement);
    };
  });
</script>

<svelte:head>
  <title>Context HMI | Operator workspace</title>
  <meta
    name="description"
    content="Interpret operator requests against declared machine context and render live, validated HMI views."
  />
</svelte:head>

<div class="sr-only" aria-live="polite">{announcement}</div>
<div class="sr-only" aria-live="polite">{alarmAnnouncement}</div>
<a class="skip-link" href="#main">Skip to workbench</a>
<header class="topbar">
  <a href="/" class="wordmark" aria-label="Context HMI home"
    ><span class="brand-symbol" aria-hidden="true">[·]</span><strong>context</strong><span>hmi</span
    ></a
  >
  <span class="topbar-divider"></span><span class="topbar-label">Operator workspace</span>
  <span class="version-label">{health?.version ?? 'Connecting'}</span>
</header>

<div class="application">
  <aside class="sidebar" aria-label="Workbench navigation">
    <p class="sidebar-heading">Workspace</p>
    <nav>
      <button class:active={section === 'overview'} onclick={() => void setSection('overview')}
        ><span aria-hidden="true">▦</span> Overview</button
      >
      <button class:active={section === 'context'} onclick={() => void setSection('context')}
        ><span aria-hidden="true">≡</span> Machine context</button
      >
      <button
        class:active={section === 'diagnostics'}
        onclick={() => void setSection('diagnostics')}
        ><span aria-hidden="true">⚙</span> Diagnostics</button
      >
      <button class:active={section === 'guide'} onclick={() => void setSection('guide')}
        ><span aria-hidden="true">?</span> Operating guide</button
      >
    </nav>
    <RoleFilter bind:role />
    <div class="sidebar-model">
      <p class="sidebar-heading">Active model</p>
      <strong>{model?.name ?? 'Connecting…'}</strong>
      <code>{model?.model_id ?? 'local service'}</code>
      {#if model}<span
          >Revision {model.revision} · generation {model.context_generation ?? 'Unknown'}</span
        >{/if}
    </div>
    <div class="sidebar-bottom">
      <span class="outline-label">External data boundary</span>
      <p>
        Values arrive through bounded telemetry ingestion. The workbench never owns source
        credentials.
      </p>
    </div>
  </aside>

  <main id="main">
    <StatusBar
      {health}
      {model}
      {scenarioName}
      {reachable}
      {readiness}
      sessionRole={sessionInfo?.role ?? role}
      {feedLabel}
      {feedStale}
      {feedAge}
      feedDisconnected={!receiving}
      onReconnect={connectFeed}
    />
    <div class="page-heading">
      <div>
        <p class="breadcrumb">Workspace / {sectionTitles[section]}</p>
        <h1 tabindex="-1" bind:this={headingElement}>{sectionTitles[section]}</h1>
      </div>
      <span class:connection-lost={!receiving} class="connection"
        ><span class="connection-marker"></span>{statusLabel}</span
      >
    </div>

    {#if errorState}
      <div class="message error" role="alert">
        <strong>Could not complete the request ({errorState.code})</strong>
        <p>{errorState.message}</p>
        <p>{errorHint(errorState.code)}</p>
        {#if !health}<button class="small-button" onclick={bootstrap}>Reconnect service</button
          >{/if}
      </div>
    {/if}

    {#if section === 'overview'}
      <div class="runtime-layout">
        <div class="runtime-main">
          <PromptPanel bind:prompt bind:interpreter {busy} {health} oncompose={compose} />
          {#if notice}<div class="message notice" role="status"><p>{notice}</p></div>{/if}
          {#if interpretation}<InterpretationReview
              {interpretation}
              {busy}
              onpick={(name) => void pickCandidate(name)}
            />{/if}
          <ExecutionPanel {execution} />
          <AlarmSummary {telemetry} fresh={feedFresh} />
          {#if view}<TaskView
              {view}
              {telemetry}
              {fresh}
              {differentContext}
              bind:inspectBindings
              bind:element={viewElement}
            />{:else}<div class="empty-view">
              <span class="empty-icon" aria-hidden="true">[ ]</span>
              <h2>{booting ? 'Connecting to the service' : 'Your task view will appear here'}</h2>
              <p>
                {booting
                  ? 'Loading declared context and provider state.'
                  : 'Ask for a view to resolve components against the current model.'}
              </p>
            </div>{/if}
        </div>
        <aside class="context-rail" aria-label="Request and data details">
          <section class="rail-section">
            <h2>Interpreted task</h2>
            {#if view}<dl class="task-facts">
                <dt>Task</dt>
                <dd>{humanize(view.task.kind)}</dd>
                <dt>Equipment</dt>
                <dd><code>{view.task.anchor_asset_id || 'All declared equipment'}</code></dd>
                <dt>Interpreter</dt>
                <dd>{viewInterpreter.startsWith('llama') ? 'Local AI' : 'Rules'}</dd>
                <dt>Model revision</dt>
                <dd>{view.model_revision}</dd>
              </dl>
              <p class="rail-note">
                Every component shows its declared binding reason and source identity.
              </p>{:else}<p class="rail-note">
                A resolved request will show its equipment scope here.
              </p>{/if}
          </section>
          <section class="rail-section">
            <h2>Model revision</h2>
            <p>
              Switch between declared configurations to revalidate a saved view and inspect binding
              changes.
            </p>
            <label for="scenario">Machine configuration</label><select
              id="scenario"
              bind:value={currentScenario}
              disabled={busy || !scenarios.length}
              onchange={switchScenario}
              >{#each scenarios as scenario (scenario.id)}<option value={scenario.id}
                  >{scenario.name}{scenario.revision ? ` · r${scenario.revision}` : ''}</option
                >{/each}</select
            >
            <p class="rail-note">
              {scenarios.find((scenario) => scenario.id === currentScenario)?.description ??
                'No alternate configuration loaded.'}
            </p>
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
    {:else if section === 'context'}
      <p class="intro">
        The declared model is the source of truth for identities, ownership, units, relationships,
        alarms, and source references. Retrieval is bounded before interpretation.
      </p>
      <ContextInspector {model} {telemetry} fresh={feedFresh} />
    {:else if section === 'diagnostics'}
      <p class="intro">
        Inspect reachability, provider readiness, telemetry freshness, execution cost, model
        generation, and recent failures. A provider failure does not stop current telemetry.
      </p>
      <DiagnosticsPanel
        {health}
        {model}
        {reachable}
        {receiving}
        {feedAge}
        {requestMilliseconds}
        {errorLog}
        {readiness}
        session={sessionInfo}
        {execution}
        onrecheck={recheckDiagnostics}
      />
    {:else}
      <article class="guide">
        <p class="intro">
          This workspace turns natural-language operator intent into a finite, validated view. The
          service owns context interpretation, binding, reconciliation, and telemetry state. The
          client renders the declarative result.
        </p>
        <h2>Interpret with bounded context</h2>
        <p>
          Relevant assets, relationships, tags, alarms, and task vocabulary are selected before an
          interpreter runs. Static provider instructions remain stable while request-specific
          context and input stay bounded. Revision-aware caches avoid repeating equivalent work.
        </p>
        <h2>Validate before rendering</h2>
        <p>
          Rules or llama.cpp may propose a task, but they cannot choose arbitrary source addresses.
          The native engine checks asset identity, ownership, units, roles, permissions, and current
          generation before composing the screen.
        </p>
        <h2>Render live state honestly</h2>
        <p>
          The view contract contains stable component identities, binding explanations, quality,
          timestamps, alarms, and unresolved issues. This client keeps unknown, stale, delayed, and
          disconnected states visible.
        </p>
        <h2>Reconcile after a model change</h2>
        <p>
          Choose another declared configuration to revalidate the saved view. Same-ID semantic
          changes become reviewable conflicts, and unsupported replacements remain unresolved rather
          than guessed.
        </p>
        <h2>Telemetry boundary</h2>
        <p>
          Operational data is submitted through bounded ingestion and distributed independently of
          inference. The browser does not connect to controllers and does not carry credentials.
          External dictation, if used, only supplies text to the same request field.
        </p>
        <h2>What this establishes</h2>
        <p>
          It establishes a reusable JSON contract between a native service and a browser renderer.
          Product-specific runtime compatibility and process-safety certification require separate
          access and evidence.
        </p>
      </article>
    {/if}
    <footer class="page-footer">
      <span>Context HMI</span><span
        >Local-first operator workspace · Server-owned context and telemetry</span
      >
    </footer>
  </main>
</div>
