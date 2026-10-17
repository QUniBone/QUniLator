// Replaying a register the machine moves faster than the status poll samples
// it. The board sends each poll's changes as a trace - the values in the order
// written and when, in microseconds of its own clock - and a player shows them
// at that pace, a fixed delay behind the machine, so a pattern a program
// shifts sixty times a second moves on the screen as it does on the bus.
//
// The player paints the lamps itself, a frame at a time, without a render: a
// dashboard re-rendered sixty times a second would cost more than the lamps
// are worth.

export interface Trace {
  at: number[]; // board time of each change, in microseconds
  values: number[];
}

type Listener = (t: Trace, arrivalUs: number) => void;
const listeners = new Map<string, Set<Listener>>();
const key = (dev: string, param: string) => dev + '/' + param;

export function onTrace(dev: string, param: string, fn: Listener): () => void {
  const k = key(dev, param);
  let set = listeners.get(k);
  if (!set) listeners.set(k, (set = new Set()));
  set.add(fn);
  return () => set!.delete(fn);
}

// A trace event off /ws/events, handed to whoever is showing that parameter.
export function applyTrace(ev: {
  dev: string;
  param: string;
  at: number[];
  values: number[];
}): void {
  const set = listeners.get(key(ev.dev, ev.param));
  if (!set || !set.size) return;
  const arrival = performance.now() * 1000;
  const t: Trace = { at: ev.at || [], values: ev.values || [] };
  for (const fn of set) fn(t, arrival);
}

// How far behind the board a change is shown: one poll interval for it to be
// sent at all, and room for the network to deliver it late.
const DELAY_US = 180_000;
// How many recent arrivals the clock offset is judged over. The smallest gap
// between a change and its arrival is the one with the least delivery delay,
// so the minimum over a window follows the board's clock without the jitter.
const OFFSET_WINDOW = 32;

export class TracePlayer {
  private queue: { at: number; v: number }[] = [];
  private offsets: number[] = [];
  private frame = 0;
  private steady = 0;

  // `paint` shows one value; `steady` is shown whenever nothing is replaying.
  constructor(private paint: (v: number) => void) {}

  // The value the parameter stands at. It is what the lamps settle on once a
  // replay has run out, and what they show at once when nothing is playing.
  setSteady(v: number): void {
    this.steady = v;
    if (!this.frame) this.paint(v);
  }

  push(t: Trace, arrivalUs: number): void {
    const n = Math.min(t.at.length, t.values.length);
    if (!n) return;
    this.offsets.push(arrivalUs - t.at[n - 1]);
    if (this.offsets.length > OFFSET_WINDOW) this.offsets.shift();
    for (let i = 0; i < n; i++) this.queue.push({ at: t.at[i], v: t.values[i] });
    if (!this.frame) this.frame = requestAnimationFrame(this.tick);
  }

  // Drop whatever is queued and show the steady value: the machine has gone
  // dark, or the card is going away.
  stop(): void {
    if (this.frame) cancelAnimationFrame(this.frame);
    this.frame = 0;
    this.queue = [];
    this.paint(this.steady);
  }

  private tick = (): void => {
    const offset = Math.min(...this.offsets);
    const boardNow = performance.now() * 1000 - offset - DELAY_US;
    let shown: number | undefined;
    while (this.queue.length && this.queue[0].at <= boardNow) shown = this.queue.shift()!.v;
    if (shown !== undefined) this.paint(shown);
    if (this.queue.length) {
      this.frame = requestAnimationFrame(this.tick);
    } else {
      this.frame = 0;
      this.paint(this.steady);
    }
  };
}
