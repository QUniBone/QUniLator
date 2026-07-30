/* tools.test.ts: each MCP tool, called end-to-end through an in-memory MCP
 * client against the mock board, issues the expected REST request or WS
 * exchange. Auth is read from a stubbed ~/.qbone-pw.
 */
import { test, before, after, beforeEach } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { InMemoryTransport } from "@modelcontextprotocol/sdk/inMemory.js";
import { MockBoard } from "./mock-board.js";
import { QBoneClient, pendingPrompt, matchAnswer } from "../src/qbone.js";
import { registerTools } from "../src/tools.js";
import { loadConfig } from "../src/config.js";
import type { BoardConfig } from "../src/config.js";

let board: MockBoard;
let host: string;

function cfgFor(h: string, authHeader = ""): BoardConfig {
  return {
    host: h,
    httpBase: `http://${h}`,
    wsBase: `ws://${h}`,
    authHeader,
  };
}

async function connectClient(cfg: BoardConfig): Promise<Client> {
  const server = new McpServer({ name: "test", version: "0" });
  registerTools(server, new QBoneClient(cfg));
  const [clientT, serverT] = InMemoryTransport.createLinkedPair();
  await server.connect(serverT);
  const client = new Client({ name: "test-client", version: "0" });
  await client.connect(clientT);
  return client;
}

function textOf(res: unknown): string {
  const content = (res as { content: { type: string; text: string }[] })
    .content;
  return content.map((c) => c.text).join("");
}

before(async () => {
  board = new MockBoard();
  host = await board.start();
});
after(async () => {
  await board.stop();
});
beforeEach(() => {
  board.requests = [];
  board.responses.clear();
  board.consoleReplay.clear();
  board.consoleReceived.clear();
});

test("tools are all registered", async () => {
  const client = await connectClient(cfgFor(host));
  const { tools } = await client.listTools();
  const names = tools.map((t) => t.name).sort();
  assert.deepEqual(names, [
    "configs",
    "console_read",
    "console_send",
    "continue",
    "control",
    "get_devices",
    "get_log",
    "get_logging",
    "get_machine_state",
    "halt",
    "images",
    "list_xxdp_diagnostics",
    "run_xxdp_diagnostic",
    "set_device_enabled",
    "set_log_level",
    "set_param",
    "start_machine",
    "wait_for_console",
    "wait_for_halt",
    "wait_for_running",
  ]);
});

test("pendingPrompt detects a '?' prompt through the trailing non-printable", () => {
  // real console: "Change HW (L)  ? " followed by \x04
  assert.equal(
    pendingPrompt("DR>\r\nChange HW (L)  ? \x04"),
    "Change HW (L)  ?",
  );
  assert.equal(
    pendingPrompt("unit 0\r\nTKIP ADDRESS (O)  174500 ? \x04"),
    "TKIP ADDRESS (O)  174500 ?",
  );
  // not at a prompt
  assert.equal(pendingPrompt("TESTING UNIT 0\r\n"), null);
  assert.equal(pendingPrompt(".\r\n"), null);
});

test("matchAnswer picks the answer whose match substring is in the prompt", () => {
  const answers = [
    { match: "CHANGE HW", value: "Y" },
    { match: "UNITS", value: "1" },
    { match: "VECTOR", value: "" },
  ];
  assert.equal(matchAnswer("Change HW (L)  ?", answers), "Y");
  assert.equal(matchAnswer("# UNITS (D)  ?", answers), "1");
  assert.equal(matchAnswer("TK VECTOR (O)  260 ?", answers), ""); // accept default
  assert.equal(matchAnswer("SOMETHING ELSE ?", answers), undefined);
});

test("get_devices returns the backend body incl. label and status", async () => {
  board.setResponse("GET /api/devices", [
    { name: "rl0", type: "RL02", label: "RL02 cartridge disk 0 (RL02)", status: "ready" },
  ]);
  const client = await connectClient(cfgFor(host));
  const res = await client.callTool({ name: "get_devices", arguments: {} });
  const req = board.requests.at(-1)!;
  assert.equal(req.method, "GET");
  assert.equal(req.path, "/api/devices");
  const body = JSON.parse(textOf(res));
  assert.equal(body[0].label, "RL02 cartridge disk 0 (RL02)");
  assert.equal(body[0].status, "ready");
});

test("set_param writes the value to the param endpoint", async () => {
  const client = await connectClient(cfgFor(host));
  await client.callTool({
    name: "set_param",
    arguments: { device: "uda0", param: "address", value: "174400" },
  });
  const req = board.requests.at(-1)!;
  assert.equal(req.method, "PUT");
  assert.equal(req.path, "/api/devices/uda0/params/address");
  assert.deepEqual(req.body, { value: "174400" });
});

