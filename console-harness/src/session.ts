/* session.ts: one scripted (or interactive) run against one console.
 *
 * The session holds the transport open for its whole life, anchors matching
 * at the replay/live boundary, accumulates output, paces input on the guest's
 * echo, watches the machine-state events where the target has them, and
 * timestamps everything into the recorder.
 *
 * Echo pacing: the guest's console has no receive FIFO, so a byte sent while
 * the guest is between printing its prompt and issuing its read is lost.
 * A full-duplex guest echoes a character only after its driver has read it,
 * so send one character, wait for echo progress, then send the next. Echo
 * progress is counted in printable characters received since the send mark:
 * the echo is not a byte-identical mirror (BEL and ^U on a rejected line,
 * CR arriving back as CRLF, ^C echoed as two printables), so counting
 * advance is what tolerates it. A delayed echo makes us wait; only a stall
 * past the per-character window resends — resending on a merely slow echo
 * would duplicate the character.
 */
import type { Transport } from "./transport.js";
import {
  compilePattern,
  scan,
  type CompiledPattern,
  type MatchResult,
} from "./matcher.js";
import type { CastRecorder } from "./recording.js";
import type { EventSource, MachineEventName } from "./events.js";

export interface Deviation {
  /** Pattern spec matched against the step's output window. */
  match?: string;
  /** Machine event (halt, power-loss) from the events stream. */
  event?: MachineEventName;
  /** Failure message when this deviation fires. */
  fail: string;
}

export interface InputTuning {
  /** Per-character window before a character is considered dropped. */
  echoTimeoutMs: number;
  /** Total transmission attempts per character (first send + resends). */
  maxSend: number;
  /** Delay before the first character of a line (let the prompt settle). */
  promptSettleMs: number;
  /** Per-character delay in no-echo mode. */
  charDelayMs: number;
  /** Pause after a confirmed character before the next. */
  interCharDelayMs: number;
  /** Pause after the terminating CR. */
  lineSettleMs: number;
}

export const DEFAULT_TUNING: InputTuning = {
  echoTimeoutMs: 800,
  maxSend: 5,
  promptSettleMs: 400,
  charDelayMs: 50,
  interCharDelayMs: 20,
  lineSettleMs: 200,
};

export type InputMode = "echo" | "no-echo" | "raw";

export interface SessionOptions {
  /** Anchor fallback: idle gap after which pre-anchor input is complete. */
  settleMs?: number;
  /** Hard deadline for the anchor when output never goes idle. */
  anchorTimeoutMs?: number;
  /** Default expect timeout. */
  defaultTimeoutMs?: number;
  deviations?: Deviation[];
  input?: Partial<InputTuning>;
  recorder?: CastRecorder;
  events?: EventSource;
  /** Record no-echo input verbatim (default: redacted). */
  recordSecrets?: boolean;
}

export interface ExpectOutcome {
  /** Which of the given patterns matched. */
  index: number;
  match: string;
  /** Output from the step start up to the match. */
  before: string;
}

export interface FailureDiagnostics {
  patterns: string[];
  elapsedMs: number;
  /** Output from the step start to the moment of failure. */
  output: string;
  /** Bounded output tail from before the step. */
  contextTail: string;
}

export class SessionError extends Error {
  constructor(
    message: string,
    public diagnostics?: FailureDiagnostics,
  ) {
    super(
      diagnostics
        ? `${message}\n  waited for: ${diagnostics.patterns.join(" | ")}` +
            `\n  after ${diagnostics.elapsedMs} ms` +
            `\n  output since step start:\n${indent(diagnostics.output)}`
        : message,
    );
    this.name = new.target.name;
  }
}
export class ExpectTimeoutError extends SessionError {}
export class DeviationError extends SessionError {}
export class EchoStallError extends SessionError {}

function indent(text: string): string {
  const shown = text.length > 2000 ? "…" + text.slice(-2000) : text;
  return shown
    .split("\n")
    .map((l) => "    | " + l.replace(/\r/g, ""))
    .join("\n");
}

function delay(ms: number): Promise<void> {
  return new Promise((r) => setTimeout(r, ms));
}

const CONTEXT_TAIL = 500;

export class Session {
  private buf = ""; // latin1 output since the anchor; index == output byte
  private preBuf = ""; // replayed history, kept for diagnostics only
  private cursor = 0; // consumed position in buf (advances on match)
  private anchored = false;
  private closed = false;
  private closeError: Error | undefined;
  private waiters: (() => void)[] = [];
  private eventFlags = new Set<MachineEventName>();
  private tuning: InputTuning;
  private deviations: { spec: Deviation; pattern?: CompiledPattern }[];
  readonly settleMs: number;
  readonly anchorTimeoutMs: number;
  readonly defaultTimeoutMs: number;

