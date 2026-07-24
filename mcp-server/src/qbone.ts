/* qbone.ts: the thin client the tools call.
 *
 * Every REST tool is one method here; the console and wait-for tools are short
 * WebSocket subscriptions over the board's existing /ws/console/<ch> and
 * /ws/events streams — no board endpoint blocks or is added. The board replays
 * a console channel's retained ring on connect, so a snapshot is just "connect,
 * collect the replay, close", and wait-for-console matches across the
 * replayed-then-live boundary from that same accumulated text.
 */
import WebSocket from "ws";
import type { BoardConfig } from "./config.js";

export type ConsoleChannel = "0" | "1" | "ext";

// Logger severities as they cross /ws/events: lower is more severe.
export const LOG_LEVELS = {
  fatal: 1,
  error: 2,
  warning: 3,
  info: 4,
  debug: 5,
} as const;
export type LogLevelName = keyof typeof LOG_LEVELS;
const LOG_NAMES: Record<number, string> = {
  1: "fatal",
  2: "error",
  3: "warning",
  4: "info",
  5: "debug",
};

export interface LogLine {
  level: number;
  levelName: string;
  label: string;
  text: string;
}

export interface MachineState {
  halt?: boolean;
  powered?: boolean;
  leds?: number[];
  switches?: number[];
  init?: boolean;
  dcok?: boolean;
  pok?: boolean;
}

export class QBoneError extends Error {
  constructor(
    public status: number,
    message: string,
  ) {
    super(message);
    this.name = "QBoneError";
  }
}

export class QBoneClient {
  constructor(private cfg: BoardConfig) {}

  // ---- HTTP -------------------------------------------------------------

  private headers(extra?: Record<string, string>): Record<string, string> {
    const h: Record<string, string> = { ...extra };
    if (this.cfg.authHeader) h["Authorization"] = this.cfg.authHeader;
    return h;
  }

  private async request(
    method: string,
    path: string,
    body?: unknown,
  ): Promise<unknown> {
    const init: RequestInit = { method, headers: this.headers() };
    if (body !== undefined) {
      (init.headers as Record<string, string>)["Content-Type"] =
        "application/json";
      init.body = JSON.stringify(body);
    }
    const res = await fetch(this.cfg.httpBase + path, init);
    const text = await res.text();
    let parsed: unknown = undefined;
    if (text.length > 0) {
      try {
        parsed = JSON.parse(text);
      } catch {
        parsed = text;
      }
    }
    if (!res.ok) {
      const msg =
        parsed && typeof parsed === "object" && "error" in parsed
          ? String((parsed as { error: unknown }).error)
          : `HTTP ${res.status}`;
      throw new QBoneError(res.status, msg);
    }
    return parsed;
  }

  get(path: string): Promise<unknown> {
    return this.request("GET", path);
  }
  put(path: string, body: unknown): Promise<unknown> {
    return this.request("PUT", path, body);
  }
  post(path: string, body?: unknown): Promise<unknown> {
    return this.request("POST", path, body);
  }

  async getDevices(): Promise<unknown> {
    return this.get("/api/devices");
  }
  async setParam(dev: string, param: string, value: string): Promise<unknown> {
    return this.put(
      `/api/devices/${encodeURIComponent(dev)}/params/${encodeURIComponent(param)}`,
      { value },
    );
  }
  async control(action: string): Promise<unknown> {
    return this.post("/api/control", { action });
  }
  async getConfigs(): Promise<unknown> {
    return this.get("/api/configs");
  }
  async getLiveConfig(): Promise<unknown> {
    return this.get("/api/configs?current=1");
  }
  async applyConfig(name: string): Promise<unknown> {
    return this.post(`/api/configs/${encodeURIComponent(name)}/apply`);
  }
  async saveLiveConfig(name: string, live: unknown): Promise<unknown> {
    return this.put(`/api/configs/${encodeURIComponent(name)}?from=live`, live);
  }
  async setDefaultConfig(name: string): Promise<unknown> {
    return this.put(`/api/configs/${encodeURIComponent(name)}/default`, {});
  }
  async getImages(): Promise<unknown> {
    return this.get("/api/images");
  }

