/* config.ts: where the board is and how to authenticate to it.
 *
 * The board host comes from QBONE_HOST (default "qbone"); it may carry a port
 * (e.g. "127.0.0.1:8080"). The credential is QBone's HTTP basic user name and
 * password: the password is read once from ~/.qbone-pw, and the name from
 * QBONE_USER or ~/.qbone-user.
 *
 * The name is part of the credential: a QUniLator carries one identity that is
 * both the operator's account and the web login, and it answers 401 to the
 * right password under another name. A password with no name beside it is
 * therefore half a credential, and this says so rather than letting every call
 * fail with 401.
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

/** The operator's name: QBONE_USER, else ~/.qbone-user, else empty. */
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
    const user = boardUser();
    if (pw.length > 0) {
      if (user.length === 0)
        process.stderr.write(
          `no operator name: set QBONE_USER or write it to ${userFile()}\n`,
        );
      authHeader = "Basic " + Buffer.from(user + ":" + pw).toString("base64");
    }
  } catch {
    // An installation nobody has set up answers everything; no header needed.
  }
  return {
    host,
    httpBase: `http://${host}`,
    wsBase: `ws://${host}`,
    authHeader,
  };
}
