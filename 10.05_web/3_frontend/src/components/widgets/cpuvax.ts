// The emulated VAX-11/780's console card.
import { html } from '../../html';
import type { ComponentChildren } from 'preact';
import { liveSetParam } from '../../api';
import { statusParam } from '../../lib/devmodel';
import type { LiveParam } from '../../types';
import { Bit, Cap, DeviceWidget } from './base';
import type { Cells, WidgetOption } from './base';

// The processor status longword, as far as a console shows it. A VAX keeps its
// condition codes in the low four bits and the level it runs at in <20:16>, and
// the two mode fields say whose memory it is addressing.
const PSL_MODES = ['K', 'E', 'S', 'U']; // kernel, executive, supervisor, user
function decodePsl(hex: string | undefined) {
  const w = parseInt(hex || '0', 16) || 0;
  return {
    ipl: (w >>> 16) & 0x1f,
    mode: PSL_MODES[(w >>> 24) & 3],
    prevMode: PSL_MODES[(w >>> 22) & 3],
    flags: [
      ['N', 0x8],
      ['Z', 0x4],
      ['V', 0x2],
      ['C', 0x1],
    ] as [string, number][],
    value: w,
  };
}

// The VAX has its own card rather than the PDP-11's: a thirty-two bit program
// counter shown in hex, a status longword laid out differently, no switch
// register, and a UNIBUS adapter between it and the devices whose traffic an
// operator wants to see.
//
// The bus readouts are what says whether the adapter is carrying anything, so
// they are the card's own option: a machine still being brought up wants them,
// one that works does not.
export class VaxCpuWidget extends DeviceWidget {
  static options: WidgetOption[] = [
    {
      key: 'bus',
      label: 'adapter traffic',
      info: 'the register accesses, interrupts and mapped transfers the UNIBUS adapter has carried',
      def: true,
    },
  ];

  private view(): { psl?: LiveParam; cycles?: LiveParam; ints?: LiveParam; dma?: LiveParam } {
    return {
      psl: statusParam(this.d, 'PSL'),
      ...(this.on('bus')
        ? {
            cycles: statusParam(this.d, 'bus_cycles'),
            ints: statusParam(this.d, 'bus_interrupts'),
            dma: statusParam(this.d, 'dma_words'),
          }
        : {}),
    };
  }

  cells(): Cells {
    const v = this.view();
    let rows = 2; // the PC and PSL readouts, and the console switches
    if (v.psl) rows++; // the status longword, spelled out as lamps
    if (v.cycles) rows++; // what the adapter has carried
    return { w: 7, h: 3 + rows };
  }

  protected statusChip(): ComponentChildren {
    const running = this.lit(this.lamp('run_led'));
    return html`<span class=${'disk-status ' + (running ? 'ok' : 'idle')}
      >${running ? 'running' : 'halted'}</span>`;
  }

  render(): ComponentChildren {
    const { psl, cycles, ints, dma } = this.view();
    const running = this.lit(this.lamp('run_led'));
    const halted = this.lamp('halt_switch');
    const pc = statusParam(this.d, 'PC');
    const s = decodePsl(psl?.v);

    // START rebuilds the machine and boots it, so it is momentary: it is
    // pulsed and falls back on its own, like the PDP-11's.
    const pulse = (name: string, what: string) => {
      liveSetParam(this.d.name, name, '1', what);
      setTimeout(() => liveSetParam(this.d.name, name, '0', what + ' released'), 300);
    };

    return html`<div class="card diskcard cpucard">
      ${this.head()}
      <div class="card-body diskface">
        <div class="cpu-regs">
          <label>PC<span class="cpu-oct">${pc ? pc.v : '00000000'}</span></label>
          ${psl && html`<label>PSL<span class="cpu-oct">${psl.v}</span></label>`}
        </div>
        ${psl &&
        html`<div class="cpu-flags">
          <span class="cpu-mode" title="current mode / previous mode"
            >${s.mode}<span class="cpu-prev">${s.prevMode}</span></span
          >
          <span class="cpu-pri" title="priority the processor runs at">IPL${s.ipl}</span>
          ${s.flags.map((f) => html`<${Bit} lit=${!!(s.value & f[1])}>${f[0]}</${Bit}>`)}
        </div>`}
        ${cycles &&
        html`<div class="cpu-regs">
          <label>BUS<span class="cpu-oct">${cycles.v}</span></label>
          <label>INT<span class="cpu-oct">${ints ? ints.v : '0'}</span></label>
          <label>DMA<span class="cpu-oct">${dma ? dma.v : '0'}</span></label>
        </div>`}
        <div class="lamps">
          <${Cap} cls="cap-white" lit=${running}>RUN</${Cap}>
          <${Cap} cls="cap-red" lit=${halted}
            onClick=${() =>
              liveSetParam(
                this.d.name,
                'halt_switch',
                halted ? '0' : '1',
                halted ? 'HALT released' : 'HALT asserted',
              )}>HALT</${Cap}>
          <${Cap} cls="cap-white" lit=${false}
            onClick=${() => pulse('start_switch', 'START')}>START</${Cap}>
          <${Cap} cls="cap-white" lit=${false}
            onClick=${() => pulse('continue_switch', 'CONT')}>CONT</${Cap}>
        </div>
      </div></div>`;
  }
}
