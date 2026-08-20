// The performance panel: what the machine is actually doing, once a second.
//
// The board publishes rates, never totals (see 10.01_base/2_src/arm/metric.hpp):
// a counter's absolute value only says how long the board has been up, and the
// question an operator has is how fast something is going now. A rate arrives
// per device per metric in the `metrics` event; the history behind each
// sparkline is this page's, accumulated from that stream, so it starts when the
// page opens and goes with it.
//
// Rows are one per metric rather than one per device, because a drive's bytes
// and its accesses are two different measurements of it — 256 KB/s in eight
// accesses is a different machine from 256 KB/s in five hundred — and a row
// that tried to carry both would have to pick one trace to draw.
import { html } from '../html';
import type { ComponentChildren } from 'preact';
import { store, useStore } from '../store';
import { METRIC_HISTORY } from '../lib/events';
import type { DevMetric, DevMetrics } from '../types';

// A rate, in as few characters as carry it. Bytes are scaled in 1024s, the way
// every other size on this board is; counts in 1000s, because "12.3 K
// interrupts" means twelve thousand and three hundred to everyone who reads it.
// Three significant figures throughout: a rate measured over one second has no
// more than that, and a number that changes width every sample cannot be read.
function fmtRate(m: DevMetric): { n: string; unit: string } {
  const r = m.rate;
  if (m.unit === 'byte') {
    const scale = [
      [1024 * 1024 * 1024, 'GB/s'],
      [1024 * 1024, 'MB/s'],
      [1024, 'KB/s'],
    ] as const;
    for (const [div, unit] of scale)
      if (r >= div) return { n: sig3(r / div), unit };
    return { n: sig3(r), unit: 'B/s' };
  }
  const unit = m.unit === 'instruction' ? 'instr/s' : '/s';
  if (r >= 1e9) return { n: sig3(r / 1e9), unit: 'G' + unit };
  if (r >= 1e6) return { n: sig3(r / 1e6), unit: 'M' + unit };
  if (r >= 1e3) return { n: sig3(r / 1e3), unit: 'K' + unit };
  return { n: sig3(r), unit };
}

// Three significant figures, without a trailing ".0" on a whole number.
function sig3(v: number): string {
  if (v === 0) return '0';
  if (v >= 100) return String(Math.round(v));
  if (v >= 10) return (Math.round(v * 10) / 10).toFixed(1);
  return (Math.round(v * 100) / 100).toFixed(2);
}

// The trace behind a row: the last minute of samples, drawn against the highest
// of them. The scale is per row and per moment — an absolute scale would flatten
// every trace but the fastest device on the board, and the trace is there to
// show the *shape* of what a device is doing, with the number beside it saying
// how much.
//
// A row whose samples are all zero draws a flat line on the floor rather than
// dividing by nothing.
function Spark({ hist }: { hist: number[] }): ComponentChildren {
  const W = 78,
    H = 20,
    PAD = 1.5;
  if (!hist.length) return html`<svg class="perf-spark" viewBox=${`0 0 ${W} ${H}`}></svg>`;
  const max = Math.max(...hist);
  const span = max > 0 ? max : 1;
  // The x axis is always the full minute, so a trace grows in from the right as
  // the page collects samples instead of stretching a few points across the
  // card and redrawing the whole shape every second.
  const step = W / Math.max(1, METRIC_HISTORY - 1);
  const x0 = W - (hist.length - 1) * step;
  const y = (v: number) => PAD + (H - 2 * PAD) * (1 - v / span);
  const pts = hist.map((v, i) => `${(x0 + i * step).toFixed(1)},${y(v).toFixed(1)}`);
  // the filled area under the line reads at a glance where a bare polyline of
  // one-pixel spikes does not
  const area = `${x0.toFixed(1)},${H} ` + pts.join(' ') + ` ${W},${H}`;
  return html`<svg class="perf-spark" viewBox=${`0 0 ${W} ${H}`} preserveAspectRatio="none"
    aria-hidden="true">
    <polygon class="perf-spark-fill" points=${area} />
    <polyline class="perf-spark-line" points=${pts.join(' ')} />
  </svg>`;
}

// One metric of one device: what it is, how fast it is going, and the minute
// behind it.
function Row({ d, m }: { d: DevMetrics; m: DevMetric }): ComponentChildren {
  const { n, unit } = fmtRate(m);
  const hist = store.metrics.history[d.dev + '/' + m.name] || [];
  // The percentage is against the machine being emulated, and only an emulated
  // processor has one to be against. It is deliberately vague — the reference is
  // an average instruction of a model whose instructions differed by a factor of
  // three — so it is written as "about", and the exact figure it is against is
  // in the title for anyone who wants to argue with it.
  const pct =
    typeof m.pct === 'number'
      ? html`<span class="perf-pct" title=${`about ${Math.round(m.reference || 0).toLocaleString()} instructions a second`}
          >about ${m.pct >= 10 ? Math.round(m.pct) : Math.round(m.pct * 10) / 10}% of a ${d.type}</span>`
      : null;
  return html`<div class="perf-row">
    <span class="perf-label">${m.label}</span>
    <span class="perf-num mono">${n}</span>
    <span class="perf-unit">${unit}</span>
    ${pct}
    <${Spark} hist=${hist} />
  </div>`;
}

// What each kind of device is called at the head of its block. A device whose
// category is not named here is headed by its category, so a device family
// added later still reads.
const KIND_LABEL: Record<string, string> = {
  cpu: 'Processor',
  disk: 'Disk',
  tape: 'Tape',
  network: 'Network',
};

export function PerfCard(): ComponentChildren {
  useStore();
  const { devs, seen } = store.metrics;
  const powered = store.hw.powered;

  let body: ComponentChildren;
  if (powered === false)
    // Not an absence of data: the cards are off the bus and their counters
    // cannot move, so there is nothing to measure rather than nothing measured.
    body = html`<p class="perf-empty">The machine is switched off.</p>`;
  else if (!seen) body = html`<p class="perf-empty">Waiting for the first sample…</p>`;
  else if (!devs.length)
    body = html`<p class="perf-empty">No device is reporting. Nothing enabled counts anything yet.</p>`;
  else
    body = devs.map(
      (d) => html`<div class="perf-dev" key=${d.dev}>
        <div class="perf-dev-head">
          <span class="perf-dev-name">${d.dev}</span>
          <span class="perf-dev-type mono">${d.type}</span>
          <span class="perf-dev-kind">${KIND_LABEL[d.kind] || d.kind}</span>
        </div>
        ${d.metrics.map((m) => html`<${Row} d=${d} m=${m} key=${m.name} />`)}
      </div>`
    );

  return html`<div class="card perf-card">
    <div class="card-head"><h3>Performance</h3>
      <span class="spacer"></span>
      <span class="muted perf-cadence">last second</span>
    </div>
    <div class="card-body perf-body">${body}</div>
  </div>`;
}
