// Typed REST layer for /api. Every mutation toasts its outcome and refreshes
// the store the views read.
import { setStore, emit } from './store';
import { toast } from './lib/toast';
import { liveModel, replayEventValues, walkDevs } from './lib/devmodel';
import { updateConsoleSource } from './lib/terminals';
import type {
  ApiDevice,
  Settings,
  ImageInfo,
  ConfigSummary,
  ConfigSnapshot,
} from './types';

export interface ApiResult<T = Record<string, unknown>> {
  ok: boolean;
  data: T;
}

export async function apiJSON<T = Record<string, unknown>>(
  url: string,
  opts?: RequestInit
): Promise<ApiResult<T>> {
  const r = await fetch(url, opts);
  const data = (await r.json().catch(() => ({}))) as T;
  return { ok: r.ok, data };
}

export function apiSetParam(dev: string, param: string, value: string) {
  return apiJSON<{ error?: string }>(
    '/api/devices/' + encodeURIComponent(dev) + '/params/' + encodeURIComponent(param),
    {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ value: String(value) }),
    }
  );
}

export async function refreshDevices(): Promise<void> {
  const r = await fetch('/api/devices');
  if (!r.ok) throw new Error('devices fetch failed');
  setStore({ devmodel: liveModel((await r.json()) as ApiDevice[]) });
  replayEventValues();
  emit();
}

export async function liveSetParam(
  dev: string,
  param: string,
  value: string,
  okMsg: string
): Promise<void> {
  const res = await apiSetParam(dev, param, value);
  toast(dev + '.' + param + ' = ' + (value === '' ? '""' : value), res.ok ? okMsg : res.data.error || 'rejected');
  refreshDevices().catch(() => {});
}

export async function liveControl(action: string, okMsg: string): Promise<void> {
  const res = await apiJSON<{ error?: string }>('/api/control', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ action }),
  });
  toast('control: ' + action, res.ok ? okMsg : res.data.error || 'rejected');
}

export async function refreshSettings(): Promise<void> {
  const r = await fetch('/api/settings');
  if (!r.ok) throw new Error('settings fetch failed');
  setStore({ settings: (await r.json()) as Settings });
  updateConsoleSource();
}

export async function putSettings(
  patch: Record<string, unknown>,
  okMsg: string
): Promise<ApiResult<{ error?: string; warnings?: string[] }>> {
  const res = await apiJSON<{ error?: string; warnings?: string[] }>('/api/settings', {
    method: 'PUT',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(patch),
  });
  const warns = (res.data.warnings || []).join('; ');
  toast('PUT /api/settings', warns || (res.ok ? okMsg : res.data.error || 'rejected'));
  await refreshSettings().catch(() => {});
  return res;
}

export async function refreshImages(): Promise<void> {
  const r = await fetch('/api/images');
  if (!r.ok) throw new Error('images fetch failed');
  setStore({ images: (await r.json()) as ImageInfo[] });
}

// ---- configurations ----
function cfgKey(snap: ConfigSnapshot | null | undefined): string {
  return ((snap || { devices: [] }).devices || [])
    .filter((d) => d.enabled)
    .map((d) => {
      const p = d.params || {};
      return d.name + '{' + Object.keys(p).sort().map((k) => k + '=' + p[k]).join(',') + '}';
    })
    .sort()
    .join('|');
}

export function cfgDriveNames(): string[] {
  const out: string[] = [];
  walkDevs((d) => {
    if (d.params && d.params.some((p) => p.n === 'image')) out.push(d.name);
  });
  return out;
}

export async function loadConfigs(): Promise<void> {
  const r = await fetch('/api/configs');
  if (!r.ok) throw new Error('configs fetch failed');
  const body = await r.json();
  // the endpoint returns either a bare array (older form) or an object with a
  // "configs" list; accept both
  const configs: ConfigSummary[] = Array.isArray(body) ? body : body.configs || [];
  let current: ConfigSnapshot | null = null;
  try {
    const cr = await fetch('/api/configs?current=1', { signal: AbortSignal.timeout(4000) });
    if (cr.ok) current = (await cr.json()) as ConfigSnapshot;
  } catch {
    /* machine busy: leave current null */
  }
  const currentKey = current && cfgKey(current);
  await Promise.all(
    configs.map(async (c) => {
      c.snapshot = await fetch('/api/configs/' + encodeURIComponent(c.name), {
        signal: AbortSignal.timeout(4000),
      })
        .then((x) => x.json())
        .catch(() => null);
      c.loaded = current != null && c.snapshot != null && cfgKey(c.snapshot) === currentKey;
    })
  );
  setStore({ configs });
}
