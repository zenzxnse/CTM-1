<script lang="ts">
  import {
    humanize,
    type Health,
    type MachineModel,
    type Readiness,
    type SessionInfo,
    type ExecutionInfo
  } from '$lib/api';

  let {
    health,
    model,
    reachable,
    receiving,
    feedAge,
    requestMilliseconds,
    errorLog,
    readiness,
    session,
    execution,
    onrecheck
  }: {
    health: Health | null;
    model: MachineModel | null;
    reachable: boolean | null;
    receiving: boolean;
    feedAge: string;
    requestMilliseconds: number | null;
    errorLog: { time: string; code: string; message: string }[];
    readiness: Readiness | null;
    session: SessionInfo | null;
    execution: ExecutionInfo | null;
    onrecheck: () => void;
  } = $props();

  let endpoints = [
    { route: 'GET /health', use: 'Reachability, interpreter mode, session and generation' },
    {
      route: 'GET /ready',
      use: 'Readiness probe. 503 means the provider is down, not the service'
    },
    { route: 'GET /session', use: 'Server-issued role and capabilities' },
    { route: 'GET /model', use: 'Declared machine context for binding inspection' },
    { route: 'GET /scenarios', use: 'Available configurations with revisions' },
    { route: 'POST /interpret', use: 'Prompt to typed task, optional view' },
    { route: 'POST /resolve', use: 'Typed task to resolved view' },
    { route: 'POST /reconcile', use: 'Saved view checked against a revised model' },
    { route: 'POST /scenario', use: 'Active configuration change' },
    { route: 'POST /telemetry', use: 'Bounded current data ingestion' },
    { route: 'GET /telemetry', use: 'Single telemetry snapshot probe' },
    { route: 'GET /events', use: 'Telemetry stream, at most three clients' }
  ];
</script>

<section class="document-section">
  <h2>Service health</h2>
  <div class="table-scroll">
    <table>
      <thead><tr><th>Property</th><th>Value</th></tr></thead>
      <tbody>
        <tr>
          <td>Service</td>
          <td>{reachable === null ? 'Unknown' : reachable ? 'Reachable' : 'Unreachable'}</td>
        </tr>
        <tr><td>Version</td><td>{health?.version ?? 'Unknown'}</td></tr>
        <tr><td>Mode</td><td>{health ? humanize(health.mode) : 'Unknown'}</td></tr>
        <tr><td>Interpreter mode</td><td>{health?.interpreter ?? 'Unknown'}</td></tr>
        <tr>
          <td>Local AI provider</td>
          <td
            >{health
              ? health.llama_configured
                ? 'Configured, llama.cpp'
                : 'Not configured'
              : 'Unknown'}</td
          >
        </tr>
        <tr><td>Session</td><td><code>{health?.session_id ?? 'Unknown'}</code></td></tr>
        <tr><td>Context generation</td><td>{health?.context_generation ?? 'Unknown'}</td></tr>
        <tr><td>Feed</td><td>{receiving ? `Receiving, ${feedAge}` : 'Not receiving'}</td></tr>
        <tr>
          <td>Model</td>
          <td>{model ? `${model.model_id}, revision ${model.revision}` : 'Unknown'}</td>
        </tr>
        <tr>
          <td>Last request round trip</td>
          <td>{requestMilliseconds === null ? 'No measurement' : `${requestMilliseconds} ms`}</td>
        </tr>
      </tbody>
    </table>
  </div>
</section>

