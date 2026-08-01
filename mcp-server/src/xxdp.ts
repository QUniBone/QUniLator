/* xxdp.ts: run an XXDP diagnostic end to end.
 *
 * The dialog itself is ordinary console work — wait for a prompt, answer it,
 * repeat — so it is driven by the console harness rather than by console code
 * of its own: echo-paced input, matching anchored past the replayed ring,
 * per-step deadlines, and a stuck step reported at the prompt it is stuck on.
 *
 * What belongs here is what is specific to XXDP, and it is all learned from
 * recorded runs on the board:
 *
 *  - The two supervisors print the same prompts in different case (DRSXM
 *    "Change HW (L)  ?", DRSSM "CHANGE HW (L)  ?") and terminate them
 *    differently (DRSXM with ^D, DRSSM with a bare space), so prompts are
 *    matched case-insensitively and the catch-all keys on the question mark.
 *  - A logical "(L)" prompt has NO default: answering it with a bare CR
 *    prints "NO DEFAULT" and asks again, forever. Octal "(O)" and decimal
 *    "(D)" prompts do take a bare CR for the value they show.
 *  - "EOP n" is not a pass. It prints on failing runs too, followed by the
 *    error count; a pass is that count being zero.
 *  - "DR>" during a run means the diagnostic gave up and returned to the
 *    supervisor, which is a failure however clean the output looks.
 */
import {
  Session,
  WsTransport,
  MachineEvents,
  CastRecorder,
  ScriptFailure,
  runScript,
  validateScript,
  type ScriptSpec,
} from "qcon";
import type { BoardConfig } from "./config.js";
import type { ConsoleChannel } from "./qbone.js";
import type { QBoneClient } from "./qbone.js";

export interface XxdpAnswer {
  match: string;
  value: string;
}

export interface XxdpSetupStep {
  device: string;
  param?: string;
  value?: string;
  enabled?: boolean;
}

export interface XxdpRunOptions {
  diagnostic: string;
  config?: string;
  console?: ConsoleChannel;
  setup?: XxdpSetupStep[];
  answers?: XxdpAnswer[];
  runTimeoutMs?: number;
  recordPath?: string;
}

export interface XxdpResult {
  passed: boolean;
  terminatedBy: string;
  transcript: string;
  recording?: string;
}

/** The dialog as a script: the caller's answers first, then the defaults. */
export function xxdpScript(opts: XxdpRunOptions): ScriptSpec {
  const answerCases = (opts.answers ?? []).map((a) => ({
    // The caller names a prompt by a substring; anchor it to the question so
    // an answer cannot fire on the word appearing in a banner.
    match: `/${a.match.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")}[^?\\n]*\\?/i`,
    send: a.value,
    goto: "dialog",
  }));
  return {
    steps: [
      {
        name: "monitor",
        expect: [
          { match: "ENTER DATE", send: "" },
          { match: "/RESTART ADDR|\\n\\./", goto: "load" },
        ],
        timeout: "90s",
      },
      { name: "prompt", expect: "/\\n\\./" },
      { name: "load", wait: "2s", send: `R ${opts.diagnostic}` },
      {
        name: "loaded",
        expect: [
          { match: "DR>", send: "START", goto: "dialog" },
          { match: "/SWR\\s*=|NEW\\s*=/", send: "", goto: "dialog" },
          { match: "/NOT FOUND/i", fail: `${opts.diagnostic} is not on the media` },
        ],
        timeout: "90s",
      },
      {
        name: "dialog",
        expect: [
          ...answerCases,
          // A logical prompt has no default; a bare CR would loop forever.
          { match: "/\\(L\\)[^?\\n]*\\?/", send: "N", goto: "dialog" },
          // Anything else takes the value it shows.
          { match: "/\\?[\\x00-\\x20]*$/", send: "", goto: "dialog" },
          { match: "/END PASS|\\b0 (CUMULATIVE )?ERRORS/i", done: true },
          {
            match: "/DVC FTL|FTL ERR|FATAL|[1-9]\\d* (CUMULATIVE )?ERRORS/i",
            fail: "the diagnostic reported errors",
          },
          {
            match: "/\\r\\nDR>/",
            fail: "the diagnostic returned to the supervisor without finishing",
          },
        ],
        timeout: opts.runTimeoutMs ? `${opts.runTimeoutMs}ms` : "300s",
      },
    ],
  };
}

export async function runXxdpDiagnostic(
  cfg: BoardConfig,
  qbone: QBoneClient,
  opts: XxdpRunOptions,
): Promise<XxdpResult> {
  const channel = opts.console ?? "ext";
  const recorder = opts.recordPath
    ? new CastRecorder(opts.recordPath, { title: `XXDP ${opts.diagnostic}` })
    : undefined;
  const transport = new WsTransport(`${cfg.wsBase}/ws/console/${channel}`, {
    authHeader: cfg.authHeader || undefined,
  });
  const events = new MachineEvents(
    `${cfg.wsBase}/ws/events`,
    cfg.authHeader || undefined,
  );
  const session = new Session(transport, {
    recorder,
    events,
    deviations: [{ event: "halt", fail: "the CPU halted during the run" }],
  });

  let transcript = "";
  transport.onData((b) => {
    transcript += Buffer.from(b).toString("latin1");
    if (transcript.length > 40000) transcript = transcript.slice(-20000);
  });

  try {
    await events.open();
    await session.open();
    // Config and device setup, then start — all after the console is
    // listening, so the boot the first step waits for is live output.
    if (opts.config) await qbone.applyConfig(opts.config);
    if (opts.setup?.length) {
      await qbone.control("halt");
      for (const s of opts.setup) {
        if (s.enabled !== undefined)
          await qbone.setParam(s.device, "enabled", s.enabled ? "true" : "false");
        else if (s.param !== undefined)
          await qbone.setParam(s.device, s.param, s.value ?? "");
      }
    }
    await qbone.control("restart");
    await runScript(session, validateScript(xxdpScript(opts)), {}, recorder);
    await session.close(0);
    return {
      passed: true,
      terminatedBy: "end of pass with no errors",
      transcript: transcript.slice(-4000),
      recording: opts.recordPath,
    };
  } catch (err) {
    await session.close(1).catch(() => {});
    const why =
      err instanceof ScriptFailure || err instanceof Error
        ? err.message
        : String(err);
    return {
      passed: false,
      terminatedBy: why,
      transcript: transcript.slice(-4000),
      recording: opts.recordPath,
    };
  }
}
