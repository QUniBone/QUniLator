import { html } from '../html';
import { useEffect } from 'preact/hooks';
import { Router, useLocation } from 'preact-iso';
import { useStore } from '../store';
import type { HwState, Settings } from '../types';
import { Led } from './common';
import { Dashboard } from './Dashboard';
import { StoragePage } from './Storage';
import { ConfigsPage } from './Configs';
import { MachinePage } from './Machine';
import { LogPage } from './Log';

// path → sidebar label. The active item is the one whose path is the current
// path or a prefix of it (so /config/211bsd still lights "Configurations").
const NAV: [string, string][] = [
  ['/dashboard', 'Dashboard'],
  ['/storage', 'Storage'],
  ['/config', 'Configurations'],
  ['/machine', 'Machine'],
  ['/diagnostics', 'Diagnostics'],
];

function activeNav(path: string): string {
  const hit = NAV.find(([p]) => path === p || path.startsWith(p + '/'));
  return hit ? hit[0] : '/dashboard';
}

function Sidebar({ active }: { active: string }) {
  const loc = useLocation();
  return html`<aside class="sidebar">
    <div class="wordmark"><div class="mark">Q</div>
      <div><span class="name">QUniLator</span><span class="sub">QBUS bridge</span></div></div>
    <nav class="nav">${NAV.map(
      ([path, label]) => html`
      <button class=${active === path ? 'active' : ''} onClick=${() => loc.route(path)}><span>${label}</span></button>`
    )}</nav>
  </aside>`;
}

function Topbar({
  title,
  hw,
  settings,
  connected,
}: {
  title: string;
  hw: HwState;
  settings: Settings;
  connected: boolean;
}) {
  return html`<header class="topbar"><h1>${title}</h1>
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
      <${Topbar} title=${title} hw=${s.hw} settings=${s.settings} connected=${s.connected} />
      <main class="content">
        <${Router}>
          <${Redirect} path="/" to="/dashboard" />
          <${Dashboard} path="/dashboard" />
          <${StoragePage} path="/storage" />
          <${ConfigsPage} path="/config/:name?/:device?" />
          <${MachinePage} path="/machine" />
          <${LogPage} path="/diagnostics" />
          <${Redirect} default to="/dashboard" />
        </${Router}>
      </main>
    </div></div>`;
}
