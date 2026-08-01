/* timings.ts: what a run actually took, per step.
 *
 * A deadline picked by hand is either so loose that a stuck step costs
 * minutes before it says so, or so tight that a slow day fails a run that
 * would have worked. Neither is a judgement worth making from memory: a
 * recording already holds what every step took, so the deadline for the next
 * run comes from the last one.
 *
 * A step that ran several times (a dialog loop) is reported at its slowest,
 * because that is the one a deadline has to cover.
 */
import { readCast, readNotes, type StepRecord } from "./recording.js";

export interface StepTiming {
  index: number;
  name?: string;
  runs: number;
  slowestMs: number;
  totalMs: number;
  outcome: string;
  /** What to set this step's timeout to, from the slowest run. */
  suggestedMs: number;
}

export interface RunTimings {
  steps: StepTiming[];
  totalMs: number;
}

/**
 * Headroom over the slowest observed run. A guest is not a metronome — a
 * busier moment, a fuller disk, a longer directory all stretch a step — so
 * the suggestion is a multiple of what was seen, with a floor for the steps
 * that answered instantly and would otherwise get a deadline of nothing.
 */
export const DEFAULT_MARGIN = 3;
export const DEFAULT_FLOOR_MS = 3000;

export function suggestMs(
  slowestMs: number,
  margin = DEFAULT_MARGIN,
  floorMs = DEFAULT_FLOOR_MS,
): number {
  const raw = Math.max(floorMs, Math.ceil(slowestMs * margin));
  // Round to something a person would write in a script.
  const steps = [3000, 5000, 10000, 15000, 30000, 45000, 60000, 90000, 120000,
    180000, 300000, 600000];
  for (const s of steps) if (raw <= s) return s;
  return Math.ceil(raw / 60000) * 60000;
}

export function humanMs(ms: number): string {
  if (ms < 1000) return `${Math.round(ms)}ms`;
  if (ms < 60000) return `${(ms / 1000).toFixed(1)}s`;
  const m = Math.floor(ms / 60000);
  return `${m}m${Math.round((ms % 60000) / 1000)}s`;
}

/** Duration syntax a step file takes. */
export function asDuration(ms: number): string {
  if (ms % 60000 === 0) return `${ms / 60000}m`;
  if (ms % 1000 === 0) return `${ms / 1000}s`;
  return `${ms}ms`;
}

export function runTimings(
  castPath: string,
  opts: { margin?: number; floorMs?: number } = {},
): RunTimings {
  const cast = readCast(castPath);
  let records: StepRecord[] = [];
  try {
    records = readNotes(castPath).steps;
  } catch {
    throw new Error(
      `${castPath}: no step records (a run made by qcon writes them to <cast>.notes.json)`,
    );
  }
  const byKey = new Map<string, StepTiming>();
  for (const r of records) {
    const key = r.name ?? `#${r.index + 1}`;
    const ms = Math.max(0, r.endMs - r.startMs);
    const seen = byKey.get(key);
    if (seen === undefined) {
      byKey.set(key, {
        index: r.index,
        name: r.name,
        runs: 1,
        slowestMs: ms,
        totalMs: ms,
        outcome: r.outcome,
        suggestedMs: suggestMs(ms, opts.margin, opts.floorMs),
      });
    } else {
      seen.runs++;
      seen.totalMs += ms;
      if (ms > seen.slowestMs) {
        seen.slowestMs = ms;
        seen.suggestedMs = suggestMs(ms, opts.margin, opts.floorMs);
      }
      if (r.outcome === "failed") seen.outcome = "failed";
    }
  }
  const steps = [...byKey.values()].sort((a, b) => a.index - b.index);
  // The session's own length, from the cast's event intervals.
  const totalMs =
    cast.events.reduce((t, e) => t + e.interval, 0) * 1000;
  return { steps, totalMs };
}

/** A table for the terminal, and the timeout line each step should carry. */
export function formatTimings(t: RunTimings): string {
  const rows = t.steps.map((s) => ({
    step: `${s.index + 1}${s.name ? ` ${s.name}` : ""}`,
    runs: s.runs > 1 ? `x${s.runs}` : "",
    took: humanMs(s.slowestMs),
    suggest: asDuration(s.suggestedMs),
    outcome: s.outcome,
  }));
  const w = (k: keyof (typeof rows)[0]) =>
    Math.max(k.length, ...rows.map((r) => r[k].length));
  const pad = (s: string, n: number) => s + " ".repeat(Math.max(0, n - s.length));
  const wStep = Math.max(4, w("step"));
  const wRuns = Math.max(4, w("runs"));
  const wTook = Math.max(7, w("took"));
  const wSug = Math.max(7, w("suggest"));
  const head =
    `${pad("step", wStep)}  ${pad("runs", wRuns)}  ` +
    `${pad("slowest", wTook)}  ${pad("timeout", wSug)}  outcome`;
  const body = rows
    .map(
      (r) =>
        `${pad(r.step, wStep)}  ${pad(r.runs, wRuns)}  ` +
        `${pad(r.took, wTook)}  ${pad(r.suggest, wSug)}  ${r.outcome}`,
    )
    .join("\n");
  return (
    `${head}\n${"-".repeat(head.length)}\n${body}\n` +
    `\nsession ${humanMs(t.totalMs)}; "timeout" is the slowest run of each ` +
    `step with headroom — a step that answers in milliseconds still gets a ` +
    `few seconds, so a deadline never fires on a slow moment.\n`
  );
}
