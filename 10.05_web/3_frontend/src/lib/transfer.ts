// Sending a file to the guest through its console line: as typed text, by
// XMODEM, or by Kermit.
//
// Each sender works on a TransferLink, which carries bytes to the console and
// hands back what the guest sends, one byte at a time with a timeout. The
// console terminal provides the link in the browser; the protocol tests drive
// the same code against lrzsz and C-Kermit on a pseudo-terminal.
//
// The module imports nothing, so node runs it as it stands.

export interface TransferLink {
  send(bytes: Uint8Array): void;
}

export interface TransferProgress {
  sent: number; // bytes of the file handed over so far
  total: number;
  note: string; // what the transfer is doing, for the operator
}

export type ProgressFn = (p: TransferProgress) => void;

export class TransferError extends Error {}

// What the guest sends, queued until a sender asks for it.
export class ByteQueue {
  private bytes: number[] = [];
  private head = 0;
  private waiter: ((b: number) => void) | null = null;
  private timer: ReturnType<typeof setTimeout> | null = null;
  private signal: AbortSignal | undefined;

  constructor(signal?: AbortSignal) {
    this.signal = signal;
    // a cancelled transfer has a sender waiting here; wake it
    signal?.addEventListener('abort', () => this.wake(-2));
  }

  push(data: Uint8Array): void {
    for (const b of data) this.bytes.push(b);
    if (this.waiter && this.head < this.bytes.length) this.wake(this.bytes[this.head++]);
  }

  // Everything received so far is old news to a sender about to transmit.
  flush(): void {
    this.bytes = [];
    this.head = 0;
  }

  // The next byte, or -1 when none arrives within timeoutMs.
  recv(timeoutMs: number): Promise<number> {
    if (this.signal?.aborted) return Promise.reject(new TransferError('cancelled'));
    if (this.head < this.bytes.length) {
      const b = this.bytes[this.head++];
      if (this.head > 4096) {
        this.bytes = this.bytes.slice(this.head);
        this.head = 0;
      }
      return Promise.resolve(b);
    }
    return new Promise((resolve, reject) => {
      this.waiter = (b) => (b === -2 ? reject(new TransferError('cancelled')) : resolve(b));
      this.timer = setTimeout(() => this.wake(-1), timeoutMs);
    });
  }

  private wake(b: number): void {
    const w = this.waiter;
    if (!w) return;
    this.waiter = null;
    if (this.timer) clearTimeout(this.timer);
    this.timer = null;
    w(b);
  }
}

const sleep = (ms: number, signal?: AbortSignal) =>
  new Promise<void>((resolve, reject) => {
    if (signal?.aborted) return reject(new TransferError('cancelled'));
    const t = setTimeout(resolve, ms);
    signal?.addEventListener('abort', () => {
      clearTimeout(t);
      reject(new TransferError('cancelled'));
    });
  });

// ---- text, as typed ----
//
// The text goes to the guest a character at a time, as from a keyboard, and
// each check and delay below can be had alone or together.
//
// The echo check waits for the guest to echo what it was sent. A guest whose
// line takes characters into a receive buffer while it works on one can be
// sent that many characters ahead of the echo still awaited; with no buffer it
// is sent a character only once it has echoed the one before. A line end waits
// for every echo still outstanding.
//
// A guest may echo a character changed, in upper case on a terminal that is
// set for it, a tab as spaces, and may break a long line on the screen with a
// line end of its own. It may also take a character without echoing it: VMS on
// a terminal set to /NOLOWERCASE takes } and the characters after it so. So
// the check passes over line ends and other control characters, and over the
// spaces of an expanded tab; an echo that matches a character sent after the
// one awaited, ignoring case and parity, means the characters before it were
// taken without an echo; and any other printable echo is the awaited
// character's, changed. An echo that does not come within the timeout is
// counted and the text goes on.
//
// The prompt check waits, after each line end, for the guest to print its
// prompt again, which is when a program that reads a line at a time is ready
// for the next one.

export interface RawOptions {
  // line ends go to the guest as CR, which is what the Return key sends
  crForLf: boolean;
  echo: boolean;
  echoTimeoutMs: number;
  // characters the guest's line holds while it works on one, sent ahead of
  // the echo awaited
  fifo: number;
  // the text the guest prints when it wants the next line; empty for none
  prompt: string;
  promptTimeoutMs: number;
  charDelayMs: number;
  lineDelayMs: number;
}

