// The memory card, laid out like the other panels: READ and WRITE lamps in the
// black bezel, then one fact row per address range — what the machine's own
// hardware answers and what the board serves out of its DDR.
import { html } from '../../html';
import { useEffect, useState } from 'preact/hooks';
import type { ComponentChildren } from 'preact';
import { octalStr } from '../../lib/util';
import { apiJSON } from '../../api';
import { statusParam } from '../../lib/devmodel';
import { Cap, PanelWidget } from './base';

interface MemoryRange {
  slot: string;
  start: number;
  end: number;
}
interface MemoryMap {
  addr_width: number;
  iopage_start: number;
  addr_space_bytes: number;
  emulated: MemoryRange[];
  physical_end: number | null;
  probed_at: number | null;
  cpu_reserved_start: number | null;
}

// One band of the address space, as the rows read it.
interface Band {
  from: number;
  to: number; // last address of the band, included
  label: string;
}

function sizeStr(bytes: number): string {
  const kb = Math.round(bytes / 1024);
  return kb >= 1024 && kb % 1024 === 0 ? kb / 1024 + 'MB' : kb + 'KB';
}

// The bands the map implies, in address order: what the machine's own memory
// answers and what the board serves out of its DDR. The I/O page sits at the
// top of every machine and never moves, so it is not a band to be read.
//
// The CPU module's own memory is a band like the others even though no bus
// cycle ever reaches it: it is the part of the space a card may not have, and
// an operator reading the rows to find room needs to see it there.
function bands(m: MemoryMap): Band[] {
  const out: Band[] = [];
  if (m.physical_end !== null && m.physical_end > 0)
    out.push({ from: 0, to: m.physical_end, label: 'hardware' });
  for (const r of m.emulated)
    out.push({
      from: r.start,
      to: r.end,
      label: r.slot === 'memory' ? 'emulated' : 'device window',
    });
  if (m.cpu_reserved_start)
    out.push({
      from: m.cpu_reserved_start,
      to: m.iopage_start - 2,
      label: 'CPU module',
    });
  return out.sort((a, b) => a.from - b.from);
}

export class MemoryWidget extends PanelWidget {
  protected panelCls = 'memcard';
  private map: MemoryMap | null = null;

  render(): ComponentChildren {
    const [map, setMap] = useState<MemoryMap | null>(null);

    // The map follows the device: enabling the card or moving its range
    // changes what the board answers, and both arrive as a parameter change.
    const endp = statusParam(this.d, 'endaddr');
    const range =
      this.param('startaddr') + '/' + (endp ? endp.v : '') + '/' + this.d.enabled;
    useEffect(() => {
      let live = true;
      apiJSON<MemoryMap>('/api/memory/map')
        .then((r) => {
          if (live && r.ok) setMap(r.data);
        })
        .catch(() => {});
      return () => {
        live = false;
      };
    }, [range]);

    this.map = map;
    return super.render();
  }

  // The machine's traffic on the card, one lamp per direction. The PRU answers
  // the card's range on its own, so these lamps are the only place a transfer
  // shows at all.
  protected caps(): ComponentChildren {
    return html`${html`<${Cap} cls="cap-white" lit=${this.lit(this.lamp('readlamp'))}>READ</${Cap}>`}
      ${html`<${Cap} cls="cap-yellow" lit=${this.lit(this.lamp('writelamp'))}>WRITE</${Cap}>`}`;
  }

  // What answers, one row per band: where it starts and ends, and how much of
  // the space it covers. Addresses the machine leaves bare are the ones no row
  // names.
  protected foot(): ComponentChildren {
    const m = this.map;
    if (!m) return html`<div class="rl-info">reading the map…</div>`;
    const rows = bands(m).map(
      (b) => html`<div>${b.label} · ${octalStr(b.from, m.addr_width)}..${octalStr(
        b.to,
        m.addr_width
      )} · ${sizeStr(b.to - b.from + 2)}</div>`
    );
    return html`<div class="rl-info">${rows.length ? rows : 'nothing answers'}</div>`;
  }
}
