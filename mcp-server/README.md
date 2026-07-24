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
- **Credential** — QBone's HTTP basic password, read once from `~/.qbone-pw`
  (any user name, that password), the same auth the REST examples and the Vite
  dev proxy use. Override the file with `QBONE_PW_FILE` (the test suite points it
  at a stub). A board with no password answers without the header.

Control is always exposed; there is no read-only mode.

## Build and run

```sh
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
| `get_machine_state` | `/ws/events` opening `state` snapshot — `halt`, `powered`, `leds[]`, `switches[]` |
| `get_log` | tails the `/ws/events` `log` stream for a window, filtered by severity |
| `console_read` | connects `/ws/console/<ch>`, collects the replayed ring, returns it |
| `console_send` | sends bytes to `/ws/console/<ch>` |

Control:

| tool | wraps |
|---|---|
| `set_param` | `PUT /api/devices/<dev>/params/<param>` |
| `set_device_enabled` | `PUT .../params/enabled` (`true`/`false`) |
| `control` | `POST /api/control` — `powercycle`/`init`/`restart`/`dc_on`/`dc_off` |
| `halt` / `continue` | `POST /api/control` — the run controls |
| `configs` | `list` `GET /api/configs`; `apply`/`switch` `POST .../apply`; `save` reads `GET /api/configs?current=1` then `PUT /api/configs/<name>?from=live`; `set_default` `PUT .../default` |
| `images` | `list` `GET /api/images`; `upload` posts a local file to `POST /api/images`; `attach` writes a drive's `image` param (empty detaches) |

Wait-for (client-side on the existing streams, no board endpoint blocks):

| tool | how |
|---|---|
| `wait_for_halt` | holds a `/ws/events` subscription, resolves when a `state` event reports `halt` (an already-halted machine resolves at once), or times out |
| `wait_for_console` | connects `/ws/console/<channel>`, which replays its ring then streams live, resolves when the accumulated output matches a regex `pattern`, or times out |

Channels are `0` (DL11 @777560), `1` (@776500), and `ext` (the real console SLU
on `/dev/ttyS2`).

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
