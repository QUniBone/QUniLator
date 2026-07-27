import { html } from '../html';
import { useRef, useEffect, useState } from 'preact/hooks';
import type { ComponentChildren } from 'preact';
import type { LiveDev } from '../types';
import { liveSetParam } from '../api';
import { store, useStore } from '../store';
import { enabledDevices, statusParam } from '../lib/devmodel';
import { ImageField } from './common';
import {
  vcb01Connect,
  vcb01Blit,
  vcb01WireKeyboard,
  vcb01WireMouse,
  vcb01Disconnect,
} from '../lib/vcb01';

// Lamps, LEDs and the drive state machine are machine-driven, so they live in
// statusParams; configuration values stay in params (paramVal).
export function lampOn(d: LiveDev, n: string): boolean {
  const p = statusParam(d, n);
  return !!p && p.v === '1';
}
export function paramVal(d: LiveDev, n: string): string {
  const p = (d.params || []).find((q) => q.n === n);
  return p ? p.v : '';
}

// A switch cap: a coloured lens with a black silkscreen legend that lights when
// its lamp is lit. A cap with an onClick is an operator button; one without is a
// display indicator and renders inert.
function Cap({
  cls,
  lit,
  onClick,
  children,
}: {
  cls: string;
  lit: boolean;
  onClick?: () => void;
  children: ComponentChildren;
}) {
  return html`<button type="button" class=${'lampbtn ' + cls + (lit ? ' lit' : '')}
    disabled=${!onClick} onClick=${onClick || null}>${children}</button>`;
}

// The READY cap carries the drive's unit number. On a wide cap (the RA81) the
// number sits in an engraved frame beside the legend; on a narrow cap (the
// RL02) it stacks above the word.
function ReadyCap({ unit, lit, wide }: { unit: string; lit: boolean; wide?: boolean }) {
  return html`<${Cap} cls=${'cap-white' + (wide ? ' wide' : '')} lit=${lit}>
    <span class="num">${unit}</span>${wide ? html`<span class="rdy">READY</span>` : 'READY'}</${Cap}>`;
}

// The panel body every widget shares: one friendly title, the black cap bezel,
// and a foot for the medium and its tags.
// The colour cue for a verbal drive status: ready reads calm, activity and spin
// transitions stand out, an idle/off drive recedes.
const DISK_STATUS_CLS: Record<string, string> = {
  ready: 'ok',
  busy: 'busy',
  'spinning up': 'spin',
  'spinning down': 'spin',
  loaded: 'load',
  idle: 'idle',
  off: 'idle',
};
function Panel({
  d,
  caps,
  foot,
  panelCls,
}: {
  d: LiveDev;
  caps: ComponentChildren;
  foot: ComponentChildren;
  panelCls?: string;
}) {
  return html`<div class=${'rlpanel' + (panelCls ? ' ' + panelCls : '')}>
    <div class="disk-title">
      <span>${d.label || d.type}</span>
      ${d.status
        ? html`<span class=${'disk-status ' + (DISK_STATUS_CLS[d.status] || 'idle')}>${d.status}</span>`
        : null}
    </div>
    <div class="lamps">${caps}</div>
    <div class="rl-foot">${foot}</div></div>`;
}

// ---- disk media state ----
const unitOf = (d: LiveDev): string => paramVal(d, 'unit') || d.name.replace(/\D/g, '');
const driveRemovable = (d: LiveDev): boolean => d.removable === true;
function driveLocked(d: LiveDev): boolean {
  const img = (d.params || []).find((p) => p.n === 'image');
  if (img && img.ro) return true;
  return paramVal(d, 'lock') === '1' || d.locked === true;
}

// A tag on the drive foot: a fixed pack, a removable one the host can change, or
// a removable one the host currently holds locked.
function LockTag({ d }: { d: LiveDev }) {
  if (!driveRemovable(d)) return html`<span class="w-tag fixed">fixed</span>`;
  return driveLocked(d)
    ? html`<span class="w-tag locked" title="host holds the medium locked">🔒 locked</span>`
    : html`<span class="w-tag unlocked" title="medium can be changed">🔓 removable</span>`;
}

// The drive foot: its removable-media tag and the shared image picker, which is
// the drive's image-swap control.
function DiskFoot({ d }: { d: LiveDev }) {
  const img = d.img || paramVal(d, 'image');
  return html`${html`<${LockTag} d=${d} />`}${html`<${ImageField} drive=${d.name} image=${img}
    onPick=${(name: string) =>
      liveSetParam(d.name, 'image', name, name ? 'image attached' : 'image detached')} />`}`;
}

