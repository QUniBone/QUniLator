/* render.ts: a recorded session as something a person reads.
 *
 * Two shapes, from the same cast:
 *
 *   doc     a standalone HTML page: the transcript as the operator saw it,
 *           interpreted through a terminal so a redrawn line reads as the
 *           line it ended up being, split into the steps the run went
 *           through, with what was typed marked off from what the guest
 *           printed. This is the artifact a manual is built from.
 *
 *   player  a standalone HTML page embedding asciinema-player: the session
 *           replayed in its own timing, with the step markers as chapters.
 *
 * Both are single files with everything inlined, so a rendered session
 * travels as one attachment and needs no server.
 */
import { readFileSync } from "node:fs";
import { createRequire } from "node:module";
import { XtermScreen } from "./screen.js";
import { readCast, type Cast, type CastNotes } from "./recording.js";

const require = createRequire(import.meta.url);

export interface RenderOptions {
  cols?: number;
  rows?: number;
  title?: string;
}

/** A step's slice of the session, as the doc render lays it out. */
interface Block {
  label: string | null; // step heading; null for output before any step
  startMs: number;
  endMs: number;
  lines: string[];
  /** Lines typed during this block, in order, with their echo status. */
  typed: { text: string; redacted: boolean; closed: boolean }[];
}

/** Milliseconds from the session start, per event; v3 intervals are seconds
 *  relative to the previous event. */
function absoluteTimes(cast: Cast): number[] {
  let t = 0;
  return cast.events.map((e) => (t += e.interval * 1000));
}

/**
 * Split the session into blocks at the step markers, interpreting each
 * block's output through a terminal with scrollback so cursor motion,
 * erasure and CR overwrites read as the text that ended up on the screen.
 */
async function blocks(
  cast: Cast,
  notes: CastNotes | null,
  opts: RenderOptions,
): Promise<Block[]> {
  const times = absoluteTimes(cast);
  const redacted = new Set(notes?.redactedInputs ?? []);
  const out: Block[] = [];
  let cur: Block = { label: null, startMs: 0, endMs: 0, lines: [], typed: [] };
  let pending = ""; // output bytes of the current block
  let inputIndex = -1;

  const flush = async (endMs: number) => {
    cur.endMs = endMs;
    if (pending.length > 0) {
      const screen = new XtermScreen({
        cols: opts.cols ?? 80,
        rows: opts.rows ?? 24,
        scrollback: 20000,
      });
      screen.write(new Uint8Array(Buffer.from(pending, "latin1")));
      await screen.flush();
      cur.lines = screen.transcript();
      screen.dispose();
    }
    if (cur.lines.length > 0 || cur.typed.length > 0 || cur.label !== null)
      out.push(cur);
    pending = "";
  };

  for (let i = 0; i < cast.events.length; i++) {
    const ev = cast.events[i];
    switch (ev.code) {
      case "o":
        pending += ev.data;
        break;
      case "i": {
        inputIndex++;
        const isRedacted = redacted.has(inputIndex);
        // The typing arrives one character per event; join it into the line it
        // forms, so the render shows a command rather than a column of
        // letters. A carriage return ends that line: without breaking there,
        // two commands merge into one string that matches no line on screen
        // and neither gets highlighted.
        const endsLine = /[\r\n]/.test(ev.data);
        const text = ev.data.replace(/[\r\n]/g, "");
        const last = cur.typed[cur.typed.length - 1];
        if (text.length > 0) {
          if (last !== undefined && !last.closed && last.redacted === isRedacted)
            last.text += text;
          else cur.typed.push({ text, redacted: isRedacted, closed: false });
        }
        if (endsLine) {
          const open = cur.typed[cur.typed.length - 1];
          if (open !== undefined) open.closed = true;
        }
        break;
      }
      case "m": {
        // "live" and the machine actions are session events, not steps
        if (!/^step /.test(ev.data)) break;
        await flush(times[i]);
        cur = {
          label: ev.data,
          startMs: times[i],
          endMs: times[i],
          lines: [],
          typed: [],
        };
        break;
      }
      default:
        break;
    }
  }
  await flush(times.length > 0 ? times[times.length - 1] : 0);
  return out;
}

function esc(text: string): string {
  return text
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;");
}

/**
 * Mark up a transcript line: what the operator typed is highlighted where it
 * appears, so a reader can tell an answer from the prompt it answered. The
 * guest echoed the typing, so it appears once, in place, and is not repeated
 * from the input events — except where it was never echoed (a password),
 * which is shown as a redaction chip instead.
 */
function markupLine(
  line: string,
  typed: { text: string; redacted: boolean }[],
): string {
  let html = esc(line);
  for (const t of typed) {
    if (t.redacted || t.text.length === 0) continue;
    const needle = esc(t.text);
    const at = html.indexOf(needle);
    if (at < 0) continue;
    html =
      html.slice(0, at) +
      `<span class="typed">${needle}</span>` +
      html.slice(at + needle.length);
  }
  return html;
}

function fmtMs(ms: number): string {
  if (ms < 1000) return `${Math.round(ms)} ms`;
  if (ms < 60000) return `${(ms / 1000).toFixed(1)} s`;
  const m = Math.floor(ms / 60000);
  const s = Math.round((ms % 60000) / 1000);
  return `${m} m ${s} s`;
}