  async uploadImage(name: string, data: Buffer): Promise<unknown> {
    const form = new FormData();
    const bytes = new Uint8Array(data);
    form.append("file", new Blob([bytes]), name);
    const res = await fetch(this.cfg.httpBase + "/api/images", {
      method: "POST",
      headers: this.headers(),
      body: form,
    });
    const text = await res.text();
    let parsed: unknown = text;
    try {
      parsed = JSON.parse(text);
    } catch {
      /* leave as text */
    }
    if (!res.ok) {
      const msg =
        parsed && typeof parsed === "object" && "error" in parsed
          ? String((parsed as { error: unknown }).error)
          : `HTTP ${res.status}`;
      throw new QBoneError(res.status, msg);
    }
    return parsed;
  }

  // ---- WebSocket helpers ------------------------------------------------

  private openWs(path: string): WebSocket {
    const opts = this.cfg.authHeader
      ? { headers: { Authorization: this.cfg.authHeader } }
      : {};
    return new WebSocket(this.cfg.wsBase + path, opts);
  }

  private static decode(frame: WebSocket.RawData): string {
    if (Buffer.isBuffer(frame)) return frame.toString("latin1");
    if (Array.isArray(frame))
      return Buffer.concat(frame as Buffer[]).toString("latin1");
    return Buffer.from(frame as ArrayBuffer).toString("latin1");
  }

  /**
   * Snapshot a console channel: connect, collect the replayed ring plus any
   * bytes that arrive while settling, then close. Resolves once the stream has
   * been idle for settleMs after the first frame, or at timeoutMs.
   */
  consoleRead(
    channel: ConsoleChannel,
    opts: { settleMs?: number; timeoutMs?: number } = {},
  ): Promise<string> {
    const settleMs = opts.settleMs ?? 300;
    const timeoutMs = opts.timeoutMs ?? 2000;
    return new Promise((resolve, reject) => {
      const ws = this.openWs(`/ws/console/${channel}`);
      let acc = "";
      let settle: NodeJS.Timeout | undefined;
      const hard = setTimeout(finish, timeoutMs);
      function finish() {
        clearTimeout(hard);
        if (settle) clearTimeout(settle);
        try {
          ws.close();
        } catch {
          /* ignore */
        }
        resolve(acc);
      }
      ws.on("message", (data) => {
        acc += QBoneClient.decode(data);
        if (settle) clearTimeout(settle);
        settle = setTimeout(finish, settleMs);
      });
      ws.on("error", (err) => {
        clearTimeout(hard);
        if (settle) clearTimeout(settle);
        reject(err);
      });
    });
  }

  /** Send bytes to a console channel as a single binary frame, then close. */
  consoleSend(channel: ConsoleChannel, data: Buffer): Promise<void> {
    return new Promise((resolve, reject) => {
      const ws = this.openWs(`/ws/console/${channel}`);
      ws.on("open", () => {
        ws.send(data, { binary: true }, (err) => {
          if (err) {
            reject(err);
            return;
          }
          // Give the frame a moment to flush before closing the socket.
          setTimeout(() => {
            try {
              ws.close();
            } catch {
              /* ignore */
            }
            resolve();
          }, 50);
        });
      });
      ws.on("error", reject);
    });
  }

  /**
   * Connect a console channel and resolve when the accumulated output (replay
   * then live) matches the pattern, or at timeout. Returns whether it matched
   * and the text seen.
   */
  waitForConsole(
    channel: ConsoleChannel,
    pattern: RegExp,
    timeoutMs: number,
  ): Promise<{ matched: boolean; output: string }> {
    return new Promise((resolve, reject) => {
      const ws = this.openWs(`/ws/console/${channel}`);
      let acc = "";
      let done = false;
      const timer = setTimeout(() => finish(false), timeoutMs);
      function finish(matched: boolean) {
        if (done) return;
        done = true;
        clearTimeout(timer);
        try {
          ws.close();
        } catch {
          /* ignore */
        }
        resolve({ matched, output: acc });
      }
      ws.on("message", (data) => {
        acc += QBoneClient.decode(data);
        if (pattern.test(acc)) finish(true);
      });
      ws.on("error", (err) => {
        if (done) return;
        done = true;
        clearTimeout(timer);
        reject(err);
      });
    });
  }