// ---- disk widgets ----
// The RL01/RL02 front bezel: LOAD, READY, FAULT and WRITE-PROT. The lamps come
// from statusParams; LOAD and WRITE-PROT are operator buttons that toggle the
// drive's run/stop and write-protect configuration switches.
function RlWidget({ d }: { d: LiveDev }) {
  const powered = store.hw.powered !== false;
  const lit = (v: boolean) => powered && v;
  const unit = unitOf(d);
  const rsToggle = () => {
    const on = paramVal(d, 'runstopbutton') === '1';
    liveSetParam(d.name, 'runstopbutton', on ? '0' : '1',
      on ? 'pack spins down, LOAD lights when safe' : 'loading — READY after spin-up');
  };
  const wpToggle = () => {
    const on = paramVal(d, 'writeprotectbutton') === '1';
    liveSetParam(d.name, 'writeprotectbutton', on ? '0' : '1',
      on ? 'write protection released' : 'write-protected');
  };
  const caps = html`${html`<${Cap} cls="cap-yellow" lit=${lit(lampOn(d, 'loadlamp'))} onClick=${rsToggle}>LOAD</${Cap}>`}
    ${html`<${ReadyCap} unit=${unit} lit=${lit(lampOn(d, 'readylamp'))} />`}
    ${html`<${Cap} cls="cap-red" lit=${lit(lampOn(d, 'faultlamp'))}>FAULT</${Cap}>`}
    ${html`<${Cap} cls="cap-orange" lit=${lit(lampOn(d, 'writeprotectlamp'))} onClick=${wpToggle}>WRITE<br />PROT</${Cap}>`}`;
  return html`<${Panel} d=${d} caps=${caps} foot=${html`<${DiskFoot} d=${d} />`} />`;
}

// The RA81/MSCP drive: RUN-STOP, a momentary FAULT test, WRITE-PROT, the unit's
// READY cap, and the dual-port A/B selector. RUN-STOP follows the loaded medium
// and ACCESS the drive's activity lamp; FAULT and WRITE-PROT are the operator's
// own front-panel switches, held on the drive.
const RA81_SWITCHES = new Map<string, { wp: boolean; fault: boolean }>();
function ra81Switches(name: string): { wp: boolean; fault: boolean } {
  if (!RA81_SWITCHES.has(name)) RA81_SWITCHES.set(name, { wp: false, fault: false });
  return RA81_SWITCHES.get(name)!;
}
function Ra81Widget({ d }: { d: LiveDev }) {
  const powered = store.hw.powered !== false;
  const lit = (v: boolean) => powered && v;
  const [, force] = useState(0);
  const unit = unitOf(d),
    sw = ra81Switches(d.name);
  const mounted = !!(d.img || paramVal(d, 'image'));
  const active = lampOn(d, 'accesslamp');
  const toggle = (k: 'wp' | 'fault') => {
    sw[k] = !sw[k];
    force((x) => x + 1);
  };
  useEffect(() => {
    // FAULT is momentary: it lights while held and releases on mouse-up
    const up = () => {
      if (sw.fault) {
        sw.fault = false;
        force((x) => x + 1);
      }
    };
    document.addEventListener('mouseup', up);
    return () => document.removeEventListener('mouseup', up);
  }, []);
  const caps = html`${html`<${Cap} cls="cap-yellow" lit=${lit(mounted)}>RUN<br />STOP</${Cap}>`}
    <button type="button" class=${'lampbtn cap-red' + (lit(sw.fault) ? ' lit' : '')}
      onMouseDown=${() => toggle('fault')}>FAULT</button>
    ${html`<${ReadyCap} unit=${unit} lit=${lit(mounted && !active)} wide=${true} />`}
    <button type="button" class=${'lampbtn cap-yellow' + (lit(sw.wp) ? ' lit' : '')}
      onClick=${() => toggle('wp')}>WRITE<br />PROT</button>
    ${html`<${Cap} cls="cap-white legend-lg" lit=${lit(true)}>A</${Cap}>`}
    ${html`<${Cap} cls="cap-white legend-lg" lit=${false}>B</${Cap}>`}`;
  return html`<${Panel} d=${d} caps=${caps} panelCls="span2" foot=${html`<${DiskFoot} d=${d} />`} />`;
}

