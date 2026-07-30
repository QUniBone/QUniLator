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
    </div></div>`;
}
