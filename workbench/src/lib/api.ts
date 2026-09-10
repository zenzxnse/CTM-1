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
};
export type Task = {
  kind: string;
  anchor_asset_id?: string;
  model_revision: number;
  model_id?: string;
  original_request: string;
};
export type Issue = { code: string; message: string; severity?: string };
export type ViewComponent = {
  id: string;
  kind: 'gauge' | 'value' | 'status' | 'alarm';
  label: string;
  asset_id: string;
  tag_id: string;
  role: string;
  unit: string;
  reason: string;
  alarm_id?: string;
  status?: string;
  relationship_id?: string;
};
export type View = {
  view_id: string;
  model_revision: number;
  model_id?: string;
  status: string;
  session_id?: string;
  context_generation?: number;
  title: string;
  task: Task;
  components: ViewComponent[];
  issues: Issue[];
  dependencies: Record<string, unknown>[];
  changes?: { kind: string; message: string; tag_id?: string }[];
};
export type Telemetry = {
  model_revision: number;
  model_id?: string;
  tick: number;
  quality: string;
  session_id?: string;
  context_generation?: number;
  values: Record<string, { value: number | string | boolean; quality: string; unit: string }>;
  alarms: Alarm[];
};
export type Health = {
  version: string;
  interpreter: string;
  llama_configured: boolean;
  mode: string;
  session_id?: string;
  context_generation?: number;
  active_scenario?: string;
  simulation_running?: boolean;
};
export type Scenario = { id: string; name: string; description: string; revision?: number };
export type Interpretation = {
  status: string;
  interpreter: string;
  task?: Task;
  view?: View;
  message?: string;
  candidates?: (string | { id?: string; asset_id?: string; name: string })[];
};

/** Requests use the local native service. A failed provider call stays a failed request. */
export async function api<T>(path: string, body?: unknown): Promise<T> {
  const response = await fetch(`/api/v1/${path}`, {
    method: body === undefined ? 'GET' : 'POST',
    headers: body === undefined ? {} : { 'Content-Type': 'application/json' },
    body: body === undefined ? undefined : JSON.stringify(body),
    signal: AbortSignal.timeout(path === 'interpret' ? 35_000 : 8_000)
  });
  const result = await response.json();
  if (!response.ok)
    throw new Error(result?.error?.message ?? `Request failed (${response.status})`);
  return result as T;
}

export function displayValue(value: number | string | boolean | undefined): string {
  if (value === undefined) return 'Unknown';
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
