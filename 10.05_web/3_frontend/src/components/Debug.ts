// Debug: what the processor holds, and as many views of its memory as the
// operator wants open.
//
// The registers come from GET /api/debug/cpu, which reads a processor of the
// board's own directly and otherwise probes the bus and reports what answered —
// so a machine whose processor keeps its registers to itself says so here in
// its own words rather than showing an empty panel.
//
// Below that the operator opens views: a memory view dumps words, a disassembly
// view turns them back into instructions. Both are opened as often as wanted —
// following a data structure and the code that walks it means looking at two
// places at once, which one pane of each could not do. A processor with memory
// management has one more, the page registers of both its modes, and that one
// is a single panel: there is only one set of them to show.
//
// Nothing here polls. The registers a running processor holds change with every
// instruction, so a repeated readout shows numbers that were never all true at
// once — and it is not free: the emulation runs on whatever processor time is
// left over on the board, so anything asking it questions in the background
// slows the machine down. The board reports registers only while the CPU is
// halted, and this page reads when it is opened or told to.
import { html } from '../html';
import { useEffect, useRef, useState } from 'preact/hooks';
import type { ComponentChildren } from 'preact';
import { fetchDebugCpu, fetchDisassembly, fetchMemory } from '../api';
import { useQueryParam } from '../router';
import { octalStr } from '../lib/util';
import type { DebugCpu, DebugInstruction, DebugMmuPages, DebugRegister } from '../types';
import { Bit } from './widgets/base';

const WORD_COUNTS = [8, 16, 32, 64, 128, 256];
const WORDS_PER_ROW = 8;
// Instructions a disassembly view reads at a time, and adds on every "More".
const DISASM_LINES = 10;

// An octal field's value, or null when it is not octal at all. Empty counts as
// zero, so clearing the box reads location 0 rather than refusing.
function parseOctal(s: string): number | null {
  const t = s.trim().replace(/^0+(?=\d)/, '');
  if (t === '') return 0;
  if (!/^[0-7]+$/.test(t)) return null;
  return parseInt(t, 8);
}

/* ---- the processor ------------------------------------------------------ */

// One labelled octal readout, laid out like the console card's.
function Reg({ name, value, width }: { name: string; value: number; width?: number }) {
  return html`<label class="dbg-reg">${name}<span class="cpu-oct">${octalStr(
    value,
    width || 16
  )}</span></label>`;
}

// The status word: the mode the CPU runs in, what it interrupts at, and the
// four condition codes. A model without modes has no mode to show — the bits
// are not zero there, they do not exist — which is what `has_modes` decides.
function PswRow({ cpu }: { cpu: DebugCpu }) {
  const p = cpu.psw;
  if (!p) return null;
  return html`<div class="cpu-flags">
    ${p.has_modes &&
    html`<span class="cpu-mode" title="current mode / previous mode"
      >${(p.mode || '?')[0].toUpperCase()}<span class="cpu-prev"
        >${(p.previous_mode || '?')[0].toUpperCase()}</span
      ></span
    >`}
    <span class="cpu-pri" title="priority the CPU runs at">BR${p.priority}</span>
    <${Bit} lit=${p.t}>T</${Bit}>
    <${Bit} lit=${p.n}>N</${Bit}>
    <${Bit} lit=${p.z}>Z</${Bit}>
    <${Bit} lit=${p.v}>V</${Bit}>
    <${Bit} lit=${p.c}>C</${Bit}>
    ${cpu.mmu && html`<${Bit} lit=${cpu.mmu.enabled} wide=${true}>MMU</${Bit}>`}
  </div>`;
}

