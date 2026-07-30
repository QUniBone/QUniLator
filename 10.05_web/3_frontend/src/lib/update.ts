// The self-update, from the browser's side.
//
// The hard part is that the server being replaced is the one serving this page.
// So an install raises an overlay that expects to lose its connection, then polls
// GET /api/version until the board answers with the version it asked for, and
// reloads onto the matching bundle. A flag in sessionStorage survives a manual
// reload during that window, so the page comes back showing an update in progress
// rather than a connection error.
import { store, setStore } from '../store';
import { toast } from './toast';
import { esc } from './util';
import { fetchVersion, bundleVersion } from './version';
import type { UpdateStatus } from '../types';

// what the page was doing when the server went away
interface InstallFlag {
  from: string;
  to: string;
  started: number;
}
const FLAG_KEY = 'qunilator.installing';

function readFlag(): InstallFlag | null {
  try {
    const raw = sessionStorage.getItem(FLAG_KEY);
    if (!raw) return null;
    const f = JSON.parse(raw) as InstallFlag;
    return typeof f.to === 'string' ? f : null;
  } catch {
    return null;
  }
}

function writeFlag(f: InstallFlag | null): void {
  try {
    if (f) sessionStorage.setItem(FLAG_KEY, JSON.stringify(f));
    else sessionStorage.removeItem(FLAG_KEY);
  } catch {
    /* private mode: the overlay still runs, it just does not survive a reload */
  }
}

// ---- REST ----

export async function refreshUpdate(): Promise<UpdateStatus | null> {
  try {
    const r = await fetch('/api/update', { cache: 'no-store' });
    if (!r.ok) return null;
    const u = (await r.json()) as UpdateStatus;
    setStore({ update: u });
    return u;
  } catch {
    return null;
  }
}

async function post(
  path: string,
  body?: Record<string, unknown>
): Promise<{ ok: boolean; error?: string }> {
  const init: RequestInit = { method: 'POST' };
  if (body) {
    init.headers = { 'Content-Type': 'application/json' };
    init.body = JSON.stringify(body);
  }
  try {
    const r = await fetch(path, init);
    const data = (await r.json().catch(() => ({}))) as { error?: string };
    return { ok: r.ok, error: data.error };
  } catch {
    return { ok: false, error: 'network error' };
  }
}

export async function checkForUpdate(): Promise<void> {
  const res = await post('/api/update/check');
  toast('POST /api/update/check', res.ok ? 'checking…' : res.error || 'the check was refused');
}

export async function fetchChangelog(): Promise<string> {
  try {
    const r = await fetch('/api/update/changelog', { cache: 'no-store' });
    const d = (await r.json().catch(() => ({}))) as { changelog?: string; error?: string };
    if (!r.ok) return '';
    return d.changelog || '';
  } catch {
    return '';
  }
}

export async function dismissUpdate(version: string): Promise<void> {
  const res = await post('/api/update/dismiss', { version });
  toast(
    'POST /api/update/dismiss',
    res.ok
      ? version
        ? version + ' will not be announced again'
        : 'dismissal cleared'
      : res.error || 'rejected'
  );
  await refreshUpdate();
}

export async function upgradeOs(): Promise<void> {
  const res = await post('/api/update/os');
  toast(
    'POST /api/update/os',
    res.ok ? 'upgrading the operating system…' : res.error || 'the upgrade was refused'
  );
  await refreshUpdate();
}

// ---- the install, and riding out the restart ----

// An update is announced when the repository offers a newer version than the
// board runs and that version has not been dismissed.
export function updateAvailable(u: UpdateStatus | null): boolean {
  if (!u || !u.source_configured || !u.candidate) return false;
  if (u.state === 'ahead') return false;
  if (u.candidate === u.installed) return false;
  return u.candidate !== u.dismissed;
}

export function updateRunning(u: UpdateStatus | null): boolean {
  if (!u) return false;
  return (
    u.state === 'downloading' ||
    u.state === 'installing' ||
    u.state === 'verifying' ||
    u.state === 'os-upgrading'
  );
}

const PHASE_TEXT: Record<string, string> = {
  downloading: 'Fetching the package…',
  installing: 'Installing — the emulated machine is stopping.',
  verifying: 'Waiting for the new version to come up…',
  'os-upgrading': 'Upgrading the operating system…',
  'rolled-back': 'The update was rolled back.',
  failed: 'The update failed.',
  done: 'Installed.',
};