export interface RawResult {
  unechoed: number; // characters whose echo did not come in time
  promptsMissed: number; // line ends the prompt did not follow in time
}

export function rawBytes(data: Uint8Array, crForLf: boolean): Uint8Array {
  if (!crForLf) return data;
  const out: number[] = [];
  for (let i = 0; i < data.length; i++) {
    const b = data[i];
    if (b === 0x0d && data[i + 1] === 0x0a) continue; // CR LF is one line end
    out.push(b === 0x0a ? 0x0d : b);
  }
  return Uint8Array.from(out);
}

const lower = (b: number) => (b >= 0x41 && b <= 0x5a ? b + 32 : b);
const isLineEnd = (b: number) => b === 0x0d || b === 0x0a;

const same = (echo: number, sent: number) => lower(echo) === lower(sent & 0x7f);

// The guest's echoes, matched against the characters sent and not yet echoed.
class EchoTracker {
  // oldest first
  outstanding: number[] = [];
  unechoed = 0;
  // the last character echoed was a tab, whose spaces may still be coming
  private inTab = false;

  private input: ByteQueue;
  private timeoutMs: number;

  constructor(input: ByteQueue, timeoutMs: number) {
    this.input = input;
    this.timeoutMs = timeoutMs;
  }

  // Settle the oldest character outstanding: wait for its echo, or for
  // a later one's, within the timeout.
  async settle(): Promise<void> {
    const until = Date.now() + this.timeoutMs;
    for (;;) {
      const left = until - Date.now();
      const e = left > 0 ? await this.input.recv(left) : -1;
      if (e < 0) {
        this.outstanding.shift();
        this.unechoed++;
        this.inTab = false;
        return;
      }
      const c = e & 0x7f;
      const oldest = this.outstanding[0];
      if (isLineEnd(oldest)) {
        if (isLineEnd(c)) return this.take(1, false);
        continue;
      }
      if (c === 0x20 && this.inTab && oldest !== 0x20) continue;
      if (c === 0x09 && oldest === 0x09) return this.take(1, true);
      if (c < 0x20 || c === 0x7f) continue; // a line break or other control the guest put in
      if (oldest === 0x09 && c === 0x20) return this.take(1, true);
      const k = this.outstanding.findIndex((b, n) => n > 0 && !isLineEnd(b) && same(c, b));
      if (!same(c, oldest) && k > 0) {
        this.unechoed += k;
        return this.take(k + 1, this.outstanding[k] === 0x09);
      }
      return this.take(1, false);
    }
  }

  private take(n: number, tab: boolean): void {
    this.outstanding.splice(0, n);
    this.inTab = tab;
  }
}

// Wait for the prompt to appear in what the guest prints, within timeoutMs.
async function awaitPrompt(input: ByteQueue, prompt: string, timeoutMs: number): Promise<boolean> {
  const want = Array.from(prompt, (ch) => lower(ch.charCodeAt(0) & 0x7f));
  const seen: number[] = [];
  const until = Date.now() + timeoutMs;
  for (;;) {
    const left = until - Date.now();
    const e = left > 0 ? await input.recv(left) : -1;
    if (e < 0) return false;
    seen.push(lower(e & 0x7f));
    if (seen.length > want.length) seen.shift();
    if (seen.length === want.length && seen.every((c, k) => c === want[k])) return true;
  }
}