// The registers of a processor the board emulates: the eight the program sees,
// then the stack pointer of the mode the CPU is *not* in and the state a
// debugger reads around them.
function Registers({ cpu }: { cpu: DebugCpu }) {
  const regs: DebugRegister[] = cpu.registers || [];
  const sps: DebugRegister[] = cpu.stackpointers || [];
  return html`<div>
    <div class="cpu-regs dbg-regs">
      ${regs.map((r) => html`<${Reg} name=${r.name} value=${r.value} />`)}
    </div>
    <${PswRow} cpu=${cpu} />
    <div class="cpu-regs dbg-regs">
      ${cpu.psw && html`<${Reg} name="PSW" value=${cpu.psw.value} />`}
      ${sps.map((r) => html`<${Reg} name=${r.name} value=${r.value} />`)}
      ${cpu.ir !== undefined && html`<${Reg} name="IR" value=${cpu.ir} />`}
      ${cpu.bus_addr !== undefined && html`<${Reg} name="BA" value=${cpu.bus_addr} />`}
      ${cpu.bus_data !== undefined && html`<${Reg} name="BD" value=${cpu.bus_data} />`}
    </div>
    ${cpu.mmu &&
    html`<div class="cpu-regs dbg-regs">
      <${Reg} name="MMR0" value=${cpu.mmu.mmr0} />
      <${Reg} name="MMR1" value=${cpu.mmu.mmr1} />
      <${Reg} name="MMR2" value=${cpu.mmu.mmr2} />
    </div>`}
  </div>`;
}

// What a probe of the bus found. Every address it tried is listed, answered or
// not: that a location stayed silent is the finding, and leaving it out would
// make the panel look like it had not looked. What each address means on a
// PDP-11 comes from the board, which knows the whole I/O page rather than only
// the points it probes by name.
function ProbeTable({ cpu }: { cpu: DebugCpu }) {
  if (!cpu.probe || !cpu.probe.length) return null;
  return html`<table class="dbg-probe mono">
    <thead>
      <tr><th>address</th><th>register</th><th>read</th></tr>
    </thead>
    <tbody>
      ${cpu.probe.map(
        (p) => html`<tr class=${p.value === null ? 'dbg-silent' : ''}>
          <td>${octalStr(p.address, cpu.addr_width || 18)}</td>
          <td>${p.name || p.info || ''}</td>
          <td>${p.value === null ? 'no answer' : octalStr(p.value, 16)}</td>
        </tr>`
      )}
    </tbody>
  </table>`;
}

// The processor card: the readout when there is one, and otherwise the board's
// own account of why there is not.
function CpuCard({
  cpu,
  onProbe,
  busy,
}: {
  cpu: DebugCpu | null;
  onProbe: () => void;
  busy: boolean;
}) {
  if (!cpu) return html`<div class="card"><div class="card-body">reading the processor…</div></div>`;
  const emulated = cpu.source === 'emulated';
  const title = cpu.model || cpu.device || 'Processor';
  // What the machine is doing is only known where a processor reports it. On a
  // machine whose processor the board cannot see, the HALT line is all there is
  // — and with no processor at all even that says nothing, so rather than
  // labelling silence "running" the chip shows only what the board is sure of.
  const state = cpu.run_state || (!cpu.powered ? 'switched off' : cpu.halted ? 'halted' : '');
  return html`<div class="card">
    <div class="card-head">
      <h3>${title}</h3>
      ${state &&
      html`<span class=${'disk-status ' + (state === 'running' ? 'ok' : 'idle')}>${state}</span>`}
      <span class="pill">${cpu.source}</span>
      <div style="margin-left:auto">
        <button class="btn small" disabled=${busy} onClick=${onProbe}>
          ${emulated ? 'Read again' : 'Probe the bus'}
        </button>
      </div>
    </div>
    <div class="card-body">
      ${cpu.available
        ? html`<${Registers} cpu=${cpu} />`
        : html`<p class="muted dbg-reason">${cpu.reason || 'no processor state is readable'}</p>`}
      ${!cpu.available && html`<${ProbeTable} cpu=${cpu} />`}
      ${cpu.cycle_count !== undefined &&
      html`<p class="muted dbg-note">${cpu.cycle_count.toLocaleString()} instructions since the
        last halt${cpu.powered ? '' : ' · the machine is switched off'}</p>`}
    </div>
  </div>`;
}

/* ---- the views ---------------------------------------------------------- */

// A view the operator opened. The address travels with it so the set of views
// can be put in the URL and come back on a reload.
interface ViewSpec {
  id: number;
  kind: 'mem' | 'dis' | 'mmu';
  addr: string; // octal, as typed; memory and disassembly views only
  words: number; // memory views only
}

