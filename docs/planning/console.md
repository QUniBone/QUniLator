# Console

**Status:** Ready — implementation plan drafted in
[`console-plan.md`](../plans/console-plan.md).

The serial console: reliable startup, server-side history replayed to new
connections, and its scope within the dashboard.

## Current state

The console is bridged over WebSockets — `/ws/console/ext` for the 11/73's real
SLU on `/dev/ttyS2`, `/ws/console/0` and `/ws/console/1` for emulated DL11s. The
bridge broadcasts to all connected clients, so several viewers coexist. A client
receives only the bytes that arrive after it connects; output produced before it
connected is gone. Opening the dashboard mid-session shows an empty console until
the next byte. The console sometimes fails to come up correctly, showing no
cursor. Navigating to another page and back sometimes leaves the console
disconnected — a black screen with no cursor — because the WebSocket is torn down
on navigation and does not cleanly re-establish.

## Requirements

### Startup reliability

- The console comes up correctly every time. It does not land in the broken state
  where no cursor is shown.
- Navigating away from the console page and back reconnects it cleanly — no black
  screen, no lost cursor. Replaying the stored history (below) restores the
  screen on reconnect. Relates to web navigation
  ([web-navigation.md](web-navigation.md)).
- Console state and any output produced while the user is on another page are
  retained across navigation and restored on return, from the server-side
  history (below).

### History storage and replay

- The console output is **stored on the server** in an **in-memory ring buffer**,
  a **bounded byte cap (~256 KB) per channel** with oldest bytes dropped; not
  persisted across a service restart.
- The stored history is **raw bytes**. On connect, a client is served the raw
  history verbatim before the live stream, and xterm.js reconstructs the screen.
- **All console channels** retain and replay history uniformly — the external
  console, the emulated DL11s, and the serial-mux ports
  ([serial-ports.md](serial-ports.md)).
- On reconnect a client is replayed the **full retained history** each time.

### Scope

- On this board the console does **not** need to be reconfigurable from the
  dashboard. Its source stays fixed (`ttys2`); the dashboard presents it, it does
  not configure it.
- The console is also reachable at its **own standalone URL** — a bare
  full-screen page separate from the dashboard
  ([web-navigation.md](web-navigation.md)).

### Console SLU source across CPU boards

Where the console SLU lives depends on the CPU. The 11/53 and 11/73 integrate it
on the processor board, wired here to `/dev/ttyS2` and bridged as the external
console. Other Q-bus CPUs (11/23, 11/03) have no onboard console SLU and need an
external SLU board — or could use QBone's **emulated DL11 at 777560** as the
console instead of physical hardware. On a board with an onboard SLU the emulated
DL11 must stay disabled, since the real SLU already answers 777560 and holds
`ttyS2`; on a board without one, that emulated DL11 *is* the console.

- QBone **assists the user in setting up the console** for their CPU through an
  **interactive web-UI setup step**. The user **chooses the SLU arrangement
  directly** (onboard SLU, external SLU board, or emulated DL11), and QBone
  configures the console source and the 777560 DL11 accordingly.
- Both arrangements are supported as a choice: **bridging a physical SLU**
  (onboard or external, over `ttyS2`) or providing the console through the
  **emulated DL11 at 777560**. When the emulated DL11 is the console, it is
  enabled; when a physical SLU is present, it stays disabled to avoid the 777560
  clash.

## Open questions

### Startup reliability

- What triggers the no-cursor state — the WebSocket connecting before the bridge
  is ready, an xterm.js init race, or a missing initial redraw from the guest?
- Is the console component fully unmounted on page switch (WebSocket closed,
  xterm disposed), or kept alive and just hidden? That decides whether the fix is
  clean teardown+reconnect or keeping it mounted.
- Is it reproducible on a fresh page load, or only after a reconnect?
- Would replaying stored history on connect (below) mask or fix it, or are they
  independent?

### History storage and replay

- Relationship to the journal — the console flush already keeps the journal live;
  is the stored console log the same artifact or separate?

### Console SLU source

- The onboard-SLU-vs-emulated-DL11 conflict at 777560 is a bus-address clash — is
  the guard that prevents enabling the DL11 over a real SLU the same "probe the
  bus for conflicts" work in the repo TODO?
- The external-console bridge source (`ttys2`/`webserial`/`off`) and the emulated
  DL11 enable are the two levers the setup step sets — does the step write them
  directly, or through a higher-level "console type" it stores?
