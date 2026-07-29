// The emulated CPU's console card.
import { html } from '../../html';
import { useState } from 'preact/hooks';
import type { ComponentChildren } from 'preact';
import { liveSetParam } from '../../api';
import { statusParam } from '../../lib/devmodel';
import type { LiveParam } from '../../types';
import { Bit, Cap, DeviceWidget } from './base';
import type { Cells, WidgetOption } from './base';

// The processor status word, as its bits are laid out on a PDP-11: the
// condition codes in the low four, the T bit above them, the priority the CPU
// runs at in <7:5>, and on a model with memory management the current and
// previous mode in the top half. A KA11 keeps the word in a byte and has no
// modes, so the caller decides whether to ask for them.
const PSW_MODES = ['K', 'S', '?', 'U']; // <15:14> / <13:12>: kernel, supervisor, user
function decodePsw(oct: string | undefined) {
  const w = parseInt(oct || '0', 8) || 0;
  return {
    pri: (w >> 5) & 7,
    t: !!(w & 0o20),
    flags: [
      ['N', 0o10],
      ['Z', 0o4],
      ['V', 0o2],
      ['C', 0o1],
    ] as [string, number][],
    value: w,
    mode: PSW_MODES[(w >> 14) & 3],
    prevMode: PSW_MODES[(w >> 12) & 3],
  };
}

// An emulated CPU, as its console: the RUN lamp, the program counter, the
// status word, the bus address and data registers of the transfer in flight,
// the switch register the operator sets, and the three console switches. HALT
// is a toggle the CPU reads continuously; START and CONTINUE are momentary, so
// they are pulsed and fall back on their own.
//
// What a model does not have, it does not publish, so the card is built from
// the status parameters that are actually there: only the 11/34 carries the
// KT11-D registers, and only a machine with memory management has modes.
//
// The registers the core publishes on every pass are a live readout an operator
// may not want the screen space for, so they are an option; the card without
// them keeps the program counter, the switch register and the switches, and
// takes the rows the dropped readouts occupied back off its height.
export class CpuWidget extends DeviceWidget {
  static options: WidgetOption[] = [
    {
      key: 'status',
      label: 'live status',
      info: 'the processor status word, the bus registers and the memory-management registers, as the core publishes them',
      def: true,
    },
  ];

  // The readouts the card shows: those the model publishes, and those the live
  // status option admits. cells() and render() read the same view, so the
  // card's height always matches what is on it.
  private view(): {
    psw?: LiveParam;
    ba?: LiveParam;
    bd?: LiveParam;
    mmr0?: LiveParam;
    mmr2?: LiveParam;
  } {
    if (!this.on('status')) return {};
    return {
      psw: statusParam(this.d, 'PSW'),
      ba: statusParam(this.d, 'bus_addr'),
      bd: statusParam(this.d, 'bus_data'),
      mmr0: statusParam(this.d, 'MMR0'),
      mmr2: statusParam(this.d, 'MMR2'),
    };
  }

  cells(): Cells {
    const v = this.view();
    // the head takes three cells; each row of the card body takes one
    let rows = 2; // the PC/SR readouts, and the console switches
    if (v.psw) rows++; // the status word, spelled out as lamps
    if (v.ba || v.bd) rows++; // the bus registers
    if (v.mmr0) rows++; // the memory-management registers
    return { w: 7, h: 3 + rows };
  }

  protected statusChip(): ComponentChildren {
    const running = this.lit(this.lamp('run_led'));
    return html`<span class=${'disk-status ' + (running ? 'ok' : 'idle')}
      >${running ? 'running' : 'halted'}</span>`;
  }

  render(): ComponentChildren {
    const { psw, ba, bd, mmr0, mmr2 } = this.view();
    const running = this.lit(this.lamp('run_led'));
    const halted = this.lamp('halt_switch');
    const pc = statusParam(this.d, 'PC');
    const hasMmu = !!mmr0;
    const s = decodePsw(psw?.v);
    const [swr, setSwr] = useState<string | null>(null);

    const pulse = (name: string, what: string) => {
      liveSetParam(this.d.name, name, '1', what);
      setTimeout(() => liveSetParam(this.d.name, name, '0', what + ' released'), 300);
    };
    // octal params arrive already rendered in their base, zero-padded
    const swrValue = swr !== null ? swr : this.param('switch_reg');
    const commitSwr = () => {
      if (swr !== null) liveSetParam(this.d.name, 'switch_reg', swr, 'switch register set');
      setSwr(null);
    };

    return html`<div class="card diskcard cpucard">
      ${this.head()}
      <div class="card-body diskface">
        <div class="cpu-regs">
          <label>PC<span class="cpu-oct">${pc ? pc.v : '000000'}</span></label>
          ${psw && html`<label>PSW<span class="cpu-oct">${psw.v}</span></label>`}
          <label>SR<input class="cpu-oct cpu-swr" value=${swrValue}
            onInput=${(e: any) => setSwr(e.currentTarget.value)}
            onBlur=${commitSwr}
            onKeyDown=${(e: any) => { if (e.key === 'Enter') e.currentTarget.blur(); }} /></label>
        </div>
        ${psw && html`<div class="cpu-flags">
          ${hasMmu && html`<span class="cpu-mode" title="current mode / previous mode">
            ${s.mode}<span class="cpu-prev">${s.prevMode}</span></span>`}
          <span class="cpu-pri" title="priority the CPU runs at">BR${s.pri}</span>
          <${Bit} lit=${s.t}>T</${Bit}>
          ${s.flags.map(([n, m]) => html`<${Bit} lit=${!!(s.value & m)}>${n}</${Bit}>`)}
          ${hasMmu && html`<${Bit} lit=${this.lamp('mmu_enabled')} wide=${true}>MMU</${Bit}>`}
        </div>`}
        ${(ba || bd) && html`<div class="cpu-regs">
          ${ba && html`<label>BA<span class="cpu-oct">${ba.v}</span></label>`}
          ${bd && html`<label>BD<span class="cpu-oct">${bd.v}</span></label>`}
        </div>`}
        ${mmr0 && html`<div class="cpu-regs">
          <label>MMR0<span class="cpu-oct">${mmr0.v}</span></label>
          ${mmr2 && html`<label>MMR2<span class="cpu-oct">${mmr2.v}</span></label>`}
        </div>`}
        <div class="lamps">
          <${Cap} cls="cap-white" lit=${running}>RUN</${Cap}>
          <${Cap} cls="cap-red" lit=${halted}
            onClick=${() => liveSetParam(this.d.name, 'halt_switch', halted ? '0' : '1',
              halted ? 'HALT released' : 'HALT asserted')}>HALT</${Cap}>
          <${Cap} cls="cap-white" lit=${false}
            onClick=${() => pulse('start_switch', 'START')}>START</${Cap}>
          <${Cap} cls="cap-white" lit=${false}
            onClick=${() => pulse('continue_switch', 'CONT')}>CONT</${Cap}>
        </div>
      </div></div>`;
  }
}