// Every other disk (RK05, RX01/RX02, RS11): a READY cap with the unit number and
// an ACCESS lamp that follows the drive's activity.
function DiskWidget({ d }: { d: LiveDev }) {
  const powered = store.hw.powered !== false;
  const lit = (v: boolean) => powered && v;
  const unit = unitOf(d);
  const active = lampOn(d, 'accesslamp');
  const caps = html`${html`<${ReadyCap} unit=${unit} lit=${lit(d.enabled && !active)} />`}
    ${html`<${Cap} cls="cap-yellow" lit=${lit(active)}>ACCESS</${Cap}>`}`;
  return html`<${Panel} d=${d} caps=${caps} panelCls="span2" foot=${html`<${DiskFoot} d=${d} />`} />`;
}

function NetworkWidget({ d }: { d: LiveDev }) {
  const powered = store.hw.powered !== false;
  const iface = paramVal(d, 'interface'),
    mac = paramVal(d, 'mac');
  const caps = html`${html`<${Cap} cls="cap-white" lit=${powered && d.enabled}>LINK</${Cap}>`}
    ${html`<${Cap} cls="cap-yellow" lit=${powered && lampOn(d, 'activitylamp')}>ACT</${Cap}>`}`;
  return html`<${Panel} d=${d} caps=${caps}
    foot=${html`<div class="rl-info">${iface || '—'} · ${mac || '—'}</div>`} />`;
}

function TapeWidget({ d }: { d: LiveDev }) {
  const powered = store.hw.powered !== false;
  return html`<${Panel} d=${d} caps=${html`<${Cap} cls="cap-white" lit=${powered && d.enabled}>TAPE</${Cap}>`} foot=${''} />`;
}

function Vcb01Widget({ d }: { d: LiveDev }) {
  const cv = useRef<HTMLCanvasElement | null>(null);
  useEffect(() => {
    vcb01Connect();
    vcb01Blit();
    vcb01WireKeyboard();
    if (cv.current) vcb01WireMouse(cv.current);
    // the socket persists across widget rebuilds; closed only when the widget is gone
    return () => {};
  }, []);
  return html`<div class="rlpanel vcb01-widget">
    <div class="disk-title">${d.label || d.type}</div>
    <div class="vcb01-screen"><canvas id="vcb01-canvas" ref=${cv} tabindex="0"></canvas></div>
    <div class="vcb01-hint">click to focus — keyboard & mouse drive the board</div></div>`;
}

type Widget = (props: { d: LiveDev }) => ReturnType<typeof html>;
const WIDGET_MODELS: Record<string, Widget> = {
  RL02: RlWidget,
  RL01: RlWidget,
  RA81: Ra81Widget,
  VCB01: Vcb01Widget,
};
const WIDGET_CATEGORIES: Record<string, Widget> = {
  disk: DiskWidget,
  network: NetworkWidget,
  tape: TapeWidget,
};
export function widgetFor(d: LiveDev): Widget | null {
  return WIDGET_MODELS[d.type] || WIDGET_CATEGORIES[d.category || 'other'] || null;
}

// A device widget's natural size in dashboard grid cells (width × height). The
// dashboard places each widget as a block of this size; tuned to fit the widget
// content at the grid cell size.
export function widgetCells(d: LiveDev): { w: number; h: number } {
  if (d.type === 'VCB01') return { w: 12, h: 10 };
  if (d.type === 'RA81') return { w: 9, h: 6 };
  const cat = d.category || 'other';
  if (cat === 'disk') return d.type.startsWith('RL') ? { w: 6, h: 6 } : { w: 9, h: 6 };
  if (cat === 'network') return { w: 6, h: 4 };
  if (cat === 'tape') return { w: 7, h: 5 };
  return { w: 6, h: 5 };
}

// Render one device widget (used by the dashboard grid to place it as a card).
export function DeviceWidget({ d }: { d: LiveDev }) {
  const W = widgetFor(d);
  return W ? html`<${W} d=${d} />` : null;
}

export function Widgets() {
  useStore();
  const devs = enabledDevices().filter((d) => widgetFor(d));
  const hasVcb = devs.some((d) => d.type === 'VCB01');
  useEffect(() => {
    if (!hasVcb) vcb01Disconnect();
  }, [hasVcb]);
  if (!devs.length)
    return html`<div class="muted" style="font-size:var(--fs-1); padding:4px">No device widgets — enable a disk or network device.</div>`;
  return devs.map((d) => {
    const W = widgetFor(d)!;
    return html`<${W} d=${d} key=${d.name} />`;
  });
}
