/* render.test.ts: a recorded session rendered for a reader — the transcript
 * as it ended up on the screen, split into steps, with typing marked off. */
import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync, readFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { CastRecorder } from "../src/recording.js";
import { renderDoc, renderPlayer } from "../src/render.js";

function tmp(name: string): string {
  return join(mkdtempSync(join(tmpdir(), "qcon-render-")), name);
}

/** A session with two steps, a typed command, and a password that never echoed. */
async function sampleCast(): Promise<string> {
  const path = tmp("sample.cast");
  let now = 1000;
  const rec = new CastRecorder(path, { title: "sample run" }, () => now);
  const out = (s: string) => rec.output(new Uint8Array(Buffer.from(s, "latin1")));
  const typed = (s: string, redact = false) => {
    for (const ch of s) rec.input(new Uint8Array(Buffer.from(ch, "latin1")), { redact });
  };
  out("login: ");
  rec.marker("step 1: login");
  typed("root");
  out("root\r\n");
  now += 2000;
  out("Password:");
  rec.marker("step 2: password");
  typed("hunter2", true);
  out("\r\n# ");
  now += 3000;
  await rec.close(0);
  return path;
}

test("the doc render carries the transcript, the steps and the typing", async () => {
  const path = await sampleCast();
  const html = await renderDoc(path);
  assert.match(html, /<!doctype html>/i);
  assert.match(html, /sample run/);
  // steps become headings
  assert.match(html, /step 1: login/);
  assert.match(html, /step 2: password/);
  // the guest's output is there
  assert.match(html, /login: /);
  // what was typed is marked off from what the machine printed
  assert.match(html, /<span class="typed">root<\/span>/);
  // and the password is neither shown nor silently dropped
  assert.ok(!html.includes("hunter2"), "the password is not in the render");
  assert.match(html, /redacted/);
  // elapsed times are seconds here, not milliseconds
  assert.match(html, /2\.0 s|3\.0 s/);
});

test("a cast without a sidecar still renders", async () => {
  const path = await sampleCast();
  // hide the notes file by rendering a copy that has none
  const bare = tmp("bare.cast");
  const fs = await import("node:fs");
  fs.writeFileSync(bare, readFileSync(path, "utf8"));
  const html = await renderDoc(bare);
  assert.match(html, /step 1: login/);
});

test("the player render is one self-contained file", async () => {
  const path = await sampleCast();
  const html = renderPlayer(path);
  assert.match(html, /AsciinemaPlayer\.create/);
  // the cast rides in the page, so nothing is fetched from a server
  assert.match(html, /data:text\/plain;base64,/);
  assert.ok(!/src="http/.test(html), "no external script");
  assert.ok(html.length > 50000, "the player bundle is inlined");
});

test("a redrawn line renders as the text it ended up being", async () => {
  const path = tmp("redraw.cast");
  const rec = new CastRecorder(path, { title: "redraw" });
  rec.output(new Uint8Array(Buffer.from("checking...\rCHECKED    \r\n", "latin1")));
  await rec.close(0);
  const html = await renderDoc(path);
  assert.match(html, /CHECKED/);
  assert.ok(!html.includes("checking..."), "the overwritten text is gone");
});
