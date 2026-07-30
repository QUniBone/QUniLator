#!/usr/bin/env node
// Drive the emulated VAX's console: wait for a prompt, type a line, repeat.
//
// The console is a terminal, not a pipe. It echoes what is typed and it drops
// what arrives while the guest is not reading, so a line has to be typed a
// character at a time and each character seen back before the next is sent.
// That is what this does, and it is why pasting a line at the socket does not
// work.
//
//   node tools/vax-console.mjs \
//        --expect 'Username:' --send SYSTEM \
//        --expect 'Password:' --send-hidden MANAGER \
//        --expect '\$ ' --send 'SHOW DEVICE DU'
//
// --expect takes a regular expression matched against everything received
// since the last step. --send types a line and waits for its echo;
// --send-hidden types one that will not be echoed, for a password. --wait
// pauses. Everything received is written to stdout as it arrives.
// ws comes from the MCP server's dependencies, which is the only place in the
// tree that has it.
import { createRequire } from "node:module";
const require = createRequire(new URL("../mcp-server/package.json", import.meta.url));
const WebSocket = require("ws");
import { readFileSync } from "node:fs";
import { homedir } from "node:os";
import { join } from "node:path";

const args = process.argv.slice(2);
const host =
  (args.includes("--host") && args[args.indexOf("--host") + 1]) ||
  process.env.QBONE_HOST ||
  "unibone.huebner.org";
const channel =
  (args.includes("--channel") && args[args.indexOf("--channel") + 1]) || "vax";
const quiet = args.includes("--quiet");

let password = "";
try {
  password = readFileSync(join(homedir(), ".qbone-pw"), "utf8").trim();
} catch {
  /* a board with no password answers everything */
}
const auth = password
  ? { Authorization: "Basic " + Buffer.from(":" + password).toString("base64") }
  : {};

// The steps, in the order they were given.
const steps = [];
for (let i = 0; i < args.length; i++) {
  const take = () => args[++i];
  if (args[i] === "--expect") steps.push({ kind: "expect", value: take() });
  else if (args[i] === "--send") steps.push({ kind: "send", value: take() });
  else if (args[i] === "--send-hidden")
    steps.push({ kind: "send", value: take(), hidden: true });
  else if (args[i] === "--wait") steps.push({ kind: "wait", value: take() });
  else if (args[i] === "--timeout") steps.push({ kind: "timeout", value: take() });
  else if (args[i] === "--restart") steps.push({ kind: "restart" });
}

// Restarting from here, with the socket already open and the replayed history
// already behind us, is what makes a run readable: everything that arrives
// afterwards belongs to this boot and to no earlier one.
async function restart() {
  const res = await fetch(
    `http://${host}/api/devices/cpuvax/params/start_switch`,
    {
      method: "PUT",
      headers: { "Content-Type": "application/json", ...auth },
      body: JSON.stringify({ value: "1" }),
    },
  );
  if (!res.ok) throw new Error(`restart failed: HTTP ${res.status}`);
}

const ws = new WebSocket(`ws://${host}/ws/console/${channel}`, { headers: auth });
let buf = "";
ws.on("message", (d) => {
  const text = Buffer.isBuffer(d) ? d.toString("latin1") : String(d);
  buf += text;
  if (!quiet) process.stdout.write(text);
});
ws.on("error", (e) => {
  console.error(`console: ${e.message}`);
  process.exit(1);
});

const delay = (ms) => new Promise((r) => setTimeout(r, ms));

// Wait until what has arrived since `from` matches, and answer where it ended.
async function expect(re, timeoutMs, from) {
  const end = Date.now() + timeoutMs;
  const rx = new RegExp(re);
  while (Date.now() < end) {
    const m = rx.exec(buf.slice(from));
    if (m) return from + m.index + m[0].length;
    await delay(120);
  }
  throw new Error(`timed out waiting for /${re}/`);
}

// Type a line. Each character is confirmed by its echo before the next goes,
// which is what keeps a line intact on a terminal with no receive buffer. A
// password is not echoed, so those characters are merely paced.
async function sendLine(text, hidden) {
  for (const ch of text) {
    const mark = buf.length;
    for (let attempt = 0; attempt < 5; attempt++) {
      ws.send(Buffer.from(ch, "latin1"));
      if (hidden) {
        await delay(60);
        break;
      }
      const end = Date.now() + 800;
      while (Date.now() < end && buf.length === mark) await delay(15);
      if (buf.length > mark) break;
    }
  }
  ws.send(Buffer.from("\r"));
}

await new Promise((resolve, reject) => {
  ws.once("open", resolve);
  ws.once("error", reject);
});
// The board replays the channel's history on connect; only what arrives after
// that is this session's.
await delay(1500);
let from = buf.length;
let timeoutMs = 30000;

try {
  for (const step of steps) {
    if (step.kind === "timeout") timeoutMs = Number(step.value);
    else if (step.kind === "wait") await delay(Number(step.value));
    else if (step.kind === "restart") {
      await restart();
      from = buf.length;
    }
    else if (step.kind === "expect") from = await expect(step.value, timeoutMs, from);
    else if (step.kind === "send") {
      await sendLine(step.value, step.hidden);
      from = buf.length;
    }
  }
  await delay(1500);
  ws.close();
} catch (err) {
  console.error(`\n--- ${err.message}`);
  ws.close();
  process.exit(2);
}
