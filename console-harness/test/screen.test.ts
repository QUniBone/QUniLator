/* screen.test.ts: matching the screen a full-screen guest draws, rather than
 * the bytes it sent to draw it. */
import { test } from "node:test";
import assert from "node:assert/strict";
import { XtermScreen, screenMatches } from "../src/screen.js";
import { Session } from "../src/session.js";
import { validateScript, runScript } from "../src/steps.js";
import { MockGuest, FAST_INPUT } from "./mock-guest.js";

const esc = (s: string) => s.replace(/\\e/g, "\x1b");

/** Clear the screen, place the cursor, write — what a full-screen editor does. */
function drawFullScreen(): string {
  return esc(
    "\\e[2J\\e[H" + // clear, home
      "EDT V3.2\\e[3;20HBuffer MAIN" +
      "\\e[23;1H*Command: " +
      "\\e[10;5Hthe quick brown fox",
  );
}

test("a redrawn screen matches on content the stream does not carry in order", async () => {
  const screen = new XtermScreen({ cols: 80, rows: 24 });
  screen.write(new Uint8Array(Buffer.from(drawFullScreen(), "latin1")));
  await screen.flush();

  // The bytes carry cursor motion, so the stream order is not the screen
  // order: "Buffer MAIN" is written before the line above it in the file.
  assert.match(screen.row(0), /EDT V3\.2/);
  assert.match(screen.row(2), /Buffer MAIN/);
  assert.match(screen.row(9), /the quick brown fox/);
  assert.match(screen.row(22), /\*Command: /);

  assert.ok(screenMatches(screen, { contains: "the quick brown fox" }));
  assert.ok(screenMatches(screen, { row: 22, contains: "*Command: " }));
  assert.ok(screenMatches(screen, { row: 0, matches: "/^EDT/" }));
  assert.ok(!screenMatches(screen, { row: 0, contains: "Buffer MAIN" }));
  screen.dispose();
});

test("erasure removes text from the screen that is still in the stream", async () => {
  // The reason stream matching is not enough: the guest printed the word and
  // then erased it, so a stream match would still fire on text nobody can see.
  const screen = new XtermScreen({ cols: 40, rows: 5 });
  screen.write(
    new Uint8Array(Buffer.from(esc("PASSWORD WRONG\\e[2K\\rREADY"), "latin1")),
  );
  await screen.flush();
  assert.ok(!screenMatches(screen, { contains: "PASSWORD WRONG" }));
  assert.ok(screenMatches(screen, { contains: "READY" }));
  screen.dispose();
});

test("the cursor position is matchable", async () => {
  const screen = new XtermScreen({ cols: 80, rows: 24 });
  screen.write(new Uint8Array(Buffer.from(esc("\\e[12;41H"), "latin1")));
  await screen.flush();
  assert.deepEqual(screen.cursor(), { x: 40, y: 11 });
  assert.ok(screenMatches(screen, { cursor: { y: 11 } }));
  assert.ok(!screenMatches(screen, { cursor: { y: 3 } }));
  screen.dispose();
});

test("a step waits for screen state and answers it", async () => {
  const guest = new MockGuest({ live: true });
  guest.onLine((line) => {
    if (line === "EXIT") setTimeout(() => guest.output("\r\n$ "), 5);
  });
  const session = new Session(guest, {
    input: FAST_INPUT,
    settleMs: 30,
    anchorTimeoutMs: 300,
    defaultTimeoutMs: 2000,
    screen: { cols: 80, rows: 24 },
  });
  await session.open();
  const script = validateScript({
    steps: [
      {
        name: "editor",
        expect: [{ screen: { row: 22, contains: "*Command:" }, send: "EXIT" }],
      },
      { expect: "$ ", done: true },
    ],
  });
  const run = runScript(session, script);
  guest.output(drawFullScreen());
  const result = await run;
  assert.equal(result.stepsRun, 2);
  assert.ok(guest.received.includes("EXIT\r"));
  await session.close();
});

test("stream and screen cases coexist, whichever the guest reaches first", async () => {
  const guest = new MockGuest({ live: true });
  const session = new Session(guest, {
    input: FAST_INPUT,
    settleMs: 30,
    anchorTimeoutMs: 300,
    defaultTimeoutMs: 2000,
    screen: { cols: 80, rows: 24 },
  });
  await session.open();
  const cases = [
    { kind: "stream" as const, spec: "READY>" },
    { kind: "screen" as const, cond: { row: 0, contains: "PANIC" } },
  ];
  const p = session.expect(cases, { timeoutMs: 2000 });
  guest.output(esc("\\e[2J\\e[HPANIC: halt"));
  const outcome = await p;
  assert.equal(outcome.index, 1, "the screen case matched");
  await session.close();
});

test("a screen condition matches nothing before the guest draws it", async () => {
  const guest = new MockGuest({ live: true });
  const session = new Session(guest, {
    input: FAST_INPUT,
    settleMs: 30,
    anchorTimeoutMs: 300,
    screen: { cols: 80, rows: 24 },
    stallMs: 0,
  });
  await session.open();
  guest.output("nothing interesting\r\n");
  await assert.rejects(
    session.expect([{ kind: "screen", cond: { contains: "MENU" } }], {
      timeoutMs: 150,
    }),
    /timeout/,
  );
  await session.close();
});