test("set_log_level raises a source, get_logging lists sources", async () => {
  board.setResponse("GET /api/logging", {
    default: "warning",
    sources: [{ label: "tqk50", level: "warning" }],
  });
  const client = await connectClient(cfgFor(host));
  await client.callTool({
    name: "set_log_level",
    arguments: { source: "tqk50", level: "debug" },
  });
  const req = board.requests.at(-1)!;
  assert.equal(req.method, "PUT");
  assert.equal(req.path, "/api/logging/sources/tqk50");
  assert.deepEqual(req.body, { level: "debug" });

  const res = await client.callTool({ name: "get_logging", arguments: {} });
  assert.equal(JSON.parse(textOf(res)).default, "warning");
});

test("set_device_enabled toggles the enabled param", async () => {
  const client = await connectClient(cfgFor(host));
  await client.callTool({
    name: "set_device_enabled",
    arguments: { device: "rl0", enabled: false },
  });
  const req = board.requests.at(-1)!;
  assert.equal(req.path, "/api/devices/rl0/params/enabled");
  assert.deepEqual(req.body, { value: "false" });
});

test("control / halt / continue post the action", async () => {
  const client = await connectClient(cfgFor(host));
  await client.callTool({ name: "control", arguments: { action: "init" } });
  assert.deepEqual(board.requests.at(-1)!.body, { action: "init" });
  assert.equal(board.requests.at(-1)!.path, "/api/control");
  await client.callTool({ name: "halt", arguments: {} });
  assert.deepEqual(board.requests.at(-1)!.body, { action: "halt" });
  await client.callTool({ name: "continue", arguments: {} });
  assert.deepEqual(board.requests.at(-1)!.body, { action: "continue" });
});

test("configs: list / apply / set_default / save", async () => {
  board.setResponse("GET /api/configs?current=1", {
    devices: [{ name: "RL11", enabled: true, params: { address: "174400" } }],
  });
  const client = await connectClient(cfgFor(host));

  await client.callTool({ name: "configs", arguments: { action: "list" } });
  assert.equal(board.requests.at(-1)!.path, "/api/configs");

  await client.callTool({
    name: "configs",
    arguments: { action: "apply", name: "rt11" },
  });
  assert.equal(board.requests.at(-1)!.method, "POST");
  assert.equal(board.requests.at(-1)!.path, "/api/configs/rt11/apply");

  await client.callTool({
    name: "configs",
    arguments: { action: "set_default", name: "rt11" },
  });
  assert.equal(board.requests.at(-1)!.method, "PUT");
  assert.equal(board.requests.at(-1)!.path, "/api/configs/rt11/default");

  await client.callTool({
    name: "configs",
    arguments: { action: "save", name: "mine" },
  });
  // save reads the live setup then PUTs it under the name with from=live
  const getLive = board.requests.at(-2)!;
  const put = board.requests.at(-1)!;
  assert.equal(getLive.path, "/api/configs?current=1");
  assert.equal(put.method, "PUT");
  assert.equal(put.path, "/api/configs/mine?from=live");
  assert.deepEqual(
    (put.body as { devices: unknown[] }).devices[0],
    { name: "RL11", enabled: true, params: { address: "174400" } },
  );
});

test("images: list / attach / upload", async () => {
  const client = await connectClient(cfgFor(host));

  await client.callTool({ name: "images", arguments: { action: "list" } });
  assert.equal(board.requests.at(-1)!.path, "/api/images");

  await client.callTool({
    name: "images",
    arguments: { action: "attach", device: "rl0", image: "rt11.rl02" },
  });
  assert.equal(board.requests.at(-1)!.path, "/api/devices/rl0/params/image");
  assert.deepEqual(board.requests.at(-1)!.body, { value: "rt11.rl02" });

  const dir = mkdtempSync(join(tmpdir(), "qbone-img-"));
  const file = join(dir, "disk.rl02");
  writeFileSync(file, Buffer.from("HELLO-IMAGE"));
  await client.callTool({
    name: "images",
    arguments: { action: "upload", path: file },
  });
  const up = board.requests.at(-1)!;
  assert.equal(up.method, "POST");
  assert.equal(up.path, "/api/images");
  assert.ok(up.contentType?.startsWith("multipart/form-data"));
});