  /**
   * Hold a /ws/events subscription and resolve when a state event reports halt
   * (the opening snapshot counts, so an already-halted machine resolves at
   * once), or at timeout.
   */
  waitForHalt(timeoutMs: number): Promise<{ halted: boolean }> {
    return new Promise((resolve, reject) => {
      const ws = this.openWs("/ws/events");
      let done = false;
      const timer = setTimeout(() => finish(false), timeoutMs);
      function finish(halted: boolean) {
        if (done) return;
        done = true;
        clearTimeout(timer);
        try {
          ws.close();
        } catch {
          /* ignore */
        }
        resolve({ halted });
      }
      ws.on("message", (data) => {
        for (const ev of parseEvents(data)) {
          if (ev.t === "state" && ev.halt === true) finish(true);
        }
      });
      ws.on("error", (err) => {
        if (done) return;
        done = true;
        clearTimeout(timer);
        reject(err);
      });
    });
  }

  /**
   * Read the current machine state from the opening /ws/events snapshot,
   * merging the leading partial state frames into one view.
   */
  getMachineState(timeoutMs = 2000): Promise<MachineState> {
    return new Promise((resolve, reject) => {
      const ws = this.openWs("/ws/events");
      const state: MachineState = {};
      let settle: NodeJS.Timeout | undefined;
      let done = false;
      const hard = setTimeout(() => finish(), timeoutMs);
      function finish() {
        if (done) return;
        done = true;
        clearTimeout(hard);
        if (settle) clearTimeout(settle);
        try {
          ws.close();
        } catch {
          /* ignore */
        }
        resolve(state);
      }
      ws.on("message", (data) => {
        for (const ev of parseEvents(data)) {
          if (ev.t !== "state") continue;
          for (const k of [
            "halt",
            "powered",
            "leds",
            "switches",
            "init",
            "dcok",
            "pok",
          ] as const) {
            if (ev[k] !== undefined)
              (state as Record<string, unknown>)[k] = ev[k];
          }
          // The snapshot arrives right after connect; settle briefly to absorb
          // any partial follow-up frames, then return the merged view.
          if (settle) clearTimeout(settle);
          settle = setTimeout(finish, 150);
        }
      });
      ws.on("error", (err) => {
        if (done) return;
        done = true;
        clearTimeout(hard);
        if (settle) clearTimeout(settle);
        reject(err);
      });
    });
  }

  /**
   * Tail the /ws/events log stream for durationMs and return the log lines at
   * or above the given severity (fatal is most severe). The board keeps no log
   * history over the API, so this collects what is emitted during the window.
   */
  getLog(
    level: LogLevelName,
    durationMs: number,
    maxLines: number,
  ): Promise<LogLine[]> {
    const threshold = LOG_LEVELS[level];
    return new Promise((resolve, reject) => {
      const ws = this.openWs("/ws/events");
      const lines: LogLine[] = [];
      let done = false;
      const timer = setTimeout(finish, durationMs);
      function finish() {
        if (done) return;
        done = true;
        clearTimeout(timer);
        try {
          ws.close();
        } catch {
          /* ignore */
        }
        resolve(lines.slice(-maxLines));
      }
      ws.on("message", (data) => {
        for (const ev of parseEvents(data)) {
          if (ev.t !== "log") continue;
          const lvl = Number(ev.level);
          if (lvl <= threshold) {
            lines.push({
              level: lvl,
              levelName: LOG_NAMES[lvl] ?? String(lvl),
              label: String(ev.label ?? ""),
              text: String(ev.text ?? ""),
            });
          }
        }
      });
      ws.on("error", (err) => {
        if (done) return;
        done = true;
        clearTimeout(timer);
        reject(err);
      });
    });
  }
}

interface AnyEvent {
  t?: string;
  [k: string]: unknown;
}

function parseEvents(data: WebSocket.RawData): AnyEvent[] {
  // /ws/events frames are one JSON object each, but be tolerant of a frame
  // that batches several newline-separated objects.
  const text = Buffer.isBuffer(data)
    ? data.toString("utf8")
    : String(data as unknown);
  const out: AnyEvent[] = [];
  for (const chunk of text.split("\n")) {
    const s = chunk.trim();
    if (!s) continue;
    try {
      out.push(JSON.parse(s) as AnyEvent);
    } catch {
      /* ignore non-JSON frame */
    }
  }
  return out;
}
