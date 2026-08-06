# MCP server — implementation plan

A workstation MCP server exposing named tools for observing and controlling the
running QBone, over its REST/WS API. Implements the requirements in
[`mcp-server.md`](../planning/mcp-server.md).

## 1. Shape

- **Runs on the workstation**, reaching the board over the network via its REST/WS
  API. The board image stays lean; the workstation holds the credential.
- **Node/TypeScript**, on the official MCP TS SDK, sharing the toolchain the
  Vite/Preact frontend introduces this round.
- **Reuses QBone's HTTP basic password** from `~/.qbone-pw` — the same auth the
  REST examples use; the board host is configured (default `qbone`), no separate
  credential.
- **Control is always exposed** — no read-only mode.

It is a **thin wrapper** over the existing API: almost every tool is one REST call
or a short WS subscription. The one backend addition this plan depends on is the
verbal device-status field (§3), shared with the dashboard.

## 2. Tools

Observation:

- **`get_log`** — read the QBone log (`GET /api/log`), with a line/level filter.
- **`console_read`** — snapshot the current console buffer for a channel (the
  256 KB ring the [console plan](console-plan.md) retains, read via a short
  `/ws/console/<ch>` connect that replays history, or a snapshot endpoint if one
  is added).
- **`console_send`** — send input to a console channel.
- **`get_devices`** — the device set with parameters, the friendly `label`
  ([device-metadata-plan.md](device-metadata-plan.md)), and the live `status`
  field (§3), from `GET /api/devices`. One tool returns both the configuration and
  the live state, so they never disagree.
- **`get_machine_state`** — run/halt, power, activity LEDs, and DIP switches, from
  the `/ws/events` `state` event (`halt`, `powered`, `leds[]`, `switches[]`). A
  separate tool from device status, since it is the front-panel/bus view.

Control:

- **`set_param`** / **`set_device_enabled`** — write a parameter, enable/disable a
  device (`PUT /api/devices/<dev>/params/<param>`).
- **`control`** — `powercycle`, `init`, `restart`, `dc_on`, `dc_off`
  (`POST /api/control`).
- **`halt`** / **`continue`** — the run controls (`POST /api/control`).
- **`configs`** — list, apply, save, switch, set default
  ([configuration-model-plan.md](configuration-model-plan.md)).
- **`images`** — list, upload, attach to a removable drive (`/api/images`,
  `PUT /api/configs/.../image` or the live device image param).

## 3. Per-device runtime status

The verbal status (`off`/`idle`/`loaded`/`ready`/`busy`) is **derived in the
backend and exposed as a `status` field** in `GET /api/devices`, shared with the
dashboard ([web-dashboard-plan.md](web-dashboard-plan.md)) so the two never drift.
`get_devices` returns it directly; the MCP server does not re-derive it. This is
the single backend change the MCP server relies on, and it is shared work, not
MCP-specific.

## 4. Wait-for helpers

Implemented **client-side on the existing streams**, so the board gains no
blocking endpoints:

- **`wait_for_halt`** — the server holds a `/ws/events` subscription and resolves
  when a `state` event reports `halt=true` (or already halted), with a timeout.
- **`wait_for_console`** `{channel, pattern, timeout}` — the server connects to
  `/ws/console/<channel>`, which replays history then streams live, and resolves
  when the accumulated output matches `pattern`. Because the channel replays its
  ring on connect, a pattern that already scrolled past is still caught within the
  retained window.

These let an agent block instead of busy-looping, without new board code.

## 5. Testability

The server is tested against a **mock QBone HTTP/WS server** in the workstation
project: each tool issues the expected request; `wait_for_halt` resolves on a
scripted `state` event and times out otherwise; `wait_for_console` matches across
the replayed-then-live boundary; auth is read from a stubbed `~/.qbone-pw`. This
is a standalone Node test suite, independent of the board.

## 6. Decisions

Resolved:

- **Workstation-hosted, Node/TypeScript**, reusing `~/.qbone-pw`; control always
  exposed.
- A **thin wrapper** over the REST/WS API; **wait-for is client-side** on
  `/ws/events` and `/ws/console`, no new board endpoints.
- **Per-device verbal status is a backend field** shared with the dashboard;
  `get_devices` returns config + live state together.
- **`get_machine_state` is a separate tool** for the front-panel/bus view.
- **Console access is snapshot + send-input**, paired with `wait_for_console`.

Open:

- Whether `console_read` snapshots via a short WS connect (replay then close) or a
  small `GET /api/console/<ch>` snapshot endpoint is added for a cleaner
  request/response — an implementation choice; the WS-connect path needs no board
  change.
- Packaging/distribution of the server on the workstation (npx entry, config
  file) — settled when it is built.