export async function sendRaw(
  link: TransferLink,
  input: ByteQueue,
  data: Uint8Array,
  opts: RawOptions,
  progress: ProgressFn,
  signal?: AbortSignal
): Promise<RawResult> {
  const bytes = rawBytes(data, opts.crForLf);
  const total = bytes.length;
  const result: RawResult = { unechoed: 0, promptsMissed: 0 };
  const paced = opts.echo || opts.prompt !== '' || opts.charDelayMs > 0 || opts.lineDelayMs > 0;
  if (!paced) {
    link.send(bytes);
    progress({ sent: total, total, note: 'sent' });
    return result;
  }
  input.flush();
  const echoes = new EchoTracker(input, opts.echoTimeoutMs);
  const outstanding = echoes.outstanding;
  const window = Math.max(0, opts.fifo) + 1;
  const settle = () => echoes.settle();
  for (let i = 0; i < total; i++) {
    if (signal?.aborted) throw new TransferError('cancelled');
    const b = bytes[i];
    const lineEnd = isLineEnd(b);
    if (opts.echo) while (outstanding.length >= window) await settle();
    // without the echo check the line's own echo is still in the queue, and
    // could hold the prompt's text
    if (lineEnd && opts.prompt !== '' && !opts.echo) input.flush();
    link.send(bytes.subarray(i, i + 1));
    if (opts.echo) {
      outstanding.push(b);
      if (lineEnd || i === total - 1) while (outstanding.length) await settle();
    }
    if (lineEnd && opts.prompt !== '' && i < total - 1) {
      if (!(await awaitPrompt(input, opts.prompt, opts.promptTimeoutMs))) result.promptsMissed++;
    }
    const delay = lineEnd ? opts.lineDelayMs : opts.charDelayMs;
    if (delay > 0) await sleep(delay, signal);
    if ((i & 15) === 15 || lineEnd || i === total - 1)
      progress({ sent: i + 1, total, note: lineEnd ? 'line sent' : 'typing' });
  }
  result.unechoed = echoes.unechoed;
  return result;
}

// ---- XMODEM ----

const SOH = 0x01,
  STX = 0x02,
  EOT = 0x04,
  ACK = 0x06,
  NAK = 0x15,
  CAN = 0x18,
  SUB = 0x1a;

export interface XmodemOptions {
  // 1024-byte blocks, when the receiver asks for CRC (XMODEM-1K)
  oneK: boolean;
  // how long the receiver has to start, in milliseconds
  startTimeoutMs: number;
}

function crc16(data: Uint8Array): number {
  let crc = 0;
  for (const b of data) {
    crc ^= b << 8;
    for (let i = 0; i < 8; i++) crc = crc & 0x8000 ? ((crc << 1) ^ 0x1021) & 0xffff : (crc << 1) & 0xffff;
  }
  return crc;
}

function xmodemBlock(n: number, chunk: Uint8Array, size: number, crc: boolean): Uint8Array {
  const block = new Uint8Array(3 + size + (crc ? 2 : 1));
  block[0] = size === 1024 ? STX : SOH;
  block[1] = n & 0xff;
  block[2] = 0xff - (n & 0xff);
  const body = block.subarray(3, 3 + size);
  body.fill(SUB);
  body.set(chunk);
  if (crc) {
    const c = crc16(body);
    block[3 + size] = c >> 8;
    block[4 + size] = c & 0xff;
  } else {
    let sum = 0;
    for (const b of body) sum += b;
    block[3 + size] = sum & 0xff;
  }
  return block;
}

// The receiver's answer to a block: ACK, NAK, a cancel, or -1 for none in time.
// A single CAN can be line noise; two in a row are the receiver giving up.
async function xmodemAnswer(input: ByteQueue, timeoutMs: number, alsoNak: number): Promise<number> {
  const until = Date.now() + timeoutMs;
  let cans = 0;
  for (;;) {
    const left = until - Date.now();
    if (left <= 0) return -1;
    const b = await input.recv(left);
    if (b < 0) return -1;
    if (b === ACK) return ACK;
    if (b === NAK || b === alsoNak) return NAK;
    if (b === CAN) {
      if (++cans >= 2) return CAN;
    } else cans = 0;
  }
}

