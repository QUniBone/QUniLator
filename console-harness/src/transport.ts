/* transport.ts: the byte-stream a session drives.
 *
 * A transport carries the guest's console: binary bytes in both directions,
 * a "live" signal marking the boundary between replayed history and live
 * output (where the transport can know it), and BREAK as an out-of-band
 * action. Two implementations: the QUniLator console WebSocket and a raw/
 * telnet TCP console (a simh instance, a ser2net port).
 */
import WebSocket from "ws";
import { Socket, connect as tcpConnect } from "node:net";

import { withRetry } from "./retry.js";

export interface Transport {
  open(): Promise<void>;
  /** Send bytes toward the guest. */
  send(bytes: Uint8Array): void;
  /** Assert a line BREAK; rejects where the transport cannot express one. */
  sendBreak(): Promise<void>;
  close(): void;
  /** Bytes from the guest (replayed history first, where the server retains one). */
  onData(cb: (bytes: Uint8Array) => void): void;
  /** The replay/live boundary. Fired at most once. */
  onLive(cb: () => void): void;
  onClose(cb: (err?: Error) => void): void;
  /** A short label for diagnostics ("ws:ext", "tcp:host:2323"). */
  readonly label: string;
}

/** Callback plumbing shared by the transports. */
abstract class TransportBase implements Transport {
  private dataCbs: ((bytes: Uint8Array) => void)[] = [];
  private liveCbs: (() => void)[] = [];
  private closeCbs: ((err?: Error) => void)[] = [];
  private liveFired = false;
  private closeFired = false;

  abstract open(): Promise<void>;
  abstract send(bytes: Uint8Array): void;
  abstract sendBreak(): Promise<void>;
  abstract close(): void;
  abstract readonly label: string;

  onData(cb: (bytes: Uint8Array) => void): void {
    this.dataCbs.push(cb);
  }
  onLive(cb: () => void): void {
    if (this.liveFired) {
      cb();
      return;
    }
    this.liveCbs.push(cb);
  }
  onClose(cb: (err?: Error) => void): void {
    this.closeCbs.push(cb);
  }

  protected emitData(bytes: Uint8Array): void {
    for (const cb of this.dataCbs) cb(bytes);
  }
  protected emitLive(): void {
    if (this.liveFired) return;
    this.liveFired = true;
    for (const cb of this.liveCbs) cb();
    this.liveCbs = [];
  }
  protected emitClose(err?: Error): void {
    if (this.closeFired) return;
    this.closeFired = true;
    for (const cb of this.closeCbs) cb(err);
  }
}

export interface WsTransportOptions {
  /** "Basic …" header value; omitted when the board runs without a password. */
  authHeader?: string;
  /**
   * Send BREAK as the {"break":true} OOB TEXT frame (the default). A backend
   * older than that control-frame contract has no TEXT-frame handling and
   * would type the frame's characters into the console, so a session against
   * one sets this false and gets a refusal instead.
   */
  oobBreak?: boolean;
}

/**
 * A QUniLator console channel: /ws/console/<ch> or /ws/serial/<dev>/<line>.
 * Binary frames are the byte stream. TEXT frames are out-of-band control:
 * {"answerer":true} (ignored here), {"live":true} = replay boundary.
 */
export class WsTransport extends TransportBase {
  private ws: WebSocket | null = null;
  readonly label: string;

