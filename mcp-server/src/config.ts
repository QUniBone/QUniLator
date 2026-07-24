/* config.ts: where the board is and how to authenticate to it.
 *
 * The board host comes from QBONE_HOST (default "qbone"); it may carry a port
 * (e.g. "127.0.0.1:8080"). The credential is QBone's HTTP basic password, read
 * once from ~/.qbone-pw with any user name — the same auth the REST examples
 * and the Vite dev proxy use. QBONE_PW_FILE overrides the file location, which
 * the test suite points at a stubbed password.
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

export function loadConfig(): BoardConfig {
  const host = (process.env.QBONE_HOST ?? "qbone").trim();
  let authHeader = "";
  try {
    const pw = readFileSync(passwordFile(), "utf8").trim();
    if (pw.length > 0)
      authHeader = "Basic " + Buffer.from(":" + pw).toString("base64");
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
