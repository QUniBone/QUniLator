/* waitfor.test.ts: the client-side wait-for helpers over the mock streams.
 * wait_for_halt resolves on a scripted state event and times out otherwise;
 * wait_for_console matches across the replayed-then-live boundary.
 */
import { test, before, after, beforeEach } from "node:test";
import assert from "node:assert/strict";
import { MockBoard } from "./mock-board.js";
import { QBoneClient } from "../src/qbone.js";
import type { BoardConfig } from "../src/config.js";

let board: MockBoard;
let host: string;

function client(): QBoneClient {
  const cfg: BoardConfig = {
    host,
    httpBase: `http://${host}`,
    wsBase: `ws://${host}`,
    authHeader: "",
  };
  return new QBoneClient(cfg);
}

before(async () => {
  board = new MockBoard();
  host = await board.start();
});
after(async () => {
  await board.stop();
});
beforeEach(() => {
  board.consoleReplay.clear();
  board.stateSnapshot = { t: "state", halt: false, powered: true };
});

test("wait_for_halt resolves on a scripted state event", async () => {
  const p = client().waitForHalt(2000);
  setTimeout(() => board.broadcastEvent({ t: "state", halt: true }), 80);
  const res = await p;
  assert.equal(res.halted, true);
});

test("wait_for_halt resolves at once when already halted", async () => {
  board.stateSnapshot = { t: "state", halt: true, powered: true };
  const res = await client().waitForHalt(2000);
  assert.equal(res.halted, true);
});

test("wait_for_halt times out when no halt arrives", async () => {
  const res = await client().waitForHalt(250);
  assert.equal(res.halted, false);
});

test("wait_for_console matches a pattern in the replayed ring", async () => {
  board.consoleReplay.set("0", Buffer.from("... 73 Boot from DL0\r\n$ "));
  const res = await client().waitForConsole("0", /\$ $/, 2000);
  assert.equal(res.matched, true);
  assert.match(res.output, /Boot from DL0/);
});

test("wait_for_console matches across the replay/live boundary", async () => {
  // Replay carries a partial line; the match only completes with live bytes.
  board.consoleReplay.set("ext", Buffer.from("login: "));
  const p = client().waitForConsole("ext", /Password:/, 2000);
  setTimeout(() => board.broadcastConsole("ext", Buffer.from("root\r\nPassword:")), 80);
  const res = await p;
  assert.equal(res.matched, true);
  assert.match(res.output, /login: root/);
});

test("wait_for_console times out without a match", async () => {
  board.consoleReplay.set("1", Buffer.from("nothing interesting"));
  const res = await client().waitForConsole("1", /NEVER/, 250);
  assert.equal(res.matched, false);
});