  constructor(
    private url: string,
    private opts: WsTransportOptions = {},
  ) {
    super();
    this.label = "ws:" + url.replace(/^wss?:\/\//, "");
  }

  open(): Promise<void> {
    return withRetry(this.label, () => this.openOnce(), {
      log: (l) => process.stderr.write(l),
    });
  }

  private openOnce(): Promise<void> {
    return new Promise((resolve, reject) => {
      const headers: Record<string, string> = {};
      if (this.opts.authHeader) headers["Authorization"] = this.opts.authHeader;
      const ws = new WebSocket(this.url, { headers });
      this.ws = ws;
      let opened = false;
      ws.on("open", () => {
        opened = true;
        resolve();
      });
      ws.on("message", (data: WebSocket.RawData, isBinary: boolean) => {
        const buf = Buffer.isBuffer(data)
          ? data
          : Array.isArray(data)
            ? Buffer.concat(data)
            : Buffer.from(data);
        if (isBinary) {
          this.emitData(new Uint8Array(buf));
          return;
        }
        // OOB control frame
        try {
          const msg = JSON.parse(buf.toString("utf8")) as Record<string, unknown>;
          if (msg["live"] === true) this.emitLive();
        } catch {
          /* not JSON: ignore */
        }
      });
      // A handshake that never opened is a failed attempt, not a close of the
      // session's transport: the session subscribes before open() settles and
      // must not see the attempts a retry burns through.
      ws.on("close", () => {
        if (opened) this.emitClose();
      });
      ws.on("error", (err: Error) => {
        if (!opened) reject(err);
        else this.emitClose(err);
      });
    });
  }

  send(bytes: Uint8Array): void {
    if (!this.ws) throw new Error("transport not open");
    this.ws.send(bytes, { binary: true });
  }

  sendBreak(): Promise<void> {
    if (!this.ws) return Promise.reject(new Error("transport not open"));
    if (this.opts.oobBreak === false)
      return Promise.reject(
        new Error("BREAK is disabled for this transport (oobBreak: false)"),
      );
    this.ws.send('{"break":true}');
    return Promise.resolve();
  }

  close(): void {
    try {
      this.ws?.close();
    } catch {
      /* ignore */
    }
  }
}

// telnet protocol bytes (RFC 854)
const IAC = 255;
const TELNET_BREAK = 243;
const WILL = 251;
const WONT = 252;
const DO = 253;
const DONT = 254;
const SB = 250;
const SE = 240;

/**
 * A TCP console (a simh telnet console, a ser2net port). Telnet negotiation
 * from the peer is answered with refusals and stripped from the stream, and
 * outgoing 0xFF is escaped, so both raw and telnet servers carry the bytes
 * transparently. There is no replay concept: live fires on connect.
 */
export class TcpTransport extends TransportBase {
  private sock: Socket | null = null;
  private sawTelnet = false;
  // telnet parser state across chunk boundaries
  private tnState: "data" | "iac" | "opt" | "sb" | "sbIac" = "data";
  readonly label: string;

  constructor(
    private host: string,
    private port: number,
  ) {
    super();
    this.label = `tcp:${host}:${port}`;
  }

  open(): Promise<void> {
    return new Promise((resolve, reject) => {
      const sock = tcpConnect({ host: this.host, port: this.port });
      this.sock = sock;
      sock.on("connect", () => {
        this.emitLive();
        resolve();
      });
      sock.on("data", (chunk: Buffer) => {
        const clean = this.filterTelnet(chunk);
        if (clean.length > 0) this.emitData(new Uint8Array(clean));
      });
      sock.on("close", () => this.emitClose());
      sock.on("error", (err: Error) => {
        if (!sock.remoteAddress) reject(err);
        this.emitClose(err);
      });
    });
  }

  /** Strip telnet commands, answering DO→WONT and WILL→DONT. */
  private filterTelnet(chunk: Buffer): Buffer {
    const out: number[] = [];
    for (const b of chunk) {
      switch (this.tnState) {
        case "data":
          if (b === IAC) this.tnState = "iac";
          else out.push(b);
          break;
        case "iac":
          if (b === IAC) {
            out.push(IAC); // escaped data byte
            this.tnState = "data";
          } else if (b === SB) {
            this.tnState = "sb";
            this.sawTelnet = true;
          } else if (b === WILL || b === WONT || b === DO || b === DONT) {
            this.tnState = "opt";
            this.sawTelnet = true;
            this.pendingVerb = b;
          } else {
            this.tnState = "data"; // NOP, GA, ...
            this.sawTelnet = true;
          }
          break;
        case "opt": {
          const verb = this.pendingVerb;
          if (verb === DO) this.rawSend(Buffer.from([IAC, WONT, b]));
          else if (verb === WILL) this.rawSend(Buffer.from([IAC, DONT, b]));
          this.tnState = "data";
          break;
        }
        case "sb":
          if (b === IAC) this.tnState = "sbIac";
          break;
        case "sbIac":
          this.tnState = b === SE ? "data" : "sb";
          break;
      }
    }
    return Buffer.from(out);
  }
  private pendingVerb = 0;

  private rawSend(buf: Buffer): void {
    this.sock?.write(buf);
  }

  send(bytes: Uint8Array): void {
    if (!this.sock) throw new Error("transport not open");
    // escape 0xFF for a telnet peer; harmless duplication cannot occur on a
    // raw peer because we only escape when the peer spoke telnet first
    if (this.sawTelnet) {
      const out: number[] = [];
      for (const b of bytes) {
        out.push(b);
        if (b === IAC) out.push(IAC);
      }
      this.sock.write(Buffer.from(out));
    } else {
      this.sock.write(Buffer.from(bytes));
    }
  }

  sendBreak(): Promise<void> {
    if (!this.sock) return Promise.reject(new Error("transport not open"));
    if (!this.sawTelnet)
      return Promise.reject(
        new Error("BREAK is a telnet command; the peer did not negotiate telnet"),
      );
    this.sock.write(Buffer.from([IAC, TELNET_BREAK]));
    return Promise.resolve();
  }

  close(): void {
    try {
      this.sock?.destroy();
    } catch {
      /* ignore */
    }
  }
}
