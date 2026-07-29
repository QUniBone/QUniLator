// The dashboard widget hierarchy: one class per kind of device card, each
// carrying its own controls and the space they take on the grid.
import { html } from '../../html';
import type { ComponentChildren } from 'preact';
import type { LiveDev } from '../../types';
import { store } from '../../store';
import { statusParam } from '../../lib/devmodel';

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

// A card's size on the dashboard grid, in whole cells.
export interface Cells {
  w: number;
  h: number;
}

// A part of a widget the operator can switch off, declared by the widget class
// that carries it. What an option covers is the widget's own business: the
// dashboard offers the declared options and stores the answers beside the
// card's position, and the widget reads them in its controls and its size.
export interface WidgetOption {
  key: string;
  label: string; // the legend on the dashboard's edit overlay
  info: string; // what the option shows while it is on
  def: boolean; // the value a configuration that names none gets
}
export type WidgetOpts = Record<string, boolean>;

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

// One device's dashboard card. A widget is built fresh for every render and its
// render() runs inside the component that holds it, so its methods may use
// hooks: a device always maps to the same widget class, which keeps the hook
// order stable across renders.
export abstract class DeviceWidget {
  // the options this widget declares; a widget with nothing to configure
  // inherits the empty list
  static options: WidgetOption[] = [];

  constructor(
    protected d: LiveDev,
    protected opts: WidgetOpts
  ) {}

  // Is an option on? One the stored configuration does not name stands at the
  // default the widget declared for it.
  protected on(key: string): boolean {
    const v = this.opts[key];
    if (typeof v === 'boolean') return v;
    const o = (this.constructor as typeof DeviceWidget).options.find((q) => q.key === key);
    return o ? o.def : true;
  }

  // The machine's power gates every lamp: an unpowered panel is dark.
  protected get powered(): boolean {
    return store.hw.powered !== false;
  }
  protected lit(v: boolean): boolean {
    return this.powered && v;
  }
  protected lamp(n: string): boolean {
    return lampOn(this.d, n);
  }
  protected param(n: string): string {
    return paramVal(this.d, n);
  }

  // The card head: the device's friendly name, and the chip naming its state.
  protected head(): ComponentChildren {
    return html`<div class="card-head">
      <h3>${this.d.label || this.d.type}</h3>
      ${this.statusChip()}
    </div>`;
  }
  protected statusChip(): ComponentChildren {
    const st = this.d.status;
    return st
      ? html`<span class=${'disk-status ' + (DISK_STATUS_CLS[st] || 'idle')}>${st}</span>`
      : null;
  }

  // A widget declares no size: the dashboard measures the card it draws and gives
  // it exactly that. Keeping the size out of the widget is what lets the grid's
  // cell size change without every widget needing to be re-tuned.

  abstract render(): ComponentChildren;
}

// The panel body the device cards share: the head, the black cap bezel, and a
// foot for the medium and its tags.
export abstract class PanelWidget extends DeviceWidget {
  // an extra class on the card, for a panel that carries a wider bezel
  protected panelCls = '';
  protected abstract caps(): ComponentChildren;
  protected foot(): ComponentChildren {
    return null;
  }
  render(): ComponentChildren {
    const foot = this.foot();
    return html`<div class=${'card diskcard' + (this.panelCls ? ' ' + this.panelCls : '')}>
      ${this.head()}
      <div class="card-body diskface">
        <div class="lamps">${this.caps()}</div>
        ${foot ? html`<div class="rl-foot">${foot}</div>` : null}
      </div></div>`;
  }
}

// ---- panel primitives ----

// A switch cap: a coloured lens with a black silkscreen legend that lights when
// its lamp is lit. A cap with an onClick is an operator button; one without is a
// display indicator and renders inert.
export function Cap({
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

// One bit of a status word, as a lamp engraved with its name: dark when clear,
// lit when set. Small enough that a row of them reads as a register.
export function Bit({
  lit,
  wide,
  children,
}: {
  lit: boolean;
  wide?: boolean;
  children: ComponentChildren;
}) {
  return html`<span class=${'cpu-bit' + (wide ? ' wide' : '') + (lit ? ' lit' : '')}
    >${children}</span>`;
}

// The READY cap carries the drive's unit number. On a wide cap (the RA81) the
// number sits in an engraved frame beside the legend; on a narrow cap (the
// RL02) it stacks above the word.
export function ReadyCap({ unit, lit, wide }: { unit: string; lit: boolean; wide?: boolean }) {
  return html`<${Cap} cls=${'cap-white' + (wide ? ' wide' : '')} lit=${lit}>
    <span class="num">${unit}</span>${wide ? html`<span class="rdy">READY</span>` : 'READY'}</${Cap}>`;
}