export async function sendXmodem(
  link: TransferLink,
  input: ByteQueue,
  data: Uint8Array,
  opts: XmodemOptions,
  progress: ProgressFn,
  signal?: AbortSignal
): Promise<void> {
  const total = data.length;
  const cancel = () => link.send(Uint8Array.from([CAN, CAN, CAN, CAN, CAN, CAN, CAN, CAN]));
  try {
    // the receiver starts the transfer: NAK asks for checksums, C for CRCs
    progress({ sent: 0, total, note: 'waiting for the receiver' });
    let crc = false;
    const until = Date.now() + opts.startTimeoutMs;
    for (;;) {
      const left = until - Date.now();
      const b = left > 0 ? await input.recv(left) : -1;
      if (b < 0) throw new TransferError('the receiver did not start');
      if (b === NAK) break;
      if (b === 0x43) {
        crc = true;
        break;
      }
    }
    const size = crc && opts.oneK ? 1024 : 128;
    let n = 1;
    for (let off = 0; off < total || (off === 0 && total === 0); ) {
      // a short tail goes in a 128-byte block rather than a mostly padded 1K one
      const blockSize = size === 1024 && total - off <= 128 ? 128 : size;
      const chunk = data.subarray(off, off + blockSize);
      const block = xmodemBlock(n, chunk, blockSize, crc);
      let tries = 0;
      for (;;) {
        if (signal?.aborted) throw new TransferError('cancelled');
        if (++tries > 10) throw new TransferError(`block ${n} was not acknowledged`);
        input.flush();
        link.send(block);
        // the receiver's repeated C crosses the first block in flight
        const a = await xmodemAnswer(input, 10000, n === 1 && crc ? 0x43 : NAK);
        if (a === ACK) break;
        if (a === CAN) throw new TransferError('the receiver cancelled');
      }
      off += chunk.length;
      n++;
      progress({ sent: Math.min(off, total), total, note: `block ${n - 1}` });
      if (total === 0) break;
    }
    for (let tries = 0; ; tries++) {
      if (tries >= 10) throw new TransferError('the end of the file was not acknowledged');
      input.flush();
      link.send(Uint8Array.from([EOT]));
      const a = await xmodemAnswer(input, 10000, NAK);
      if (a === ACK) break;
      if (a === CAN) throw new TransferError('the receiver cancelled');
    }
    progress({ sent: total, total, note: 'done' });
  } catch (e) {
    cancel();
    throw e;
  }
}

// ---- Kermit ----
//
// The sending half of the Kermit protocol as the Kermit Protocol Manual
// describes it: Send-Init, File header, Data, EOF and Break, each answered by
// an ACK. Every byte on the line is printable ASCII apart from the packet's
// mark and end of line, control characters are prefixed with #, and a byte
// with its eighth bit set travels as & and its seven low bits, so the file
// arrives whole over a seven-bit line. The block check is the single
// character one every Kermit has.

const tochar = (x: number) => x + 32;
const unchar = (c: number) => c - 32;
const ctl = (c: number) => c ^ 64;
const KERMIT_MARK = 0x01;

export interface KermitOptions {
  // the name the guest's Kermit is offered
  name: string;
  // a text file goes as Kermit's canonical text, with CR LF line ends, which
  // the receiver turns into its own
  text: boolean;
  startTimeoutMs: number;
}

interface KermitPacket {
  seq: number;
  type: string;
  data: number[];
}

function kermitCheck(chars: number[]): number {
  let s = 0;
  for (const c of chars) s += c;
  return tochar((s + ((s & 192) >> 6)) & 63);
}

function kermitPacket(seq: number, type: string, data: number[], eol: number, npad: number, padc: number): Uint8Array {
  const body = [tochar(data.length + 3), tochar(seq & 63), type.charCodeAt(0), ...data];
  const out = new Array<number>(npad).fill(padc);
  out.push(KERMIT_MARK, ...body, kermitCheck(body), eol);
  return Uint8Array.from(out);
}

// The next good packet the guest sends, or null for none in time. Bytes outside
// a packet are dropped, and a seven-bit line's parity bit is stripped.
async function kermitReceive(input: ByteQueue, timeoutMs: number): Promise<KermitPacket | null> {
  const until = Date.now() + timeoutMs;
  const next = async () => {
    const left = until - Date.now();
    const b = left > 0 ? await input.recv(left) : -1;
    return b < 0 ? -1 : b & 0x7f;
  };
  for (;;) {
    let b = await next();
    if (b < 0) return null;
    if (b !== KERMIT_MARK) continue;
    const len = await next();
    if (len < 0) return null;
    if (len === KERMIT_MARK || unchar(len) < 3) continue;
    const body = [len];
    let restart = false;
    for (let i = 0; i < unchar(len); i++) {
      b = await next();
      if (b < 0) return null;
      if (b === KERMIT_MARK) {
        restart = true;
        break;
      }
      body.push(b);
    }
    if (restart) continue;
    const check = body.pop()!;
    if (kermitCheck(body) !== check) continue;
    return { seq: unchar(body[1]), type: String.fromCharCode(body[2]), data: body.slice(3) };
  }
}

