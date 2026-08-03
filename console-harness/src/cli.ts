#!/usr/bin/env node
/* cli.ts: the qcon command.
 *
 *   qcon run <script.yaml> [--host H] [--console CH] [--var NAME=VALUE]...
 *                          [--record OUT.cast] [--pw-file PATH] [--verbose]
 *   qcon record <out.cast> --console CH [--host H] [--pw-file PATH]
 *   qcon break [--console CH] [--host H]
 *   qcon render <session.cast> [--mode doc|player] [--out FILE] [--title T]
 *   qcon timings <session.cast> [--margin N]
 *
 * run executes a step file against a console and always records the session
 * (default: <script-stem>-<timestamp>.cast in the working directory). Exit
 * status: 0 pass, 1 script failure, 2 usage or connection error.
 *
 * record taps a channel and writes its output to a cast until interrupted —
 * a host-side capture of whatever the console prints (this client's view;
 * a capture of every client's input is the board-side recorder's job).
 */
import { writeFileSync } from "node:fs";
import { basename } from "node:path";
import { parseArgs } from "node:util";
import {
  makeTarget,
  resolveConsoleChannel,
  consoleWsUrl,
  eventsWsUrl,
  applyConfig,
  control,
} from "./board.js";
import { WsTransport, TcpTransport, type Transport } from "./transport.js";
import { Session } from "./session.js";
import { MachineEvents } from "./events.js";
import { CastRecorder } from "./recording.js";
import { loadScript, runScript, parseDuration, ScriptFailure } from "./steps.js";
import { renderDoc, renderPlayer } from "./render.js";
import { runTimings, formatTimings } from "./timings.js";

function usage(): never {
  process.stderr.write(
    "usage: qcon run <script.yaml> [--host H] [--console CH] [--var N=V]... [--record OUT] [--pw-file P] [--verbose]\n" +
      "       qcon record <out.cast> --console CH [--host H] [--pw-file P]\n" +
      "       qcon break [--console CH] [--host H] [--pw-file P]\n" +
      "       qcon render <session.cast> [--mode doc|player] [--out FILE] [--title T]\n" +
      "       qcon timings <session.cast> [--margin N]\n",
  );
  process.exit(2);
}

function timestamp(): string {
  return new Date().toISOString().replace(/[-:]/g, "").replace(/\..*/, "");
}

interface CommonOpts {
  host: string;
  pwFile?: string;
  channel?: string;
  verbose: boolean;
}

function makeTransport(opts: CommonOpts, channel: string): Transport {
  const tcp = /^tcp:([^:]+):(\d+)$/.exec(channel);
  if (tcp) return new TcpTransport(tcp[1], parseInt(tcp[2], 10));
  const target = makeTarget(opts.host, opts.pwFile);
  return new WsTransport(consoleWsUrl(target, channel), {
    authHeader: target.authHeader,
  });
}

async function resolveChannel(
  opts: CommonOpts,
  fromScript?: string,
): Promise<string> {
  const requested = opts.channel ?? fromScript ?? "auto";
  if (requested !== "auto") return requested;
  return resolveConsoleChannel(makeTarget(opts.host, opts.pwFile));
}

async function cmdRun(scriptPath: string, opts: CommonOpts, args: {
  vars: Record<string, string>;
  recordPath?: string;
}): Promise<number> {
  const script = loadScript(scriptPath);
  const host = opts.host !== "" ? opts.host : (script.host ?? "qbone");
  const common = { ...opts, host };
  const channel = await resolveChannel(common, script.console);
  const isTcp = channel.startsWith("tcp:");
  const target = makeTarget(host, opts.pwFile);

  const castPath =
    args.recordPath ??
    `${basename(scriptPath).replace(/\.[^.]*$/, "")}-${timestamp()}.cast`;
  const recorder = new CastRecorder(castPath, {
    title: script.title ?? basename(scriptPath),
  });

  const events = isTcp
    ? undefined
    : new MachineEvents(eventsWsUrl(target), target.authHeader);
  const transport = makeTransport(common, channel);
  const session = new Session(transport, {
    recorder,
    events,
    deviations: script.deviations,
    defaultTimeoutMs:
      script.timeout !== undefined ? parseDuration(script.timeout) : undefined,
    settleMs:
      script.settle !== undefined ? parseDuration(script.settle) : undefined,
  });
  if (opts.verbose)
    transport.onData((b) =>
      process.stderr.write(Buffer.from(b).toString("latin1")),
    );

  // vars: CLI --var wins over the environment
  const vars: Record<string, string> = {};
  for (const [k, v] of Object.entries(process.env))
    if (v !== undefined) vars[k] = v;
  Object.assign(vars, args.vars);

  try {
    if (events) await events.open();
    await session.open();
    // The machine is started only now, with the console connected and the
    // matching anchor set, so the boot it prints is live output the first
    // step sees rather than a replay the script has to reason about.
    if (script.machine && !isTcp) {
      const m = script.machine;
      if (m.config !== undefined) {
        process.stderr.write(`applying configuration ${m.config}\n`);
        recorder.marker(`config ${m.config}`);
        await applyConfig(target, m.config);
      }
      const start = m.start ?? "restart";
      if (start !== "none") {
        process.stderr.write(`starting the machine (${start})\n`);
        recorder.marker(start);
        await control(target, start);
      }
      if (m.settle !== undefined)
        await new Promise((r) => setTimeout(r, parseDuration(m.settle!)));
    }
    const result = await runScript(session, script, vars, recorder);
    await session.close(0);
    process.stdout.write(
      `PASS ${basename(scriptPath)} (${result.stepsRun} steps) — recorded ${castPath}\n`,
    );
    return 0;
  } catch (err) {
    await session.close(1).catch(() => {});
    if (err instanceof ScriptFailure) {
      process.stderr.write(`FAIL ${basename(scriptPath)}: ${err.message}\n`);
      process.stderr.write(`recorded ${castPath}\n`);
      return 1;
    }
    throw err;
  }
}

