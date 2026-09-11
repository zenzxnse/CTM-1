export type SourceReference = {
  server: string;
  namespace_uri: string;
  identifier: string;
};
export type Asset = { id: string; name: string; kind: string; aliases: string[] };
export type Tag = {
  id: string;
  asset_id: string;
  name: string;
  role: string;
  data_type: string;
  unit: string;
  source: SourceReference;
};
export type Relationship = { id: string; from: string; to: string; kind: string };
export type Alarm = {
  id: string;
  name: string;
  asset_id: string;
  tag_id: string;
  severity: string;
  operator?: string;
  threshold?: number;
  active?: boolean;
};
export type EngineeringArtifact = {
  id: string;
  kind: string;
  format: string;
  path: string;
  bytes: number;
  records: number;
  fingerprint: string;
};
export type MachineModel = {
  model_id: string;
  revision: number;
  name: string;
  session_id?: string;
  context_generation?: number;
  assets: Asset[];
  tags: Tag[];
  relationships: Relationship[];
  alarms: Alarm[];
  metadata?: {
    provenance?: {
      schema?: string;
      manifest?: string;
      artifacts?: EngineeringArtifact[];
    };
  };
};
export type Task = {
  kind: string;
  anchor_asset_id?: string;
  measurement_roles?: string[];
  model_revision: number;
  model_id?: string;
  original_request: string;
};
export type Issue = { code: string; message: string; severity?: string };
export type ViewComponent = {
  id: string;
  kind: 'gauge' | 'value' | 'status' | 'alarm' | string;
  label: string;
  asset_id: string;
  tag_id: string;
  role: string;
  unit: string;
  reason: string;
  alarm_id?: string;
  status?: string;
  relationship_id?: string;
  relationship_kind?: string;
  relationship_from?: string;
  relationship_to?: string;
};
export type View = {
  view_id: string;
  model_revision: number;
  model_id?: string;
  status: string;
  session_id?: string;
  context_generation?: number;
  reconciled?: boolean;
  title: string;
  task: Task;
  components: ViewComponent[];
  issues: Issue[];
  dependencies: Record<string, unknown>[];
  changes?: { kind: string; message: string; tag_id?: string }[];
};
export type TelemetrySample = {
  value: number | string | boolean | null;
  quality: string;
  unit: string;
  timestamp_ms?: number;
  age_ms?: number | null;
};
export type TelemetryAlarm = {
  id: string;
  alarm_id: string;
  active: boolean;
  asset_id: string;
  tag_id: string;
  name: string;
  severity: string;
  value: number | null;
  threshold: number | null;
};
export type Telemetry = {
  schema_version?: string;
  model_revision: number;
  model_id?: string;
  source_id?: string;
  sequence?: number;
  quality: string;
  session_id?: string;
  context_generation?: number;
  values: Record<string, TelemetrySample>;
  alarms: TelemetryAlarm[];
};
export type Health = {
  version: string;
  interpreter: string;
  llama_configured: boolean;
  mode: string;
  session_id?: string;
  context_generation?: number;
  active_scenario?: string;
  lifecycle?: Record<
    string,
    { component?: string; state?: string; reason?: string; transitions?: number }
  >;
};
export type SessionInfo = {
  role: string;
  actor: string;
  capabilities: string[];
  session_id: string;
  context_generation: number;
  authentication: string;
};
export type ProviderReadiness = {
  provider: string;
  configured: boolean;
  ready: boolean;
  reason?: string;
};
export type Readiness = {
  ready: boolean;
  service: { ready: boolean };
  source: { kind: string; ready: boolean };
  inference: ProviderReadiness;
};
export type Scenario = {
  id: string;
  model_id?: string;
  revision?: number;
  name: string;
  description: string;
};
export type Interpretation = {
  status: string;
  interpreter: string;
  prompt?: string;
  message?: string;
  supported_tasks?: string[];
  candidates?: (
    string | { asset_id?: string; name?: string; kind?: string; score?: number; eligible?: boolean }
  )[];
  task?: Task;
  view?: View;
  execution?: ExecutionInfo;
};

export type CacheObservation = {
  state?: 'hit' | 'miss' | 'bypass' | string;
  hits?: number;
  misses?: number;
  evictions?: number;
  entries?: number;
};
export type ContextObservation = {
  bytes?: number;
  estimated_tokens?: number;
  assets?: number;
  relationships?: number;
  tags?: number;
  alarms?: number;
  truncated?: boolean;
  candidates?: number;
};
export type ProviderUsage = {
  reported?: boolean;
  provider_call?: boolean;
  prompt_tokens?: number;
  completion_tokens?: number;
  total_tokens?: number;
  cached_prompt_tokens?: number;
};
export type ExecutionInfo = {
  cache?: {
    interpretation?: CacheObservation;
    retrieval?: CacheObservation;
  };
  context?: ContextObservation;
  provider_usage?: ProviderUsage;
  elapsed_ms?: number;
  model_id?: string;
  model_revision?: number;
  context_generation?: number;
  stages?: Record<string, number>;
  errors?: Issue[];
};
export type ClientSurface = {
  width_px: number;
  height_px: number;
  size_class: 'small' | 'medium' | 'large';
};
export type InterpretationRequest = {
  prompt: string;
  interpreter?: string;
  client?: ClientSurface;
};