  constructor(
    private transport: Transport,
    private opts: SessionOptions = {},
  ) {
    this.tuning = { ...DEFAULT_TUNING, ...opts.input };
    this.settleMs = opts.settleMs ?? 300;
    this.anchorTimeoutMs = opts.anchorTimeoutMs ?? 2000;
    this.defaultTimeoutMs = opts.defaultTimeoutMs ?? 30000;
    this.deviations = (opts.deviations ?? []).map((spec) => ({
      spec,
      pattern: spec.match !== undefined ? compilePattern(spec.match) : undefined,
    }));
  }

  /** Open the transport and wait for the anchor: matching starts here. */
  async open(): Promise<void> {
    this.transport.onData((bytes) => {
      const text = Buffer.from(bytes).toString("latin1");
      if (this.anchored) {
        this.buf += text;
        this.opts.recorder?.output(bytes);
      } else {
        this.preBuf += text;
      }
      this.lastDataMs = Date.now();
      this.wake();
    });
    this.transport.onLive(() => {
      this.anchor();
    });
    this.transport.onClose((err) => {
      this.closed = true;
      this.closeError = err;
      this.wake();
    });
    this.opts.events?.onEvent((ev) => {
      this.eventFlags.add(ev);
      this.wake();
    });
    await this.transport.open();
    await this.waitForAnchor();
  }

  private lastDataMs = 0;

  private anchor(): void {
    if (this.anchored) return;
    this.anchored = true;
    this.opts.recorder?.marker("live");
    this.wake();
  }

  /**
   * The replay boundary. A transport that knows it (the {"live":true} frame,
   * a TCP connect) fires onLive; otherwise the replay is over when the
   * stream has been idle for settleMs, with anchorTimeoutMs as the hard cap
   * for a console that is busy printing right now.
   */
  private async waitForAnchor(): Promise<void> {
    const opened = Date.now();
    this.lastDataMs = opened;
    while (!this.anchored) {
      if (this.closed)
        throw new SessionError(
          `console connection closed before anchoring${this.closeError ? `: ${this.closeError.message}` : ""}`,
        );
      const now = Date.now();
      if (now - opened >= this.anchorTimeoutMs) break;
      if (now - this.lastDataMs >= this.settleMs) break;
      await this.waitChange(
        Math.min(
          this.settleMs - (now - this.lastDataMs),
          this.anchorTimeoutMs - (now - opened),
        ),
      );
    }
    this.anchor();
  }

  private wake(): void {
    const ws = this.waiters;
    this.waiters = [];
    for (const w of ws) w();
  }

  /** Resolve on the next state change (data, close, event) or after ms. */
  private waitChange(ms: number): Promise<void> {
    return new Promise((resolve) => {
      let done = false;
      const timer = setTimeout(() => {
        if (done) return;
        done = true;
        resolve();
      }, Math.max(1, ms));
      this.waiters.push(() => {
        if (done) return;
        done = true;
        clearTimeout(timer);
        resolve();
      });
    });
  }

  /** Output accumulated since the step mark (diagnostics, branching). */
  outputSince(mark: number): string {
    return this.buf.slice(mark);
  }

  /** Current step mark: where the next expect's window begins. */
  mark(): number {
    return this.cursor;
  }

  private diagnostics(
    patterns: string[],
    stepMark: number,
    startedMs: number,
  ): FailureDiagnostics {
    const pre =
      stepMark >= CONTEXT_TAIL
        ? this.buf.slice(stepMark - CONTEXT_TAIL, stepMark)
        : this.preBuf.slice(-(CONTEXT_TAIL - stepMark)) +
          this.buf.slice(0, stepMark);
    return {
      patterns,
      elapsedMs: Date.now() - startedMs,
      output: this.buf.slice(stepMark),
      contextTail: pre,
    };
  }

