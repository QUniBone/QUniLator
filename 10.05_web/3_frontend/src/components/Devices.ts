import { html } from '../html';
import { useState } from 'preact/hooks';
import { imageLabel } from '../lib/util';
import { liveSetParam } from '../api';
import { useStore } from '../store';
import { Toggle, Chip, ImageField } from './common';
import type { LiveDev, LiveParam } from '../types';

function ParamRow({ dev, p }: { dev: string; p: LiveParam }) {
  const short = p.s && p.s !== p.n ? html` <span class="p-short">(${p.s})</span>` : null;
  let ctl;
  if (p.n === 'image' && p.ro)
    ctl = html`<span class="ro" title=${p.v}>${p.v ? imageLabel(p.v) : 'no image'}</span>`;
  else if (p.n === 'image')
    ctl = html`<${ImageField} drive=${dev} image=${p.v}
      onPick=${(name: string) =>
        liveSetParam(dev, 'image', name, name ? 'image attached' : 'image detached')} />`;
  else if (p.ro)
    ctl = html`<span class="ro">${p.t === 'oct' ? html`<span class="octal"></span>` : null}${p.v}</span>`;
  else if (p.t === 'enum')
    ctl = html`<select onChange=${(e: Event) =>
      liveSetParam(dev, p.s || p.n, (e.target as HTMLSelectElement).value, 'parameter set')}>
      ${(p.opts || []).map((o) => html`<option selected=${o === p.v}>${o}</option>`)}</select>`;
  else
    ctl = html`<input type="text" value=${p.v}
      onChange=${(e: Event) =>
        liveSetParam(dev, p.s || p.n, (e.target as HTMLInputElement).value, 'parameter set')} />`;
  return html`<div class="p-name">${p.n}${short}</div>
    <div class="p-val">${ctl}${p.u ? html`<span class="unit">${p.u}</span>` : null}</div>
    <div class="p-info">${p.i}</div>`;
}

function ParamsBox({ dev, params, open }: { dev: string; params: LiveParam[]; open: boolean }) {
  return html`<div class="params" hidden=${!open}><div class="p-grid">
    ${params.map((p) => html`<${ParamRow} dev=${dev} p=${p} key=${p.n} />`)}</div></div>`;
}

function DriveRow({ d }: { d: LiveDev }) {
  const [open, setOpen] = useState(false);
  const takesImage = d.params.some((p) => p.n === 'image');
  return html`<div>
    <div class="dev-sub">
      <span class=${'act-led' + (d.activity ? ' on' : '')} title="activity"></span>
      <span class="devname">${d.name}</span>
      <span class="muted" style="font-size:var(--fs-1)">${d.type}</span>
      ${
        takesImage
          ? d.img
            ? html`<span class="imgpath" title=${d.img}>${imageLabel(d.img)}</span>`
            : html`<span class="imgpath" style="color:var(--ink-faint)">no image attached</span>`
          : null
      }
      <span class="spacer"></span>
      <${Chip} cls=${d.enabled ? 'ok' : 'off'}>${d.enabled ? 'enabled' : 'disabled'}</${Chip}>
      <${Toggle} checked=${d.enabled} onChange=${(on: boolean) =>
        liveSetParam(
          d.name,
          'enabled',
          on ? '1' : '0',
          on ? 'device enabled — registers installed on the bus' : 'device disabled'
        )} />
      <button class="btn small" onClick=${() => setOpen((o) => !o)}>Parameters</button>
    </div>
    <${ParamsBox} dev=${d.name} params=${d.params} open=${open} />
  </div>`;
}

function DeviceCard({ c }: { c: LiveDev }) {
  const [open, setOpen] = useState(false);
  const addr = (c.params.find((p) => p.n === 'base_addr') || { v: '—' }).v;
  return html`<div class="card dev-card">
    <div class="card-head dev-head">
      <span class="devname">${c.name}</span>
      <span class="devtype">${c.type}${c.info ? ' — ' + c.info : ''}</span>
      <span class="pill addr octal mono">${addr}</span>
      <${Chip} cls=${c.enabled ? 'ok' : 'off'}>${c.enabled ? 'enabled' : 'disabled'}</${Chip}>
      <${Toggle} checked=${c.enabled} onChange=${(on: boolean) =>
        liveSetParam(
          c.name,
          'enabled',
          on ? '1' : '0',
          on ? 'device enabled — registers installed on the bus' : 'device disabled'
        )} />
      <button class="btn small" onClick=${() => setOpen((o) => !o)}>Parameters</button>
    </div>
    <${ParamsBox} dev=${c.name} params=${c.params} open=${open} />
    ${c.enabled ? (c.drives || []).map((d) => html`<${DriveRow} d=${d} key=${d.name} />`) : null}
  </div>`;
}

export function DevicesPage() {
  const s = useStore();
  return html`<section class="page active" data-page="devices">
    <div>${s.devmodel.map((c) => html`<${DeviceCard} c=${c} key=${c.name} />`)}</div></section>`;
}
