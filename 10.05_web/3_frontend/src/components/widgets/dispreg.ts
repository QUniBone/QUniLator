// The console switch and display register at 777570, as the lights and
// switches of a PDP-11 console.
import { html } from '../../html';
import { useState } from 'preact/hooks';
import type { ComponentChildren } from 'preact';
import { liveSetParam } from '../../api';
import { statusParam } from '../../lib/devmodel';
import { DeviceWidget } from './base';

// Bits 15..0 in the groups a PDP-11 console draws them in: bit 15 on its own,
// then five octal digits of three, so the lights read as the octal value does.
const GROUPS = [[15], [14, 13, 12], [11, 10, 9], [8, 7, 6], [5, 4, 3], [2, 1, 0]];

const octal = (v: number): string => (v & 0o177777).toString(8).padStart(6, '0');

// The display shows what a program last wrote, as sixteen lamps and the octal
// value beside them. The switches are sixteen toggles a program reads, with
// the same value as a field, so a whole word can be typed as well as clicked.
export class DispRegWidget extends DeviceWidget {
  render(): ComponentChildren {
    const shown = parseInt(statusParam(this.d, 'display')?.v || '0', 8) || 0;
    const sw = parseInt(this.param('switches') || '0', 8) || 0;
    const [typed, setTyped] = useState<string | null>(null);

    const setSwitches = (v: number) =>
      liveSetParam(this.d.name, 'switches', octal(v), 'switches set');
    const commit = () => {
      // a value that is not a sixteen-bit octal word leaves the switches as
      // they stand, and the field shows them again
      if (typed !== null && /^[0-7]{1,6}$/.test(typed) && parseInt(typed, 8) <= 0o177777)
        setSwitches(parseInt(typed, 8));
      setTyped(null);
    };

    const row = (cell: (bit: number) => ComponentChildren) =>
      GROUPS.map((g) => html`<span class="dsp-group">${g.map(cell)}</span>`);

    return html`<div class="card diskcard dspcard">
      ${this.head()}
      <div class="card-body diskface">
        <div class="dsp-row">
          <span class="dsp-legend">DISPLAY</span>
          <span class="dsp-bits">
            ${row((b) => html`<span class=${'led' + (this.lit(!!(shown & (1 << b))) ? ' on' : '')}
              title=${'bit ' + b}></span>`)}
          </span>
          <span class="cpu-oct">${octal(this.powered ? shown : 0)}</span>
        </div>
        <div class="dsp-row">
          <span class="dsp-legend">SWITCHES</span>
          <span class="dsp-bits">
            ${row((b) => html`<button type="button" class=${'dsp-sw' + (sw & (1 << b) ? ' up' : '')}
              title=${'switch ' + b} onClick=${() => setSwitches(sw ^ (1 << b))}></button>`)}
          </span>
          <input class="cpu-oct cpu-swr" value=${typed !== null ? typed : octal(sw)}
            onInput=${(e: any) => setTyped(e.currentTarget.value)}
            onBlur=${commit}
            onKeyDown=${(e: any) => { if (e.key === 'Enter') e.currentTarget.blur(); }} />
        </div>
        <div class="dsp-row dsp-numbers">
          <span class="dsp-legend"></span>
          <span class="dsp-bits">${row((b) => html`<span class="dsp-num">${b}</span>`)}</span>
        </div>
      </div></div>`;
  }
}