// One byte in Kermit's encoding: the eighth bit as &, a control character as #
// and the character with bit 6 flipped, and either prefix itself behind #.
function kermitEncode(b: number, qbin: boolean): number[] {
  const out: number[] = [];
  if (qbin && b & 0x80) {
    out.push(0x26);
    b &= 0x7f;
  }
  if (b < 32 || b === 127) out.push(0x23, ctl(b));
  else if (b === 0x23 || (qbin && b === 0x26)) out.push(0x23, b);
  else out.push(b);
  return out;
}

function kermitDecodeText(data: number[]): string {
  const out: number[] = [];
  for (let i = 0; i < data.length; i++) {
    if (data[i] === 0x23 && i + 1 < data.length) {
      const c = data[++i];
      out.push((c & 0x7f) >= 0x3f && (c & 0x7f) <= 0x5f ? ctl(c) : c);
    } else out.push(data[i]);
  }
  return String.fromCharCode(...out);
}

export function kermitText(data: Uint8Array): Uint8Array {
  const out: number[] = [];
  for (let i = 0; i < data.length; i++) {
    const b = data[i];
    if (b === 0x0a && (i === 0 || data[i - 1] !== 0x0d)) out.push(0x0d);
    out.push(b);
  }
  return Uint8Array.from(out);
}

// A name the guest's file system takes: the base name, with anything but
// letters, digits, dot, dash and underscore made an underscore.
export function kermitName(name: string): string {
  const base = name.replace(/^.*[\\/]/, '').replace(/[^A-Za-z0-9._-]/g, '_');
  return base || 'FILE';
}

export async function sendKermit(
  link: TransferLink,
  input: ByteQueue,
  data: Uint8Array,
  opts: KermitOptions,
  progress: ProgressFn,
  signal?: AbortSignal
): Promise<void> {
  const bytes = opts.text ? kermitText(data) : data;
  const total = data.length;
  // what goes on the line until the receiver says otherwise
  let eol = 0x0d,
    npad = 0,
    padc = 0,
    maxl = 80,
    timeoutMs = 5000,
    qbin = false;
  let seq = 0;
  let sentBytes = 0;
  // set when the receiver ended the transfer with an error packet of its own
  let stopped = false;

  // Send a packet until the receiver acknowledges it, and return the ACK.
  const exchange = async (type: string, payload: number[], tries: number, note: string) => {
    for (let t = 0; t < tries; t++) {
      if (signal?.aborted) throw new TransferError('cancelled');
      input.flush();
      link.send(kermitPacket(seq, type, payload, eol, npad, padc));
      const until = Date.now() + timeoutMs;
      for (;;) {
        const p = await kermitReceive(input, until - Date.now());
        if (!p) break;
        if (p.type === 'E') {
          stopped = true;
          throw new TransferError('the receiver stopped: ' + kermitDecodeText(p.data));
        }
        if (p.type === 'Y' && p.seq === (seq & 63)) {
          seq = (seq + 1) & 63;
          return p;
        }
        // a NAK for the next packet acknowledges this one
        if (p.type === 'N' && p.seq === ((seq + 1) & 63)) {
          seq = (seq + 1) & 63;
          return p;
        }
        if (p.type === 'N') break;
      }
      progress({ sent: sentBytes, total, note: note + ', retrying' });
    }
    throw new TransferError(`no answer to the ${note}`);
  };

  try {
    // Send-Init: our longest packet, the receiver's timeout, no padding, CR as
    // end of line, # for control prefixing, & asking for eighth-bit prefixing,
    // the one-character block check, and no repeat counts
    progress({ sent: 0, total, note: 'waiting for the receiver' });
    const init = [tochar(94), tochar(10), tochar(0), ctl(0), tochar(13), 0x23, 0x26, 0x31, 0x20];
    const startTries = Math.max(1, Math.ceil(opts.startTimeoutMs / timeoutMs));
    const ack = await exchange('S', init, startTries, 'invitation');
    const p = ack.data;
    if (p.length > 0) maxl = Math.min(94, Math.max(20, unchar(p[0])));
    if (p.length > 1 && unchar(p[1]) > 0) timeoutMs = Math.max(2, unchar(p[1])) * 1000 + 2000;
    if (p.length > 2) npad = unchar(p[2]);
    if (p.length > 3) padc = ctl(p[3]);
    if (p.length > 4 && unchar(p[4]) > 0) eol = unchar(p[4]);
    // the receiver agrees to eighth-bit prefixing with Y, or by naming &
    qbin = p.length > 6 && (p[6] === 0x59 || p[6] === 0x26);
    if (!qbin && bytes.some((b) => b & 0x80))
      throw new TransferError('the receiver does not take eight-bit data on this line');

    const name: number[] = [];
    for (const c of kermitName(opts.name)) name.push(...kermitEncode(c.charCodeAt(0), qbin));
    await exchange('F', name, 10, 'file header');

    // a data packet holds what fits between the header and the block check,
    // and no byte's encoding is split across two
    const room = maxl - 3;
    let off = 0;
    while (off < bytes.length) {
      const chunk: number[] = [];
      let n = off;
      while (n < bytes.length) {
        const enc = kermitEncode(bytes[n], qbin);
        if (chunk.length + enc.length > room) break;
        chunk.push(...enc);
        n++;
      }
      await exchange('D', chunk, 10, 'data');
      off = n;
      // the text form inserts CRs, so progress is counted in the file's bytes
      sentBytes = opts.text ? Math.min(total, Math.round((off / bytes.length) * total)) : off;
      progress({ sent: sentBytes, total, note: 'data' });
    }
    await exchange('Z', [], 10, 'end of file');
    await exchange('B', [], 10, 'end of transmission');
    progress({ sent: total, total, note: 'done' });
  } catch (e) {
    if (stopped) throw e;
    const msg: number[] = [];
    for (const c of 'Cancelled') msg.push(c.charCodeAt(0));
    link.send(kermitPacket(seq, 'E', msg, eol, npad, padc));
    throw e;
  }
}

