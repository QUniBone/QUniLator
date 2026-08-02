/* recording.test.ts: the cast a run leaves behind — valid asciicast v3,
 * direction-tagged events, step markers, redaction, echo spans. */
import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { Session } from "../src/session.js";
import { CastRecorder, readCast, readNotes } from "../src/recording.js";
import { validateScript, runScript } from "../src/steps.js";
import { MockGuest, FAST_INPUT } from "./mock-guest.js";

function castPath(name: string): string {
  return join(mkdtempSync(join(tmpdir(), "qcon-test-")), name);
}

test("a scripted run records a valid v3 cast with markers and redaction", async () => {
  const path = castPath("run.cast");
  const recorder = new CastRecorder(path, { title: "test run" });
  const guest = new MockGuest({ live: true });
  guest.onLine((line) => {
    if (line === "root") setTimeout(() => guest.output("\r\nPassword:"), 5);
    if (line === "secret") setTimeout(() => guest.output("\r\n# "), 5);
  });
  const session = new Session(guest, {
    input: FAST_INPUT,
    settleMs: 30,
    anchorTimeoutMs: 300,
    defaultTimeoutMs: 2000,
    recorder,
  });
  await session.open();
  const script = validateScript({
    steps: [
      { name: "login", expect: "login: ", send: "root" },
      { name: "password", expect: "Password:", send: "secret", mode: "no-echo" },
      { expect: "# ", done: true },
    ],
  });
  const run = runScript(session, script, {}, recorder);
  guest.output("login: ");
  await run;
  await session.close(0);

  const cast = readCast(path);
  assert.equal(cast.header["version"], 3);
  const term = cast.header["term"] as { cols: number; rows: number };
  assert.equal(term.cols, 80);
  assert.equal(cast.header["title"], "test run");

  const codes = new Set(cast.events.map((e) => e.code));
  assert.ok(codes.has("o"), "output events");
  assert.ok(codes.has("i"), "input events");
  assert.ok(codes.has("m"), "markers");
  assert.equal(cast.events[cast.events.length - 1].code, "x");
  for (const ev of cast.events)
    assert.ok(ev.interval >= 0, "intervals are non-negative");

  // step markers name the steps
  const markers = cast.events.filter((e) => e.code === "m").map((e) => e.data);
  assert.ok(markers.some((m) => m.includes("login")));
  assert.ok(markers.some((m) => m.includes("password")));

  // the password went over the wire but not into the file
  const inputs = cast.events.filter((e) => e.code === "i").map((e) => e.data);
  const typed = inputs.join("");
  assert.ok(typed.includes("r"), "normal input recorded");
  assert.ok(!typed.includes("secret".slice(0, 3)), "password bytes absent");
  assert.ok(typed.includes("•"), "redacted input shows its rhythm");
  assert.ok(guest.received.includes("secret\r"), "guest still got the password");

  const notes = readNotes(path);
  assert.ok(notes.redactedInputs.length >= "secret".length);
  assert.equal(notes.steps.length, 3);
  assert.equal(notes.steps[0].outcome, "matched");
  assert.ok(notes.echoSpans.length >= 2, "echo spans for the typed lines");
});

test("recordSecrets keeps no-echo input verbatim", async () => {
  const path = castPath("secrets.cast");
  const recorder = new CastRecorder(path);
  const guest = new MockGuest({ echo: false, live: true });
  const session = new Session(guest, {
    input: FAST_INPUT,
    settleMs: 30,
    anchorTimeoutMs: 300,
    recorder,
    recordSecrets: true,
  });
  await session.open();
  await session.sendLine("hunter2", { mode: "no-echo" });
  await session.close(0);
  const cast = readCast(path);
  const typed = cast.events
    .filter((e) => e.code === "i")
    .map((e) => e.data)
    .join("");
  assert.ok(typed.includes("hunter2"));
});

test("output events reproduce the byte stream in order", async () => {
  const path = castPath("bytes.cast");
  const recorder = new CastRecorder(path);
  const guest = new MockGuest({ live: true });
  const session = new Session(guest, {
    input: FAST_INPUT,
    settleMs: 30,
    anchorTimeoutMs: 300,
    recorder,
  });
  await session.open();
  guest.output("one ");
  guest.output("two \x1b[7mthree\x1b[0m");
  await new Promise((r) => setTimeout(r, 20));
  await session.close(0);
  const cast = readCast(path);
  const out = cast.events
    .filter((e) => e.code === "o")
    .map((e) => e.data)
    .join("");
  assert.equal(out, "one two \x1b[7mthree\x1b[0m");
});