// The modal that stays up across the restart. Built imperatively rather than as a
// component: it has to outlive the store, the socket and any re-render, and it
// must not depend on the server it is waiting for.
function installOverlay(from: string, to: string): {
  phase: (state: string, detail?: string) => void;
  fail: (title: string, detail: string, journal: string[]) => void;
} {
  const host = document.createElement('div');
  host.className = 'modal-overlay';
  host.innerHTML =
    '<div class="card modal-card update-overlay">' +
    '<div class="card-head"><h3>Updating ' +
    esc(from) +
    ' → ' +
    esc(to) +
    '</h3></div>' +
    '<div class="card-body">' +
    '<p class="upd-phase">Starting the install…</p>' +
    '<div class="upd-bar"><div class="upd-bar-fill"></div></div>' +
    '<p class="muted upd-detail">The interface reconnects on its own, in about half a minute. ' +
    'Leave this page open.</p>' +
    '<pre class="upd-journal" hidden></pre>' +
    '<div class="upd-actions" hidden><button class="btn primary" data-upd-reload>Reload</button></div>' +
    '</div></div>';
  document.body.appendChild(host);
  const q = (sel: string) => host.querySelector(sel) as HTMLElement;
  host.addEventListener('click', (e) => {
    if ((e.target as HTMLElement).closest('[data-upd-reload]')) location.reload();
  });
  return {
    phase(state: string, detail?: string) {
      q('.upd-phase').textContent = PHASE_TEXT[state] || 'Working…';
      if (detail) q('.upd-detail').textContent = detail;
    },
    fail(title: string, detail: string, journal: string[]) {
      q('.upd-phase').textContent = title;
      q('.upd-phase').classList.add('err');
      q('.upd-bar').setAttribute('hidden', '');
      q('.upd-detail').textContent = detail;
      if (journal.length) {
        const pre = q('.upd-journal');
        pre.textContent = journal.join('\n');
        pre.removeAttribute('hidden');
      }
      q('.upd-actions').removeAttribute('hidden');
    },
  };
}

const POLL_LIMIT_MS = 120000;

// Wait for the board to come back on `to`, then reload. A connection refusal is
// "not back yet", not a fault: the server is being replaced.
async function awaitNewVersion(
  to: string,
  ui: ReturnType<typeof installOverlay>
): Promise<void> {
  const deadline = Date.now() + POLL_LIMIT_MS;
  for (;;) {
    await new Promise((r) => setTimeout(r, 1000));
    const v = await fetchVersion();
    if (v && v.version === to) {
      writeFlag(null);
      location.reload();
      return;
    }
    if (v) {
      // The server answers, on a version that is not the target: either the
      // install has not restarted it yet, or it failed and was rolled back.
      const u = await fetch('/api/update', { cache: 'no-store' })
        .then((r) => (r.ok ? (r.json() as Promise<UpdateStatus>) : null))
        .catch(() => null);
      if (u && (u.state === 'failed' || u.state === 'rolled-back')) {
        writeFlag(null);
        // the updater's message is a fragment, not a sentence, so it is
        // punctuated here rather than assumed to end in a full stop
        const why = u.error ? u.error.replace(/[.;]\s*$/, '') + '. ' : '';
        ui.fail(
          u.state === 'rolled-back' ? 'The update was rolled back' : 'The update failed',
          why +
            'The board is running ' +
            v.version +
            (u.state === 'rolled-back' ? ' again.' : '.'),
          u.journal || []
        );
        return;
      }
      if (u) ui.phase(u.state);
    }
    if (Date.now() > deadline) {
      writeFlag(null);
      ui.fail(
        'The board has not come back',
        'It has been two minutes. On the board, "systemctl status ' +
          (store.update?.package || 'qbone') +
          '" says whether the service is running, and ' +
          '"journalctl -u qunilator-update" says what the install did.',
        []
      );
      return;
    }
  }
}

// Start an install and follow it through the restart. Returns once the outcome is
// known — normally by reloading the page.
export async function installUpdate(to: string): Promise<void> {
  const from = store.update?.installed || bundleVersion;
  const res = await post('/api/update/install', { version: to });
  if (!res.ok) {
    toast('POST /api/update/install', res.error || 'the install was refused');
    return;
  }
  writeFlag({ from, to, started: Date.now() });
  const ui = installOverlay(from, to);
  await awaitNewVersion(to, ui);
}

// Called once at startup: a page loaded (or reloaded) during an install window
// picks the overlay back up instead of showing a connection error.
export function resumeInstallIfPending(): void {
  const f = readFlag();
  if (!f) return;
  if (Date.now() - f.started > POLL_LIMIT_MS * 2) {
    writeFlag(null);
    return;
  }
  if (f.to === bundleVersion) {
    // this page already is the new version: the install finished
    writeFlag(null);
    return;
  }
  const ui = installOverlay(f.from, f.to);
  awaitNewVersion(f.to, ui).catch(() => {});
}