<section class="document-section">
  <h2>Last execution</h2>
  {#if execution === null}
    <p class="empty-results">No interpretation has been requested in this session.</p>
  {:else}
    <div class="table-scroll">
      <table>
        <thead><tr><th>Property</th><th>Value</th></tr></thead>
        <tbody>
          <tr><td>Elapsed</td><td>{execution.elapsed_ms ?? 'Unknown'} ms</td></tr>
          <tr><td>Model revision</td><td>{execution.model_revision ?? 'Unknown'}</td></tr>
          <tr><td>Context generation</td><td>{execution.context_generation ?? 'Unknown'}</td></tr>
          <tr><td>Context size</td><td>{execution.context?.bytes ?? 'Unknown'} bytes</td></tr>
          <tr
            ><td>Estimated tokens</td><td>{execution.context?.estimated_tokens ?? 'Unknown'}</td
            ></tr
          >
          <tr><td>Retrieved candidates</td><td>{execution.context?.candidates ?? 'Unknown'}</td></tr
          >
          <tr><td>Context truncation</td><td>{execution.context?.truncated ? 'Yes' : 'No'}</td></tr>
          <tr
            ><td>Interpretation cache</td><td
              >{execution.cache?.interpretation?.state ?? 'Unknown'}</td
            ></tr
          >
          <tr><td>Retrieval cache</td><td>{execution.cache?.retrieval?.state ?? 'Unknown'}</td></tr>
          <tr
            ><td>Provider tokens</td><td
              >{execution.provider_usage?.total_tokens ?? 'Not reported'}</td
            ></tr
          >
          <tr
            ><td>Cached prompt tokens</td><td
              >{execution.provider_usage?.cached_prompt_tokens ?? 'Not reported'}</td
            ></tr
          >
        </tbody>
      </table>
    </div>
  {/if}
</section>

<section class="document-section">
  <h2>Lifecycle</h2>
  {#if health?.lifecycle && Object.keys(health.lifecycle).length}
    <div class="table-scroll">
      <table>
        <thead><tr><th>Component</th><th>State</th><th>Reason</th><th>Transitions</th></tr></thead>
        <tbody>
          {#each Object.entries(health.lifecycle) as [name, state] (name)}
            <tr
              ><td>{state.component ?? name}</td><td>{state.state ?? 'Unknown'}</td><td
                >{state.reason ?? 'None reported'}</td
              ><td>{state.transitions ?? 'Unknown'}</td></tr
            >
          {/each}
        </tbody>
      </table>
    </div>
  {:else}
    <p class="empty-results">Lifecycle state is not available from the service.</p>
  {/if}
</section>

<section class="document-section">
  <h2>Readiness</h2>
  {#if readiness === null}
    <p class="empty-results">The readiness probe is unavailable. Provider state is unknown.</p>
  {:else}
    <div class="table-scroll">
      <table>
        <thead><tr><th>Component</th><th>State</th><th>Detail</th></tr></thead>
        <tbody>
          <tr
            ><td>Service</td><td>{readiness.service?.ready ? 'Ready' : 'Not ready'}</td><td
              >Liveness is separate from provider readiness.</td
            ></tr
          >
          <tr
            ><td>Source</td><td>{readiness.source?.ready ? 'Ready' : 'Not ready'}</td><td
              >{readiness.source?.kind ?? 'Unknown'}</td
            ></tr
          >
          <tr
            ><td>Inference provider</td><td>{readiness.inference?.ready ? 'Ready' : 'Not ready'}</td
            ><td
              >{readiness.inference?.provider ?? 'Unknown'}{readiness.inference?.reason
                ? `, ${readiness.inference.reason}`
                : ''}</td
            ></tr
          >
        </tbody>
      </table>
    </div>
    <p class="section-note">
      A provider failure does not stop telemetry or existing views. The readiness probe sends no
      prompt and no secret.
    </p>
  {/if}
  <button class="small-button" onclick={onrecheck}>Recheck readiness</button>
</section>

<section class="document-section">
  <h2>Session</h2>
  {#if session === null}
    <p class="empty-results">Session information is unavailable from the service.</p>
  {:else}
    <div class="table-scroll">
      <table>
        <thead><tr><th>Property</th><th>Value</th></tr></thead>
        <tbody>
          <tr><td>Role</td><td>{session.role}</td></tr>
          <tr><td>Actor</td><td><code>{session.actor}</code></td></tr>
          <tr><td>Authentication</td><td>{session.authentication}</td></tr>
          <tr><td>Capabilities</td><td>{session.capabilities.join(', ') || 'None issued'}</td></tr>
          <tr><td>Context generation</td><td>{session.context_generation}</td></tr>
        </tbody>
      </table>
    </div>
  {/if}
</section>

<section class="document-section">
  <h2>Recent failures</h2>
  {#if errorLog.length}
    <ul class="log-list">
      {#each errorLog as entry (entry.time + entry.code)}
        <li>
          <span class="inline-tag">{entry.code}</span>
          {entry.message}
          <small>{entry.time}</small>
        </li>
      {/each}
    </ul>
  {:else}
    <p class="empty-results">No failed request in this session.</p>
  {/if}
</section>

<section class="document-section">
  <h2>Endpoint reference</h2>
  <p class="section-note">
    The workbench uses only the accepted /api/v1 contract. Mutations require JSON and are rejected
    cross-origin. The provider path never falls back to rules after a failure.
  </p>
  <div class="table-scroll">
    <table>
      <thead><tr><th>Endpoint</th><th>Used for</th></tr></thead>
      <tbody>
        {#each endpoints as endpoint (endpoint.route)}
          <tr><td><code>{endpoint.route}</code></td><td>{endpoint.use}</td></tr>
        {/each}
      </tbody>
    </table>
  </div>
</section>
