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
import { DebugPage } from './Debug';
import {
  SystemAccessPage,
  SystemNetworkPage,
  SystemRecordingsPage,
  SystemSerialPage,
  SystemUpdatePage,
} from './System';
import { SelftestPage } from './Selftest';
import { CatalogPage } from './Catalog';
import { updateAvailable, updateRunning } from '../lib/update';
import { dismissNotice } from '../api';

// path → sidebar label. The active item is the one whose path is the current
// path or a prefix of it (so /config/211bsd still lights "Configurations").
const NAV: [string, string][] = [
  ['/dashboard', 'Dashboard'],
  ['/storage', 'Storage'],
  ['/config', 'Configurations'],
  ['/catalog', 'Catalogue'],
  ['/machine', 'Machine'],
  ['/debug', 'Debug'],
  ['/diagnostics', 'Diagnostics'],
  ['/system', 'System'],
];

// The System entry's own pages, one function each. The sidebar opens these
// under System while one of them is the current path, and the update badge
// routes straight at the last of them.
const SYSTEM_NAV: [string, string][] = [
  ['/system/access', 'Access'],
  ['/system/network', 'Network and shell'],
  ['/system/serial', 'Serial ports'],
  ['/system/selftest', 'Self-test'],
  ['/system/recordings', 'Recordings'],
  ['/system/update', 'Updates'],
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
        ? html`<button class="upd-badge" onClick=${() => guardedRoute(loc, '/system/update')}
            title="a newer ${s.update!.package} package is published">Update ${s.update!.candidate}</button>`
        : null
    }
    <nav class="nav">${NAV.map(
      ([path, label]) => html`
      <button class=${active === path ? 'active' : ''} onClick=${() => guardedRoute(loc, path)}><span>${label}</span>${
        label === 'System' && offered ? html`<span class="nav-dot" aria-label="update available"></span>` : null
      }</button>
      ${
        path === '/system' && active === '/system'
          ? html`<div class="subnav">${SYSTEM_NAV.map(
              ([sub, subLabel]) => html`
              <button class=${loc.path === sub ? 'active' : ''}
                onClick=${() => guardedRoute(loc, sub)}><span>${subLabel}</span>${
                sub === '/system/update' && offered
                  ? html`<span class="nav-dot" aria-label="update available"></span>`
                  : null
              }</button>`
            )}</div>`
          : null
      }`
    )}</nav>
    <div class="foot">
      <a href="https://qunilator.com/" target="_blank" rel="noopener">Manual ↗</a>
    </div>
  </aside>`;
}

/* One of the backplane's two power signals, as the card read it off the bus.
 *
 * These are bus lines, not the machine's power switch: a supply drives them and
 * the card reads them, the emulated processor and the emulated cards never
 * touch them, and switching the machine off in this interface leaves them where
 * the backplane holds them. So the pill says what was measured and the hover
 * says what that means — including the third answer, that nothing is reading
 * the bus and the signal's state is therefore not known.
 */
const SIGNAL_INFO: Record<string, Record<string, string>> = {
  DCOK: {
    what: 'BDCOK, the backplane signal that says the DC supply is good. Measured on the bus — it is not the machine’s power switch.',
    asserted: 'Asserted: DC power on the backplane is good.',
    negated: 'Negated: DC power is failing or absent. The bus is being held in reset.',
    unknown: 'Not known: nothing is reading the bus, so this signal has not been measured.',
  },
  POK: {
    what: 'BPOK, the backplane signal that says the AC supply is good. Measured on the bus — it is not the machine’s power switch.',
    asserted: 'Asserted: AC power is good.',
    negated: 'Negated: AC power is failing. A processor that is running takes its power-fail trap.',
    unknown: 'Not known: nothing is reading the bus, so this signal has not been measured.',
  },
};

/* Whether QUniLator's emulation is on the bus at all.
 *
 * A different question from DCOK and POK, and about a different thing: those
 * are signals the backplane carries and the card reads, this is the state of
 * QUniLator's own emulation. The machine's power switch installs and removes
 * the configuration's cards, and while they are out nothing of the emulation
 * answers an address, though the board goes on carrying it.
 *
 * Whether the processor is executing is a third question again, and stays where
 * it is: a real machine can sit halted with a fully populated bus, so RUN and
 * HALT belong to the machine and are on the dashboard's console.
 */
const BUS_INFO: Record<string, string> = {
  what:
    'QUniLator’s emulation, as it stands on the bus. This is QUniLator’s own state — ' +
    'not the backplane’s power, which DCOK and POK report, and not whether the ' +
    'processor is executing, which the RUN lamp reports.',
  active:
    'Active: the configuration’s cards are installed and answering their addresses, ' +
    'and the emulated processor, if the machine has one, is on the bus.',
  dark:
    'Dark: no emulated card is on the bus and no emulated processor is running. ' +
    'QUniLator still carries the configuration — AUX ON on the dashboard puts it back.',
  unknown: 'Not known: this page has not yet heard from QUniLator.',
};

function EmulationPill({ powered }: { powered: boolean | null }) {
  const key = powered === null ? 'unknown' : powered ? 'active' : 'dark';
  const word = powered === null ? 'bus' : powered ? 'bus active' : 'bus dark';
  return html`<span class=${'pill' + (powered === null ? ' unknown' : '')}
    title=${BUS_INFO.what + '\n\n' + BUS_INFO[key]}
    >${html`<${Led} on=${powered} green=${true} title="emulation" />`}${word}${
      powered === null ? html`<span class="pill-q">?</span>` : null
    }</span>`;
}

function BusSignalPill({ name, state }: { name: string; state: boolean | null }) {
  const info = SIGNAL_INFO[name];
  const key = state === null ? 'unknown' : state ? 'asserted' : 'negated';
  return html`<span class=${'pill' + (state === null ? ' unknown' : '')}
    title=${info.what + '\n\n' + info[key]}
    >${html`<${Led} on=${state} green=${true} title=${name} />`}${name}${
      state === null ? html`<span class="pill-q">?</span>` : null
    }</span>`;
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
    ${html`<${BusSignalPill} name="DCOK" state=${hw.dcok} />`}
    ${html`<${BusSignalPill} name="POK" state=${hw.pok} />`}
    ${html`<${EmulationPill} powered=${hw.powered} />`}
    <span class="pill mono">addr ${settings.address_width}-bit</span>
    <span class="pill">${html`<${Led} on=${connected} green=${true} title="link" />`}${
      connected ? 'connected' : 'disconnected'
    }</span></header>`;
}