/**
 * A structured request failure. Codes come from the service error envelope, or are
 * `network` and `timeout` when the browser could not complete the request.
 */
export class ApiError extends Error {
  readonly code: string;
  readonly status: number;
  constructor(code: string, message: string, status: number) {
    super(message);
    this.name = 'ApiError';
    this.code = code;
    this.status = status;
  }
}

/** Requests use the local native service. A failed provider call stays a failed request. */
export async function api<T>(path: string, body?: unknown): Promise<T> {
  let response: Response;
  try {
    response = await fetch(`/api/v1/${path}`, {
      method: body === undefined ? 'GET' : 'POST',
      headers: body === undefined ? {} : { 'Content-Type': 'application/json' },
      body: body === undefined ? undefined : JSON.stringify(body),
      signal: AbortSignal.timeout(path === 'interpret' ? 35_000 : 8_000)
    });
  } catch (failure) {
    if (failure instanceof DOMException && failure.name === 'TimeoutError')
      throw new ApiError('timeout', 'The local service did not respond in time.', 0);
    throw new ApiError('network', 'The local service is not reachable.', 0);
  }
  let payload: unknown = null;
  try {
    payload = await response.json();
  } catch {
    /* a body that is not JSON is reported through the envelope fallback below */
  }
  if (!response.ok) {
    const envelope = payload as { error?: { code?: string; message?: string } } | null;
    throw new ApiError(
      envelope?.error?.code ?? 'http_error',
      envelope?.error?.message ?? `The request failed with HTTP ${response.status}.`,
      response.status
    );
  }
  return payload as T;
}

/**
 * Readiness may legitimately answer HTTP 503 while the service itself stays healthy,
 * so the status is not treated as a transport failure.
 */
export async function fetchReady(): Promise<Readiness> {
  let response: Response;
  try {
    response = await fetch('/api/v1/ready', { signal: AbortSignal.timeout(8_000) });
  } catch (failure) {
    if (failure instanceof DOMException && failure.name === 'TimeoutError')
      throw new ApiError('timeout', 'The readiness probe did not respond in time.', 0);
    throw new ApiError('network', 'The local service is not reachable.', 0);
  }
  try {
    return (await response.json()) as Readiness;
  } catch {
    throw new ApiError(
      'http_error',
      `The readiness probe failed with HTTP ${response.status}.`,
      response.status
    );
  }
}

export function displayValue(value: number | string | boolean | null | undefined): string {
  if (value === undefined || value === null) return 'Unknown';
  if (typeof value === 'boolean') return value ? 'On' : 'Off';
  if (typeof value === 'number') {
    if (!Number.isFinite(value)) return 'Unknown';
    return new Intl.NumberFormat('en', { maximumFractionDigits: 1 }).format(value);
  }
  return value;
}

export function humanize(value: string): string {
  return value.replaceAll('_', ' ').replaceAll('-', ' ');
}

export function severityRank(severity: string): number {
  const value = severity.toLowerCase();
  if (value === 'critical' || value === 'high') return 3;
  if (value === 'medium') return 2;
  if (value === 'low') return 1;
  return 0;
}

export function qualityLabel(quality: string): string {
  if (quality === 'good') return 'Good';
  if (quality === 'partial') return 'Partial';
  if (quality === 'stale') return 'Stale';
  if (quality === 'paused') return 'Paused';
  if (quality === 'bad') return 'Bad';
  return 'Unknown';
}

export function errorHint(code: string): string {
  if (code === 'network' || code === 'timeout')
    return 'The workbench could not reach the local service. Telemetry and requests are unavailable until it responds.';
  if (code === 'stale_context')
    return 'The active model or session changed while the service handled the request. Compose the view again against the current context.';
  if (code === 'lost_update')
    return 'The saved view changed on the service while it was being reconciled. Compose the view again.';
  if (code === 'unknown_view')
    return 'The service no longer holds this view. Compose the view again from the current model.';
  if (code === 'view_tampered')
    return 'The supplied view differs from the trusted server copy. Compose the view again.';
  if (code === 'provider_timeout' || code === 'provider_http' || code === 'provider_schema')
    return 'The selected interpreter provider failed. The service did not fall back to rules. Telemetry is unaffected.';
  if (code === 'provider_unconfigured')
    return 'The selected interpreter is not configured on the service. Use the rules interpreter.';
  if (code === 'inference_busy')
    return 'The service reached its concurrent interpretation limit. Wait for the current request to finish.';
  if (code === 'cross_origin')
    return 'The service rejected this request because it arrived from another origin.';
  if (code === 'events_busy')
    return 'The service reached its telemetry stream client limit. Close another workbench tab or reconnect later.';
  return 'The service reported a failure. The message above comes from the local service.';
}
