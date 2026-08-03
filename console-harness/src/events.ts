/* events.ts: the QUniLator /ws/events watcher a session holds alongside the
 * console. State frames report halt/powered; a session treats a halt or a
 * power loss during a step as a deviation the moment it arrives instead of
 * waiting out the step's deadline. On a non-QUniLator target there is no
 * events stream and the session simply runs without one.
 */
import WebSocket from "ws";

import { withRetry } from "./retry.js";

export type MachineEventName = "halt" | "power-loss";

/** What a session needs from an events stream (MachineEvents or a test fake). */
export interface EventSource {
  onEvent(cb: (ev: MachineEventName) => void): void;
  close(): void;
}

export class MachineEvents implements EventSource {
  private ws: WebSocket | null = null;
  private cbs: ((ev: MachineEventName) => void)[] = [];
  private lastHalt: boolean | null = null;
  private lastPowered: boolean | null = null;

  constructor(
    private url: string,
    private authHeader?: string,
  ) {}

  onEvent(cb: (ev: MachineEventName) => void): void {
    this.cbs.push(cb);
  }

  open(): Promise<void> {
    return withRetry("events stream", () => this.openOnce(), {
      log: (l) => process.stderr.write(l),
    });
  }

  private openOnce(): Promise<void> {
    return new Promise((resolve, reject) => {
      const headers: Record<string, string> = {};
      if (this.authHeader) headers["Authorization"] = this.authHeader;
      const ws = new WebSocket(this.url, { headers });
      this.ws = ws;
      let opened = false;
      ws.on("open", () => {
        opened = true;
        resolve();
      });
      ws.on("message", (data: WebSocket.RawData) => {
        const text = Buffer.isBuffer(data)
          ? data.toString("utf8")
          : String(data);
        for (const chunk of text.split("\n")) {
          const s = chunk.trim();
          if (!s) continue;
          let ev: Record<string, unknown>;
          try {
            ev = JSON.parse(s) as Record<string, unknown>;
          } catch {
            continue;
          }
          if (ev["t"] !== "state") continue;
          this.applyState(ev);
        }
      });
      ws.on("error", (err: Error) => {
        if (!opened) reject(err);
      });
    });
  }

  /**
   * The opening snapshot sets the baseline; a later transition to halted or
   * unpowered raises the event. An already-halted machine at open is the
   * caller's business to check, not a mid-step deviation.
   */
  private applyState(ev: Record<string, unknown>): void {
    if (typeof ev["halt"] === "boolean") {
      const halt = ev["halt"];
      if (this.lastHalt === false && halt) this.emit("halt");
      this.lastHalt = halt;
    }
    if (typeof ev["powered"] === "boolean") {
      const powered = ev["powered"];
      if (this.lastPowered === true && !powered) this.emit("power-loss");
      this.lastPowered = powered;
    }
  }

  private emit(ev: MachineEventName): void {
    for (const cb of this.cbs) cb(ev);
  }

  close(): void {
    try {
      this.ws?.close();
    } catch {
      /* ignore */
    }
  }
}