async function cmdRecord(outPath: string, opts: CommonOpts): Promise<number> {
  const channel = await resolveChannel(opts);
  const recorder = new CastRecorder(outPath, {
    title: `${opts.host}:${channel}`,
  });
  const transport = makeTransport(opts, channel);
  let bytes = 0;
  transport.onData((b) => {
    recorder.output(b);
    bytes += b.length;
  });
  await transport.open();
  process.stderr.write(`recording ${opts.host} channel ${channel} to ${outPath} — ^C to stop\n`);
  await new Promise<void>((resolve) => {
    transport.onClose(() => resolve());
    process.on("SIGINT", () => resolve());
  });
  transport.close();
  await recorder.close(0);
  process.stderr.write(`${bytes} bytes recorded\n`);
  return 0;
}

async function cmdBreak(opts: CommonOpts): Promise<number> {
  const channel = await resolveChannel(opts);
  const transport = makeTransport(opts, channel);
  await transport.open();
  await transport.sendBreak();
  transport.close();
  process.stderr.write(`BREAK sent on ${channel}\n`);
  return 0;
}

async function cmdRender(
  castPath: string,
  mode: string,
  outPath: string | undefined,
  title: string | undefined,
): Promise<number> {
  const html =
    mode === "player"
      ? renderPlayer(castPath, { title })
      : await renderDoc(castPath, { title });
  const out =
    outPath ?? castPath.replace(/\.cast$/, "") + (mode === "player" ? "-player.html" : ".html");
  writeFileSync(out, html);
  process.stderr.write(`wrote ${out} (${(html.length / 1024).toFixed(0)} KB)\n`);
  return 0;
}

// What each step took, and what its deadline should therefore be. A deadline
// set from a measured run is the point of recording one: it is neither so
// loose that a stuck step costs minutes before it says so, nor so tight that a
// slow moment fails a run that would have worked.
function cmdTimings(castPath: string, margin?: number): number {
  const t = runTimings(castPath, { margin });
  process.stdout.write(formatTimings(t));
  return 0;
}

async function main(): Promise<number> {
  const { values, positionals } = parseArgs({
    allowPositionals: true,
    options: {
      host: { type: "string", default: "" },
      console: { type: "string" },
      "pw-file": { type: "string" },
      var: { type: "string", multiple: true, default: [] },
      record: { type: "string" },
      verbose: { type: "boolean", default: false },
      mode: { type: "string", default: "doc" },
      margin: { type: "string" },
      out: { type: "string" },
      title: { type: "string" },
    },
  });
  const [cmd, arg] = positionals;
  const opts: CommonOpts = {
    host: values.host ?? "",
    pwFile: values["pw-file"],
    channel: values.console,
    verbose: values.verbose ?? false,
  };
  const vars: Record<string, string> = {};
  for (const kv of values.var ?? []) {
    const eq = kv.indexOf("=");
    if (eq < 1) usage();
    vars[kv.slice(0, eq)] = kv.slice(eq + 1);
  }

  switch (cmd) {
    case "run":
      if (!arg) usage();
      return cmdRun(arg, { ...opts, host: opts.host || "" }, {
        vars,
        recordPath: values.record,
      });
    case "record":
      if (!arg) usage();
      return cmdRecord(arg, { ...opts, host: opts.host || "qbone" });
    case "break":
      return cmdBreak({ ...opts, host: opts.host || "qbone" });
    case "render":
      if (!arg) usage();
      return cmdRender(arg, values.mode ?? "doc", values.out, values.title);
    case "timings":
      if (!arg) usage();
      return cmdTimings(
        arg,
        values.margin === undefined ? undefined : Number(values.margin),
      );
    default:
      usage();
  }
}

main().then(
  (code) => process.exit(code),
  (err: unknown) => {
    // Anything landing here is unexpected (script failures return a code), so
    // the stack is part of the report.
    process.stderr.write(
      `qcon: ${err instanceof Error ? (err.stack ?? err.message) : String(err)}\n`,
    );
    process.exit(2);
  },
);
