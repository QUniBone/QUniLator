---
title: The MCP server
description: Give a model the run of your machine — power, devices, disk images, the console and XXDP diagnostics — as named tools over QUniLator's own API.
---

The QUniLator repository carries an [MCP](https://modelcontextprotocol.io)
server that exposes a running QUniLator as named tools: switch the machine on,
set a device parameter, swap a disk pack, hold a dialog on the console, run an
XXDP diagnostic, read the log. Point an MCP client at it and a model can operate
the machine rather than only talk about one.

It is a thin wrapper — almost every tool is one REST call or a short WebSocket
subscription against the same [API](https://github.com/QUniBone/QUniLator/blob/main/10.05_web/docs/api.md)
the web interface uses.

## Where it runs

**On your workstation, reaching QUniLator over the network.** Nothing is
installed on the card, the release image carries nothing extra, and the
credential stays on the machine you already trust with it. One client can drive
several QUniLators by running the server once per card.

> [!WARNING]
> **Control is always exposed**
>
> There is no read-only mode. A client wired to this server can power-cycle the
> machine, rewrite device parameters and detach a mounted pack. Point it at a
> machine you are willing to have operated.

## Installing it

It is a Node/TypeScript project in the repository, so start from a checkout. The
server depends on `qcon`, the console harness beside it, which is built first:

```sh
git clone https://github.com/QUniBone/QUniLator.git
cd QUniLator/console-harness && npm install && npm run build
cd ../mcp-server        && npm install && npm run build
```

`npm start` then serves over stdio, which is how a client will run it. Stdout
carries the protocol and diagnostics go to stderr, so the first line you see is
the board it is talking to:

```
qbone-mcp-server: board qbone
```

## Telling it which QUniLator, and who you are

| | |
|---|---|
| `QBONE_HOST` | the QUniLator to drive; default `qbone`. May carry a port — `127.0.0.1:8080`. |
| `~/.qbone-pw` | the web password, read once at startup |
| `QBONE_USER`, or `~/.qbone-user` | the user name |

The variables are named for the QBUS card they were written against and apply to
either card; a UniBone goes in `QBONE_HOST` like anything else.

> [!IMPORTANT]
> **The name is part of the credential**
>
> A QUniLator set up through the first-run dialog carries one identity that is
> both your account on the BeagleBone and your web login. It answers `401` to the
> right password under the wrong name, so set `QBONE_USER` to the name you
> created. An installation predating that dialog carries only a password and
> takes any name.

## Wiring it into a client

Point the client at the built entry and pass the host in the environment. This
is a Claude Code / Claude Desktop style `mcpServers` entry, with two QUniLators
as two servers:

```json
{
  "mcpServers": {
    "qbone": {
      "type": "stdio",
      "command": "node",
      "args": ["/path/to/QUniLator/mcp-server/dist/src/index.js"],
      "env": { "QBONE_HOST": "qbone", "QBONE_USER": "you" }
    },
    "unibone": {
      "type": "stdio",
      "command": "node",
      "args": ["/path/to/QUniLator/mcp-server/dist/src/index.js"],
      "env": { "QBONE_HOST": "unibone.local", "QBONE_USER": "you" }
    }
  }
}
```

No secret goes in the client configuration: the password is read from
`~/.qbone-pw` when the server starts.

## What it offers

**Looking at the machine** — `get_machine_state` (running, halted, powered, the
panel LEDs and switches), `get_devices` (the device set with every parameter),
`get_logging` and `get_log`.

**Running it** — `start_machine` brings the machine up *running* and confirms it,
which is the reliable way to begin anything; `halt` and `continue` are the run
controls; `control` is the raw power actions for when you want one specifically.

**Changing it** — `set_param`, `set_device_enabled`, `configs` (list, apply,
save, set default) and `images` (list, upload a file from the workstation,
attach one to a drive).

**The console** — `console_session_open` holds one connection for a whole dialog,
with `console_expect`, `console_send_line` and `console_send_break` inside it and
`console_session_close` at the end. `console_read` and `console_send` are the
one-shot versions, and `wait_for_console` waits for a pattern without holding a
session. Every one of them takes a channel: `0` and `1` are the emulated DL11
lines at 777560 and 776500, `ext` is the real console SLU on `/dev/ttyS2`, and
`vax` is the emulated VAX-11/780's own console.

**Diagnostics** — `list_xxdp_diagnostics` searches the XXDP 2.5 pack's directory,
and `run_xxdp_diagnostic` runs one end to end and reports whether it passed.

**Waiting** — `wait_for_running`, `wait_for_halt`, `wait_for_console`.

## Worked examples

Each of these is one thing you ask for, and the tool calls that answer it.

### Boot a machine and wait for it

`start_machine` releases HALT and restarts the CPU from the power-up vector,
then waits for the machine to report itself running. The boot ROM auto-boots the
first bootable device, so there is no boot dialog to drive — the next thing to
do is wait for the guest to say something you recognise.

```json
{ "name": "start_machine", "arguments": { "action": "restart" } }
{ "name": "wait_for_console",
  "arguments": { "channel": "0", "pattern": "login:", "timeout_ms": 120000 } }
```

### Swap the pack in a drive

```json
{ "name": "images", "arguments": { "action": "list" } }
{ "name": "images", "arguments": {
    "action": "attach", "device": "rl0", "image": "images/dl/xxdp25.rl02" } }
```

`attach` writes the drive's `image` parameter; an empty `image` detaches. To
bring a file up from the workstation first, `{"action": "upload", "path":
"/home/you/rt11.rl02"}`.

### Hold a dialog on the console

One session for the whole conversation, so matching is anchored where the last
step ended rather than replaying the scrollback. Input is paced on the guest's
echo, which is what makes it reliable on an SLU with no receive FIFO.

```json
{ "name": "console_session_open", "arguments": { "channel": "0" } }
{ "name": "console_expect", "arguments": {
    "session": "…", "patterns": ["login:"] } }
{ "name": "console_send_line", "arguments": {
    "session": "…", "text": "root" } }
{ "name": "console_expect", "arguments": {
    "session": "…", "patterns": ["Password:", "# "] } }
{ "name": "console_session_close", "arguments": { "session": "…" } }
```

A password prompt does not echo, so send it with `"mode": "no-echo"` — it goes
out on a fixed delay, is never resent, and is recorded redacted.

### Run a DEC diagnostic

`run_xxdp_diagnostic` applies a configuration, sets the devices up, brings the
machine up running, loads the diagnostic over the console, answers the DRS
dialog and reports `{passed, terminatedBy, transcript}`. Diagnostic names drop
the leading class letter of the DEC part number — `CZTKAE0` is `ZTKAE0`.

```json
{ "name": "list_xxdp_diagnostics", "arguments": { "match": "^ZRL" } }
{ "name": "run_xxdp_diagnostic", "arguments": {
    "diagnostic": "ZRLGE0",
    "config": "xxdp",
    "setup": [
      { "device": "rl1", "enabled": true },
      { "device": "rl1", "param": "image",
        "value": "images/dl/scratch.rl02" }
    ],
    "answers": [
      { "match": "CHANGE HW", "value": "Y" },
      { "match": "UNITS", "value": "1" }
    ]
  } }
```

### Find out why a device is misbehaving

Every log source sits at `warning`, so raise the one you are interested in
before reading, and put it back afterwards — a source left at `debug` costs the
running machine.

```json
{ "name": "set_log_level", "arguments": { "source": "rl", "level": "debug" } }
{ "name": "get_log",       "arguments": { "level": "debug", "duration_ms": 5000 } }
{ "name": "set_log_level", "arguments": { "source": "rl", "level": "warning" } }
```

## The developer's version

[`mcp-server/README.md`](https://github.com/QUniBone/QUniLator/blob/main/mcp-server/README.md)
in the repository covers the same configuration, the full tool table against the
API calls each one makes, and the test suite.
