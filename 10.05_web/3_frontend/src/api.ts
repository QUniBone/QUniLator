// Typed REST layer for /api. Every mutation toasts its outcome and refreshes
// the store the views read.
import { setStore, emit } from './store';
import { toast } from './lib/toast';
import { liveModel, replayEventValues } from './lib/devmodel';
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
// GET /api/configs → {current, default, modified, configs[]}. The current /
// default pointers and the live modified flag come straight from the backend;
// the /ws/events "config" event keeps them fresh between loads.
export async function loadConfigs(): Promise<void> {
  const r = await fetch('/api/configs');
  if (!r.ok) throw new Error('configs fetch failed');
  const body = await r.json();
  const configs: ConfigSummary[] = Array.isArray(body) ? body : body.configs || [];
  setStore({
    configs,
    configCurrent: body.current || '',
    configDefault: body.default || '',
    configModified: 'modified' in body ? body.modified : null,
  });
}

// The stored document of a named configuration.
export async function fetchConfigSnapshot(name: string): Promise<ConfigSnapshot | null> {
  return fetch('/api/configs/' + encodeURIComponent(name), { signal: AbortSignal.timeout(4000) })
    .then((x) => (x.ok ? (x.json() as Promise<ConfigSnapshot>) : null))
    .catch(() => null);
}

// The live setup in stored-snapshot shape (GET /api/configs?current=1). Answers
// 503 while the machine is busy, so a null here means "try again".
export async function fetchLiveSnapshot(): Promise<ConfigSnapshot | null> {
  return fetch('/api/configs?current=1', { signal: AbortSignal.timeout(4000) })
    .then((x) => (x.ok ? (x.json() as Promise<ConfigSnapshot>) : null))
    .catch(() => null);
}

// Save / Save As: write the live setup under <name> with ?from=live, which
// makes <name> the current configuration and clears modified.
export async function saveConfigFromLive(name: string): Promise<boolean> {
  const snap = await fetchLiveSnapshot();
  if (!snap) {
    toast('PUT /api/configs/' + name + '?from=live', 'the machine is busy — try again');
    return false;
  }
  const res = await apiJSON<{ error?: string }>(
    '/api/configs/' + encodeURIComponent(name) + '?from=live',
    { method: 'PUT', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(snap) }
  );
  toast('PUT /api/configs/' + name + '?from=live', res.ok ? 'saved from the live setup' : res.data.error || 'save failed');
  await loadConfigs().catch(() => {});
  return res.ok;
}

// Stored edit: write an offline-edited document to <name>. The running machine
// and the current pointer are left alone.
export async function saveConfigDoc(name: string, doc: ConfigSnapshot): Promise<boolean> {
  const res = await apiJSON<{ error?: string }>('/api/configs/' + encodeURIComponent(name), {
    method: 'PUT',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(doc),
  });
  toast('PUT /api/configs/' + name, res.ok ? 'stored configuration saved' : res.data.error || 'save rejected');
  await loadConfigs().catch(() => {});
  return res.ok;
}

// Apply / Revert: restore <name> onto the machine and make it current.
export async function applyConfig(name: string): Promise<boolean> {
  const res = await apiJSON<{ ok?: boolean; errors?: string[] }>(
    '/api/configs/' + encodeURIComponent(name) + '/apply',
    { method: 'POST' }
  );
  toast(
    'POST /api/configs/' + name + '/apply',
    res.ok && res.data.ok
      ? 'configuration applied'
      : 'applied with rejections: ' + ((res.data.errors || []).join('; ') || 'request failed')
  );
  await refreshDevices().catch(() => {});
  await loadConfigs().catch(() => {});
  return res.ok;
}

export async function renameConfig(name: string, newName: string): Promise<boolean> {
  const res = await apiJSON<{ error?: string }>('/api/configs/' + encodeURIComponent(name) + '/rename', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ name: newName }),
  });
  toast('POST /api/configs/' + name + '/rename', res.ok ? 'renamed to ' + newName : res.data.error || 'rename rejected');
  await loadConfigs().catch(() => {});
  return res.ok;
}

export async function setDefaultConfig(name: string): Promise<boolean> {
  const res = await apiJSON<{ error?: string }>('/api/configs/' + encodeURIComponent(name) + '/default', {
    method: 'PUT',
  });
  toast('PUT /api/configs/' + name + '/default', res.ok ? 'set as startup default' : res.data.error || 'rejected');
  await loadConfigs().catch(() => {});
  return res.ok;
}

export async function deleteConfig(name: string): Promise<boolean> {
  const res = await apiJSON<{ error?: string }>('/api/configs/' + encodeURIComponent(name), {
    method: 'DELETE',
  });
  toast('DELETE /api/configs/' + name, res.ok ? 'configuration deleted' : res.data.error || 'delete refused');
  await loadConfigs().catch(() => {});
  return res.ok;
}

// Assign an image to one drive of a stored configuration, in the file, without
// disturbing the running machine (PUT …/devices/<device>/image).
export async function setStoredImage(name: string, drive: string, image: string): Promise<boolean> {
  const res = await apiJSON<{ error?: string }>(
    '/api/configs/' + encodeURIComponent(name) + '/devices/' + encodeURIComponent(drive) + '/image',
    { method: 'PUT', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ value: image }) }
  );
  toast(name + ':' + drive + ' = ' + (image || '""'), res.ok ? 'configuration image set' : res.data.error || 'rejected');
  await loadConfigs().catch(() => {});
  return res.ok;
}