// The views as one query parameter: "m777560.40,d146326,u" — kind, address, and
// for a memory view the word count. Short enough to read in the address bar,
// which is the point of putting it there. The page registers have no address of
// their own: the whole set is one panel, so its letter stands alone.
function encodeViews(views: ViewSpec[]): string {
  return views
    .map((v) =>
      v.kind === 'mem'
        ? 'm' + v.addr + '.' + v.words.toString(8)
        : v.kind === 'dis'
          ? 'd' + v.addr
          : 'u'
    )
    .join(',');
}

function decodeViews(s: string): ViewSpec[] {
  const out: ViewSpec[] = [];
  for (const part of s.split(',')) {
    const m = /^([mdu])([0-7]*)(?:\.([0-7]+))?$/.exec(part.trim());
    if (!m) continue;
    out.push({
      id: out.length + 1,
      kind: m[1] === 'm' ? 'mem' : m[1] === 'd' ? 'dis' : 'mmu',
      addr: m[2] || '0',
      words: m[3] ? parseInt(m[3], 8) : 64,
    });
  }
  return out;
}

// The head every view carries: what it is, and the button that closes it.
function ViewHead({
  title,
  onClose,
  children,
}: {
  title: string;
  onClose: () => void;
  children: ComponentChildren;
}) {
  return html`<div class="card-head dbg-view-head">
    <h3>${title}</h3>
    <div class="dbg-view-ctl">${children}</div>
    <button class="btn small dbg-close" title="close this view" onClick=${onClose}>✕</button>
  </div>`;
}

// Rows of eight words with the bytes of each shown beside them, which is how a
// dump is read: the words are what the machine stores and the characters are
// what a person recognises.
//
// A word that nothing answered is a hole in the row rather than a missing row:
// walking the I/O page or the top of a machine's memory means walking past
// addresses that belong to nobody, and where those are is what the operator is
// reading the dump to find out.
function dumpRows(base: number, words: (number | null)[], width: number): ComponentChildren {
  const rows = [];
  for (let i = 0; i < words.length; i += WORDS_PER_ROW) {
    const chunk = words.slice(i, i + WORDS_PER_ROW);
    // low byte first: a PDP-11 word holds its first character in the low half
    const text = chunk
      .flatMap((w) => (w === null ? [null, null] : [w & 0xff, (w >> 8) & 0xff]))
      .map((b) => (b === null ? ' ' : b >= 32 && b < 127 ? String.fromCharCode(b) : '·'))
      .join('');
    rows.push(html`<tr>
      <td class="dbg-addr">${octalStr(base + i * 2, width)}</td>
      ${chunk.map((w) =>
        w === null
          ? // as wide as the six octal digits it stands in for, in plain
            // characters: the cell is preformatted, so this keeps the columns
            // lined up whatever font the page falls back to
            html`<td class="dbg-silent" title="nothing answered this address">------</td>`
          : html`<td>${octalStr(w, 16)}</td>`
      )}
      ${Array.from({ length: WORDS_PER_ROW - chunk.length }, () => html`<td></td>`)}
      <td class="dbg-text">${text}</td>
    </tr>`);
  }
  return rows;
}

