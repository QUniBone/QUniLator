/* steps.ts: the declarative step file and its executor.
 *
 * A script is YAML: header defaults (console, timeout, deviations), then
 * steps. A step waits for a prompt and answers it; a step with several
 * expect cases branches on whichever the guest printed:
 *
 *   steps:
 *     - expect: "login: "
 *       send: root
 *     - expect: "Password:"
 *       send: ${ROOT_PW}
 *       mode: no-echo
 *     - name: shell
 *       expect:
 *         - match: "Load TCP/IP? "
 *           send: N
 *           goto: shell
 *         - match: "# "
 *           done: true
 *
 * ${NAME} interpolates from the vars map (CLI --var, environment), keeping
 * passwords and host-specific values out of checked-in scripts.
 */
import { readFileSync } from "node:fs";
import { parse as parseYaml } from "yaml";
import {
  Session,
  SessionError,
  type Awaited,
  type Deviation,
  type InputMode,
} from "./session.js";
import type { ScreenCondition } from "./screen.js";
import type { CastRecorder } from "./recording.js";

export interface CaseSpec {
  /** Text to wait for in the byte stream. Alternative to `screen`. */
  match?: string;
  /**
   * Screen state to wait for, for a guest that redraws in place instead of
   * appending lines: {row?, contains?, matches?, cursor?}.
   */
  screen?: ScreenCondition;
  send?: string;
  mode?: InputMode;
  break?: boolean;
  goto?: string;
  fail?: string;
  done?: boolean;
}

export interface StepSpec {
  name?: string;
  expect?: string | CaseSpec[];
  screen?: ScreenCondition;
  send?: string;
  mode?: InputMode;
  break?: boolean;
  goto?: string;
  done?: boolean;
  timeout?: string | number;
  deviations?: Deviation[];
  /** Pause before acting — for a guest that needs a moment after its banner. */
  wait?: string | number;
  /**
   * How long the console may sit quiet at an unmatched prompt before this
   * step is called stuck ("0" to disable). Defaults to the script's `stall`,
   * then to the session's.
   */
  stall?: string | number;
}

/**
 * What to do to the machine once the session is connected and anchored, so
 * the boot the script drives is produced with the console already listening.
 * Doing it the other way round — start the machine, then connect — races the
 * boot against the connect and leaves the script matching a replayed ring.
 */
export interface MachineSpec {
  /** Configuration to apply before starting. */
  config?: string;
  /**
   * How to bring the machine up: `restart` (release HALT and restart from
   * the power-up vector, keeping the applied configuration), `dc_on`
   * (logical power-on, which re-runs the DIP configuration selection), or
   * `none` to leave the machine alone.
   */
  start?: "restart" | "dc_on" | "none";
  /** Pause after starting before the first step. */
  settle?: string | number;
}

export interface ScriptSpec {
  console?: string;
  host?: string;
  title?: string;
  machine?: MachineSpec;
  timeout?: string | number;
  settle?: string | number;
  /** Default unmatched-prompt stall window for every step. */
  stall?: string | number;
  deviations?: Deviation[];
  steps: StepSpec[];
}

export class ScriptFailure extends Error {
  constructor(
    message: string,
    public stepIndex: number,
    public stepName?: string,
    public cause?: Error,
  ) {
    super(
      `step ${stepIndex + 1}${stepName ? ` (${stepName})` : ""}: ${message}` +
        (cause ? `\n${cause.message}` : ""),
    );
    this.name = "ScriptFailure";
  }
}

/** "500ms", "30s", "10m", "2h", or a bare number of seconds → ms. */
export function parseDuration(spec: string | number): number {
  if (typeof spec === "number") return spec * 1000;
  const m = /^(\d+(?:\.\d+)?)(ms|s|m|h)?$/.exec(spec.trim());
  if (!m) throw new Error(`invalid duration: ${JSON.stringify(spec)}`);
  const n = parseFloat(m[1]);
  switch (m[2] ?? "s") {
    case "ms":
      return n;
    case "s":
      return n * 1000;
    case "m":
      return n * 60 * 1000;
    case "h":
      return n * 60 * 60 * 1000;
    default:
      throw new Error(`invalid duration unit in ${JSON.stringify(spec)}`);
  }
}

