// Replaying a register the machine moves faster than the status poll samples
// it. The board sends each poll's changes as a trace - the values in the order
// written and when, in microseconds of its own clock - and a player shows them
// at that pace behind the machine, so a pattern a program shifts sixty times a
// second moves on the screen as it does on the bus. The delay is one poll
// interval plus however much delivery time varies on the network the page is
// on: a few milliseconds beside the board, more across the internet.
//
// The player paints the lamps itself, a frame at a time, without a render: a
// dashboard re-rendered sixty times a second would cost more than the lamps
// are worth.

export interface Trace {
  at: number[]; // board time of each change, in microseconds
  values: number[];
  sent?: number; // board time the frame left, on the same clock
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
  now?: number;
}): void {
  const set = listeners.get(key(ev.dev, ev.param));
  if (!set || !set.size) return;
  const arrival = performance.now() * 1000;
  const t: Trace = { at: ev.at || [], values: ev.values || [], sent: ev.now };
  for (const fn of set) fn(t, arrival);
}

// How far behind the board a change is shown. A change waits up to one poll
// interval to be sent; past that, the delay covers how much later than the
// quickest one a frame arrives, at the 95th percentile, plus a margin. Bounded
// below so a replay never runs into the poll, and above so a bad network costs
// smoothness rather than a lamp that lags the machine by seconds.
const POLL_US = 100_000;
const MARGIN_US = 20_000;
const MIN_DELAY_US = 120_000;
const MAX_DELAY_US = 500_000;
// A board that sends no "now" leaves only the last change's time to judge
// delivery by, which already varies by a poll interval; such a trace is shown
// this fixed distance behind.
const FIXED_DELAY_US = 180_000;
// How many recent arrivals delivery is judged over, about six seconds of a
// moving display. The smallest gap between sending and arrival is the one with
// the least delivery delay, so the minimum over the window follows the board's
// clock and the spread above it is the jitter.
const OFFSET_WINDOW = 64;
// How far a falling delay moves toward its target per trace: a delay that drops
// at once would skip whatever lies between, so it comes down over a couple of
// seconds, and goes up at once, where the cost is a short pause.
const DELAY_EASE = 0.05;

export class TracePlayer {
  private queue: { at: number; v: number }[] = [];
  private offsets: number[] = [];
  private frame = 0;
  private steady = 0;
  private delay = FIXED_DELAY_US;

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
    this.offsets.push(arrivalUs - (t.sent ?? t.at[n - 1]));
    if (this.offsets.length > OFFSET_WINDOW) this.offsets.shift();
    if (t.sent !== undefined) {
      const sorted = [...this.offsets].sort((a, b) => a - b);
      const jitter = sorted[Math.floor((sorted.length - 1) * 0.95)] - sorted[0];
      const target = Math.min(
        MAX_DELAY_US,
        Math.max(MIN_DELAY_US, POLL_US + jitter + MARGIN_US)
      );
      this.delay = target > this.delay ? target : this.delay + (target - this.delay) * DELAY_EASE;
    }
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
    const boardNow = performance.now() * 1000 - offset - this.delay;
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