function MemoryView({
  spec,
  cpu,
  onChange,
  onClose,
}: {
  spec: ViewSpec;
  cpu: DebugCpu | null;
  onChange: (patch: Partial<ViewSpec>) => void;
  onClose: () => void;
}) {
  const [addrText, setAddrText] = useState(spec.addr);
  const [words, setWords] = useState<(number | null)[]>([]);
  const [base, setBase] = useState(0);
  const [error, setError] = useState('');
  const [busy, setBusy] = useState(false);
  const width = cpu?.addr_width || 18;
  // A switched-off machine grants the board nothing, so a read of it waits out
  // the board's whole timeout to learn what the power flag already said.
  const dark = cpu !== null && !cpu.powered;

  const read = async (text: string, n: number) => {
    const addr = parseOctal(text);
    if (addr === null) {
      setError('an address is octal: digits 0 to 7');
      return;
    }
    setBusy(true);
    setError('');
    // an odd address names the byte inside a word; the dump reads words
    const start = addr & ~1;
    const r = await fetchMemory(start, n);
    setBase(start);
    setWords(r.words);
    setError(r.ok ? '' : r.error);
    setBusy(false);
  };

  useEffect(() => {
    if (cpu === null || dark) return;
    read(spec.addr, spec.words);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [spec.addr, spec.words, cpu === null, dark]);

  const commit = (text: string) => {
    setAddrText(text);
    onChange({ addr: text.trim() || '0' });
  };
  const step = (delta: number) => {
    const cur = parseOctal(addrText) ?? 0;
    commit(Math.max(0, cur + delta).toString(8));
  };
  const pc = (cpu?.registers || []).find((r) => r.name === 'PC');

  return html`<div class="card dbg-view">
    <${ViewHead} title="Memory" onClose=${onClose}>
      <input class="mono dbg-input" value=${addrText} size="9"
        title="start address, octal" aria-label="start address, octal"
        onInput=${(e: Event) => setAddrText((e.target as HTMLInputElement).value)}
        onBlur=${() => commit(addrText)}
        onKeyDown=${(e: KeyboardEvent) => {
          if (e.key === 'Enter') (e.currentTarget as HTMLInputElement).blur();
        }} />
      <select class="mono dbg-input" value=${String(spec.words)}
        title="words to read" aria-label="words to read"
        onChange=${(e: Event) =>
          onChange({ words: parseInt((e.target as HTMLSelectElement).value, 10) })}>
        ${WORD_COUNTS.map((n) => html`<option value=${String(n)}>${n}</option>`)}
      </select>
      <button class="btn small" onClick=${() => step(-spec.words * 2)} disabled=${busy}>−</button>
      <button class="btn small" onClick=${() => step(spec.words * 2)} disabled=${busy}>+</button>
      ${pc && html`<button class="btn small" onClick=${() => commit(octalStr(pc.value, 16))}
        >at PC</button>`}
      <button class="btn small" onClick=${() => read(addrText, spec.words)}
        disabled=${busy}>Read</button>
    </${ViewHead}>
    <div class="card-body">
      ${error && html`<p class="dbg-error">${error}</p>`}
      ${dark &&
      html`<p class="muted dbg-reason">The machine is switched off: it grants the board no bus
        cycles, so there is nothing to read.</p>`}
      ${words.length > 0 &&
      html`<div class="dbg-dump-wrap"><table class="dbg-dump mono">
        ${dumpRows(base, words, width)}
      </table></div>`}
    </div>
  </div>`;
}

// One line of the listing, laid out as a listing is: the address, the words the
// instruction occupies, then what it does. The columns are padded rather than
// laid out in a grid because the whole line is monospace anyway, and a listing
// that can be selected and pasted as text is worth more than one that cannot.
// What the board has to say about an instruction: why it is not available on
// this model, or what the addresses it names mean. "" when it is plain code.
function commentOf(i: DebugInstruction): string {
  if (i.comment) return i.comment;
  return (i.known_addresses || [])
    .map((k) => octalStr(k.address, 16) + ' = ' + k.info)
    .join(', ');
}

function listingLine(i: DebugInstruction, width: number): string {
  const words = i.words.map((w) => octalStr(w, 16)).join(' ');
  return (
    octalStr(i.address, width) +
    '  ' +
    words.padEnd(21) +
    ' ' +
    i.mnemonic.padEnd(7) +
    ' ' +
    i.operands
  );
}

function DisassemblyView({
  spec,
  cpu,
  onChange,
  onClose,
}: {
  spec: ViewSpec;
  cpu: DebugCpu | null;
  onChange: (patch: Partial<ViewSpec>) => void;
  onClose: () => void;
}) {
  const [addrText, setAddrText] = useState(spec.addr);
  const [lines, setLines] = useState<DebugInstruction[]>([]);
  const [next, setNext] = useState<number | null>(null);
  const [model, setModel] = useState('');
  const [error, setError] = useState('');
  const [busy, setBusy] = useState(false);
  const box = useRef<HTMLDivElement | null>(null);
  const width = cpu?.addr_width || 18;
  const dark = cpu !== null && !cpu.powered;

  // `append` is what the More button does: the earlier instructions stay and
  // the new ones go under them, so a listing is followed rather than replaced.
  const read = async (addr: number, append: boolean) => {
    setBusy(true);
    setError('');
    const r = await fetchDisassembly(addr, DISASM_LINES);
    if (!r.ok || !r.listing) {
      setError(r.error);
      setBusy(false);
      return;
    }
    setModel(r.listing.model);
    setLines((old) => (append ? old.concat(r.listing!.instructions) : r.listing!.instructions));
    // Where a following listing continues. A region that ran out of readable
    // memory has nothing to continue into, and says so instead of offering it.
    setNext(r.listing.complete ? r.listing.next : null);
    if (!r.listing.complete) setError(r.listing.reason || '');
    setBusy(false);
  };

  useEffect(() => {
    if (cpu === null || dark) return;
    const addr = parseOctal(spec.addr);
    if (addr === null) {
      setError('an address is octal: digits 0 to 7');
      return;
    }
    read(addr & ~1, false);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [spec.addr, cpu === null, dark]);

  // A listing followed with More grows past its box; keep the newest lines in
  // view, which is where the reading is.
  useEffect(() => {
    const el = box.current;
    if (el && lines.length > DISASM_LINES) el.scrollTop = el.scrollHeight;
  }, [lines.length]);

  const commit = (text: string) => {
    setAddrText(text);
    onChange({ addr: text.trim() || '0' });
  };
  const pc = (cpu?.registers || []).find((r) => r.name === 'PC');

  return html`<div class="card dbg-view">
    <${ViewHead} title=${'Disassembly' + (model ? ' · ' + model : '')} onClose=${onClose}>
      <input class="mono dbg-input" value=${addrText} size="9"
        title="start address, octal" aria-label="start address, octal"
        onInput=${(e: Event) => setAddrText((e.target as HTMLInputElement).value)}
        onBlur=${() => commit(addrText)}
        onKeyDown=${(e: KeyboardEvent) => {
          if (e.key === 'Enter') (e.currentTarget as HTMLInputElement).blur();
        }} />
      ${pc && html`<button class="btn small" onClick=${() => commit(octalStr(pc.value, 16))}
        >at PC</button>`}
      <button class="btn small" disabled=${busy || next === null}
        onClick=${() => next !== null && read(next, true)}>More</button>
    </${ViewHead}>
    <div class="card-body">
      ${error && html`<p class="dbg-error">${error}</p>`}
      ${dark &&
      html`<p class="muted dbg-reason">The machine is switched off: it grants the board no bus
        cycles, so there is nothing to disassemble.</p>`}
      ${lines.length > 0 &&
      html`<div class="dbg-listing mono" ref=${box}>
        ${lines.map((i) => {
          const code = listingLine(i, width);
          const note = commentOf(i);
          // The listing does not scroll sideways — a code column that moves
          // under the reader is worse than one that ends. What does not fit is
          // the comment, so it is the part that is cut, and the whole of it is
          // one hover away. The line carries it too, for an instruction whose
          // own operands reach the edge.
          return html`<div class=${'dbg-code' + (i.available ? '' : ' dbg-code-odd')}
            title=${note ? code + '  ; ' + note : code}>
            <span class="dbg-code-text">${code}</span>
            ${note && html`<span class="dbg-comment" title=${note}>  ; ${note}</span>`}
          </div>`;
        })}
      </div>`}
    </div>
  </div>`;
}

/* ---- the memory management registers ------------------------------------ */

// A page descriptor read out in words: every field the KT11-D has. PDR<2:1> is
// the access field, <3> the direction the page grows in, <14:8> how far it
// reaches, and <6> whether anything has been written into it since the
// descriptor was last loaded. The rest of the word does not exist on this
// model and always reads zero.
//
// The length is given in words rather than in the 32-word blocks PLF counts:
// what the reader is checking is how far the page reaches, and PLF is there
// beside it for whoever is checking the register itself.
const ACF_NAMES = ['non-resident: any access aborts', 'read-only: a write aborts',
  'not implemented: any access aborts', 'read/write'];

function pdrTitle(pdr: number): string {
  const plf = (pdr >> 8) & 0o177;
  const down = (pdr & 0o10) !== 0;
  // upward, blocks 0..PLF are inside the page; downward, PLF..127 are
  const blocks = down ? 128 - plf : plf + 1;
  return (
    ACF_NAMES[(pdr >> 1) & 3] +
    ', expands ' +
    (down ? 'downward' : 'upward') +
    ', ' +
    blocks * 32 +
    ' words (PLF ' +
    plf +
    ')' +
    (pdr & 0o100 ? ', written into' : '')
  );
}

// Where the page address register sends its page. The PAF is a physical address
// in 64-byte units, so the page's first word lands at PAF<<6 whatever virtual
// address the page itself covers.
function parTitle(par: number, page: number, width: number): string {
  return (
    'virtual ' +
    octalStr(page * 0o20000, 16) +
    ' relocates to ' +
    octalStr((par & 0o7777) * 64, width)
  );
}

// The eight pages of both modes, one row per page, the two registers of a mode
// beside each other. Read down a column and it is one mode's map of the whole
// virtual address space; read across a row and it is what the two modes make of
// the same 8K of it.
//
// Which pair is in force is the mode in the status word, and that column is
// marked: the registers of the mode the CPU is not in are as real as the
// others, they are simply not what the next reference goes through.
function MmuTable({
  kernel,
  user,
  mode,
  width,
}: {
  kernel: DebugMmuPages;
  user: DebugMmuPages;
  mode?: string;
  width: number;
}) {
  const live = (m: string) => (m === mode ? ' dbg-mmu-live' : '');
  const cells = (p: DebugMmuPages, m: string, page: number) => [
    html`<td class=${'dbg-mmu-par' + live(m)} title=${parTitle(p.par[page], page, width)}
      >${octalStr(p.par[page], 16)}</td>`,
    html`<td class=${live(m)} title=${pdrTitle(p.pdr[page])}>${octalStr(p.pdr[page], 16)}</td>`,
  ];
  return html`<table class="dbg-mmu mono">
    <thead>
      <tr>
        <th></th>
        <th colspan="2" class=${'dbg-mmu-mode' + live('user')}>user</th>
        <th colspan="2" class=${'dbg-mmu-mode' + live('kernel')}>kernel</th>
      </tr>
      <tr>
        <th>page</th>
        <th class=${'dbg-mmu-par' + live('user')}>PAR</th>
        <th class=${live('user')}>PDR</th>
        <th class=${'dbg-mmu-par' + live('kernel')}>PAR</th>
        <th class=${live('kernel')}>PDR</th>
      </tr>
    </thead>
    <tbody>
      ${Array.from({ length: 8 }, (_, page) => {
        const base = page * 0o20000;
        return html`<tr>
          <td class="dbg-mmu-page"
            title=${'virtual ' + octalStr(base, 16) + ' to ' + octalStr(base + 0o17777, 16)}
            >${page}</td>
          ${cells(user, 'user', page)}
          ${cells(kernel, 'kernel', page)}
        </tr>`;
      })}
    </tbody>
  </table>`;
}

// The panel: the page registers of a processor that has them, and otherwise the
// reason there are none to show. They are internal to the CPU — no bus cycle
// reaches them — so this is the only place they can be seen.
function MmuView({ cpu, onClose }: { cpu: DebugCpu | null; onClose: () => void }) {
  const mmu = cpu?.mmu;
  const pages = mmu && mmu.kernel && mmu.user;
  // The mode the next reference relocates through. A mode field holding one of
  // the two encodings an 11/34 does not have marks no column, which is the
  // truth: neither map is the one that reference would use.
  const mode = cpu?.psw?.has_modes ? cpu.psw.mode : undefined;
  return html`<div class="card dbg-view">
    <${ViewHead} title="Memory management" onClose=${onClose}>
      ${mmu &&
      html`<span class=${'disk-status ' + (mmu.enabled ? 'ok' : 'idle')}
        >${mmu.enabled ? 'relocating' : 'off'}</span>`}
      ${pages && mode && html`<span class="pill">in ${mode} mode</span>`}
    </${ViewHead}>
    <div class="card-body">
      ${!pages &&
      html`<p class="muted dbg-reason">${
        cpu === null
          ? 'reading the processor…'
          : cpu.mmu
            ? 'this processor reports no page registers'
            : cpu.reason ||
              'no memory management registers are readable: this processor either has none or ' +
                'is not holding still to be read'
      }</p>`}
      ${pages &&
      html`<${MmuTable} kernel=${mmu!.kernel!} user=${mmu!.user!} mode=${mode}
        width=${cpu?.addr_width || 18} />`}
      ${pages &&
      !mmu!.enabled &&
      html`<p class="muted dbg-note">Relocation is off: these registers hold what was last
        written into them, and the machine is addressing memory unmapped.</p>`}
    </div>
  </div>`;
}

/* ---- the page ----------------------------------------------------------- */

export function DebugPage() {
  const [cpu, setCpu] = useState<DebugCpu | null>(null);
  const [busy, setBusy] = useState(false);
  // The set of open views lives in the URL, so a reload — or a link to a
  // colleague — brings back the same screen.
  const [viewsQ, setViewsQ] = useQueryParam('v');
  const [views, setViews] = useState<ViewSpec[]>(() => decodeViews(viewsQ || ''));
  const nextId = useRef(views.length + 1);

  const sync = (next: ViewSpec[]) => {
    setViews(next);
    setViewsQ(encodeViews(next));
  };
  const add = (kind: 'mem' | 'dis') => {
    // A new view opens where the operator is most likely to look: the program
    // counter of a halted processor, else the bottom of memory.
    const pc = (cpu?.registers || []).find((r) => r.name === 'PC');
    sync(
      views.concat({
        id: nextId.current++,
        kind,
        addr: pc ? octalStr(pc.value, 16) : '0',
        words: 64,
      })
    );
  };
  const change = (id: number, patch: Partial<ViewSpec>) =>
    sync(views.map((v) => (v.id === id ? { ...v, ...patch } : v)));
  const close = (id: number) => sync(views.filter((v) => v.id !== id));
  // The page registers are one set, not a place in memory: a second panel of
  // them would show the same eight rows, so the button opens the one and closes
  // it again rather than adding.
  const mmuOpen = views.find((v) => v.kind === 'mmu');
  const toggleMmu = () => {
    if (mmuOpen) close(mmuOpen.id);
    else sync(views.concat({ id: nextId.current++, kind: 'mmu', addr: '0', words: 64 }));
  };

  const read = async (probe = false) => {
    setBusy(true);
    setCpu(await fetchDebugCpu(probe));
    setBusy(false);
  };
  // Once, when the page opens. Every later read is one the operator asked for.
  useEffect(() => {
    read();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  return html`<section class="page active" data-page="debug">
    <p class="lede">What the processor holds and what is in its memory. Registers are read while the
      CPU is halted — of a running one they would be numbers that were never all true at once, and
      asking repeatedly costs the machine speed. Memory is read by DMA whether the machine runs or
      not, so the views below work whichever kind of processor it has.</p>

    <${CpuCard} cpu=${cpu} onProbe=${() => read(true)} busy=${busy} />

    <div class="dbg-toolbar">
      <button class="btn" onClick=${() => add('mem')}>New Memory View</button>
      <button class="btn" onClick=${() => add('dis')}>New Disassembly</button>
      ${cpu?.mmu &&
      html`<button class="btn" onClick=${toggleMmu}
        >${mmuOpen ? 'Hide' : 'Show'} Memory Management</button>`}
      ${cpu?.mmu?.enabled &&
      html`<span class="dbg-warn-inline">Memory management is on: these views read physical
        addresses, and the program counter above is a virtual one.</span>`}
    </div>

    <div class="dbg-views">
      ${views.map((v) =>
        v.kind === 'mem'
          ? html`<${MemoryView} key=${v.id} spec=${v} cpu=${cpu}
              onChange=${(p: Partial<ViewSpec>) => change(v.id, p)}
              onClose=${() => close(v.id)} />`
          : v.kind === 'dis'
            ? html`<${DisassemblyView} key=${v.id} spec=${v} cpu=${cpu}
                onChange=${(p: Partial<ViewSpec>) => change(v.id, p)}
                onClose=${() => close(v.id)} />`
            : html`<${MmuView} key=${v.id} cpu=${cpu} onClose=${() => close(v.id)} />`
      )}
      ${views.length === 0 &&
      html`<p class="muted dbg-empty">No views open. Open as many as the work needs — following a
        data structure and the code that walks it means looking at two places at once.</p>`}
    </div>
  </section>`;
}
