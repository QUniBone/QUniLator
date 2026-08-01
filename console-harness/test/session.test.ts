/* session.test.ts: the session core against the mock guest — echo pacing,
 * drop recovery, no-echo mode, anchoring, deadlines and deviations. */
import { test } from "node:test";
import assert from "node:assert/strict";
import {
  Session,
  DeviationError,
  EchoStallError,
  ExpectTimeoutError,
} from "../src/session.js";
import type { MachineEventName } from "../src/events.js";
import { MockGuest, FAST_INPUT } from "./mock-guest.js";

function makeSession(guest: MockGuest, extra = {}) {
  return new Session(guest, {
    input: FAST_INPUT,
    settleMs: 30,
    anchorTimeoutMs: 300,
    defaultTimeoutMs: 2000,
    ...extra,
  });
}

test("echo-paced line arrives intact", async () => {
  const guest = new MockGuest({ echoDelayMs: 10, live: true });
  const session = makeSession(guest);
  await session.open();
  await session.sendLine("R ZTKAE0");
  assert.equal(guest.received, "R ZTKAE0\r");
  await session.close();
});

test("a dropped character is resent, not lost", async () => {
  const guest = new MockGuest({ dropNext: 1, live: true });
  const session = makeSession(guest);
  await session.open();
  await session.sendLine("HELLO");
  assert.equal(guest.received, "HELLO\r");
  await session.close();
});

test("a delayed echo waits and never duplicates", async () => {
  // echo arrives at 80 ms, inside the 120 ms window: the sender must wait,
  // not resend — a resend would type the character twice
  const guest = new MockGuest({ echoDelayMs: 80, live: true });
  const session = makeSession(guest);
  await session.open();
  await session.sendLine("ABC");
  assert.equal(guest.received, "ABC\r");
  await session.close();
});

test("no-echo mode sends each character once without confirmation", async () => {
  const guest = new MockGuest({ echo: false, live: true });
  const session = makeSession(guest);
  await session.open();
  await session.sendLine("SECRET", { mode: "no-echo" });
  assert.equal(guest.received, "SECRET\r");
  await session.close();
});

test("echo mode against a non-echoing guest fails the step", async () => {
  const guest = new MockGuest({ echo: false, live: true });
  const session = makeSession(guest);
  await session.open();
  await assert.rejects(session.sendLine("X"), EchoStallError);
  // every transmission of the character reached the guest: maxSend attempts
  assert.equal(guest.received, "X".repeat(FAST_INPUT.maxSend));
  await session.close();
});

test("expect matches live output, not the replayed history", async () => {
  const guest = new MockGuest({ replay: "login: stale\r\n", live: true });
  const session = makeSession(guest);
  await session.open();
  await assert.rejects(
    session.expect("login: ", { timeoutMs: 100 }),
    ExpectTimeoutError,
  );
  guest.output("login: ");
  const m = await session.expect("login: ", { timeoutMs: 500 });
  assert.equal(m.match, "login: ");
  await session.close();
});

test("settle fallback anchors past the replay without a live frame", async () => {
  const guest = new MockGuest({ replay: "old prompt> " });
  const session = makeSession(guest);
  await session.open(); // anchors after the 30 ms settle
  await assert.rejects(
    session.expect("prompt> ", { timeoutMs: 100 }),
    ExpectTimeoutError,
  );
  guest.output("fresh prompt> ");
  const m = await session.expect("prompt> ", { timeoutMs: 500 });
  assert.equal(m.before, "fresh ");
  await session.close();
});

test("a deviation pattern fails the step before its deadline", async () => {
  const guest = new MockGuest({ live: true });
  const session = makeSession(guest, {
    deviations: [{ match: "\r\n@", fail: "CPU dropped to ODT" }],
  });
  await session.open();
  const started = Date.now();
  const p = assert.rejects(
    session.expect("login: ", { timeoutMs: 5000 }),
    (err: Error) => {
      assert.ok(err instanceof DeviationError);
      assert.match(err.message, /CPU dropped to ODT/);
      return true;
    },
  );
  guest.output("halting\r\n@");
  await p;
  assert.ok(Date.now() - started < 2000, "deviation must preempt the deadline");
  await session.close();
});

test("the expected pattern wins when a deviation also matches", async () => {
  const guest = new MockGuest({ live: true });
  const session = makeSession(guest, {
    deviations: [{ match: "@", fail: "unexpected ODT" }],
  });
  await session.open();
  guest.output("@");
  const m = await session.expect("@", { timeoutMs: 500 });
  assert.equal(m.match, "@");
  await session.close();
});

test("a machine event fails the step immediately", async () => {
  const guest = new MockGuest({ live: true });
  let fire: ((ev: MachineEventName) => void) | undefined;
  const events = {
    onEvent(cb: (ev: MachineEventName) => void) {
      fire = cb;
    },
    close() {},
  };
  const session = makeSession(guest, {
    events,
    deviations: [{ event: "halt" as const, fail: "CPU halted mid-step" }],
  });
  await session.open();
  const p = assert.rejects(
    session.expect("never", { timeoutMs: 5000 }),
    (err: Error) => {
      assert.match(err.message, /CPU halted mid-step/);
      return true;
    },
  );
  fire!("halt");
  await p;
  await session.close();
});

test("timeout diagnostics carry the output since the step began", async () => {
  const guest = new MockGuest({ live: true });
  const session = makeSession(guest);
  await session.open();
  guest.output("boot rom v2.1\r\ntesting memory...\r\n");
  await new Promise((r) => setTimeout(r, 20));
  try {
    await session.expect("login: ", { timeoutMs: 100 });
    assert.fail("must time out");
  } catch (err) {
    assert.ok(err instanceof ExpectTimeoutError);
    assert.ok(err.diagnostics);
    assert.match(err.diagnostics.output, /testing memory/);
    assert.deepEqual(err.diagnostics.patterns, ["login: "]);
  }
  await session.close();
});

test("consecutive expects consume the buffer in order", async () => {
  const guest = new MockGuest({ live: true });
  const session = makeSession(guest);
  await session.open();
  guest.output("first> second> ");
  const a = await session.expect("> ", { timeoutMs: 500 });
  assert.equal(a.before, "first");
  const b = await session.expect("> ", { timeoutMs: 500 });
  assert.equal(b.before, "second");
  await session.close();
});

test("regex patterns and control characters match the stream", async () => {
  const guest = new MockGuest({ live: true });
  const session = makeSession(guest);
  await session.open();
  guest.output("\r\nSWR = 000000 NEW = ");
  const m = await session.expect(["/SWR\\s*=.*NEW\\s*=/"], { timeoutMs: 500 });
  assert.equal(m.index, 0);
  await session.close();
});

test("sendBreak reaches the transport and is recorded as an action", async () => {
  const guest = new MockGuest({ live: true });
  const session = makeSession(guest);
  await session.open();
  await session.sendBreak();
  assert.equal(guest.breaks, 1);
  await session.close();
});