/**
 * The hardware taken for work no page may act during: the checks a power-up runs
 * before it drives the bus, or the interactive menu having the hardware.
 *
 * Every connected page raises this at once, because the reason travels in the
 * state frame and a page that connects mid-operation starts from a snapshot
 * carrying it. QUniLator refuses anything that would change the machine while
 * it is held, so the lock is what the operator sees instead of a screenful of
 * buttons that answer 409. It clears when QUniLator says it is free — there is
 * nothing to dismiss, and a stuck one is a hold that was never given up.
 *
 * The reason is the whole message: QUniLator names what holds it and what ends
 * the wait, which differs by holder — a power-up finishes on its own, an
 * interactive session ends when whoever started it exits. A line of our own
 * here could only be true for one of them.
 */
function HardwareHeld({ reason }: { reason: string }) {
  if (!reason) return null;
  return html`<div class="modal-overlay held-overlay" role="alertdialog" aria-live="assertive">
    <div class="card modal-card held-card">
      <div class="held-spinner" aria-hidden="true"></div>
      <p class="held-reason">${reason}</p>
    </div>
  </div>`;
}

/**
 * QUniLator's standing notice: something it did on its own, that no request of
 * the operator's would show them. Today that is a configuration marked to
 * start itself, which put cards - and possibly a processor - on a bus at boot
 * with nobody watching.
 *
 * It is a bar rather than a modal because it reports something already done:
 * there is no decision to take, only a thing to know. It stands until it is
 * dismissed, and the dismissal goes to QUniLator, because it is the record
 * that a person saw the warning rather than a preference of one browser.
 */
function Notice({ text }: { text: string }) {
  if (!text) return null;
  return html`<div class="notice-bar" role="status">
    <span class="notice-text">${text}</span>
    <button class="btn small" onClick=${() => dismissNotice()}>Dismiss</button>
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
  const navTitle = (NAV.find(([p]) => p === active) || [, 'Dashboard'])[1] as string;
  const sub = SYSTEM_NAV.find(([p]) => loc.path === p);
  const title = sub ? navTitle + ' · ' + sub[1] : navTitle;
  // The self-test page is the one place the hardware lock must not cover: a
  // running test holds the hardware by design, and that page's Stop button is
  // how the hold ends. Every other page (and tab) keeps the modal.
  const heldHidden = loc.path.startsWith('/system/selftest') && s.selftest.running !== null;
  return html`<div class="app">
    <${Sidebar} active=${active} />
    <div class="main">
      <${Topbar} title=${title} hw=${s.hw} settings=${s.settings} connected=${s.connected}
        update=${s.update} onUpdateClick=${() => guardedRoute(loc, '/system/update')} />
      <${Notice} text=${s.notice} />
      <main class="content">
        <${Router}>
          <${Redirect} path="/" to="/dashboard" />
          <${Dashboard} path="/dashboard" />
          <${StoragePage} path="/storage/:path*" />
          <${ConfigsPage} path="/config/:name?/:device?" />
          <${CatalogPage} path="/catalog" />
          <${MachinePage} path="/machine" />
          <${DebugPage} path="/debug" />
          <${LogPage} path="/diagnostics" />
          <${Redirect} path="/system" to="/system/access" />
          <${SystemAccessPage} path="/system/access" />
          <${SystemNetworkPage} path="/system/network" />
          <${SystemSerialPage} path="/system/serial" />
          <${SelftestPage} path="/system/selftest" />
          <${SystemRecordingsPage} path="/system/recordings" />
          <${SystemUpdatePage} path="/system/update" />
          <${Redirect} default to="/dashboard" />
        </${Router}>
      </main>
    </div>
    <${HardwareHeld} reason=${heldHidden ? '' : s.heldBy} />
  </div>`;
}
