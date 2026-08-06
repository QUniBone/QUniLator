# QBone MCP server

A workstation [MCP](https://modelcontextprotocol.io) server that exposes named
tools for observing and controlling a running QBone over its REST/WS API. It is
a thin wrapper: almost every tool is one REST call or a short WebSocket
subscription. It runs on the workstation and reaches the board over the network;
the board image stays lean and the credential lives here.

This is a standalone Node/TypeScript project — not the browser frontend, not the
board C++.

## Configuration

- **Board host** — `QBONE_HOST` (default `qbone`). May carry a port, e.g.
  `QBONE_HOST=127.0.0.1:8080`.
- **Credential** — QBone's HTTP basic user name and password, the same auth the
  REST examples and the Vite dev proxy use. The password is read once from
  `~/.qbone-pw`, the name from `QBONE_USER` or `~/.qbone-user`. Override the
  files with `QBONE_PW_FILE` and `QBONE_USER_FILE` (the test suite points them
  at stubs). A board with no password answers without the header.

  **The name is part of the credential.** A board provisioned through the
  first-run dialog carries one identity that is both the operator's account and
  the web login, and it answers `401` to the right password under the wrong
  name. A board set up before that dialog carries only a password and takes any
  name, which is what an unset name still serves.

Control is always exposed; there is no read-only mode.

## Build and run

`qcon`, the console harness in `../console-harness`, is a `file:` dependency
resolved to its `dist/`, so it is built first — a fresh clone has no `dist/` and
the build fails on `Cannot find module 'qcon'`.

```sh
(cd ../console-harness && npm install && npm run build)
npm install
npm run build      # tsc, emits dist/
npm start          # serve over stdio (QBONE_HOST=qbone by default)
```

`npm start` runs `node dist/src/index.js`, which speaks MCP over stdio. Diagnostics
go to stderr; stdout carries the protocol.

## Wiring it into an MCP client

Point the client at the built entry, passing the host in the environment. For a
Claude Code / Claude Desktop style `mcpServers` entry:

```json
{
  "mcpServers": {
    "qbone": {
      "command": "node",
      "args": ["/absolute/path/to/mcp-server/dist/src/index.js"],
      "env": { "QBONE_HOST": "qbone" }
    }
  }
}
```

The password is read from `~/.qbone-pw` at startup; no secret goes in the client
config.

## Tools

Observation:

| tool | wraps |
|---|---|
| `get_devices` | `GET /api/devices` — device set, parameters, `label`, `status` (returned as-is) |
| `get_machine_state` | `/ws/events` opening `state` snapshot — derived `running` (powered && HALT released), plus `halt`, `powered`, `leds[]`, `switches[]`. Check `running` before booting or running anything |
| `get_log` | tails the `/ws/events` `log` stream for a window, filtered by severity (each line: label, level, text). Sources default to `warning` — raise the one you want with `set_log_level` first |
| `get_logging` | `GET /api/logging` — the log sources and their current levels |
| `console_read` | connects `/ws/console/<ch>`, collects the replayed ring, returns it |
| `console_send` | sends bytes to `/ws/console/<ch>` |

Console sessions — one connection held for a whole dialog, with matching anchored
where the last step ended. This is what anything with more than one prompt uses;
`console_read`/`console_send` are the one-shot pair.

| tool | |
|---|---|
| `console_session_open` | opens the session and returns its id. Takes `record_path` for an asciicast v3 recording, and `deviations` — patterns or a `halt`/`power-loss` event that fail a step the moment they appear. Idle sessions close after 15 minutes |
| `console_expect` | waits for one of several patterns (fixed text, or `/regex/flags`) and reports which matched. Fails early on a deviation, on a halt, and when the console falls quiet at a prompt none of the patterns match |
| `console_send_line` | types a line and terminates it with CR. `echo` (default) paces each character on the guest's echo; `no-echo` sends on a fixed delay and records redacted, for a password prompt; `raw` writes unpaced |
| `console_send_break` | asserts a line BREAK, which is a line condition rather than a character |
| `console_session_close` | releases the connection and finishes the recording |
| `console_sessions` | lists the open sessions |

Control:

| tool | wraps |
|---|---|
| `set_param` | `PUT /api/devices/<dev>/params/<param>` |
| `set_device_enabled` | `PUT .../params/enabled` (`true`/`false`) |
| `set_log_level` | `PUT /api/logging/sources/<source>` — raise a source to `debug` before `get_log`, lower it after |
| `start_machine` | bring the machine up **running** and confirm it: `restart` (default) or `dc_on`, then wait for a `state` frame reporting the CPU running. The reliable way to begin a test run — the boot ROM then auto-boots the first bootable device |
| `control` | `POST /api/control` — `powercycle`/`init`/`restart`/`dc_on`/`dc_off`. `powercycle` does NOT release HALT (a halted CPU comes up in ODT) and re-runs the DIP→config selection, re-applying that config; `restart` comes up running keeping the applied config. Prefer `start_machine` |
| `halt` / `continue` | `POST /api/control` — the run controls. After a `halt`, reboot with `start_machine`, not `powercycle` |
| `configs` | `list` `GET /api/configs`; `apply`/`switch` `POST .../apply`; `save` reads `GET /api/configs?current=1` then `PUT /api/configs/<name>?from=live`; `set_default` `PUT .../default` |
| `images` | `list` `GET /api/images`; `upload` posts a local file to `POST /api/images`; `attach` writes a drive's `image` param (empty detaches) |

Diagnostics:

| tool | how |
|---|---|
| `list_xxdp_diagnostics` | what is on the XXDP 2.5 RL02 pack, answered from `data/xxdp25-rl02.json` rather than from the board. `match` is a case-insensitive regular expression on the file name — `^ZTK` the TK50, `^ZRL` the RL, `^ZUD` the UDA50, `^ZRQ` the RQDX, `^ZRX` the RX. Names carry the extension and drop the leading class letter of the DEC part number (CZTKAE0 is `ZTKAE0.BIN`), which is what `run_xxdp_diagnostic` wants |
| `run_xxdp_diagnostic` | applies a config and device setup, brings the machine up running, loads the diagnostic over the console, drives the DRS dialog from `answers`, and reports `{passed, terminatedBy, transcript}` |

`data/xxdp25-rl02.json` is the pack's directory, 727 files, read off the image
by `tools/xxdp-dir.py`. Regenerate it after changing the pack:

	./tools/xxdp-dir.py --json xxdp25.rl02 > mcp-server/data/xxdp25-rl02.json

Wait-for (client-side on the existing streams, no board endpoint blocks):

| tool | how |
|---|---|
| `wait_for_halt` | holds a `/ws/events` subscription, resolves when a `state` event reports `halt` (an already-halted machine resolves at once), or times out |
| `wait_for_running` | same, resolves when a `state` event reports the CPU running (powered with HALT released); counterpart to `wait_for_halt` |
| `wait_for_console` | connects `/ws/console/<channel>`, which replays its ring then streams live, resolves when the accumulated output matches a regex `pattern`, or times out |

Channels are `0` (DL11 @777560), `1` (@776500), `ext` (the real console SLU on
`/dev/ttyS2`) and `vax` (the emulated VAX-11/780's own console).

### Note on `get_log`

The board keeps no server-side log history and has no `GET /api/log`; log lines
are pushed live over `/ws/events`. `get_log` therefore tails that stream for
`duration_ms` and returns the lines at or above `level`. Only messages the logger
actually emits appear — a target below the configured log level produces nothing
during the window (adjust levels via the board's logging endpoints).

## Tests

```sh
npm test
```

Compiles, then runs the `node:test` suite against a mock QBone HTTP/WS server
(`test/mock-board.ts`). Each tool is driven end-to-end through an in-memory MCP
client and asserted to issue the expected request; `wait_for_halt` resolves on a
scripted `state` event and times out otherwise; `wait_for_console` matches across
the replayed-then-live boundary; auth is read from a stubbed `~/.qbone-pw`.
