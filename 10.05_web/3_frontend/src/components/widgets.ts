import { html } from '../html';
import { useState, useRef, useEffect } from 'preact/hooks';
import type { ComponentChildren } from 'preact';
import type { LiveDev } from '../types';
import { imageLabel } from '../lib/util';
import { liveSetParam, refreshDevices } from '../api';
import { pickImage } from '../lib/modals';
import { useStore } from '../store';
import { enabledDevices } from '../lib/devmodel';
import {
  vcb01Connect,
  vcb01Blit,
  vcb01WireKeyboard,
  vcb01WireMouse,
  vcb01Disconnect,
} from '../lib/vcb01';

export function lampOn(d: LiveDev, n: string): boolean {
  const p = (d.params || []).find((q) => q.n === n);
  return !!p && p.v === '1';
}
export function paramVal(d: LiveDev, n: string): string {
  const p = (d.params || []).find((q) => q.n === n);
  return p ? p.v : '';
}
function unitOf(d: LiveDev): string {
  return paramVal(d, 'unit') || d.name.replace(/\D/g, '');
}
function driveRemovable(d: LiveDev): boolean {
  return d.removable === true;
}
function driveLocked(d: LiveDev): boolean {
  const img = (d.params || []).find((p) => p.n === 'image');
  if (img && img.ro) return true;
  return lampOn(d, 'lock') || d.locked === true;
}

export async function openImagePicker(drive: string, current: string): Promise<void> {
  const name = await pickImage('Change image · ' + drive, 'Detach — leave the drive empty', current);
  if (name === null) return;
  liveSetParam(drive, 'image', name, name ? 'image attached' : 'image detached');
  setTimeout(() => refreshDevices().catch(() => {}), 300);
}

function LockTag({ d }: { d: LiveDev }) {
  if (!driveRemovable(d)) return html`<span class="w-tag fixed">fixed</span>`;
  return driveLocked(d)
    ? html`<span class="w-tag locked" title="host holds the medium locked">🔒 locked</span>`
    : html`<span class="w-tag unlocked" title="medium can be changed">🔓 removable</span>`;
}

function Swap({ d, cls }: { d: LiveDev; cls: string }) {
  const img = d.img || paramVal(d, 'image');
  const txt = img ? imageLabel(img) : 'no image';
  if (driveRemovable(d) && !driveLocked(d))
    return html`<button class=${cls + ' swap mono'} title=${img || 'change image'}
      onClick=${(e: Event) => {
        e.preventDefault();
        openImagePicker(d.name, img);
      }}>${txt} · change…</button>`;
  return html`<div class=${cls + ' mono'} title=${img || ''}>${txt}</div>`;
}

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
  return html`<button class=${'lampbtn ' + cls + (lit ? ' lit' : '')}
    disabled=${!onClick} onClick=${onClick || null}>${children}</button>`;
}

function ReadyCap({ unit, lit, wide }: { unit: string; lit: boolean; wide?: boolean }) {
  return html`<${Cap} cls=${'cap-white' + (wide ? ' wide' : '')} lit=${lit}>
    <span class="num">${unit}</span>${wide ? html`<span class="rdy">READY</span>` : 'READY'}</${Cap}>`;
}

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
    <div class="plabel"><span>${d.type}</span><span>${d.name}</span></div>
    <div class="lamps">${caps}</div>
    <div class="rl-foot">${foot}</div></div>`;
}

function RlWidget({ d }: { d: LiveDev }) {
  const unit = unitOf(d);
  const rsToggle = () => {
    const on = paramVal(d, 'runstopbutton') === '1';
    liveSetParam(
      d.name,
      'runstopbutton',
      on ? '0' : '1',
      on ? 'pack spins down, LOAD lights when safe' : 'loading — READY after spin-up'
    );
  };
  const wpToggle = () => {
    const on = paramVal(d, 'writeprotectbutton') === '1';
    liveSetParam(
      d.name,
      'writeprotectbutton',
      on ? '0' : '1',
      on ? 'write protection released' : 'write-protected'
    );
  };
  return html`<div class="rlpanel">
    <div class="plabel"><span>${d.type}</span><span>${d.name}</span></div>
    <div class="lamps">
      <button class=${'lampbtn cap-yellow' + (lampOn(d, 'loadlamp') ? ' lit' : '')} onClick=${rsToggle}>LOAD</button>
      <button class=${'lampbtn cap-white' + (lampOn(d, 'readylamp') ? ' lit' : '')} disabled><span class="num">${unit}</span>READY</button>
      <button class=${'lampbtn cap-red' + (lampOn(d, 'faultlamp') ? ' lit' : '')} disabled>FAULT</button>
      <button class=${'lampbtn cap-orange' + (lampOn(d, 'writeprotectlamp') ? ' lit' : '')} onClick=${wpToggle}>WRITE<br />PROT</button>
    </div>
    <div class="rl-foot">${html`<${LockTag} d=${d} />`}${html`<${Swap} d=${d} cls="rl-img" />`}</div></div>`;
}

interface Ra81Sw {
  wp: boolean;
  fault: boolean;
}
const RA81_SWITCHES = new Map<string, Ra81Sw>();
function ra81Switches(name: string): Ra81Sw {
  if (!RA81_SWITCHES.has(name)) RA81_SWITCHES.set(name, { wp: false, fault: false });
  return RA81_SWITCHES.get(name)!;
}
function Ra81Widget({ d }: { d: LiveDev }) {
  const [, force] = useState(0);
  const unit = unitOf(d),
    sw = ra81Switches(d.name);
  const mounted = !!(d.img || paramVal(d, 'image'));
  const active = lampOn(d, 'accesslamp');
  const toggle = (k: keyof Ra81Sw) => {
    sw[k] = !sw[k];
    force((x) => x + 1);
  };
  useEffect(() => {
    const up = () => {
      if (sw.fault) {
        sw.fault = false;
        force((x) => x + 1);
      }
    };
    document.addEventListener('mouseup', up);
    return () => document.removeEventListener('mouseup', up);
  }, []);
  const caps = html`
    ${html`<${Cap} cls="cap-yellow" lit=${mounted}>RUN<br />STOP</${Cap}>`}
    <button class=${'lampbtn cap-red' + (sw.fault ? ' lit' : '')}
      onMouseDown=${() => toggle('fault')}>FAULT</button>
    ${html`<${ReadyCap} unit=${unit} lit=${mounted && !active} wide=${true} />`}
    <button class=${'lampbtn cap-yellow' + (sw.wp ? ' lit' : '')} onClick=${() => toggle('wp')}>WRITE<br />PROT</button>
    ${html`<${Cap} cls="cap-white legend-lg" lit=${true}>A</${Cap}>`}
    ${html`<${Cap} cls="cap-white legend-lg" lit=${false}>B</${Cap}>`}`;
  return html`<${Panel} d=${d} caps=${caps} panelCls="span2"
    foot=${html`${html`<${LockTag} d=${d} />`}${html`<${Swap} d=${d} cls="rl-img" />`}`} />`;
}

function DiskWidget({ d }: { d: LiveDev }) {
  const active = lampOn(d, 'accesslamp'),
    unit = unitOf(d);
  const caps = html`${html`<${ReadyCap} unit=${unit} lit=${d.enabled && !active} />`}
    ${html`<${Cap} cls="cap-yellow" lit=${active}>ACCESS</${Cap}>`}`;
  return html`<${Panel} d=${d} caps=${caps} panelCls="span2"
    foot=${html`${html`<${LockTag} d=${d} />`}${html`<${Swap} d=${d} cls="rl-img" />`}`} />`;
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
