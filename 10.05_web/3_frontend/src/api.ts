// Typed REST layer for /api. Every mutation toasts its outcome and refreshes
// the store the views read.
import { setStore, emit } from './store';
import { toast } from './lib/toast';
import { subURL } from './lib/util';
import { liveModel, replayEventValues } from './lib/devmodel';
import { updateConsoleSource } from './lib/terminals';
import type {
  ApiDevice,
  Settings,
  ImageListing,
  ImageContents,
  ConfigSummary,
  ConfigSnapshot,
  LogLine,
  LogLevelName,
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

// ---- diagnostics log ----
const LOG_LEVELS: Record<number, LogLevelName> = {
  1: 'FATAL',
  2: 'ERROR',
  3: 'WARNING',
  4: 'INFO',
  5: 'DEBUG',
};

// A page of the persisted log journal, newest-first, for the diagnostics view.
// `before` pages older entries (omit for the latest). Returns the entries as
// received (newest-first) and whether older entries remain.
export async function fetchLogPage(
  before?: number,
  limit = 200
): Promise<{ entries: LogLine[]; more: boolean }> {
  const q = new URLSearchParams();
  if (before) q.set('before', String(before));
  q.set('limit', String(limit));
  const r = await fetch('/api/log?' + q.toString()).catch(() => null);
  if (!r || !r.ok) return { entries: [], more: false };
  const d = await r.json().catch(() => ({}));
  const entries: LogLine[] = (d.entries || []).map(
    (e: { id: number; time: string; level: number; label: string; text: string }) => ({
      id: e.id,
      t: e.time,
      lvl: LOG_LEVELS[e.level] || 'INFO',
      src: e.label,
      msg: String(e.text).replace(/^\[[^\]]*\]\s*/, ''),
    })
  );
  return { entries, more: !!d.more };
}

export async function refreshImages(): Promise<void> {
  const r = await fetch('/api/images');
  if (!r.ok) throw new Error('images fetch failed');
  const body = (await r.json()) as ImageListing;
  setStore({ images: body.images || [], dirs: body.dirs || [] });
}

// Upload one or more files into `dir` (a folder subpath, "" for the root). The
// backend reads the `dir` field first, so it is appended before the files.
// onProgress reports overall bytes 0..1 during the transfer.
export function uploadImages(
  files: File[],
  dir: string,
  onProgress?: (frac: number) => void
): Promise<{ ok: boolean; names: string[]; error?: string }> {
  return new Promise((resolve) => {
    const form = new FormData();
    files.forEach((f) => form.append('file', f, f.name));
    const xhr = new XMLHttpRequest();
    // target folder is a query parameter so the server knows it before parsing
    // the body, independent of multipart field order
    const q = dir ? '?dir=' + encodeURIComponent(dir) : '';
    xhr.open('POST', '/api/images' + q);
    xhr.upload.onprogress = (ev) => {
      if (onProgress && ev.lengthComputable) onProgress(ev.loaded / ev.total);
    };
    xhr.onload = () => {
      let data: { names?: string[]; error?: string } = {};
      try {
        data = JSON.parse(xhr.responseText);
      } catch {
        /* ignore */
      }
      const ok = xhr.status >= 200 && xhr.status < 300;
      const names = data.names || files.map((f) => f.name);
      toast('POST /api/images', ok ? names.join(', ') + ' uploaded' : data.error || 'upload failed');
      refreshImages().catch(() => {});
      resolve({ ok, names, error: data.error });
    };
    xhr.onerror = () => {
      toast('POST /api/images', 'upload failed');
      resolve({ ok: false, names: [], error: 'network error' });
    };
    xhr.send(form);
  });
}

// Read the file listing inside a disk/tape image (read-only). Returns the
// parsed JSON on success, or an {filesystem:'unknown', error} object the caller
// renders as the friendly note. The subpath is encoded per-segment (keeping the
// slashes) — never double-encoded.
export async function imageContents(subpath: string): Promise<ImageContents> {
  try {
    const r = await fetch(subURL('/api/images/', subpath) + '/contents');
    const data = (await r.json().catch(() => ({}))) as ImageContents;
    if (!r.ok)
      return { filesystem: 'unknown', error: (data as { error?: string }).error || 'read failed' };
    return data;
  } catch {
    return { filesystem: 'unknown', error: 'network error' };
  }
}

// Delete an image file by its subpath. Refused (409) while attached or used.
export async function deleteImage(subpath: string): Promise<boolean> {
  const res = await apiJSON<{ error?: string }>(subURL('/api/images/', subpath), { method: 'DELETE' });
  toast('DELETE /api/images/' + subpath, res.ok ? 'image deleted' : res.data.error || 'delete failed');
  await refreshImages().catch(() => {});
  return res.ok;
}

// Create a folder at a subpath.
export async function createFolder(path: string): Promise<boolean> {
  const res = await apiJSON<{ error?: string }>('/api/folders', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ path }),
  });
  toast('POST /api/folders ' + path, res.ok ? 'folder created' : res.data.error || 'create failed');
  await refreshImages().catch(() => {});
  return res.ok;
}

