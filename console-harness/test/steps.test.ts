/* steps.test.ts: the declarative step engine — validation, interpolation,
 * durations, branching, and failure reporting. */
import { test } from "node:test";
import assert from "node:assert/strict";
import { Session } from "../src/session.js";
import {
  validateScript,
  runScript,
  parseDuration,
  interpolate,
  ScriptFailure,
  type ScriptSpec,
} from "../src/steps.js";
import { MockGuest, FAST_INPUT } from "./mock-guest.js";

function makeSession(guest: MockGuest) {
  return new Session(guest, {
    input: FAST_INPUT,
    settleMs: 30,
    anchorTimeoutMs: 300,
    defaultTimeoutMs: 2000,
  });
}

test("parseDuration accepts ms/s/m/h and bare seconds", () => {
  assert.equal(parseDuration("500ms"), 500);
  assert.equal(parseDuration("30s"), 30000);
  assert.equal(parseDuration("10m"), 600000);
  assert.equal(parseDuration("2h"), 7200000);
  assert.equal(parseDuration("45"), 45000);
  assert.equal(parseDuration(3), 3000);
  assert.throws(() => parseDuration("soon"));
});

test("interpolation substitutes vars and rejects unknown names", () => {
  assert.equal(interpolate("boot ${DEV}", { DEV: "du0" }), "boot du0");
  assert.throws(() => interpolate("${MISSING}", {}), /undefined variable/);
});

test("validation rejects a bad script shape", () => {
  assert.throws(() => validateScript({}), /steps/);
  assert.throws(
    () => validateScript({ steps: [{ expect: "x", goto: "nowhere" }] }),
    /goto target/,
  );
  assert.throws(
    () => validateScript({ steps: [{}] }),
    /does nothing/,
  );
  assert.throws(
    () => validateScript({ steps: [{ expect: [] }] }),
    /expect list is empty/,
  );
});

test("a linear dialog runs to completion", async () => {
  const guest = new MockGuest({ live: true });
  guest.onLine((line) => {
    if (line === "root") setTimeout(() => guest.output("\r\nPassword:"), 5);
    if (line === "hunter2") setTimeout(() => guest.output("\r\n# "), 5);
  });
  const session = makeSession(guest);
  await session.open();
  const script: ScriptSpec = {
    steps: [
      { expect: "login: ", send: "root" },
      { expect: "Password:", send: "${PW}", mode: "no-echo" },
      { expect: "# ", done: true },
    ],
  };
  const run = runScript(session, validateScript(script), { PW: "hunter2" });
  guest.output("login: ");
  const result = await run;
  assert.equal(result.stepsRun, 3);
  assert.ok(guest.received.includes("root\r"));
  assert.ok(guest.received.includes("hunter2\r"));
  await session.close();
});

test("branching answers a side prompt and returns", async () => {
  const guest = new MockGuest({ live: true });
  guest.onLine((line) => {
    if (line === "GO") setTimeout(() => guest.output("\r\ncontinue? "), 5);
    if (line === "y") setTimeout(() => guest.output("\r\nDONE\r\n"), 5);
  });
  const session = makeSession(guest);
  await session.open();
  const script = validateScript({
    steps: [
      { expect: "> ", send: "GO" },
      {
        name: "wait",
        expect: [
          { match: "continue? ", send: "y", goto: "wait" },
          { match: "DONE", done: true },
        ],
      },
    ],
  });
  const run = runScript(session, script);
  guest.output("> ");
  const result = await run;
  assert.equal(result.stepsRun, 3); // step 2 ran twice via goto
  assert.ok(guest.received.includes("y\r"));
  await session.close();
});

test("an explicit fail case reports the step", async () => {
  const guest = new MockGuest({ live: true });
  const session = makeSession(guest);
  await session.open();
  const script = validateScript({
    steps: [
      {
        name: "boot",
        expect: [
          { match: "Ready", done: true },
          { match: "?BOOT ERROR", fail: "the boot ROM rejected the device" },
        ],
      },
    ],
  });
  const run = assert.rejects(runScript(session, script), (err: Error) => {
    assert.ok(err instanceof ScriptFailure);
    assert.equal(err.stepIndex, 0);
    assert.equal(err.stepName, "boot");
    assert.match(err.message, /boot ROM rejected/);
    return true;
  });
  guest.output("?BOOT ERROR\r\n");
  await run;
  await session.close();
});

test("a prompt the guest keeps re-asking fails as a livelock", async () => {
  // A logical prompt with no default: the guest re-asks it after every bare
  // CR. Without loop detection the script spins until the step deadline.
  const guest = new MockGuest({ live: true });
  guest.onLine((line) => {
    if (line === "") setTimeout(() => guest.output("\r\nNO DEFAULT\r\nCHANGE SW (L)  ? "), 2);
  });
  const session = makeSession(guest);
  await session.open();
  const script = validateScript({
    steps: [
      {
        name: "dialog",
        expect: [{ match: "(L)  ? ", send: "", goto: "dialog" }],
        timeout: "5s",
      },
    ],
  });
  const run = assert.rejects(runScript(session, script), (err: Error) => {
    assert.ok(err instanceof ScriptFailure);
    assert.match(err.message, /re-asked/);
    assert.match(err.message, /no default/);
    return true;
  });
  guest.output("CHANGE SW (L)  ? ");
  await run;
  await session.close();
});

test("a step timeout becomes a ScriptFailure with diagnostics", async () => {
  const guest = new MockGuest({ live: true });
  const session = makeSession(guest);
  await session.open();
  const script = validateScript({
    steps: [{ name: "never", expect: "login: ", timeout: "100ms" }],
  });
  guest.output("something else entirely");
  await assert.rejects(runScript(session, script), (err: Error) => {
    assert.ok(err instanceof ScriptFailure);
    assert.match(err.message, /never/);
    assert.match(err.message, /something else entirely/);
    return true;
  });
  await session.close();
});

test("a send beside a list of expect cases is refused, not ignored", () => {
  // The case that matched carries the answer, so a send here would never be
  // sent; a script that does this waits out its deadline for no reason.
  assert.throws(
    () =>
      validateScript({
        steps: [{ expect: [{ match: "x" }], send: "hello" }],
      }),
    /belongs on the expect case/,
  );
});