/** ${NAME} substitution; an unknown name is an error, not empty text. */
export function interpolate(
  text: string,
  vars: Record<string, string>,
): string {
  return text.replace(/\$\{([A-Za-z_][A-Za-z0-9_]*)\}/g, (_, name: string) => {
    const v = vars[name];
    if (v === undefined) throw new Error(`undefined variable \${${name}}`);
    return v;
  });
}

function bad(where: string, what: string): never {
  throw new Error(`script ${where}: ${what}`);
}

/** Parse and structurally validate a script file. */
export function loadScript(path: string): ScriptSpec {
  const doc: unknown = parseYaml(readFileSync(path, "utf8"));
  return validateScript(doc);
}

export function validateScript(doc: unknown): ScriptSpec {
  if (typeof doc !== "object" || doc === null || Array.isArray(doc))
    bad("top level", "must be a mapping");
  const s = doc as Record<string, unknown>;
  if (!Array.isArray(s["steps"]) || s["steps"].length === 0)
    bad("top level", "needs a non-empty steps list");
  const script = s as unknown as ScriptSpec;
  if (script.machine !== undefined) {
    const m = script.machine;
    if (typeof m !== "object" || m === null) bad("machine", "must be a mapping");
    if (
      m.start !== undefined &&
      !["restart", "dc_on", "none"].includes(m.start)
    )
      bad("machine", `start must be restart, dc_on or none (got ${m.start})`);
  }
  const names = new Set<string>();
  script.steps.forEach((step, i) => {
    const where = `step ${i + 1}`;
    if (typeof step !== "object" || step === null) bad(where, "must be a mapping");
    if (step.name !== undefined) {
      if (names.has(step.name)) bad(where, `duplicate name ${step.name}`);
      names.add(step.name);
    }
    if (Array.isArray(step.expect)) {
      if (step.expect.length === 0) bad(where, "expect list is empty");
      for (const c of step.expect) {
        if (typeof c !== "object" || c === null)
          bad(where, "every expect case must be a mapping");
        const hasMatch = typeof c.match === "string";
        const hasScreen = typeof c.screen === "object" && c.screen !== null;
        if (!hasMatch && !hasScreen)
          bad(where, "every expect case needs a match or a screen condition");
        if (hasMatch && hasScreen)
          bad(where, "an expect case names either match or screen, not both");
      }
    } else if (step.expect !== undefined && typeof step.expect !== "string") {
      bad(where, "expect must be a string or a list of cases");
    }
    if (
      step.expect === undefined &&
      step.screen === undefined &&
      step.send === undefined &&
      !step.break &&
      !step.done &&
      step.wait === undefined
    )
      bad(where, "does nothing (no expect, screen, send, break, wait or done)");
  });
  // goto targets must exist
  const check = (g: string | undefined, where: string) => {
    if (g !== undefined && !names.has(g)) bad(where, `goto target ${g} not found`);
  };
  script.steps.forEach((step, i) => {
    check(step.goto, `step ${i + 1}`);
    if (Array.isArray(step.expect))
      for (const c of step.expect) check(c.goto, `step ${i + 1}`);
  });
  return script;
}

export interface RunResult {
  stepsRun: number;
}

const JUMP_LIMIT = 10000;

/**
 * Consecutive identical (step, case, matched text) iterations tolerated
 * before the run is called livelocked. A dialog loop that answers a prompt
 * and jumps back is the normal shape of a DRS-style question set, but the
 * same prompt matching the same case with the same text over and over means
 * the guest did not accept the answer — a prompt with no default re-asked
 * forever, say. Failing here reports the prompt that would not take the
 * answer, which a step deadline minutes later cannot.
 */
const LOOP_LIMIT = 5;

/**
 * Run the steps against an open session. Throws ScriptFailure on a failed
 * step (deviation, timeout, echo stall, or an explicit fail case), with the
 * session's diagnostics attached.
 */
