/* mock-board.ts: a standalone HTTP/WS stand-in for QBone used by the tests.
 * It records every REST request, returns canned bodies, and lets a test script
 * the /ws/events and /ws/console/<ch> streams (replay frames and live frames).
 */
import { createServer, type IncomingMessage, type Server } from "node:http";
import { WebSocketServer, type WebSocket } from "ws";
import type { AddressInfo } from "node:net";

export interface RecordedRequest {
  method: string;
  path: string;
  auth?: string;
  body?: unknown;
  contentType?: string;
}

interface ConsoleConn {
  channel: string;
  ws: WebSocket;
}

export class MockBoard {
  requests: RecordedRequest[] = [];
  responses = new Map<string, { status?: number; body: unknown }>();
  eventsClients = new Set<WebSocket>();
  consoleClients = new Set<ConsoleConn>();
  /** Replay bytes handed to each console channel on connect. */
  consoleReplay = new Map<string, Buffer>();
  /** Bytes a console client sent us, per channel. */
  consoleReceived = new Map<string, Buffer[]>();
  /** State snapshot sent on each /ws/events connect. */
  stateSnapshot: Record<string, unknown> = {
    t: "state",
    halt: false,
    powered: true,
    leds: [0, 0, 0, 0],
    switches: [1, 0, 1, 0],
  };

  private http!: Server;
  private wss!: WebSocketServer;
  private port = 0;

  /** key is `${METHOD} ${path}` */
  setResponse(key: string, body: unknown, status = 200): void {
    this.responses.set(key, { status, body });
  }

  async start(): Promise<string> {
    this.http = createServer((req, res) => this.handleHttp(req, res));
    this.wss = new WebSocketServer({ noServer: true });
    this.http.on("upgrade", (req, socket, head) => {
      this.wss.handleUpgrade(req, socket, head, (ws) => {
        this.handleWs(ws, req);
      });
    });
    await new Promise<void>((resolve) =>
      this.http.listen(0, "127.0.0.1", resolve),
    );
    this.port = (this.http.address() as AddressInfo).port;
    return `127.0.0.1:${this.port}`;
  }

  async stop(): Promise<void> {
    for (const ws of this.eventsClients) ws.close();
    for (const c of this.consoleClients) c.ws.close();
    await new Promise<void>((resolve) => this.wss.close(() => resolve()));
    await new Promise<void>((resolve) => this.http.close(() => resolve()));
  }

  // ---- events / console driving ----------------------------------------

  broadcastEvent(ev: Record<string, unknown>): void {
    const msg = JSON.stringify(ev);
    for (const ws of this.eventsClients) ws.send(msg);
  }

  broadcastConsole(channel: string, data: Buffer): void {
    for (const c of this.consoleClients)
      if (c.channel === channel) c.ws.send(data);
  }

  // ---- HTTP -------------------------------------------------------------

  private handleHttp(req: IncomingMessage, res: import("node:http").ServerResponse): void {
    const chunks: Buffer[] = [];
    req.on("data", (c) => chunks.push(c as Buffer));
    req.on("end", () => {
      const raw = Buffer.concat(chunks);
      const contentType = req.headers["content-type"];
      let body: unknown = undefined;
      if (raw.length > 0) {
        if (contentType?.includes("application/json")) {
          try {
            body = JSON.parse(raw.toString("utf8"));
          } catch {
            body = raw.toString("utf8");
          }
        } else {
          body = raw; // multipart etc.: keep raw for size checks
        }
      }
      const path = req.url ?? "";
      this.requests.push({
        method: req.method ?? "GET",
        path,
        auth: req.headers["authorization"] as string | undefined,
        body,
        contentType,
      });
      const key = `${req.method} ${path}`;
      const canned = this.responses.get(key);
      const status = canned?.status ?? 200;
      const payload = canned ? canned.body : { ok: true };
      res.writeHead(status, { "Content-Type": "application/json" });
      res.end(JSON.stringify(payload));
    });
  }

  // ---- WebSocket --------------------------------------------------------

  private handleWs(ws: WebSocket, req: IncomingMessage): void {
    const url = req.url ?? "";
    if (url === "/ws/events") {
      this.eventsClients.add(ws);
      ws.send(JSON.stringify(this.stateSnapshot));
      ws.on("close", () => this.eventsClients.delete(ws));
      return;
    }
    const m = /^\/ws\/console\/([^/]+)$/.exec(url);
    if (m) {
      const channel = m[1];
      const conn = { channel, ws };
      this.consoleClients.add(conn);
      const replay = this.consoleReplay.get(channel);
      if (replay && replay.length > 0) ws.send(replay);
      ws.on("message", (data: Buffer) => {
        const arr = this.consoleReceived.get(channel) ?? [];
        arr.push(Buffer.isBuffer(data) ? data : Buffer.from(data as ArrayBuffer));
        this.consoleReceived.set(channel, arr);
      });
      ws.on("close", () => this.consoleClients.delete(conn));
      return;
    }
    ws.close();
  }
}
