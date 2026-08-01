/* screen.ts: what the guest's output would put on a terminal screen.
 *
 * A line-oriented dialog is matched against the byte stream, because the
 * stream is what the guest appended. A full-screen program is not: it moves
 * the cursor and redraws in place, so the bytes carry motion and erasure
 * rather than the text now on the screen. Matching those needs the screen
 * the bytes produce, which means interpreting them the way a terminal would.
 *
 * The interpreter is xterm.js running headless — the same engine that draws
 * the live console in the web UI, so a script's view of the screen and an
 * operator's are the same. It is reached through the ScreenModel interface,
 * which is where another emulation (a strict VT52, a plain teletype) would
 * be substituted for a guest xterm does not suit.
 */
import pkg from "@xterm/headless";
const { Terminal } = pkg;
type XTerm = InstanceType<typeof Terminal>;

export interface ScreenModel {
  /** Feed guest output. */
  write(bytes: Uint8Array): void;
  /** One row's text, 0-based from the top of the viewport, right-trimmed. */
  row(y: number): string;
  /** Every row of the viewport. */
  rows(): string[];
  /** The whole viewport as one newline-joined string. */
  text(): string;
  /**
   * Every line the session produced — scrollback and viewport — with the
   * trailing blank lines dropped. This is the readable transcript: a line
   * the guest rewrote in place reads as what it ended up saying.
   */
  transcript(): string[];
  cursor(): { x: number; y: number };
  readonly cols: number;
  readonly rows_: number;
  dispose(): void;
}

export interface ScreenSize {
  cols?: number;
  rows?: number;
  /** Scrollback kept; a script matching the viewport rarely needs any. */
  scrollback?: number;
}

export class XtermScreen implements ScreenModel {
  private term: XTerm;
  readonly cols: number;
  readonly rows_: number;

  constructor(size: ScreenSize = {}) {
    this.cols = size.cols ?? 80;
    this.rows_ = size.rows ?? 24;
    this.term = new Terminal({
      cols: this.cols,
      rows: this.rows_,
      scrollback: size.scrollback ?? 0,
      allowProposedApi: true,
      // A PDP-11 console carries bytes xterm has no sequence for — the boot
      // ROM's DEL padding among them — and the parser reports each one. They
      // are fill characters a real terminal ignores, so silence the log
      // rather than let a boot bury the run's own output.
      logLevel: "off",
    });
  }

  write(bytes: Uint8Array): void {
    this.term.write(bytes);
  }

  /** Resolve once the interpreter has consumed everything written so far. */
  flush(): Promise<void> {
    return new Promise((resolve) => this.term.write("", () => resolve()));
  }

  row(y: number): string {
    const line = this.term.buffer.active.getLine(
      this.term.buffer.active.viewportY + y,
    );
    return line ? line.translateToString(true) : "";
  }

  rows(): string[] {
    const out: string[] = [];
    for (let y = 0; y < this.rows_; y++) out.push(this.row(y));
    return out;
  }

  text(): string {
    return this.rows().join("\n");
  }

  transcript(): string[] {
    const buf = this.term.buffer.active;
    const out: string[] = [];
    for (let y = 0; y < buf.length; y++) {
      const line = buf.getLine(y);
      out.push(line ? line.translateToString(true) : "");
    }
    while (out.length > 0 && out[out.length - 1].trim() === "") out.pop();
    return out;
  }

  cursor(): { x: number; y: number } {
    const b = this.term.buffer.active;
    return { x: b.cursorX, y: b.cursorY };
  }

  dispose(): void {
    this.term.dispose();
  }
}

/**
 * A screen condition in a step file:
 *
 *   screen: {contains: "MicroEMACS"}        anywhere on the screen
 *   screen: {row: 23, contains: "-- more"}  on one row
 *   screen: {row: 0, matches: "/^EDT/"}     regular expression on a row
 *   screen: {cursor: {y: 5}}                where the cursor sits
 */
export interface ScreenCondition {
  contains?: string;
  matches?: string;
  row?: number;
  cursor?: { x?: number; y?: number };
}

function asRegExp(spec: string): RegExp {
  const m = /^\/(.*)\/([a-z]*)$/s.exec(spec);
  return m ? new RegExp(m[1], m[2]) : new RegExp(spec);
}

export function screenMatches(
  screen: ScreenModel,
  cond: ScreenCondition,
): boolean {
  if (cond.cursor !== undefined) {
    const c = screen.cursor();
    if (cond.cursor.x !== undefined && c.x !== cond.cursor.x) return false;
    if (cond.cursor.y !== undefined && c.y !== cond.cursor.y) return false;
  }
  const haystack = cond.row !== undefined ? screen.row(cond.row) : screen.text();
  if (cond.contains !== undefined && !haystack.includes(cond.contains))
    return false;
  if (cond.matches !== undefined && !asRegExp(cond.matches).test(haystack))
    return false;
  // A condition naming only a cursor position is satisfied by the check above.
  return (
    cond.contains !== undefined ||
    cond.matches !== undefined ||
    cond.cursor !== undefined
  );
}

/** Describe a condition for a diagnostic. */
export function describeCondition(cond: ScreenCondition): string {
  const parts: string[] = [];
  if (cond.row !== undefined) parts.push(`row ${cond.row}`);
  if (cond.contains !== undefined)
    parts.push(`contains ${JSON.stringify(cond.contains)}`);
  if (cond.matches !== undefined) parts.push(`matches ${cond.matches}`);
  if (cond.cursor !== undefined)
    parts.push(`cursor ${JSON.stringify(cond.cursor)}`);
  return `screen{${parts.join(", ")}}`;
}
