// Debug: what the processor holds, and what is in memory.
//
// One screen for both kinds of machine. The registers come from
// GET /api/debug/cpu, which reads a processor of the board's own directly and
// otherwise probes the bus and reports what answered — so a machine whose
// processor keeps its registers to itself says so here in its own words rather
// than showing an empty panel. The memory pane is the same for either: the
// board is bus master, so it DMAs the words out without the CPU.
import { html } from '../html';
import { useEffect, useState } from 'preact/hooks';
import type { ComponentChildren } from 'preact';
import { fetchDebugCpu, fetchMemory } from '../api';
import { useQueryParam } from '../router';
import { octalStr } from '../lib/util';
import type { DebugCpu, DebugRegister } from '../types';
import { Bit } from './widgets/base';

// Nothing here polls. The registers a running processor holds change with every
// instruction, so a repeated readout shows numbers that were never all true at
// once — and it is not free: the emulation runs on whatever processor time is
// left over on the board, so anything asking it questions in the background
// slows the machine down. The board reports registers only while the CPU is
// halted, and this page reads them when it is opened or told to.

const WORD_COUNTS = [8, 16, 32, 64, 128, 256];
const WORDS_PER_ROW = 8;

// An octal field's value, or null when it is not octal at all. Empty counts as
// zero, so clearing the box reads location 0 rather than refusing.
function parseOctal(s: string): number | null {
  const t = s.trim().replace(/^0+(?=\d)/, '');
  if (t === '') return 0;
  if (!/^[0-7]+$/.test(t)) return null;
  return parseInt(t, 8);
}

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
// make the panel look like it had not looked.
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
          <td>${p.name || ''}</td>
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

// The memory pane. Rows of eight words with the bytes of each shown beside
// them, which is how a dump is read: the words are what the machine stores and
// the characters are what a person recognises.
//
// A word that nothing answered is a hole in the row rather than a missing row:
// walking the I/O page or the top of a machine's memory means walking past
// addresses that belong to nobody, and where those are is what the operator is
// reading the dump to find out.
function dumpRows(
  base: number,
  words: (number | null)[],
  width: number
): ComponentChildren {
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

export function DebugPage() {
  const [cpu, setCpu] = useState<DebugCpu | null>(null);
  const [busy, setBusy] = useState(false);

  // The screen reproduces from its URL: where the dump starts and how much of
  // it, in the octal an operator types.
  const [addrQ, setAddrQ] = useQueryParam('addr');
  const [countQ, setCountQ] = useQueryParam('n');
  const [addrText, setAddrText] = useState(addrQ || '0');
  const count = parseInt(countQ || '', 10) || 64;
  const [words, setWords] = useState<(number | null)[]>([]);
  const [memBase, setMemBase] = useState(0);
  const [memError, setMemError] = useState('');
  const [memBusy, setMemBusy] = useState(false);

  const read = async (probe = false) => {
    setBusy(true);
    const c = await fetchDebugCpu(probe);
    setCpu(c);
    setBusy(false);
  };

  // Once, when the page opens. Every later read is one the operator asked for.
  useEffect(() => {
    read();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  const readMemory = async (text: string, n: number) => {
    const addr = parseOctal(text);
    if (addr === null) {
      setMemError('an address is octal: digits 0 to 7');
      return;
    }
    setMemBusy(true);
    setMemError('');
    // an odd address names the byte inside a word; the dump reads words
    const base = addr & ~1;
    const r = await fetchMemory(base, n);
    setMemBase(base);
    setWords(r.words);
    setMemError(r.ok ? '' : r.error);
    setMemBusy(false);
  };

  // A switched-off machine grants the board nothing, so a read of it waits out
  // the board's whole timeout to learn what the power flag already said. The
  // pane says so instead, and the Read button is still there for an operator
  // who wants the cycles made anyway.
  const dark = cpu !== null && !cpu.powered;
  useEffect(() => {
    if (cpu === null || dark) return;
    readMemory(addrQ || '0', count);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [addrQ, countQ, cpu === null, dark]);

  const commitAddr = () => setAddrQ(addrText.trim() || '0');
  const step = (delta: number) => {
    const cur = parseOctal(addrText) ?? 0;
    const next = Math.max(0, cur + delta);
    setAddrText(next.toString(8));
    setAddrQ(next.toString(8));
  };
  // The program counter is where an operator wants to look first, and typing it
  // over from the readout above is exactly the transcription a panel should
  // save. Only offered when a processor actually publishes one.
  const pc = (cpu?.registers || []).find((r) => r.name === 'PC');

  const width = cpu?.addr_width || 18;
  return html`<section class="page active" data-page="debug">
    <p class="lede">What the processor holds and what is in memory. Registers are read while the CPU
      is halted — of a running one they would be numbers that were never all true at once, and
      asking repeatedly costs the machine speed. Memory is read by DMA whether the machine runs or
      not, so the pane works whichever kind of processor it has.</p>

    <${CpuCard} cpu=${cpu} onProbe=${() => read(true)} busy=${busy} />

    <div class="card" style="margin-top:14px">
      <div class="card-head"><h3>Memory</h3>
        <div class="dbg-mem-ctl">
          <label class="dbg-field">address<input class="mono" value=${addrText} size="9"
            onInput=${(e: Event) => setAddrText((e.target as HTMLInputElement).value)}
            onBlur=${commitAddr}
            onKeyDown=${(e: KeyboardEvent) => {
              if (e.key === 'Enter') (e.currentTarget as HTMLInputElement).blur();
            }} /></label>
          <label class="dbg-field">words<select class="mono" value=${String(count)}
            onChange=${(e: Event) => setCountQ((e.target as HTMLSelectElement).value)}>
            ${WORD_COUNTS.map((n) => html`<option value=${String(n)}>${n}</option>`)}
          </select></label>
          <button class="btn small" onClick=${() => step(-count * 2)} disabled=${memBusy}>−</button>
          <button class="btn small" onClick=${() => step(count * 2)} disabled=${memBusy}>+</button>
          ${pc &&
          html`<button class="btn small" onClick=${() => {
            setAddrText(octalStr(pc.value, 16));
            setAddrQ(pc.value.toString(8));
          }}>at PC</button>`}
          <button class="btn small" onClick=${() => readMemory(addrText, count)}
            disabled=${memBusy}>Read</button>
        </div>
      </div>
      <div class="card-body">
        ${memError && html`<p class="dbg-error">${memError}</p>`}
        ${dark &&
        html`<p class="muted dbg-reason">The machine is switched off: it grants the board no bus
          cycles, so there is nothing to read. Reading anyway waits out the board's timeout and
          then says the same thing.</p>`}
        ${cpu?.mmu?.enabled &&
        html`<p class="dbg-warn">Memory management is on. The program counter above is a virtual
          address and this pane reads physical ones, so the words at the same number are not the
          instructions the CPU is running — “at PC” lands on the raw number, not on the code.</p>`}
        ${words.length > 0 &&
        html`<div class="dbg-dump-wrap"><table class="dbg-dump mono">
          ${dumpRows(memBase, words, width)}
        </table></div>`}
        <p class="muted dbg-note">Read over the bus as ${width}-bit physical addresses — what a card
          answers, not what a program with memory management on would see at the same number.</p>
      </div>
    </div>
  </section>`;
}
