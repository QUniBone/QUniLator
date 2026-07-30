// The memory card: the machine's address space drawn as a bar, so what the
// machine carries, what the board serves and where the I/O page starts can be
// read at a glance.
import { html } from '../../html';
import { useEffect, useState } from 'preact/hooks';
import type { ComponentChildren } from 'preact';
import { octalStr } from '../../lib/util';
import { toast } from '../../lib/toast';
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
// answers, what the board serves out of its DDR, and the I/O page on top.
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
  out.push({
    from: m.iopage_start,
    to: m.addr_space_bytes - 2,
    cls: 'iopage',
    label: 'I/O page',
  });
  return out.sort((a, b) => a.from - b.from);
}

export class MemoryWidget extends DeviceWidget {
  render(): ComponentChildren {
    const [map, setMap] = useState<MemoryMap | null>(null);
    const [probing, setProbing] = useState(false);

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

    // Sizing takes the bus for the length of the sweep, so it is a button and
    // not something this card does on its own.
    const probe = async () => {
      setProbing(true);
      const r = await apiJSON<{ physical_end: number | null; error?: string }>(
        '/api/memory/probe',
        { method: 'POST' }
      );
      setProbing(false);
      if (!r.ok) {
        toast('memory probe', r.data.error || 'refused');
        return;
      }
      const end = r.data.physical_end;
      toast(
        'memory probe',
        end === null || end === undefined
          ? 'the machine answers no memory of its own'
          : 'the machine answers up to ' + octalStr(end, map ? map.addr_width : 22)
      );
      const m = await apiJSON<MemoryMap>('/api/memory/map');
      if (m.ok) setMap(m.data);
    };

    return html`<div class="card diskcard memcard">
      ${this.head()}
      <div class="card-body membody">
        ${map ? this.bar(map) : html`<div class="mem-empty">reading the map…</div>`}
        <button type="button" class="mem-probe" disabled=${probing} onClick=${probe}>
          ${probing ? 'sizing…' : 'size the machine'}
        </button>
      </div></div>`;
  }

  // The bar: the whole address space left to right, each band drawn to scale
  // and named beneath. Space no band covers reads as nothing answering there.
  private bar(m: MemoryMap): ComponentChildren {
    const total = m.addr_space_bytes;
    const segs = bands(m).map((b) => {
      const w = ((b.to - b.from + 2) / total) * 100;
      return html`<span
        class=${'mem-seg mem-' + b.cls}
        style=${'width:' + w.toFixed(3) + '%'}
        title=${octalStr(b.from, m.addr_width) +
        '..' +
        octalStr(b.to, m.addr_width) +
        ' — ' +
        sizeStr(b.to - b.from + 2)}
      ></span>`;
    });
    const legend = bands(m).map(
      (b) => html`<div class="mem-legend-row">
        <span class=${'mem-swatch mem-' + b.cls}></span>
        <span class="mem-name">${b.label}</span>
        <span class="mem-addr"
          >${octalStr(b.from, m.addr_width)}..${octalStr(b.to, m.addr_width)}</span
        >
        <span class="mem-size">${sizeStr(b.to - b.from + 2)}</span>
      </div>`
    );
    return html`<div class="mem-map">
      <div class="mem-bar">${segs}</div>
      <div class="mem-legend">${legend}</div>
      ${m.probed_at === null
        ? html`<div class="mem-note">the machine's own memory has not been sized</div>`
        : null}
    </div>`;
  }
}