const DOC_CSS = `
:root { color-scheme: light dark; }
* { box-sizing: border-box; }
body { margin: 0; padding: 2rem 1rem 4rem;
  font: 15px/1.5 -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
  background: #fbfbfa; color: #1c1c1a; }
main { max-width: 62rem; margin: 0 auto; }
h1 { font-size: 1.5rem; margin: 0 0 .25rem; }
.meta { color: #6a6a63; font-size: .875rem; margin-bottom: 2rem; }
section { margin: 0 0 1.75rem; }
h2 { font-size: .8125rem; text-transform: uppercase; letter-spacing: .06em;
  color: #6a6a63; font-weight: 600; margin: 0 0 .4rem;
  display: flex; justify-content: space-between; align-items: baseline; gap: 1rem; }
h2 .elapsed { font-weight: 400; text-transform: none; letter-spacing: 0;
  font-variant-numeric: tabular-nums; }
pre { margin: 0; padding: .9rem 1.1rem; overflow-x: auto;
  background: #ffffff; border: 1px solid #e4e4de; border-radius: 6px;
  font: 13px/1.45 ui-monospace, SFMono-Regular, Menlo, monospace;
  white-space: pre; }
.typed { background: #fff2c4; color: #6b4a00; font-weight: 600;
  border-radius: 3px; padding: 0 2px; }
.redacted { background: #ececff; color: #3b3b8f; border-radius: 3px;
  padding: 0 4px; font-style: italic; }
.legend { font-size: .8125rem; color: #6a6a63; margin: -1.25rem 0 2rem; }
.legend .typed, .legend .redacted { font-size: .8125rem; }
@media (prefers-color-scheme: dark) {
  body { background: #14140f; color: #e6e6df; }
  .meta, h2, .legend { color: #9a9a8f; }
  pre { background: #1c1c17; border-color: #33332b; }
  .typed { background: #4a3a00; color: #ffd97a; }
  .redacted { background: #26264a; color: #b9b9ff; }
}
`;

export async function renderDoc(
  castPath: string,
  opts: RenderOptions = {},
): Promise<string> {
  const cast = readCast(castPath);
  let notes: CastNotes | null = null;
  try {
    notes = JSON.parse(
      readFileSync(castPath + ".notes.json", "utf8"),
    ) as CastNotes;
  } catch {
    // A cast without a sidecar (a board-side capture, a foreign recording)
    // renders without step records; the markers still delimit the steps.
  }
  const bs = await blocks(cast, notes, opts);
  const title =
    opts.title ?? (cast.header["title"] as string | undefined) ?? castPath;
  const total = bs.length > 0 ? bs[bs.length - 1].endMs : 0;

  const sections = bs
    .map((b) => {
      const body = b.lines
        .map((l) => markupLine(l, b.typed))
        .join("\n")
        .replace(/\n+$/, "");
      const redactions = b.typed.filter((t) => t.redacted);
      const chip = redactions.length
        ? `\n<span class="redacted">${redactions.length} character${redactions.length === 1 ? "" : "s"} typed, not echoed (redacted)</span>`
        : "";
      if (body.length === 0 && chip.length === 0) return "";
      return (
        `<section>` +
        (b.label
          ? `<h2><span>${esc(b.label)}</span><span class="elapsed">${fmtMs(b.endMs - b.startMs)}</span></h2>`
          : "") +
        `<pre>${body}${chip}</pre></section>`
      );
    })
    .join("\n");

  return `<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>${esc(title)}</title>
<style>${DOC_CSS}</style>
</head><body><main>
<h1>${esc(title)}</h1>
<div class="meta">${bs.filter((b) => b.label).length} steps · ${fmtMs(total)} · recorded from ${esc(castPath.split("/").pop() ?? castPath)}</div>
<div class="legend"><span class="typed">highlighted</span> is what the operator typed; everything else is what the machine printed.</div>
${sections}
</main></body></html>
`;
}

export function renderPlayer(
  castPath: string,
  opts: RenderOptions = {},
): string {
  const cast = readFileSync(castPath, "utf8");
  const js = readFileSync(
    require.resolve("asciinema-player/dist/bundle/asciinema-player.min.js"),
    "utf8",
  );
  const css = readFileSync(
    require.resolve("asciinema-player/dist/bundle/asciinema-player.css"),
    "utf8",
  );
  const header = JSON.parse(cast.split("\n")[0] || "{}") as Record<string, unknown>;
  const title =
    opts.title ?? (header["title"] as string | undefined) ?? castPath;
  // The cast rides in the page as a data: URL, so the player fetches it from
  // the document itself and the file stays self-contained.
  const dataUrl =
    "data:text/plain;base64," + Buffer.from(cast, "utf8").toString("base64");
  return `<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>${esc(title)}</title>
<style>${css}</style>
<style>
  body { margin: 0; padding: 2rem 1rem; background: #14140f; color: #e6e6df;
    font: 15px/1.5 -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif; }
  main { max-width: 68rem; margin: 0 auto; }
  h1 { font-size: 1.25rem; margin: 0 0 1rem; }
  .hint { color: #9a9a8f; font-size: .8125rem; margin-top: 1rem; }
</style>
</head><body><main>
<h1>${esc(title)}</h1>
<div id="player"></div>
<div class="hint">Space plays and pauses; the step markers are chapters — [ and ] step between them.</div>
</main>
<script>${js}</script>
<script>
  AsciinemaPlayer.create(${JSON.stringify(dataUrl)}, document.getElementById("player"), {
    idleTimeLimit: 2, fit: "width", pauseOnMarkers: false, theme: "asciinema"
  });
</script>
</body></html>
`;
}
