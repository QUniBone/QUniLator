/* matcher.ts: pattern specs as they appear in step files and tool calls.
 *
 * A spec is either fixed text ("login: ") or a regular expression written
 * /like this/i. Fixed text matches literally, control characters included,
 * so a prompt can be pinned to a line start with "\r\n@".
 */

export interface CompiledPattern {
  source: string; // the spec as written, for diagnostics
  regex: RegExp;
}

export function escapeRegExp(text: string): string {
  return text.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
}

export function compilePattern(spec: string): CompiledPattern {
  const m = /^\/(.*)\/([a-z]*)$/s.exec(spec);
  if (m) {
    let flags = m[2];
    if (!flags.includes("s")) flags += "s"; // '.' spans the byte stream
    return { source: spec, regex: new RegExp(m[1], flags) };
  }
  return { source: spec, regex: new RegExp(escapeRegExp(spec)) };
}

export interface MatchResult {
  /** Index of the pattern that matched (into the list given). */
  index: number;
  /** The matched text. */
  match: string;
  /** Text from the scan start up to the match. */
  before: string;
  /** End offset of the match within the scanned text. */
  end: number;
}

/** First pattern (in list order) that matches; earliest occurrence wins ties. */
export function scan(
  text: string,
  patterns: CompiledPattern[],
): MatchResult | null {
  let best: MatchResult | null = null;
  for (let i = 0; i < patterns.length; i++) {
    const m = patterns[i].regex.exec(text);
    if (!m) continue;
    if (best === null || m.index < best.end - best.match.length) {
      best = {
        index: i,
        match: m[0],
        before: text.slice(0, m.index),
        end: m.index + m[0].length,
      };
    }
  }
  return best;
}
