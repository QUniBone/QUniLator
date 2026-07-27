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
  // Derived: the CPU is executing (powered and the HALT line released). This is
  // the one field to check before trying to boot or run anything on the machine.
  running?: boolean;
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
  async getLogging(): Promise<unknown> {
    return this.get("/api/logging");
  }
  async setLogLevel(source: string, level: string): Promise<unknown> {
    return this.put(
      `/api/logging/sources/${encodeURIComponent(source)}`,
      { level },
    );
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
   * Hold a /ws/events subscription and resolve when a state event reports the
   * CPU running (powered with HALT released; the opening snapshot counts), or
   * at timeout. The counterpart to waitForHalt for confirming a power-up.
   */
  waitForRunning(timeoutMs: number): Promise<{ running: boolean }> {
    return new Promise((resolve, reject) => {
      const ws = this.openWs("/ws/events");
      let done = false;
      const timer = setTimeout(() => finish(false), timeoutMs);
      function finish(running: boolean) {
        if (done) return;
        done = true;
        clearTimeout(timer);
        try {
          ws.close();
        } catch {
          /* ignore */
        }
        resolve({ running });
      }
      ws.on("message", (data) => {
        for (const ev of parseEvents(data)) {
          if (ev.t === "state" && ev.powered === true && ev.halt === false)
            finish(true);
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
   * Bring the machine up running and confirm it: issue the power-up action
   * (restart releases HALT then restarts the CPU from the power-up vector;
   * dc_on logically powers on running), then wait for a state frame reporting
   * the CPU running. Returns the confirmed state so the caller never proceeds
   * against a machine still halted in ODT.
   *
   * Prefer this over a bare `powercycle`, which does NOT release the HALT line:
   * on a machine you have `halt`ed, `powercycle` comes up halted in micro-ODT.
   */
  async powerUp(
    action: "restart" | "dc_on" = "restart",
    timeoutMs = 8000,
  ): Promise<MachineState> {
    await this.control(action);
    await this.waitForRunning(timeoutMs);
    return this.getMachineState();
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
        // The CPU is running when it is powered and the HALT line is released.
        if (state.powered !== undefined || state.halt !== undefined)
          state.running = state.powered === true && state.halt === false;
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

  //
  // runXxdpDiagnostic: end-to-end run of an XXDP diagnostic. Applies a config,
  // applies device setup, brings the machine up running (the boot ROM auto-boots
  // XXDP), then over the real console SLU loads the diagnostic, drives the DRS
  // Change-HW dialog from an answer map, and returns pass/fail with a transcript.
  // Encapsulates the console quirks: /ws/console/ext replays its whole history on
  // connect (so only NEW output since a send is trusted), each '?' prompt is
  // terminated by a non-printable, and the SLU has no RX FIFO (send one paced
  // char at a time).
  //
  async runXxdpDiagnostic(opts: XxdpRunOptions): Promise<XxdpResult> {
    const passPattern = opts.passPattern ?? "END PASS";
    const failMarkers = opts.failMarkers ?? [
      "FTL", "FATAL", "No unit", "NOT FOUND", "Invalid Char", "?BAD", "ILL ",
    ];
    const answers = opts.answers ?? [];

    // 1. config + device setup (with the CPU halted for register changes)
    if (opts.config) {
      await this.applyConfig(opts.config);
      await delay(1500);
    }
    await this.control("halt");
    await this.waitForHalt(4000);
    for (const s of opts.setup ?? []) {
      if (s.enabled !== undefined)
        await this.setParam(s.device, "enabled", s.enabled ? "true" : "false");
      else if (s.param !== undefined)
        await this.setParam(s.device, s.param, s.value ?? "");
    }

    // 2. bring the machine up running (restart keeps the applied config)
    await this.powerUp("restart", opts.bootTimeoutMs ?? 9000);

    // 3. drive the console
    const ws = this.openWs("/ws/console/ext");
    let buf = "";
    ws.on("message", (d: WebSocket.RawData) => {
      buf += Buffer.isBuffer(d) ? d.toString("latin1") : String(d);
    });
    const sendLine = async (s: string): Promise<void> => {
      for (const ch of s) {
        ws.send(Buffer.from(ch, "latin1"));
        await delay(30);
      }
      ws.send(Buffer.from("\r"));
      await delay(150);
    };
    const waitFor = async (
      test: (b: string) => string | null,
      timeoutMs: number,
    ): Promise<string | null> => {
      const end = Date.now() + timeoutMs;
      while (Date.now() < end) {
        const r = test(buf);
        if (r) return r;
        await delay(120);
      }
      return null;
    };
    const finish = (passed: boolean, by: string): XxdpResult => {
      try {
        ws.close();
      } catch {
        /* ignore */
      }
      return { passed, terminatedBy: by, transcript: buf.slice(-3000) };
    };

    try {
      await delay(500);
      // Boot: probe for the XXDP monitor by sending a CR until a '.' prompt
      // answers (the boot ignores input until the monitor is up).
      let atMonitor = false;
      const bootEnd = Date.now() + (opts.monitorTimeoutMs ?? 25000);
      while (Date.now() < bootEnd) {
        const mark = buf.length;
        await sendLine("");
        await delay(1200);
        const seg = buf.slice(mark);
        if (/\.\s*$/.test(seg.replace(/[\x00-\x1f]+$/, "")) || /XXDP/.test(seg)) {
          atMonitor = true;
          break;
        }
      }
      if (!atMonitor) return finish(false, "no XXDP monitor prompt after boot");

      // Load the diagnostic
      const mark0 = buf.length;
      await sendLine("R " + opts.diagnostic);
      const loaded = await waitFor((b) => {
        const seg = b.slice(mark0);
        if (/DR>/.test(seg)) return "DR";
        if (/NOT FOUND|\?\s*$/.test(seg.replace(/[\x00-\x1f]+$/, ""))) return "fail";
        return null;
      }, opts.loadTimeoutMs ?? 15000);
      if (loaded !== "DR")
        return finish(false, `diagnostic ${opts.diagnostic} did not load (DR> not reached)`);

      // START and drive the DRS dialog
      await sendLine("START");
      const deadline = Date.now() + (opts.runTimeoutMs ?? 180000);
      let lastAnswered = "";
      while (Date.now() < deadline) {
        await delay(120);
        const tail = buf.slice(-400);
        if (tail.includes(passPattern)) return finish(true, passPattern);
        const prompt = pendingPrompt(buf);
        if (prompt && prompt !== lastAnswered) {
          const ans = matchAnswer(prompt, answers);
          if (ans === undefined)
            return finish(false, `unhandled prompt: ${prompt}`);
          lastAnswered = prompt;
          await sendLine(ans);
          continue;
        }
        const fm = failMarkers.find((m) => tail.includes(m));
        if (fm && !prompt) return finish(false, `fail marker: ${fm.trim()}`);
      }
      return finish(false, "timeout waiting for END PASS");
    } catch (err) {
      return finish(false, `error: ${String(err)}`);
    }
  }
}

function delay(ms: number): Promise<void> {
  return new Promise((r) => setTimeout(r, ms));
}

//
// pendingPrompt: if the console is currently sitting at a '?' prompt (the tail,
// stripped of trailing spaces and the non-printable terminator, ends with '?'),
// return that prompt's line; else null.
//
export function pendingPrompt(buf: string): string | null {
  const t = buf.replace(/[\x00-\x20]+$/, "");
  if (!t.endsWith("?")) return null;
  const lines = buf.replace(/\r/g, "\n").split("\n").filter((l) => l.includes("?"));
  if (!lines.length) return null;
  // strip leading/trailing spaces and control chars (e.g. the '?' terminator)
  return lines[lines.length - 1]
    .replace(/^[\x00-\x20]+/, "")
    .replace(/[\x00-\x20]+$/, "");
}

//
// matchAnswer: first answer whose (case-insensitive) match substring is in the
// prompt; undefined if none. An empty value means "send a bare CR" (accept the
// prompt's default).
//
export function matchAnswer(
  prompt: string,
  answers: XxdpAnswer[],
): string | undefined {
  const up = prompt.toUpperCase();
  for (const a of answers)
    if (up.includes(a.match.toUpperCase())) return a.value;
  return undefined;
}

export interface XxdpSetupStep {
  device: string;
  param?: string;
  value?: string;
  enabled?: boolean;
}
export interface XxdpAnswer {
  match: string;
  value: string;
}
export interface XxdpRunOptions {
  config?: string;
  setup?: XxdpSetupStep[];
  diagnostic: string;
  answers?: XxdpAnswer[];
  passPattern?: string;
  failMarkers?: string[];
  bootTimeoutMs?: number;
  monitorTimeoutMs?: number;
  loadTimeoutMs?: number;
  runTimeoutMs?: number;
}
export interface XxdpResult {
  passed: boolean;
  terminatedBy: string;
  transcript: string;
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
