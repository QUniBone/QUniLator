import { html } from '../html';
import { useState, useEffect, useRef } from 'preact/hooks';
import { useRoute, useLocation } from 'preact-iso';
import { useStore, store, emit } from '../store';
import { useQueryParam } from '../router';
import { liveControl, putSettings } from '../api';
import { devEnabled } from '../lib/devmodel';
import {
  initLiveTerminal,
  teardownTerminals,
  liveTab,
  serialConnect,
  serialDisconnect,
  serialConnected,
} from '../lib/terminals';
import { DangerButton } from './common';
import { Widgets } from './widgets';
import type { BusState, HwState, Settings, TermKey } from '../types';

const KEY_TO_CH: Record<TermKey, string> = { slu0: '0', slu1: '1', serial: 'ext' };
const CH_TO_KEY: Record<string, TermKey> = { '0': 'slu0', '1': 'slu1', ext: 'serial' };

function Control() {
  return html`<div class="card ctrl-card"><div class="card-head"><h3>Control</h3></div>
    <div class="card-body ctrl-btns">
      ${html`<${DangerButton} action="halt" label="Halt" armLabel="Click again to halt"
        onFire=${() => {
          liveControl('halt', 'HALT asserted — CPU stopped');
          store.bus.halted = true;
          emit();
        }} />`}
      ${html`<${DangerButton} action="continue" label="Continue" armLabel="Click again to continue"
        onFire=${() => {
          liveControl('continue', 'HALT released — CPU running');
          store.bus.halted = false;
          emit();
        }} />`}
      ${html`<${DangerButton} action="powercycle" label="Power cycle" armLabel="Click again to power cycle"
        onFire=${() => {
          liveControl('powercycle', 'devices reset, CPU reboots');
          store.bus.halted = false;
          emit();
        }} />`}
    </div></div>`;
}

function FrontPanel({ bus, hw }: { bus: BusState; hw: HwState }) {
  return html`<div class="card frontpanel"><div class="card-head"><h3>Front panel</h3></div>
    <div class="card-body">
      <div class="fp-row"><span class="fp-k">Bus</span>
        <span class=${'chip ' + (bus.halted ? 'out' : 'ok')}>run</span>
        <span class=${'chip ' + (bus.halted ? 'warn' : 'out')}>halt</span>
        <span class=${'chip ' + (bus.init ? 'ok' : 'out')}>init</span></div>
      <div class="fp-row"><span class="fp-k">LEDs</span><span class="ledrow">
        ${hw.leds.map((v) => html`<span class=${'led' + (v ? ' on' : '')}></span>`)}</span></div>
      <div class="fp-row"><span class="fp-k">DIP</span><span class="dipsw">
        ${hw.dip.map((v) => html`<span class=${'dip' + (v ? ' on' : '')}></span>`)}</span></div>
    </div></div>`;
}

