import { html } from '../html';
import { useEffect, useRef } from 'preact/hooks';
import { useStore, store, emit } from '../store';
import { liveControl } from '../api';
import { initLiveTerminal, teardownTerminals } from '../lib/terminals';
import { Led } from './common';
import { Widgets } from './widgets';

// One switch of the 11/03 bezel: a bat-handle toggle with a silkscreen legend
// above (two-position) and/or below it. `momentary` springs back and fires on
// click; `two` reflects and sets a position.
function PanelSwitch({
  kind,
  label,
  pos,
  posLabels,
  disabled,
  onFire,
  onToggle,
}: {
  kind: 'momentary' | 'two';
  label: string;
  pos?: 'top' | 'bottom';
  posLabels?: [string, string];
  disabled?: boolean;
  onFire?: () => void;
  onToggle?: () => void;
}) {
  const click = () => {
    if (disabled) return;
    if (kind === 'momentary') onFire?.();
    else onToggle?.();
  };
  return html`<div class=${'cp-ctrl cp-sw ' + kind + (disabled ? ' off' : '')} data-pos=${pos || 'mid'}>
    <span class="cp-el"><button class="cp-toggle" type="button" disabled=${!!disabled}
      role=${kind === 'two' ? 'switch' : undefined}
      aria-checked=${kind === 'two' ? pos === 'top' : undefined}
      aria-label=${posLabels ? label + ' ' + posLabels[0] + '/' + posLabels[1] : label}
      onClick=${click}><span class="cp-bat"></span></button></span>
    <span class="cp-leg">${label}${
      posLabels
        ? html`<span class="cp-pos"><span>${posLabels[0]}</span><span>${posLabels[1]}</span></span>`
        : null
    }</span>
  </div>`;
}

// The PDP-11/03 control bezel: PWR OK + RUN lamps and the RESTART /
// HALT-ENABLE / AUX-ON-OFF switches, the single run-state and power controls.
// AUX ON/OFF is the auxiliary DC power switch, driving the dc_on/dc_off flag.
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
        <div class="cp-lampbezel">
          <span class="cp-lampcell">${html`<${Led} on=${powered} title="PWR OK" />`}</span>
          <span class="cp-lampcell">${html`<${Led} on=${run} title="RUN" />`}</span>
        </div>
        <div class="cp-lamplegs"><span class="cp-lampcell">PWR OK</span><span class="cp-lampcell">RUN</span></div>
      </div>
      <img class="cp-logo" src="/digital-logo.svg" alt="digital" width="72" height="21" />
      <div class="cp-switches">
        ${html`<${PanelSwitch} kind="momentary" label="RESTART" disabled=${!powered} onFire=${restart} />`}
        ${html`<${PanelSwitch} kind="two" label="HALT"
          pos=${halted ? 'bottom' : 'top'} disabled=${!powered} onToggle=${setHalt} />`}
        ${html`<${PanelSwitch} kind="two" label="AUX" posLabels=${['ON', 'OFF']}
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
      <div class="fp-block">
        <div class="fp-cells fp-cells-led">${s.hw.leds.map(
          (v, i) => html`<div class="fp-cell">${html`<${Led} on=${powered && v} title=${'Activity ' + i} />`}
            <span class="fp-n">${i}</span></div>`
        )}</div></div>
      <div class="fp-block">
        <div class="fp-cells fp-cells-sw">${s.hw.dip.map(
          (v, i) => html`<div class="fp-cell"><span class=${'dip' + (v ? ' on' : '')}></span>
            <span class="fp-n">${i + 1}</span></div>`
        )}</div></div>
    </div></div>`;
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
    <div class="dash-panels">
      ${html`<${ControlPanel} />`}
      ${html`<${FrontPanel} />`}
    </div>
    <div class="dash-top" style="margin-top:14px">
      <div class="dash-term">${html`<${TerminalHost} />`}</div>
    </div>
    <div class="widget-grid" style="margin-top:14px">${html`<${Widgets} />`}</div>
  </section>`;
}