// Remove an empty folder by its subpath. Refused (409) when not empty.
export async function deleteFolder(path: string): Promise<boolean> {
  const res = await apiJSON<{ error?: string }>(subURL('/api/folders/', path), { method: 'DELETE' });
  toast('DELETE /api/folders/' + path, res.ok ? 'folder removed' : res.data.error || 'remove failed');
  await refreshImages().catch(() => {});
  return res.ok;
}

// Rename or move a file or folder (subpaths). Refused (409) when the source is
// attached/used or the target exists.
export async function moveImage(from: string, to: string): Promise<boolean> {
  const res = await apiJSON<{ error?: string }>('/api/move', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ from, to }),
  });
  toast('POST /api/move ' + from + ' → ' + to, res.ok ? 'moved' : res.data.error || 'move failed');
  await refreshImages().catch(() => {});
  return res.ok;
}

// ---- copy-on-write overlays ----
// The three overlay operations answer 409 while the machine runs (they need the
// disk quiescent) and 404 when the image has no active overlay. The status code
// is returned so the caller can surface the 409 case inline; other failures are
// toasted like the rest of the image mutations. A success refreshes the image
// list so the overlay readout updates.
export interface OverlayResult {
  ok: boolean;
  status: number;
  error?: string;
}

async function overlayOp(
  subpath: string,
  op: 'discard' | 'commit' | 'export',
  okMsg: string,
  body?: Record<string, unknown>
): Promise<OverlayResult> {
  const url = subURL('/api/images/', subpath) + '/overlay/' + op;
  const init: RequestInit = { method: 'POST' };
  if (body) {
    init.headers = { 'Content-Type': 'application/json' };
    init.body = JSON.stringify(body);
  }
  let r: Response;
  try {
    r = await fetch(url, init);
  } catch {
    toast('POST ' + url, 'network error');
    return { ok: false, status: 0, error: 'network error' };
  }
  const data = (await r.json().catch(() => ({}))) as { error?: string };
  if (r.ok) {
    toast('POST ' + url, okMsg);
    await refreshImages().catch(() => {});
  } else if (r.status !== 409) {
    // the 409 "machine must be halted" case is shown inline by the caller
    toast('POST ' + url, data.error || 'overlay operation failed');
  }
  return { ok: r.ok, status: r.status, error: data.error };
}

// Throw the overlay away and revert the disk to its pristine base.
export function discardOverlay(subpath: string): Promise<OverlayResult> {
  return overlayOp(subpath, 'discard', 'overlay discarded — reverted to base');
}

// Fold the overlay into the base image (permanent) and clear the overlay.
export function commitOverlay(subpath: string): Promise<OverlayResult> {
  return overlayOp(subpath, 'commit', 'overlay consolidated into the base image');
}

// Write a flattened base+overlay standalone image to `dest`, leaving both intact.
export function exportOverlay(subpath: string, dest: string): Promise<OverlayResult> {
  return overlayOp(subpath, 'export', 'flattened image written to ' + dest, { dest });
}

// ---- configurations ----
// GET /api/configs → {current, modified, configs[]}. The current pointer and
// the live modified flag come straight from the backend; the /ws/events
// "config" event keeps them fresh between loads. Each summary carries its
// dip_value, the DIP setting that selects it at power-on.
export async function loadConfigs(): Promise<void> {
  const r = await fetch('/api/configs');
  if (!r.ok) throw new Error('configs fetch failed');
  const body = await r.json();
  const configs: ConfigSummary[] = Array.isArray(body) ? body : body.configs || [];
  setStore({
    configs,
    configCurrent: body.current || '',
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

// Set a configuration's operator-friendly title (file metadata only; the
// running machine and the current pointer are untouched). An empty value
// clears it back to the name.
export async function setConfigTitle(name: string, title: string): Promise<boolean> {
  const res = await apiJSON<{ error?: string }>(
    '/api/configs/' + encodeURIComponent(name) + '/title',
    {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ value: title }),
    }
  );
  toast('PUT /api/configs/' + name + '/title', res.ok ? 'title set' : res.data.error || 'rejected');
  await loadConfigs().catch(() => {});
  return res.ok;
}

// Bind a configuration to a DIP-switch value (0..15), so the board loads it at
// power-on when the switches read that value; null clears the binding. At most
// one configuration may hold a value (the backend refuses a taken one with 409).
// Store the dashboard layout for a configuration (per-config metadata; the
// running machine is untouched). Pass null to clear it.
export async function setConfigLayout(name: string, layout: unknown): Promise<boolean> {
  const res = await apiJSON<{ error?: string }>(
    '/api/configs/' + encodeURIComponent(name) + '/layout',
    {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ value: layout }),
    }
  );
  if (!res.ok) toast('PUT /api/configs/' + name + '/layout', res.data.error || 'rejected');
  return res.ok;
}

export async function setConfigDip(name: string, dip: number | null): Promise<boolean> {
  const res = await apiJSON<{ error?: string }>(
    '/api/configs/' + encodeURIComponent(name) + '/dip',
    {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ value: dip }),
    }
  );
  toast(
    'PUT /api/configs/' + name + '/dip',
    res.ok ? (dip == null ? 'DIP binding cleared' : 'selected by DIP ' + dip) : res.data.error || 'rejected'
  );
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
