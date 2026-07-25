import { html } from '../html';
import { useRef, useEffect } from 'preact/hooks';
import type { ComponentChildren } from 'preact';
import type { LiveDev, DiskStatus } from '../types';
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

function Cap({
  cls,
  lit,
  children,
}: {
  cls: string;
  lit: boolean;
  children: ComponentChildren;
}) {
  return html`<span class=${'lampbtn ' + cls + (lit ? ' lit' : '')}>${children}</span>`;
}

function Panel({
  d,
  caps,
  foot,
}: {
  d: LiveDev;
  caps: ComponentChildren;
  foot: ComponentChildren;
}) {
  return html`<div class="rlpanel">
    <div class="plabel"><span>${d.type}</span><span>${d.name}</span></div>
    <div class="lamps">${caps}</div>
    <div class="rl-foot">${foot}</div></div>`;
}

// ---- disk widgets: the drive's front bezel as labelled switch caps ----
// The drive's verbal state. The backend computes it and sends `status`; until
// that is deployed the widget derives a sensible value from the parameters the
// drive already exposes. A powered-down machine reads every drive dark.
function diskStatus(d: LiveDev): DiskStatus {
  if (store.hw.powered === false) return 'off';
  if (d.status) return d.status;
  if (!d.enabled) return 'off';
  if (!(d.img || paramVal(d, 'image'))) return 'idle';
  if (lampOn(d, 'accesslamp') || d.activity) return 'busy';
  return 'ready';
}

// The drive front: a plate of labelled caps that follow the drive's lamps. The
// RL02/RL01 carry LOAD/READY/FAULT/WRITE-PROT; other disks (RA81, RK05) show
// READY and ACCESS. One friendly title names the drive; the image picker below
// changes the medium.
function DiskWidget({ d }: { d: LiveDev }) {
  const powered = store.hw.powered !== false;
  const st = diskStatus(d);
  const lit = (v: boolean) => powered && v;
  const img = d.img || paramVal(d, 'image');
  const unit = paramVal(d, 'unit') || d.name.replace(/\D/g, '');
  const hasRlLamps = !!statusParam(d, 'loadlamp');
  const readyCap = html`<${Cap} cls="cap-white" lit=${lit(st === 'ready' || st === 'busy')}>
    <span class="num">${unit}</span>READY</${Cap}>`;
  const caps = hasRlLamps
    ? html`${html`<${Cap} cls="cap-yellow" lit=${lit(lampOn(d, 'loadlamp'))}>LOAD</${Cap}>`}
        ${readyCap}
        ${html`<${Cap} cls="cap-red" lit=${lit(lampOn(d, 'faultlamp'))}>FAULT</${Cap}>`}
        ${html`<${Cap} cls="cap-orange" lit=${lit(lampOn(d, 'writeprotectlamp'))}>WRITE<br />PROT</${Cap}>`}`
    : html`${readyCap}
        ${html`<${Cap} cls="cap-yellow" lit=${lit(st === 'busy' || lampOn(d, 'accesslamp'))}>ACCESS</${Cap}>`}`;
  return html`<div class="rlpanel disk-widget">
    <div class="disk-title">${d.label || d.type}</div>
    <div class="lamps">${caps}</div>
    <div class="rl-foot">${html`<${ImageField} drive=${d.name} image=${img}
      onPick=${(name: string) =>
        liveSetParam(d.name, 'image', name, name ? 'image attached' : 'image detached')} />`}</div>
  </div>`;
}

function NetworkWidget({ d }: { d: LiveDev }) {
  const iface = paramVal(d, 'interface'),
    mac = paramVal(d, 'mac');
  const caps = html`${html`<${Cap} cls="cap-white" lit=${d.enabled}>LINK</${Cap}>`}
    ${html`<${Cap} cls="cap-yellow" lit=${lampOn(d, 'activitylamp')}>ACT</${Cap}>`}`;
  return html`<${Panel} d=${d} caps=${caps}
    foot=${html`<div class="rl-info">${iface || '—'} · ${mac || '—'}</div>`} />`;
}

function TapeWidget({ d }: { d: LiveDev }) {
  return html`<${Panel} d=${d} caps=${html`<${Cap} cls="cap-white" lit=${d.enabled}>TAPE</${Cap}>`} foot=${''} />`;
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
    <div class="plabel"><span>${d.type}</span><span>${d.name}</span></div>
    <div class="vcb01-screen"><canvas id="vcb01-canvas" ref=${cv} tabindex="0"></canvas></div>
    <div class="vcb01-hint">click to focus — keyboard & mouse drive the board</div></div>`;
}

type Widget = (props: { d: LiveDev }) => ReturnType<typeof html>;
const WIDGET_MODELS: Record<string, Widget> = {
  VCB01: Vcb01Widget,
};
const WIDGET_CATEGORIES: Record<string, Widget> = {
  disk: DiskWidget,
  network: NetworkWidget,
  tape: TapeWidget,
};
function widgetFor(d: LiveDev): Widget | null {
  return WIDGET_MODELS[d.type] || WIDGET_CATEGORIES[d.category || 'other'] || null;
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
