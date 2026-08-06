# MCP server

**Status:** Ready — implementation plan drafted in
[`mcp-server-plan.md`](../plans/mcp-server-plan.md).

An MCP server exposing a clear interface to the running QBone, so an agent can
observe and control the machine through defined tools rather than raw HTTP and
WebSocket calls.

## Current state

QBone is driven through the REST API and the console WebSockets. An agent works
against those primitives directly. There is no MCP surface with named tools for
the common operations.

## Requirements

The server provides:

- **Log access** — read the QBone log.
- **Console access** — read the current console buffer (snapshot) and send input.
- **Device status** — the device set and their state/parameters.
- **Per-device runtime status** — each device's live operational state (e.g. a
  disk drive's `loaded`/`ready`/`busy`/`idle`, a controller's activity, an
  interface's link state), the same status the dashboard widgets show.
- **Device parameter writes** — set parameters, enable/disable devices.
- **System control and reset** — power-cycle and init.
- **Halt / run** — halt and continue the machine.
- **Configurations** — apply, save, and switch configurations
  ([configuration-model.md](configuration-model.md)).
- **Images** — list images, upload a new one, attach one to a removable drive.
- **Wait-for / poll helpers** — block until the machine halts, or until console
  output matches a pattern, so an agent does not busy-loop.

## Decisions

- **The server runs on the workstation**, reaching the board over the network via
  its REST/WS API. The board image stays lean; the workstation holds the
  credential.
- **Control is always exposed** — no separate read-only mode.
- **Console access is snapshot + send-input** (request/response), paired with the
  wait-for-pattern helper for change detection, rather than a live subscription.
- **Reuses QBone's HTTP basic password** from `~/.qbone-pw` — the same auth the
  REST examples use; no separate credential.

## Open questions

- The **wait-for helpers** likely need new backend support (a long-poll or event
  endpoint the server blocks on). What does that endpoint look like?
- Is reading the front-panel / bus state (LEDs, switches) part of device status,
  or a separate tool?
- Is per-device runtime status a separate tool from the static device/parameter
  list, or one tool returning both the configuration and the live state? It
  should draw from the same source as the dashboard's verbal disk status
  ([web-dashboard.md](web-dashboard.md)) so the two never disagree.
- Is it a thin wrapper over the existing REST/WS API, or does it need new backend
  endpoints (e.g. the wait-for helpers)?
