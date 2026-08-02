/* sessions.ts: console sessions for the MCP tools.
 *
 * The stateless console tools open a socket per call, so each one receives the
 * channel's replayed history and has to reason about it, and each send is a
 * burst the guest's FIFO-less SLU drops characters from. A session instead
 * holds one connection for as long as the agent is driving the machine: the
 * replay is consumed once at the anchor, matching runs against what the guest
 * has printed since the last step, and input is paced on the guest's echo.
 *
 * Sessions live in this process, keyed by an id the agent passes back. One
 * abandoned by an agent that stopped mid-dialog would hold its socket and, on
 * a console channel, the terminal-answerer role, so a session is closed after
 * IDLE_TIMEOUT_MS without a call.
 */
import {
  Session,
  WsTransport,
  MachineEvents,
  CastRecorder,
  type Deviation,
  type InputMode,
} from "qcon";
import type { BoardConfig } from "./config.js";
import type { ConsoleChannel } from "./qbone.js";

const IDLE_TIMEOUT_MS = 15 * 60 * 1000;
const MAX_SESSIONS = 8;

export interface SessionInfo {
  id: string;
  channel: string;
  openedAt: number;
  lastUsedAt: number;
  recordingPath?: string;
}

interface Entry {
  info: SessionInfo;
  session: Session;
  recorder?: CastRecorder;
  timer: NodeJS.Timeout;
}

export class SessionManager {
  private entries = new Map<string, Entry>();
  private counter = 0;

  constructor(private cfg: BoardConfig) {}

  private touch(entry: Entry): void {
    entry.info.lastUsedAt = Date.now();
    clearTimeout(entry.timer);
    entry.timer = setTimeout(() => {
      void this.close(entry.info.id);
    }, IDLE_TIMEOUT_MS);
    entry.timer.unref?.();
  }

  async open(opts: {
    channel: ConsoleChannel;
    deviations?: Deviation[];
    recordPath?: string;
    timeoutMs?: number;
  }): Promise<SessionInfo> {
    if (this.entries.size >= MAX_SESSIONS)
      throw new Error(
        `too many open console sessions (${MAX_SESSIONS}); close one first`,
      );
    const id = `s${++this.counter}`;
    const recorder = opts.recordPath
      ? new CastRecorder(opts.recordPath, { title: `${this.cfg.host}:${opts.channel}` })
      : undefined;
    const transport = new WsTransport(
      `${this.cfg.wsBase}/ws/console/${opts.channel}`,
      { authHeader: this.cfg.authHeader || undefined },
    );
    const events = new MachineEvents(
      `${this.cfg.wsBase}/ws/events`,
      this.cfg.authHeader || undefined,
    );
    const session = new Session(transport, {
      recorder,
      events,
      deviations: opts.deviations,
      defaultTimeoutMs: opts.timeoutMs,
    });
    await events.open();
    await session.open();
    const info: SessionInfo = {
      id,
      channel: opts.channel,
      openedAt: Date.now(),
      lastUsedAt: Date.now(),
      recordingPath: opts.recordPath,
    };
    const entry: Entry = {
      info,
      session,
      recorder,
      timer: setTimeout(() => void this.close(id), IDLE_TIMEOUT_MS),
    };
    entry.timer.unref?.();
    this.entries.set(id, entry);
    return info;
  }

  private entry(id: string): Entry {
    const e = this.entries.get(id);
    if (e === undefined)
      throw new Error(
        `no console session ${id} (it may have been closed, or timed out after ${IDLE_TIMEOUT_MS / 60000} minutes idle)`,
      );
    this.touch(e);
    return e;
  }

  get(id: string): Session {
    return this.entry(id).session;
  }

  list(): SessionInfo[] {
    return [...this.entries.values()].map((e) => e.info);
  }

  async close(id: string): Promise<{ closed: boolean; recording?: string }> {
    const e = this.entries.get(id);
    if (e === undefined) return { closed: false };
    clearTimeout(e.timer);
    this.entries.delete(id);
    await e.session.close(0);
    return { closed: true, recording: e.info.recordingPath };
  }

  async closeAll(): Promise<void> {
    for (const id of [...this.entries.keys()]) await this.close(id);
  }
}

export type { Deviation, InputMode };