export async function runScript(
  session: Session,
  script: ScriptSpec,
  vars: Record<string, string> = {},
  recorder?: CastRecorder,
): Promise<RunResult> {
  const defaultTimeout =
    script.timeout !== undefined ? parseDuration(script.timeout) : undefined;
  const defaultStall =
    script.stall !== undefined ? parseDuration(script.stall) : undefined;
  const nameToIndex = new Map<string, number>();
  script.steps.forEach((s, i) => {
    if (s.name !== undefined) nameToIndex.set(s.name, i);
  });

  let stepsRun = 0;
  let jumps = 0;
  let i = 0;
  let lastIteration = ""; // (step, case, matched text) of the previous pass
  let repeats = 0;
  while (i < script.steps.length) {
    const step = script.steps[i];
    const label = `step ${i + 1}${step.name ? `: ${step.name}` : ""}`;
    stepsRun++;
    recorder?.marker(label);
    const stepStartMs = recorder?.elapsedMs() ?? 0;

    let chosen: CaseSpec | undefined;
    try {
      if (step.wait !== undefined)
        await new Promise((r) => setTimeout(r, parseDuration(step.wait!)));
      if (step.expect !== undefined || step.screen !== undefined) {
        const cases: CaseSpec[] = Array.isArray(step.expect)
          ? step.expect
          : [
              {
                match: step.expect,
                screen: step.screen,
                send: step.send,
                mode: step.mode,
                break: step.break,
                goto: step.goto,
                done: step.done,
              },
            ];
        const awaited: Awaited[] = cases.map((c) =>
          c.screen !== undefined
            ? { kind: "screen", cond: c.screen }
            : { kind: "stream", spec: interpolate(c.match!, vars) },
        );
        const outcome = await session.expect(awaited, {
          timeoutMs:
            step.timeout !== undefined
              ? parseDuration(step.timeout)
              : defaultTimeout,
          stallMs:
            step.stall !== undefined
              ? parseDuration(step.stall)
              : defaultStall,
          deviations: step.deviations,
        });
        chosen = cases[outcome.index];
        const iteration = `${i} ${outcome.index} ${outcome.match}`;
        if (iteration === lastIteration) {
          if (++repeats >= LOOP_LIMIT)
            throw new ScriptFailure(
              `the guest re-asked ${JSON.stringify(outcome.match)} ` +
                `${repeats + 1} times running — it does not accept ` +
                `${chosen.send === "" ? "a bare CR (the prompt has no default?)" : JSON.stringify(chosen.send ?? "")}`,
              i,
              step.name,
            );
        } else {
          lastIteration = iteration;
          repeats = 0;
        }
        if (chosen.fail !== undefined)
          throw new ScriptFailure(chosen.fail, i, step.name);
        if (chosen.break) await session.sendBreak();
        if (chosen.send !== undefined)
          await session.sendLine(interpolate(chosen.send, vars), {
            mode: chosen.mode ?? step.mode,
          });
      } else {
        // send-only step
        if (step.break) await session.sendBreak();
        if (step.send !== undefined)
          await session.sendLine(interpolate(step.send, vars), {
            mode: step.mode,
          });
        chosen = step as CaseSpec;
      }
    } catch (err) {
      recorder?.step({
        index: i,
        name: step.name,
        outcome: "failed",
        detail: err instanceof Error ? err.message.split("\n")[0] : String(err),
        startMs: stepStartMs,
        endMs: recorder?.elapsedMs() ?? 0,
      });
      if (err instanceof ScriptFailure) throw err;
      if (err instanceof SessionError)
        throw new ScriptFailure(err.message, i, step.name, err);
      throw err;
    }
    recorder?.step({
      index: i,
      name: step.name,
      outcome: step.expect !== undefined ? "matched" : "sent",
      startMs: stepStartMs,
      endMs: recorder?.elapsedMs() ?? 0,
    });

    if (chosen.done) return { stepsRun };
    if (chosen.goto !== undefined) {
      if (++jumps > JUMP_LIMIT)
        throw new ScriptFailure(`jump limit exceeded (${JUMP_LIMIT})`, i, step.name);
      i = nameToIndex.get(chosen.goto)!;
      continue;
    }
    i++;
  }
  return { stepsRun };
}
