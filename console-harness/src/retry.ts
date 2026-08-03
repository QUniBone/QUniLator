/* retry.ts: connection retries against a board that is coming up. A deploy
 * restarts the service: the old process can take its stop-timeout to die and
 * the new one seconds more to listen, and in that window a request is refused,
 * reset, or answered with a half-written response. Those failures are
 * transient and retrying is the right response; anything else — an HTTP error
 * status, bad auth, a script failure — propagates at once.
 */

const TRANSIENT_CODES = new Set([
  "ECONNREFUSED",
  "ECONNRESET",
  "EPIPE",
  "ETIMEDOUT",
  "EHOSTUNREACH",
  "ENETUNREACH",
  "UND_ERR_SOCKET",
  "UND_ERR_CONNECT_TIMEOUT",
  "HPE_INVALID_CONSTANT",
]);

const TRANSIENT_MESSAGES =
  /parse error|socket hang up|other side closed|fetch failed|terminated/i;

/** A failure of the connection itself, as against an answer that is an error. */
export function isTransient(err: unknown): boolean {
  let e = err as (Error & { code?: string; cause?: unknown }) | undefined;
  for (let depth = 0; e !== undefined && depth < 5; depth++) {
    if (e.code !== undefined && TRANSIENT_CODES.has(e.code)) return true;
    if (typeof e.message === "string" && TRANSIENT_MESSAGES.test(e.message))
      return true;
    e = e.cause as (Error & { code?: string; cause?: unknown }) | undefined;
  }
  return false;
}

export interface RetryOptions {
  /** Give up this long after the first attempt (default 90 s). */
  timeoutMs?: number;
  /** Pause between attempts (default 2 s). */
  delayMs?: number;
  /** Where the one "waiting for the board" notice goes. */
  log?: (line: string) => void;
}

/**
 * Runs the attempt, and on a transient connection failure keeps re-running it
 * until the deadline, announcing the wait once. The error that ends the last
 * attempt is the one thrown.
 */
export async function withRetry<T>(
  what: string,
  attempt: () => Promise<T>,
  opts: RetryOptions = {},
): Promise<T> {
  const timeoutMs = opts.timeoutMs ?? 90_000;
  const delayMs = opts.delayMs ?? 2_000;
  const deadline = Date.now() + timeoutMs;
  let announced = false;
  for (;;) {
    try {
      return await attempt();
    } catch (err) {
      if (!isTransient(err) || Date.now() + delayMs > deadline) throw err;
      if (!announced) {
        announced = true;
        opts.log?.(
          `${what}: ${err instanceof Error ? err.message : String(err)} — ` +
            `waiting for the board (up to ${Math.round(timeoutMs / 1000)}s)\n`,
        );
      }
      await new Promise((r) => setTimeout(r, delayMs));
    }
  }
}