// ---- receiving ----

export interface ReceivedFile {
  name: string;
  data: Uint8Array;
}

function concat(chunks: Uint8Array[]): Uint8Array {
  const out = new Uint8Array(chunks.reduce((n, c) => n + c.length, 0));
  let off = 0;
  for (const c of chunks) {
    out.set(c, off);
    off += c.length;
  }
  return out;
}

// CR LF as LF, for a text file kept on this side.
export function textToLf(data: Uint8Array): Uint8Array {
  const out: number[] = [];
  for (let i = 0; i < data.length; i++) if (!(data[i] === 0x0d && data[i + 1] === 0x0a)) out.push(data[i]);
  return Uint8Array.from(out);
}

export interface XmodemReceiveOptions {
  // how long the sender has to start, in milliseconds
  startTimeoutMs: number;
  // XMODEM fills the last block with SUB; take those bytes off the end
  stripPadding: boolean;
}

// Receive one file by XMODEM. The receiver starts the transfer, asking for
// CRCs with C; a sender that does not answer them within half the start time
// is asked for checksums with NAK. Blocks of 128 and 1024 bytes are taken, and
// a block sent again because its ACK was lost is acknowledged and dropped.
export async function receiveXmodem(
  link: TransferLink,
  input: ByteQueue,
  opts: XmodemReceiveOptions,
  progress: ProgressFn,
  signal?: AbortSignal
): Promise<Uint8Array> {
  const chunks: Uint8Array[] = [];
  let received = 0;
  const cancel = () => link.send(Uint8Array.from([CAN, CAN, CAN, CAN, CAN, CAN, CAN, CAN]));
  const byte = async (ms: number) => {
    const b = await input.recv(ms);
    if (b < 0) throw new TransferError('timeout');
    return b;
  };
  try {
    progress({ sent: 0, total: 0, note: 'waiting for the sender' });
    let crc = true;
    let first = -1;
    const started = Date.now();
    while (first < 0) {
      if (signal?.aborted) throw new TransferError('cancelled');
      const waited = Date.now() - started;
      if (waited > opts.startTimeoutMs) throw new TransferError('the sender did not start');
      crc = waited < opts.startTimeoutMs / 2;
      link.send(Uint8Array.from([crc ? 0x43 : NAK]));
      const until = Date.now() + 3000;
      for (;;) {
        const left = until - Date.now();
        const b = left > 0 ? await input.recv(left) : -1;
        if (b < 0) break;
        if (b === SOH || b === STX || b === EOT) {
          first = b;
          break;
        }
      }
    }
    let expected = 1;
    let errors = 0;
    let b = first;
    for (;;) {
      if (signal?.aborted) throw new TransferError('cancelled');
      if (b < 0) {
        b = await input.recv(10000);
        if (b < 0) {
          if (++errors > 10) throw new TransferError('the sender stopped sending');
          link.send(Uint8Array.from([NAK]));
          continue;
        }
      }
      if (b === EOT) {
        link.send(Uint8Array.from([ACK]));
        break;
      }
      if (b === CAN) {
        if ((await input.recv(1000)) === CAN) throw new TransferError('the sender cancelled');
        b = -1;
        continue;
      }
      if (b !== SOH && b !== STX) {
        b = -1; // noise between blocks
        continue;
      }
      const size = b === STX ? 1024 : 128;
      b = -1;
      let block: Uint8Array;
      let n: number;
      let good: boolean;
      try {
        n = await byte(1000);
        const notN = await byte(1000);
        block = new Uint8Array(size);
        for (let i = 0; i < size; i++) block[i] = await byte(1000);
        if (crc) {
          const hi = await byte(1000);
          const lo = await byte(1000);
          good = ((hi << 8) | lo) === crc16(block);
        } else {
          let sum = 0;
          for (const x of block) sum += x;
          good = (await byte(1000)) === (sum & 0xff);
        }
        good = good && n + notN === 255;
      } catch (e) {
        if (!(e instanceof TransferError) || e.message !== 'timeout') throw e;
        good = false;
        n = -1;
        block = new Uint8Array(0);
      }
      if (!good) {
        if (++errors > 10) throw new TransferError('too many damaged blocks');
        input.flush();
        link.send(Uint8Array.from([NAK]));
        continue;
      }
      errors = 0;
      if (n === (expected & 0xff)) {
        chunks.push(block);
        received += size;
        expected++;
        progress({ sent: received, total: 0, note: `block ${expected - 1}` });
      } else if (n !== ((expected - 1) & 0xff)) {
        throw new TransferError(`block ${n} arrived where ${expected & 0xff} was due`);
      }
      link.send(Uint8Array.from([ACK]));
    }
    let data = concat(chunks);
    if (opts.stripPadding) {
      let end = data.length;
      while (end > 0 && data[end - 1] === SUB) end--;
      data = data.subarray(0, end);
    }
    progress({ sent: data.length, total: data.length, note: 'done' });
    return data;
  } catch (e) {
    cancel();
    throw e;
  }
}

