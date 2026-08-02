import { html } from '../html';
import { useEffect } from 'preact/hooks';
import { Router, useLocation } from 'preact-iso';
import { useStore } from '../store';
import type { HwState, Settings, UpdateStatus } from '../types';
import { Led } from './common';
import { guardedRoute } from '../lib/navguard';
import { Dashboard } from './Dashboard';
import { StoragePage } from './Storage';
import { ConfigsPage } from './Configs';
import { MachinePage } from './Machine';
import { LogPage } from './Log';
import { SystemPage } from './System';
import { updateAvailable, updateRunning } from '../lib/update';

// path → sidebar label. The active item is the one whose path is the current
// path or a prefix of it (so /config/211bsd still lights "Configurations").
const NAV: [string, string][] = [
  ['/dashboard', 'Dashboard'],
  ['/storage', 'Storage'],
  ['/config', 'Configurations'],
  ['/machine', 'Machine'],
  ['/diagnostics', 'Diagnostics'],
  ['/system', 'System'],
];

function activeNav(path: string): string {
  const hit = NAV.find(([p]) => path === p || path.startsWith(p + '/'));
  return hit ? hit[0] : '/dashboard';
}

// the backend's platform string, cased for the wordmark
const BUS_LABEL: Record<string, string> = { QBUS: 'QBus', UNIBUS: 'Unibus', HOST: 'Host' };

function Sidebar({ active }: { active: string }) {
  const loc = useLocation();
  const s = useStore();
  const bus = BUS_LABEL[s.platform] || s.platform || '';
  const offered = updateAvailable(s.update);
  return html`<aside class="sidebar">
    <div class="wordmark"><div class="mark">Q</div>
      <div><span class="name">QUniLator</span><span class="sub">${bus}</span></div></div>
    ${
      offered
        ? html`<button class="upd-badge" onClick=${() => guardedRoute(loc, '/system')}
            title="a newer ${s.update!.package} package is published">Update ${s.update!.candidate}</button>`
        : null
    }
    <nav class="nav">${NAV.map(
      ([path, label]) => html`
      <button class=${active === path ? 'active' : ''} onClick=${() => guardedRoute(loc, path)}><span>${label}</span>${
        label === 'System' && offered ? html`<span class="nav-dot" aria-label="update available"></span>` : null
      }</button>`
    )}</nav>
  </aside>`;
}

function Topbar({
  title,
  hw,
  settings,
  connected,
  update,
  onUpdateClick,
}: {
  title: string;
  hw: HwState;
  settings: Settings;
  connected: boolean;
  update: UpdateStatus | null;
  onUpdateClick: () => void;
}) {
  return html`<header class="topbar"><h1>${title}</h1>
    ${
      updateRunning(update)
        ? html`<button class="pill upd-pill busy" onClick=${onUpdateClick}
            title="an update is running">${update!.state}</button>`
        : updateAvailable(update)
          ? html`<button class="pill upd-pill" onClick=${onUpdateClick}
              title="a newer ${update!.package} package is published">Update ${update!.candidate}</button>`
          : null
    }
    <span class="pill">${html`<${Led} on=${hw.dcok} green=${true} title="DCOK" />`}DCOK</span>
    <span class="pill">${html`<${Led} on=${hw.pok} green=${true} title="POK" />`}POK</span>
    <span class="pill mono">addr ${settings.address_width}-bit</span>
    <span class="pill">${html`<${Led} on=${connected} green=${true} title="link" />`}${
      connected ? 'connected' : 'disconnected'
    }</span></header>`;
}

/**
 * The board taken for work no page may act during: the checks a power-up runs
 * before it drives the bus, or the interactive menu having the hardware.
 *
 * Every connected page raises this at once, because the reason travels in the
 * state frame and a page that connects mid-operation starts from a snapshot
 * carrying it. The board refuses anything that would change the machine while
 * it is held, so the lock is what the operator sees instead of a screenful of
 * buttons that answer 409. It clears when the board says it is free — there is
 * nothing to dismiss, and a stuck one is a board that never let go.
 *
 * The reason is the whole message: the board names what holds it and what ends
 * the wait, which differs by holder — a power-up finishes on its own, an
 * interactive session ends when whoever started it exits. A line of our own
 * here could only be true for one of them.
 */
function BoardHeld({ reason }: { reason: string }) {
  if (!reason) return null;
  return html`<div class="modal-overlay held-overlay" role="alertdialog" aria-live="assertive">
    <div class="card modal-card held-card">
      <div class="held-spinner" aria-hidden="true"></div>
      <p class="held-reason">${reason}</p>
    </div>
  </div>`;
}

function Redirect({ to }: { to: string }) {
  const loc = useLocation();
  useEffect(() => {
    loc.route(to, true);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);
  return null;
}

export function App() {
  const s = useStore();
  const loc = useLocation();
  const active = activeNav(loc.path);
  const title = (NAV.find(([p]) => p === active) || [, 'Dashboard'])[1] as string;
  return html`<div class="app">
    <${Sidebar} active=${active} />
    <div class="main">
      <${Topbar} title=${title} hw=${s.hw} settings=${s.settings} connected=${s.connected}
        update=${s.update} onUpdateClick=${() => guardedRoute(loc, '/system')} />
      <main class="content">
        <${Router}>
          <${Redirect} path="/" to="/dashboard" />
          <${Dashboard} path="/dashboard" />
          <${StoragePage} path="/storage/:path*" />
          <${ConfigsPage} path="/config/:name?/:device?" />
          <${MachinePage} path="/machine" />
          <${LogPage} path="/diagnostics" />
          <${SystemPage} path="/system" />
          <${Redirect} default to="/dashboard" />
        </${Router}>
      </main>
    </div>
    <${BoardHeld} reason=${s.heldBy} />
  </div>`;
}
