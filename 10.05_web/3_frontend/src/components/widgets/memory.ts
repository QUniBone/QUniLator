// The memory card: the machine's address space drawn as a bar, so what the
// machine carries, what the board serves and where the I/O page starts can be
// read at a glance.
import { html } from '../../html';
import { useEffect, useState } from 'preact/hooks';
import type { ComponentChildren } from 'preact';
import type { LiveDev } from '../../types';
import { octalStr } from '../../lib/util';
import { apiJSON, liveSetParam } from '../../api';
import { alertModal } from '../../lib/modals';
import { statusParam } from '../../lib/devmodel';
import { DeviceWidget, paramVal } from './base';

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

// The card is placed the way a card is described: a start address and a size.
// The last address it answers follows from the two and is read here rather than
// set. The machine can refuse a placement — a range it already answers would
// put two cards on one cycle — and then the card stays where it was, so the
// fields go back to what they held and the reason is put in front of the
// operator.
function Placement({ d }: { d: LiveDev }) {
  const start = paramVal(d, 'startaddr');
  const size = paramVal(d, 'size');
  const endp = statusParam(d, 'endaddr');
  const end = endp ? endp.v : '';
  const [startDraft, setStartDraft] = useState(start);
  const [sizeDraft, setSizeDraft] = useState(size);
  useEffect(() => setStartDraft(start), [start]);
  useEffect(() => setSizeDraft(size), [size]);

  const place = async (param: string, value: string) => {
    if (value === paramVal(d, param)) return;
    const res = await liveSetParam(d.name, param, value, 'card placed');
    if (!res.ok) {
      setStartDraft(start);
      setSizeDraft(size);
      await alertModal(
        'Card not placed',
        res.data.error || 'The machine refused the placement.'
      );
    }
  };
  const commitStart = () => place('startaddr', startDraft);
  const commitSize = () => place('size', sizeDraft);

  return html`<div class="mem-place">
    <label class="mem-field"
      ><span>start</span>
      <input class="mono" type="text" value=${startDraft} spellcheck=${false}
        onInput=${(e: Event) => setStartDraft((e.target as HTMLInputElement).value)}
        onChange=${commitStart} onBlur=${commitStart} />
    </label>
    <label class="mem-field"
      ><span>size</span>
      <input type="text" value=${sizeDraft} placeholder="256 KB" spellcheck=${false}
        onInput=${(e: Event) => setSizeDraft((e.target as HTMLInputElement).value)}
        onChange=${commitSize} onBlur=${commitSize} />
    </label>
    <span class="mem-note">ends at <span class="mem-addr mono">${end}</span></span>
  </div>`;
}

export class MemoryWidget extends DeviceWidget {
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

    return html`<div class="card diskcard memcard">
      ${this.head()}
      <div class="card-body membody">
        ${map ? this.ranges(map) : html`<div class="mem-empty">reading the map…</div>`}
        ${html`<${Placement} d=${this.d} />`}
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