export interface KermitReceiveOptions {
  startTimeoutMs: number;
  // a file the sender marks as text, or every file when it does not say, is
  // kept with LF line ends
  textToLf: boolean;
}

// The receiving half of Kermit: the guest's Kermit sends, this side answers its
// Send-Init with its own, and takes every file up to the Break. The answer
// agrees to the sender's control prefix, asks for eighth-bit prefixing when
// the sender offers it, takes repeat counts when the sender offers them, and
// keeps to the single-character block check.
export async function receiveKermit(
  link: TransferLink,
  input: ByteQueue,
  opts: KermitReceiveOptions,
  progress: ProgressFn,
  signal?: AbortSignal
): Promise<ReceivedFile[]> {
  let eol = 0x0d,
    npad = 0,
    padc = 0,
    timeoutMs = 7000,
    qctl = 0x23,
    qbin = 0,
    rept = 0;
  const files: ReceivedFile[] = [];
  let name = '';
  let chunks: number[] = [];
  let received = 0;
  let seq = 0;
  let last: Uint8Array | null = null;
  const reply = (s: number, type: string, data: number[] = []) => {
    last = kermitPacket(s, type, data, eol, npad, padc);
    input.flush();
    link.send(last);
  };
  const decode = (data: number[]): number[] => {
    const out: number[] = [];
    for (let i = 0; i < data.length; i++) {
      let count = 1;
      let c = data[i];
      if (rept && c === rept && i + 2 < data.length) {
        count = unchar(data[++i]);
        c = data[++i];
      }
      let bit8 = 0;
      if (qbin && c === qbin && i + 1 < data.length) {
        bit8 = 0x80;
        c = data[++i];
      }
      if (c === qctl && i + 1 < data.length) {
        c = data[++i];
        if ((c & 0x7f) >= 0x3f && (c & 0x7f) <= 0x5f) c = ctl(c);
      }
      for (let k = 0; k < count; k++) out.push(c | bit8);
    }
    return out;
  };
  const text = (data: number[]) => String.fromCharCode(...decode(data));

  try {
    progress({ sent: 0, total: 0, note: 'waiting for the sender' });
    // the sender's Send-Init, which a NAK for packet 0 asks for again
    let init: KermitPacket | null = null;
    const until = Date.now() + opts.startTimeoutMs;
    while (!init) {
      if (signal?.aborted) throw new TransferError('cancelled');
      if (Date.now() > until) throw new TransferError('the sender did not start');
      const p = await kermitReceive(input, 5000);
      if (p && p.type === 'S') init = p;
      else if (p && p.type === 'E') throw new TransferError('the sender stopped: ' + text(p.data));
      else link.send(kermitPacket(0, 'N', [], eol, npad, padc));
    }
    const sp = init.data;
    if (sp.length > 1 && unchar(sp[1]) > 0) timeoutMs = Math.max(2, unchar(sp[1])) * 1000 + 2000;
    if (sp.length > 2) npad = unchar(sp[2]);
    if (sp.length > 3) padc = ctl(sp[3]);
    if (sp.length > 4 && unchar(sp[4]) > 0) eol = unchar(sp[4]);
    if (sp.length > 5 && sp[5] !== 0x20) qctl = sp[5];
    // eighth-bit prefixing: asked for with Y when the sender offers it, agreed
    // to when the sender names its prefix
    let qbinAnswer = 0x4e;
    const offered = sp.length > 6 ? sp[6] : 0x4e;
    if (offered === 0x59) {
      qbin = 0x26;
      qbinAnswer = 0x26;
    } else if ((offered >= 33 && offered <= 62) || (offered >= 96 && offered <= 126)) {
      qbin = offered;
      qbinAnswer = 0x59;
    }
    let reptAnswer = 0x20;
    if (sp.length > 8 && sp[8] !== 0x20 && sp[8] !== qctl && sp[8] !== qbin) {
      rept = sp[8];
      reptAnswer = sp[8];
    }
    const ours = [tochar(94), tochar(10), tochar(0), ctl(0), tochar(13), qctl, qbinAnswer, 0x31, reptAnswer];
    reply(init.seq, 'Y', ours);
    seq = (init.seq + 1) & 63;

    let tries = 0;
    for (;;) {
      if (signal?.aborted) throw new TransferError('cancelled');
      const p = await kermitReceive(input, timeoutMs);
      if (!p) {
        if (++tries > 10) throw new TransferError('the sender stopped sending');
        // a lost ACK is sent again; a packet that never came is asked for
        if (last) link.send(last);
        else reply(seq, 'N');
        continue;
      }
      tries = 0;
      if (p.type === 'E') throw new TransferError('the sender stopped: ' + text(p.data));
      if (p.seq === ((seq - 1) & 63)) {
        // the sender did not see the ACK for this one
        if (last) link.send(last);
        continue;
      }
      if (p.seq !== seq) continue;
      switch (p.type) {
        case 'S':
          reply(p.seq, 'Y', ours);
          break;
        case 'F':
          name = text(p.data).replace(/^.*[\\/\]:>]/, '') || 'FILE';
          chunks = [];
          progress({ sent: received, total: 0, note: 'receiving ' + name });
          reply(p.seq, 'Y');
          break;
        case 'D':
          for (const b of decode(p.data)) chunks.push(b);
          received += p.data.length;
          progress({ sent: chunks.length, total: 0, note: 'receiving ' + name });
          reply(p.seq, 'Y');
          break;
        case 'Z': {
          // Z with D in its data field is the sender throwing the file away
          if (!(p.data.length > 0 && p.data[0] === 0x44)) {
            const data = Uint8Array.from(chunks);
            files.push({ name, data: opts.textToLf ? textToLf(data) : data });
          }
          reply(p.seq, 'Y');
          break;
        }
        case 'B':
          reply(p.seq, 'Y');
          progress({ sent: received, total: received, note: 'done' });
          return files;
        default:
          // attributes and anything else this side does not use
          reply(p.seq, 'Y');
      }
      seq = (seq + 1) & 63;
    }
  } catch (e) {
    if (!(e instanceof TransferError && e.message.startsWith('the sender stopped'))) {
      const msg = Array.from('Cancelled', (c) => c.charCodeAt(0));
      link.send(kermitPacket(seq, 'E', msg, eol, npad, padc));
    }
    throw e;
  }
}
