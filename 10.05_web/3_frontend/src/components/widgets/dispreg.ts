// The console switch and display register at 777570, as the lights and
// switches of a PDP-11 console.
import { html } from '../../html';
import { useEffect, useRef, useState } from 'preact/hooks';
import type { ComponentChildren } from 'preact';
import { liveSetParam } from '../../api';
import { store } from '../../store';
import { statusParam } from '../../lib/devmodel';
import { DeviceWidget } from './base';
import { onTrace, TracePlayer } from '../../lib/trace';

// Bits 15..0 in the groups a PDP-11 console draws them in: bit 15 on its own,
// then five octal digits of three, so the lights read as the octal value does.
const GROUPS = [[15], [14, 13, 12], [11, 10, 9], [8, 7, 6], [5, 4, 3], [2, 1, 0]];

const octal = (v: number): string => (v & 0o177777).toString(8).padStart(6, '0');

// The display shows what a program writes, as sixteen lamps and the octal value
// beside them. The lamps replay every change the board logged, at the pace the
// program made them, so a moving pattern moves; the octal value is the word as
// it stands. The switches are sixteen toggles a program reads, with
// the same value as a field, so a whole word can be typed as well as clicked.
export class DispRegWidget extends DeviceWidget {
  render(): ComponentChildren {
    const shown = parseInt(statusParam(this.d, 'display')?.v || '0', 8) || 0;
    const sw = parseInt(this.param('switches') || '0', 8) || 0;
    const [typed, setTyped] = useState<string | null>(null);

    // The lamps are painted by the player, never by a render, so a re-render
    // cannot put back a word the replay has moved past.
    const bits = useRef<HTMLSpanElement>(null);
    const player = useRef<TracePlayer | null>(null);
    if (!player.current)
      player.current = new TracePlayer((v) =>
        bits.current?.querySelectorAll<HTMLElement>('.dsp-lamp').forEach((el) =>
          el.classList.toggle('lit', !!(v & (1 << Number(el.dataset.bit))))));
    const powered = this.powered;
    useEffect(() => onTrace(this.d.name, 'display', (t, at) => {
      if (store.hw.powered !== false) player.current!.push(t, at);
    }), [this.d.name]);
    useEffect(() => {
      player.current!.setSteady(powered ? shown : 0);
      if (!powered) player.current!.stop();
    }, [shown, powered]);
    useEffect(() => () => player.current!.stop(), []);

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
          <span class="dsp-bits" ref=${bits}>
            ${row((b) => html`<span class="led dsp-lamp" data-bit=${b} title=${'bit ' + b}></span>`)}
          </span>
          <span class="cpu-oct">${octal(powered ? shown : 0)}</span>
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
