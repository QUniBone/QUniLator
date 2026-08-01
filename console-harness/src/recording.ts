/* recording.ts: asciicast v3 writer and reader, plus the annotation sidecar.
 *
 * The .cast file is strictly standard asciicast v3 (NDJSON: a header object,
 * then [interval, code, data] events with intervals relative to the previous
 * event in seconds), so any player takes it. What the format has no field
 * for — which output bytes are the guest's echo of which input, step records,
 * redaction spans — rides in <file>.notes.json next to it.
 *
 * Bytes are recorded as latin1-decoded strings: the guests speak 7-bit ASCII
 * and latin1 round-trips every byte value through JSON.
 */
import { createWriteStream, readFileSync, writeFileSync, WriteStream } from "node:fs";

export interface CastHeader {
  cols?: number;
  rows?: number;
  termType?: string;
  title?: string;
  env?: Record<string, string>;
}

export interface EchoSpan {
  /** Indexes (0-based, in event order) of the 'i' events this span covers. */
  inputEvents: number[];
  /** Byte range [start, end) of echoed output, counted over all 'o' data. */
  outputRange: [number, number];
}

export interface StepRecord {
  index: number;
  name?: string;
  outcome: "matched" | "failed" | "sent";
  detail?: string;
  startMs: number;
  endMs: number;
}

export interface CastNotes {
  echoSpans: EchoSpan[];
  steps: StepRecord[];
  /** 'i' event indexes whose data was redacted (no-echo input). */
  redactedInputs: number[];
}

export class CastRecorder {
  private stream: WriteStream;
  private lastMs: number;
  private startMs: number;
  private eventCount = 0;
  private outputBytes = 0;
  private notes: CastNotes = { echoSpans: [], steps: [], redactedInputs: [] };
  private closed = false;

  constructor(
    readonly path: string,
    header: CastHeader = {},
    private now: () => number = Date.now,
  ) {
    this.stream = createWriteStream(path);
    this.startMs = this.now();
    this.lastMs = this.startMs;
    const h: Record<string, unknown> = {
      version: 3,
      term: {
        cols: header.cols ?? 80,
        rows: header.rows ?? 24,
        type: header.termType ?? "vt100",
      },
      timestamp: Math.floor(this.startMs / 1000),
    };
    if (header.title !== undefined) h["title"] = header.title;
    if (header.env !== undefined) h["env"] = header.env;
    this.stream.write(JSON.stringify(h) + "\n");
  }

  private writeEvent(code: string, data: string): number {
    const nowMs = this.now();
    const interval = Math.max(0, nowMs - this.lastMs) / 1000;
    this.lastMs = nowMs;
    this.stream.write(JSON.stringify([interval, code, data]) + "\n");
    return this.eventCount++;
  }

  /** Guest output. Returns the byte offset of this chunk in the output stream. */
  output(bytes: Uint8Array): number {
    const at = this.outputBytes;
    this.outputBytes += bytes.length;
    this.writeEvent("o", Buffer.from(bytes).toString("latin1"));
    return at;
  }

  /** Position in the output byte stream (for echo spans). */
  outputPosition(): number {
    return this.outputBytes;
  }

  /**
   * Operator/script input as sent. Redacted input is written as one bullet
   * per byte so the typing rhythm survives while the bytes do not.
   */
  input(bytes: Uint8Array, opts: { redact?: boolean } = {}): number {
    const text = opts.redact
      ? "•".repeat(bytes.length)
      : Buffer.from(bytes).toString("latin1");
    const idx = this.writeEvent("i", text);
    if (opts.redact) this.notes.redactedInputs.push(idx);
    return idx;
  }

  marker(label: string): void {
    this.writeEvent("m", label);
  }

  echoSpan(span: EchoSpan): void {
    this.notes.echoSpans.push(span);
  }

  step(rec: StepRecord): void {
    this.notes.steps.push(rec);
  }

  elapsedMs(): number {
    return this.now() - this.startMs;
  }

  /** Finish the cast (exit status) and write the sidecar. */
  close(exitStatus = 0): Promise<void> {
    if (this.closed) return Promise.resolve();
    this.closed = true;
    this.writeEvent("x", String(exitStatus));
    writeFileSync(this.notesPath(), JSON.stringify(this.notes, null, 2) + "\n");
    return new Promise((resolve, reject) => {
      this.stream.end((err?: Error | null) => (err ? reject(err) : resolve()));
    });
  }

  notesPath(): string {
    return this.path + ".notes.json";
  }
}

// ---- reader (tests, and later the renderer) -------------------------------

export interface CastEvent {
  interval: number;
  code: string;
  data: string;
}

export interface Cast {
  header: Record<string, unknown>;
  events: CastEvent[];
}

export function readCast(path: string): Cast {
  const lines = readFileSync(path, "utf8").split("\n").filter((l) => l.trim());
  if (lines.length === 0) throw new Error(`${path}: empty cast`);
  const header = JSON.parse(lines[0]) as Record<string, unknown>;
  if (header["version"] !== 3)
    throw new Error(`${path}: not an asciicast v3 file`);
  const events: CastEvent[] = [];
  for (const line of lines.slice(1)) {
    const ev = JSON.parse(line) as [number, string, string];
    if (!Array.isArray(ev) || ev.length !== 3)
      throw new Error(`${path}: malformed event: ${line}`);
    events.push({ interval: ev[0], code: ev[1], data: ev[2] });
  }
  return { header, events };
}

export function readNotes(castPath: string): CastNotes {
  return JSON.parse(readFileSync(castPath + ".notes.json", "utf8")) as CastNotes;
}
