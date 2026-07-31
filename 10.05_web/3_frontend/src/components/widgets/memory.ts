// The memory card: the machine's address space drawn as a bar, so what the
// machine carries, what the board serves and where the I/O page starts can be
// read at a glance.
import { html } from '../../html';
import { useEffect, useState } from 'preact/hooks';
import type { ComponentChildren } from 'preact';
import { octalStr } from '../../lib/util';
import { apiJSON } from '../../api';
import { DeviceWidget } from './base';

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
}

// One band of the address space, as the bar draws it.
interface Band {
  from: number;
  to: number; // last address of the band, included
  cls: string;
  label: string;
}

function sizeStr(bytes: number): string {
  const kb = Math.round(bytes / 1024);
  return kb >= 1024 && kb % 1024 === 0 ? kb / 1024 + ' MB' : kb + ' KB';
}

// The bands the map implies, in address order: what the machine's own memory
// answers and what the board serves out of its DDR. The I/O page sits at the
// top of every machine and never moves, so the bar draws it as the end of the
// space rather than as a band to be read.
function bands(m: MemoryMap): Band[] {
  const out: Band[] = [];
  if (m.physical_end !== null && m.physical_end > 0)
    out.push({ from: 0, to: m.physical_end, cls: 'phys', label: 'machine' });
  for (const r of m.emulated)
    out.push({
      from: r.start,
      to: r.end,
      cls: r.slot === 'memory' ? 'emul' : 'devwin',
      label: r.slot === 'memory' ? 'board' : 'device window',
    });
  return out.sort((a, b) => a.from - b.from);
}

export class MemoryWidget extends DeviceWidget {
  render(): ComponentChildren {
    const [map, setMap] = useState<MemoryMap | null>(null);

    // The map follows the device: enabling the card or moving its range
    // changes what the board answers, and both arrive as a parameter change.
    const range = this.param('startaddr') + '/' + this.param('endaddr') + '/' + this.d.enabled;
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

    return html`<div class="card diskcard memcard">
      ${this.head()}
      <div class="card-body membody">
        ${map ? this.ranges(map) : html`<div class="mem-empty">reading the map…</div>`}
      </div></div>`;
  }

  // What answers, one row per band: where it starts and ends, and how much of
  // the space it covers. Addresses the machine leaves bare are the ones no row
  // names.
  private ranges(m: MemoryMap): ComponentChildren {
    const rows = bands(m).map(
      (b) => html`<div class="mem-legend-row">
        <span class="mem-name">${b.label}</span>
        <span class="mem-addr"
          >${octalStr(b.from, m.addr_width)}..${octalStr(b.to, m.addr_width)}</span
        >
        <span class="mem-size">${sizeStr(b.to - b.from + 2)}</span>
      </div>`
    );
    return html`<div class="mem-map">
      ${rows.length
        ? html`<div class="mem-legend">${rows}</div>`
        : html`<div class="mem-empty">nothing answers</div>`}
    </div>`;
  }
}
