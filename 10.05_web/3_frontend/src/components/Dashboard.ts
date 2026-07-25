import { html } from '../html';
import { useState, useEffect, useRef } from 'preact/hooks';
import { useRoute, useLocation } from 'preact-iso';
import { useStore, store, emit } from '../store';
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
import { Led } from './common';
import { Widgets } from './widgets';
import type { Settings, TermKey } from '../types';

const KEY_TO_CH: Record<TermKey, string> = { slu0: '0', slu1: '1', serial: 'ext' };
const CH_TO_KEY: Record<string, TermKey> = { '0': 'slu0', '1': 'slu1', ext: 'serial' };

// One switch of the 11/03 bezel: a bat-handle toggle with a silkscreen legend
// above (two-position) and/or below it. `momentary` springs back and fires on
// click; `two` reflects and sets a position.
function PanelSwitch({
  kind,
  top,
  bottom,
  pos,
  disabled,
  onFire,
  onToggle,
}: {
  kind: 'momentary' | 'two';
  top?: string;
  bottom: string;
  pos?: 'top' | 'bottom';
  disabled?: boolean;
  onFire?: () => void;
  onToggle?: () => void;
}) {
  const click = () => {
    if (disabled) return;
    if (kind === 'momentary') onFire?.();
    else onToggle?.();
  };
  return html`<div class=${'cp-sw ' + kind + (disabled ? ' off' : '')} data-pos=${pos || 'mid'}>
    <span class=${'cp-leg' + (top ? '' : ' ph')}>${top || ''}</span>
    <button class="cp-toggle" type="button" disabled=${!!disabled}
      role=${kind === 'two' ? 'switch' : undefined}
      aria-checked=${kind === 'two' ? pos === 'top' : undefined}
      aria-label=${top ? top + ' / ' + bottom : bottom}
      onClick=${click}><span class="cp-bat"></span></button>
    <span class="cp-leg">${bottom}</span>
  </div>`;
}

// The PDP-11/03 control bezel: PWR OK + RUN lamps and the RESTART /
// HALT-ENABLE / DC-ON-OFF switches, the single run-state and power controls.
function ControlPanel() {
  const s = useStore();
  const powered = s.hw.powered !== false;
  // HALT/ENABLE holds its last definite position: keep the last known halt
  // reading and only move the switch when a new definite value arrives, so a
  // transitional gap does not flicker it.
  const lastHalt = useRef(s.bus.halted);
  if (typeof s.bus.halted === 'boolean') lastHalt.current = s.bus.halted;
  const halted = lastHalt.current;
  const run = powered && !halted;

  const restart = () => liveControl('restart', 'reset and restart from boot');
  const setHalt = () => {
    if (halted) {
      liveControl('continue', 'HALT released — CPU running');
      store.bus.halted = false;
    } else {
      liveControl('halt', 'HALT asserted — CPU stopped');
      store.bus.halted = true;
    }
    emit();
  };
  // power is authoritative from the backend's `powered` state event; do not
  // guess it optimistically, so a machine that has not yet learned dc_on/dc_off
  // does not strand the UI in a frozen "off" it never actually entered
  const setPower = () =>
    powered
      ? liveControl('dc_off', 'DC off — machine powered down')
      : liveControl('dc_on', 'DC on — machine powered up');

  return html`<div class="card cp-card">
    <div class="card-head"><h3>Control panel</h3></div>
    <div class="cp-body">
      <div class="cp-lamps">
        <div class="cp-lamp">${html`<${Led} on=${powered} title="DC ON" />`}<span class="cp-leg">DC ON</span></div>
        <div class="cp-lamp">${html`<${Led} on=${run} title="RUN" />`}<span class="cp-leg">RUN</span></div>
      </div>
      <div class="cp-switches">
        ${html`<${PanelSwitch} kind="momentary" bottom="RESTART" disabled=${!powered} onFire=${restart} />`}
        ${html`<${PanelSwitch} kind="two" top="ENABLE" bottom="HALT"
          pos=${halted ? 'bottom' : 'top'} disabled=${!powered} onToggle=${setHalt} />`}
        ${html`<${PanelSwitch} kind="two" top="DC ON" bottom="OFF"
          pos=${powered ? 'top' : 'bottom'} onToggle=${setPower} />`}
      </div>
    </div>
  </div>`;
}

// The cape's own front panel: activity LEDs and DIP switches, display-only, in
// the shared LED visual language. Dark while the machine is powered off.
function FrontPanel() {
  const s = useStore();
  const powered = s.hw.powered !== false;
  return html`<div class="card frontpanel"><div class="card-head"><h3>Front panel</h3></div>
    <div class="card-body">
      <div class="fp-block"><span class="fp-k">Activity</span>
        <div class="fp-cells">${s.hw.leds.map(
          (v, i) => html`<div class="fp-cell">${html`<${Led} on=${powered && v} title=${'Activity ' + i} />`}
            <span class="fp-n">${i}</span></div>`
        )}</div></div>
      <div class="fp-block"><span class="fp-k">DIP switches</span>
        <div class="fp-cells">${s.hw.dip.map(
          (v, i) => html`<div class="fp-cell"><span class=${'dip' + (v ? ' on' : '')}></span>
            <span class="fp-n">${i + 1}</span></div>`
        )}</div></div>
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
  useStore();
  return html`<section class="page active" data-page="dashboard">
    <div class="dash-top">
      <div class="dash-left">${html`<${ControlPanel} />`}${html`<${FrontPanel} />`}</div>
      <div class="dash-term">${html`<${TerminalHost} />`}</div>
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
