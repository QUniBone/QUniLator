/* config.ts: where the board is and how to authenticate to it.
 *
 * The board host comes from QBONE_HOST (default "qbone"); it may carry a port
 * (e.g. "127.0.0.1:8080"). The credential is QBone's HTTP basic user name and
 * password: the password is read once from ~/.qbone-pw, and the name from
 * QBONE_USER or ~/.qbone-user.
 *
 * The name is part of the credential. A board provisioned through the
 * first-run dialog carries one identity that is both the operator's account
 * and the web login, and it answers 401 to the right password under the wrong
 * name. A board set up before that dialog existed carries only a password and
 * takes any name, which is what the empty name below still serves.
 *
 * QBONE_PW_FILE and QBONE_USER_FILE override the file locations, which the
 * test suite points at stubbed credentials.
 */
import { readFileSync } from "node:fs";
import { homedir } from "node:os";
import { join } from "node:path";

export interface BoardConfig {
  host: string;
  httpBase: string;
  wsBase: string;
  authHeader: string;
}

function passwordFile(): string {
  return process.env.QBONE_PW_FILE ?? join(homedir(), ".qbone-pw");
}

function userFile(): string {
  return process.env.QBONE_USER_FILE ?? join(homedir(), ".qbone-user");
}

/** The board account's name: QBONE_USER, else ~/.qbone-user, else empty. */
function boardUser(): string {
  const fromEnv = process.env.QBONE_USER?.trim();
  if (fromEnv) return fromEnv;
  try {
    return readFileSync(userFile(), "utf8").trim();
  } catch {
    return "";
  }
}

export function loadConfig(): BoardConfig {
  const host = (process.env.QBONE_HOST ?? "qbone").trim();
  let authHeader = "";
  try {
    const pw = readFileSync(passwordFile(), "utf8").trim();
    if (pw.length > 0)
      authHeader =
        "Basic " + Buffer.from(boardUser() + ":" + pw).toString("base64");
  } catch {
    // A board with no password answers everything; leave the header empty.
  }
  return {
    host,
    httpBase: `http://${host}`,
    wsBase: `ws://${host}`,
    authHeader,
  };
}