test("console_send delivers bytes to the channel", async () => {
  const client = await connectClient(cfgFor(host));
  await client.callTool({
    name: "console_send",
    arguments: { channel: "ext", text: "boot", append_cr: true },
  });
  await new Promise((r) => setTimeout(r, 100));
  const got = Buffer.concat(board.consoleReceived.get("ext") ?? []).toString(
    "latin1",
  );
  assert.equal(got, "boot\r");
});

test("console_read returns the replayed ring", async () => {
  board.consoleReplay.set("ext", Buffer.from("@@@ console banner\r\n"));
  const client = await connectClient(cfgFor(host));
  const res = await client.callTool({
    name: "console_read",
    arguments: { channel: "ext", settle_ms: 80, timeout_ms: 800 },
  });
  const body = JSON.parse(textOf(res));
  assert.match(body.output, /console banner/);
});

test("get_machine_state reads the events snapshot", async () => {
  const client = await connectClient(cfgFor(host));
  const res = await client.callTool({
    name: "get_machine_state",
    arguments: {},
  });
  const st = JSON.parse(textOf(res));
  assert.equal(st.powered, true);
  assert.equal(st.halt, false);
  assert.equal(st.running, true); // derived: powered && !halt
  assert.deepEqual(st.switches, [1, 0, 1, 0]);
  assert.deepEqual(st.leds, [0, 0, 0, 0]);
});

test("get_machine_state reports running:false when the CPU is halted", async () => {
  board.stateSnapshot = { ...board.stateSnapshot, halt: true };
  const client = await connectClient(cfgFor(host));
  const res = await client.callTool({ name: "get_machine_state", arguments: {} });
  const st = JSON.parse(textOf(res));
  assert.equal(st.halt, true);
  assert.equal(st.running, false);
  board.stateSnapshot = { ...board.stateSnapshot, halt: false };
});

test("start_machine issues the power-up action and confirms running", async () => {
  const client = await connectClient(cfgFor(host));
  const res = await client.callTool({ name: "start_machine", arguments: {} });
  // default action is restart
  assert.deepEqual(board.requests.at(-1)!.body, { action: "restart" });
  const st = JSON.parse(textOf(res));
  assert.equal(st.running, true);
});

test("start_machine accepts dc_on and wait_for_running resolves on a running snapshot", async () => {
  const client = await connectClient(cfgFor(host));
  await client.callTool({ name: "start_machine", arguments: { action: "dc_on" } });
  assert.deepEqual(board.requests.at(-1)!.body, { action: "dc_on" });
  const res = await client.callTool({
    name: "wait_for_running",
    arguments: { timeout_ms: 2000 },
  });
  assert.equal(JSON.parse(textOf(res)).running, true);
});

test("get_log collects log events over the window, filtered by level", async () => {
  const client = await connectClient(cfgFor(host));
  // Emit events shortly after the tool subscribes.
  setTimeout(() => {
    board.broadcastEvent({ t: "log", level: 2, label: "web", text: "an error" });
    board.broadcastEvent({ t: "log", level: 5, label: "web", text: "chatter" });
    board.broadcastEvent({ t: "log", level: 3, label: "PRU", text: "a warning" });
  }, 60);
  const res = await client.callTool({
    name: "get_log",
    arguments: { level: "warning", duration_ms: 400, max_lines: 100 },
  });
  const lines = JSON.parse(textOf(res)) as { text: string; levelName: string }[];
  const texts = lines.map((l) => l.text);
  assert.ok(texts.includes("an error"));
  assert.ok(texts.includes("a warning"));
  assert.ok(!texts.includes("chatter")); // debug filtered out at warning
});

test("auth: Authorization header comes from a stubbed ~/.qbone-pw", async () => {
  const dir = mkdtempSync(join(tmpdir(), "qbone-pw-"));
  const pwFile = join(dir, ".qbone-pw");
  writeFileSync(pwFile, "s3cr3t\n");
  const prevFile = process.env.QBONE_PW_FILE;
  const prevHost = process.env.QBONE_HOST;
  process.env.QBONE_PW_FILE = pwFile;
  process.env.QBONE_HOST = host;
  try {
    const cfg = loadConfig();
    const expected = "Basic " + Buffer.from(":s3cr3t").toString("base64");
    assert.equal(cfg.authHeader, expected);
    const client = await connectClient(cfg);
    await client.callTool({ name: "get_devices", arguments: {} });
    assert.equal(board.requests.at(-1)!.auth, expected);
  } finally {
    if (prevFile === undefined) delete process.env.QBONE_PW_FILE;
    else process.env.QBONE_PW_FILE = prevFile;
    if (prevHost === undefined) delete process.env.QBONE_HOST;
    else process.env.QBONE_HOST = prevHost;
  }
});