export function TermTabs({ settings, select }: { settings: Settings; select: (key: TermKey) => void }) {
  const s = useStore();
  const en0 = devEnabled('DL11'),
    en1 = devEnabled('DL11b');
  const src = (settings.external_console || {}).source || 'off';
  const [baud, setBaud] = useState(() =>
    src === 'webserial'
      ? localStorage.getItem('webserial.baud') || '38400'
      : String((settings.external_console || {}).baud || 38400)
  );
  useEffect(() => {
    setBaud(
      src === 'webserial'
        ? localStorage.getItem('webserial.baud') || '38400'
        : String((settings.external_console || {}).baud || 38400)
    );
  }, [src, (settings.external_console || {}).baud]);
  useEffect(() => {
    // keep the active tab valid as SLU devices come and go. This is a
    // correctness fixup, not user navigation, so switch the tab locally rather
    // than pushing a URL change.
    if ((s.activeTerm === 'slu0' && !en0) || (s.activeTerm === 'slu1' && !en1))
      liveTab(en0 ? 'slu0' : en1 ? 'slu1' : 'serial');
  }, [en0, en1]);
  const onBaud = (v: string) => {
    setBaud(v);
    const b = parseInt(v, 10);
    if (src === 'ttys2') putSettings({ external_console: { baud: b } }, 'baud set');
    else localStorage.setItem('webserial.baud', String(b));
  };
  const tab = (key: TermKey, label: string, show: boolean) =>
    show === false
      ? null
      : html`<button class=${s.activeTerm === key ? 'active' : ''} onClick=${() => select(key)}>${label}</button>`;
  return html`<div class=${'term-tabs' + (s.termReady ? ' ready' : '')}>
    ${tab('slu0', 'SLU0 · 777560', en0)}
    ${tab('slu1', 'SLU1 · 776500', en1)}
    ${tab('serial', 'Console', true)}
    ${
      s.activeTerm === 'serial'
        ? html`<span id="serial-bar" style="display:flex; align-items:center; gap:6px; margin-left:auto">
      <select aria-label="console source" value=${src}
        onChange=${(e: Event) =>
          putSettings(
            { external_console: { source: (e.target as HTMLSelectElement).value } },
            'console source set'
          )}>
        <option value="webserial">Mac (Web Serial)</option>
        <option value="ttys2">BBB /dev/ttyS2</option>
        <option value="off">Off</option></select>
      ${
        src !== 'off'
          ? html`<select aria-label="baud rate" value=${baud} disabled=${serialConnected()}
        onChange=${(e: Event) => onBaud((e.target as HTMLSelectElement).value)}>
        ${['300', '1200', '2400', '4800', '9600', '19200', '38400'].map(
          (b) => html`<option value=${b}>${b}</option>`
        )}</select>`
          : null
      }
      ${
        src === 'webserial'
          ? html`<button class="btn small"
        onClick=${() => {
          if (!('serial' in navigator)) return;
          serialConnected() ? serialDisconnect() : serialConnect(parseInt(baud, 10));
        }}>
        ${serialConnected() ? 'Disconnect' : 'Connect'}</button>`
          : null
      }
    </span>`
        : null
    }
  </div>`;
}

export function TerminalHost() {
  const host = useRef<HTMLDivElement | null>(null);
  useEffect(() => {
    if (host.current) initLiveTerminal(host.current);
    return () => teardownTerminals();
  }, []);
  return html`<div class="term" ref=${host} tabindex="0" aria-label="VT100 terminal, 80 columns by 24 rows"></div>`;
}

export function Dashboard() {
  const s = useStore();
  const [ch, setCh] = useQueryParam('console');
  useEffect(() => {
    if (ch && CH_TO_KEY[ch]) liveTab(CH_TO_KEY[ch]);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);
  const select = (key: TermKey) => {
    liveTab(key);
    setCh(KEY_TO_CH[key]);
  };
  return html`<section class="page active" data-page="dashboard">
    <div class="dash-top">
      <div class="dash-left">${html`<${Control} />`}${html`<${FrontPanel} bus=${s.bus} hw=${s.hw} />`}</div>
      <div class="dash-term">${html`<${TermTabs} settings=${s.settings} select=${select} />`}${html`<${TerminalHost} />`}</div>
    </div>
    <div class="widget-grid" style="margin-top:14px">${html`<${Widgets} />`}</div>
  </section>`;
}

// Standalone console route: the same terminal component the dashboard embeds,
// without the dashboard's control row, front panel and device widgets. The
// channel is a path segment (/console/<channel>), so a tab switch is a push.
export function ConsolePage() {
  const s = useStore();
  const { params } = useRoute();
  const loc = useLocation();
  const channel = params.channel;
  useEffect(() => {
    if (channel && CH_TO_KEY[channel]) liveTab(CH_TO_KEY[channel]);
  }, [channel]);
  const select = (key: TermKey) => loc.route('/console/' + KEY_TO_CH[key]);
  return html`<section class="page active console-standalone" data-page="console">
    ${html`<${TermTabs} settings=${s.settings} select=${select} />`}${html`<${TerminalHost} />`}
  </section>`;
}
