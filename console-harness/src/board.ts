/* board.ts: reaching a QUniLator board — auth and console-channel resolution.
 *
 * Auth is HTTP basic with any user name and the board password; the
 * workstation convention keeps that password in ~/.qbone-pw. A board running
 * without a password needs no header.
 *
 * `console: auto` resolves which channel is the machine's console from the
 * board's configuration, so a script names the machine, not the wiring:
 * an external bridge on ttyS2 is the real console SLU (channel ext);
 * otherwise the enabled DL11 at 777560 answers (channel 0), or DL11b
 * (channel 1). Anything else needs an explicit channel.
 */
import { readFileSync } from "node:fs";
import { homedir } from "node:os";
import { join } from "node:path";

export interface BoardTarget {
  host: string;
  authHeader?: string;
}

export function loadPassword(pwFile?: string): string | undefined {
  const path = pwFile ?? join(homedir(), ".qbone-pw");
  try {
    return readFileSync(path, "utf8").trim();
  } catch {
    return undefined;
  }
}

export function basicAuth(password: string): string {
  return "Basic " + Buffer.from(":" + password).toString("base64");
}

export function makeTarget(host: string, pwFile?: string): BoardTarget {
  const pw = loadPassword(pwFile);
  return { host, authHeader: pw !== undefined ? basicAuth(pw) : undefined };
}

async function apiGet(target: BoardTarget, path: string): Promise<unknown> {
  const headers: Record<string, string> = {};
  if (target.authHeader) headers["Authorization"] = target.authHeader;
  const res = await fetch(`http://${target.host}${path}`, { headers });
  if (!res.ok) throw new Error(`GET ${path}: HTTP ${res.status}`);
  return res.json();
}

export async function resolveConsoleChannel(
  target: BoardTarget,
): Promise<string> {
  const settings = (await apiGet(target, "/api/settings")) as {
    external_console?: { source?: string };
  };
  if (settings.external_console?.source === "ttys2") return "ext";
  const devices = (await apiGet(target, "/api/devices")) as {
    name: string;
    enabled: boolean;
  }[];
  if (devices.find((d) => d.name === "DL11" && d.enabled)) return "0";
  if (devices.find((d) => d.name === "DL11b" && d.enabled)) return "1";
  throw new Error(
    `${target.host}: no console found (no external bridge on ttys2, no enabled DL11) — pass an explicit channel`,
  );
}

export function consoleWsUrl(target: BoardTarget, channel: string): string {
  return `ws://${target.host}/ws/console/${channel}`;
}

export function eventsWsUrl(target: BoardTarget): string {
  return `ws://${target.host}/ws/events`;
}