  /**
   * Wait until one of the patterns matches the output produced since the
   * step began. The expected patterns win over deviations when both match;
   * a deviation (pattern or machine event) fails the step the moment it
   * appears. The match consumes the window up to and including itself.
   */
  async expect(
    patternSpecs: string | string[],
    opts: { timeoutMs?: number; deviations?: Deviation[] } = {},
  ): Promise<ExpectOutcome> {
    const specs = Array.isArray(patternSpecs) ? patternSpecs : [patternSpecs];
    const patterns = specs.map(compilePattern);
    const extraDevs = (opts.deviations ?? []).map((spec) => ({
      spec,
      pattern: spec.match !== undefined ? compilePattern(spec.match) : undefined,
    }));
    const devs = [...this.deviations, ...extraDevs];
    const timeoutMs = opts.timeoutMs ?? this.defaultTimeoutMs;
    const stepMark = this.cursor;
    const started = Date.now();
    const deadline = started + timeoutMs;

    for (;;) {
      const text = this.buf.slice(stepMark);
      const m: MatchResult | null = scan(text, patterns);
      if (m) {
        this.cursor = stepMark + m.end;
        return { index: m.index, match: m.match, before: m.before };
      }
      for (const d of devs) {
        if (d.pattern && d.pattern.regex.test(text))
          throw new DeviationError(
            `deviation: ${d.spec.fail}`,
            this.diagnostics(specs, stepMark, started),
          );
        if (d.spec.event && this.eventFlags.has(d.spec.event))
          throw new DeviationError(
            `deviation: ${d.spec.fail}`,
            this.diagnostics(specs, stepMark, started),
          );
      }
      if (this.closed)
        throw new SessionError(
          `console connection closed${this.closeError ? `: ${this.closeError.message}` : ""}`,
          this.diagnostics(specs, stepMark, started),
        );
      const left = deadline - Date.now();
      if (left <= 0)
        throw new ExpectTimeoutError(
          `timeout waiting for prompt`,
          this.diagnostics(specs, stepMark, started),
        );
      await this.waitChange(Math.min(left, 200));
    }
  }

  private sendByte(ch: string, redact: boolean): number | undefined {
    const bytes = Buffer.from(ch, "latin1");
    this.transport.send(new Uint8Array(bytes));
    return this.opts.recorder?.input(new Uint8Array(bytes), { redact });
  }

  /**
   * Type a line and terminate it with CR. Mode:
   *  - echo (default): paced on the guest's echo, bounded resend on a stall.
   *  - no-echo: fixed per-character delay, single transmission — for
   *    prompts that deliberately do not echo (passwords). Recorded redacted
   *    unless the session was opened with recordSecrets.
   */
  async sendLine(
    text: string,
    opts: {
      mode?: InputMode;
      appendCr?: boolean;
      timeoutMs?: number; // per-character echo window override
    } = {},
  ): Promise<void> {
    const mode = opts.mode ?? "echo";
    if (mode === "raw") {
      await this.send(Buffer.from(text + (opts.appendCr === false ? "" : "\r"), "latin1"));
      return;
    }
    const t = this.tuning;
    const redact = mode === "no-echo" && !this.opts.recordSecrets;
    const inputEvents: number[] = [];
    await delay(t.promptSettleMs);
    const mark = this.buf.length;
    const startedMs = Date.now();

    if (mode === "no-echo") {
      for (const ch of text) {
        const idx = this.sendByte(ch, redact);
        if (idx !== undefined) inputEvents.push(idx);
        await delay(t.charDelayMs);
      }
    } else {
      const echoTimeout = opts.timeoutMs ?? t.echoTimeoutMs;
      const printableSince = () =>
        this.buf.slice(mark).replace(/[\x00-\x1f]/g, "").length;
      let confirmed = 0;
      for (const ch of text) {
        let progressed = false;
        for (let attempt = 0; attempt < t.maxSend; attempt++) {
          const idx = this.sendByte(ch, false);
          if (idx !== undefined && attempt === 0) inputEvents.push(idx);
          const end = Date.now() + echoTimeout;
          while (Date.now() < end && printableSince() <= confirmed)
            await this.waitChange(Math.min(15, end - Date.now()));
          if (printableSince() > confirmed) {
            progressed = true;
            break;
          }
        }
        if (!progressed)
          throw new EchoStallError(
            `character ${JSON.stringify(ch)} not echoed after ${t.maxSend} sends`,
            {
              patterns: [`echo of ${JSON.stringify(text)}`],
              elapsedMs: Date.now() - startedMs,
              output: this.buf.slice(mark),
              contextTail: this.buf.slice(
                Math.max(0, mark - CONTEXT_TAIL),
                mark,
              ),
            },
          );
        confirmed = printableSince();
        await delay(t.interCharDelayMs);
      }
    }

    if (opts.appendCr !== false) {
      const idx = this.sendByte("\r", redact);
      if (idx !== undefined) inputEvents.push(idx);
    }
    await delay(t.lineSettleMs);
    this.opts.recorder?.echoSpan({
      inputEvents,
      outputRange: [mark, this.buf.length],
    });
  }

  /** Raw bytes, unpaced (binary protocols). */
  async send(bytes: Uint8Array): Promise<void> {
    this.transport.send(bytes);
    this.opts.recorder?.input(bytes);
  }

  async sendBreak(): Promise<void> {
    await this.transport.sendBreak();
    this.opts.recorder?.marker("BREAK");
  }

  async close(exitStatus = 0): Promise<void> {
    this.closed = true;
    this.transport.close();
    this.opts.events?.close();
    await this.opts.recorder?.close(exitStatus);
  }
}
