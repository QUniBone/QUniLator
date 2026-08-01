/* mock-guest.ts: an in-memory Transport that behaves like a guest on a
 * FIFO-less console SLU. It scripts the faults the harness exists to
 * survive: echo after a delay, a dropped character (a byte arriving while
 * the guest is not reading), CR echoed as CRLF, a rejected line answered
 * with BEL + ^U, deliberate non-echo (a password prompt), and a replayed
 * history prefix on connect.
 */
import type { Transport } from "../src/transport.js";

export interface MockGuestOptions {
  /** Echo received characters (default true). */
  echo?: boolean;
  /** Delay before a character's echo appears. */
  echoDelayMs?: number;
  /** Echo CR as CRLF (what a real terminal driver does; default true). */
  crlf?: boolean;
  /** Swallow the next N received bytes entirely (the no-FIFO drop). */
  dropNext?: number;
  /** Bytes emitted on open, before the live boundary (the ring replay). */
  replay?: string;
  /** Emit the live signal after the replay (the {"live":true} frame). */
  live?: boolean;
}

export class MockGuest implements Transport {
  readonly label = "mock";
  /** Everything the guest actually received (survived the drop). */
  received = "";
  breaks = 0;
  private dataCbs: ((b: Uint8Array) => void)[] = [];
  private liveCbs: (() => void)[] = [];
  private closeCbs: ((err?: Error) => void)[] = [];
  private lineBuf = "";
  private lineCbs: ((line: string) => void)[] = [];
  private opts: Required<Omit<MockGuestOptions, "replay">> & {
    replay?: string;
  };

  constructor(opts: MockGuestOptions = {}) {
    this.opts = {
      echo: opts.echo ?? true,
      echoDelayMs: opts.echoDelayMs ?? 5,
      crlf: opts.crlf ?? true,
      dropNext: opts.dropNext ?? 0,
      live: opts.live ?? false,
      replay: opts.replay,
    };
  }

  open(): Promise<void> {
    queueMicrotask(() => {
      if (this.opts.replay !== undefined && this.opts.replay.length > 0)
        this.emit(this.opts.replay);
      if (this.opts.live) for (const cb of this.liveCbs) cb();
    });
    return Promise.resolve();
  }

  /** Guest output toward the console. */
  output(text: string): void {
    this.emit(text);
  }

  /** Called with each completed input line (CR-terminated, CR stripped). */
  onLine(cb: (line: string) => void): void {
    this.lineCbs.push(cb);
  }

  private emit(text: string): void {
    const bytes = new Uint8Array(Buffer.from(text, "latin1"));
    for (const cb of this.dataCbs) cb(bytes);
  }

  send(bytes: Uint8Array): void {
    for (const b of bytes) {
      if (this.opts.dropNext > 0) {
        this.opts.dropNext--;
        continue; // lost: the guest was not reading and there is no FIFO
      }
      const ch = String.fromCharCode(b);
      this.received += ch;
      if (this.opts.echo) {
        const echoed = ch === "\r" && this.opts.crlf ? "\r\n" : ch;
        setTimeout(() => this.emit(echoed), this.opts.echoDelayMs);
      }
      if (ch === "\r") {
        const line = this.lineBuf;
        this.lineBuf = "";
        for (const cb of this.lineCbs) cb(line);
      } else {
        this.lineBuf += ch;
      }
    }
  }

  sendBreak(): Promise<void> {
    this.breaks++;
    return Promise.resolve();
  }

  close(): void {
    for (const cb of this.closeCbs) cb();
  }

  onData(cb: (b: Uint8Array) => void): void {
    this.dataCbs.push(cb);
  }
  onLive(cb: () => void): void {
    this.liveCbs.push(cb);
  }
  onClose(cb: (err?: Error) => void): void {
    this.closeCbs.push(cb);
  }
}

/** Input tuning that keeps the test suite fast without changing semantics. */
export const FAST_INPUT = {
  echoTimeoutMs: 120,
  maxSend: 5,
  promptSettleMs: 5,
  charDelayMs: 2,
  interCharDelayMs: 1,
  lineSettleMs: 5,
};
