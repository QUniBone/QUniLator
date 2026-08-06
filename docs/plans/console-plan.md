# Console — implementation plan

Makes the serial console start reliably, survive navigation, and replay
server-side history to every new connection, and adds the interactive console
setup step. Implements the requirements in
[`console.md`](../planning/console.md). Built on the Preact
router ([web-navigation.md](../planning/web-navigation.md)).

## 1. What exists today

Two backends broadcast console bytes with the same shape — a `clients` set and
the non-blocking `web_ws_try_send` helper:

- `webconsole_ext.cpp` (`/ws/console/ext`) bridges the real SLU on `/dev/ttyS2`;
  a reader thread polls the tty and forwards to all clients.
- `webconsole.cpp` (`/ws/console/0`,`/1`) taps the emulated DL11s; a 20 ms flush
  thread forwards each device's `xmt_buffer`.

Neither retains history: a client added in `ws_ready_handler` receives only bytes
that arrive after it connects, so a page opened mid-session shows an empty
console until the next byte.

On the frontend, the three xterm instances live in **module state**
(`index.html`, `TERMS`), created once and `term.open()`ed into DOM nodes appended
to the dashboard's terminal host. `initLiveTerminal` early-returns `if (TERMS)`.
When the dashboard unmounts on navigation its host DOM is destroyed; on return a
new host mounts but the guard skips re-attachment, leaving the terminals parented
to the detached old host — the black screen with no cursor.

## 2. Server-side history and replay

A small shared channel type, `console_channel_c`, owns the retained history and
the client set, and both backends use it:

- **Ring buffer**, a bounded byte cap **256 KB per channel**, oldest bytes
  dropped, raw bytes, in memory only (not persisted across a service restart).
- **`append(bytes)`** — called by the reader/flush thread: under the channel lock
  it appends to the ring and broadcasts to every client with `web_ws_try_send`,
  dropping the ones that report dead.
- **`add_client(conn)`** — called from `ws_ready_handler`: under the **same
  lock**, it sends the current ring snapshot to `conn` as binary, then inserts it
  into the client set. Holding the one lock across snapshot-send-then-insert makes
  the handoff atomic — the new client gets the full history followed by the live
  stream with no gap and no duplicated byte. The snapshot goes out through
  `web_ws_try_send`; a client that cannot absorb it is dropped rather than blocking
  the producer, the same contract the broadcaster already lives by.

xterm.js reconstructs the screen from the replayed raw bytes, so a fresh connect
or a reconnect repaints without any server-side screen model.

**All channels** use this uniformly — the external console, the emulated DL11s,
and the serial-mux ports ([serial-ports.md](../planning/serial-ports.md)) each
own a `console_channel_c`.

### Relationship to the journal

Separate artifacts. The journal is the **logger's** file sink — rendered,
timestamped log lines ([logging.md](../planning/logging.md)). The console ring
holds **raw tty/DL11 bytes** for screen reconstruction. They share nothing;
neither replaces the other.

## 3. Startup and navigation reliability

The frontend moves the terminal off module state into a component that follows
the router's lifecycle, chosen as **dispose-and-recreate**:

- **Unmount** (navigating away, or the standalone page closing) closes the
  channel WebSocket and calls `term.dispose()`.
- **Mount** creates a fresh `Terminal`, opens it into the current host element
  *after* the element is in the document with layout, and connects the
  WebSocket; the server replays history (§2) and the screen repaints.

This removes the `if (TERMS)` guard and the detached-host bug with it — every
mount is a clean build against the live DOM. The same component backs the
dashboard-embedded console and the standalone `/console` route, so both paths are
identical. Local scroll position and scrollback beyond the 256 KB cap are not
preserved across navigation; the retained history repaints the visible screen.

The residual fresh-load no-cursor case (open before the bridge produces a byte) is
covered by the same replay: on connect the client receives whatever the ring
holds, and if the ring is empty the cursor still renders because `term.open()`
now runs against a laid-out element. Reconnect after a dropped socket keeps the
existing 2 s retry, now followed by a history replay that restores the screen
rather than resuming mid-stream.

## 4. Console setup step

An interactive web-UI step helps the user set the console up for their CPU board.
The user chooses the SLU arrangement directly; the choice is stored as a single
high-level field and the backend derives the levers:

- **`console_type`** in `settings.json`, one of `onboard-slu`, `external-slu`,
  `emulated-dl11`.
- The backend derives:
  - `onboard-slu` / `external-slu` → `external_console.source = ttys2`, the
    777560 DL11 **disabled** (the real SLU answers 777560 and holds `ttyS2`).
  - `emulated-dl11` → the 777560 DL11 **enabled** as the console, `external_console`
    off.
- The **conflict guard** lives in one place: `console_type` cannot select
  `emulated-dl11` while a physical SLU holds `ttyS2`, and cannot leave the
  external bridge on `ttys2` while the 777560 DL11 is enabled — the same 777560
  clash `console_conflict()` already checks, now expressed as one setting whose
  derivation is always consistent. On this board `console_type` stays
  `onboard-slu`.

`console_type` is a board setting, applied independently of the configuration
(like `external_console` today), so switching configurations never disturbs it.

## 5. API / WS

- `/ws/console/ext`, `/ws/console/0`, `/ws/console/1` unchanged in shape; each now
  replays its ring on connect.
- Serial-mux channels get `/ws/console/mux<n>/<port>` on the same channel type
  ([serial-ports.md](../planning/serial-ports.md)).
- `console_type` is read/written through `PUT /api/settings` alongside
  `external_console`; `GET /api/settings` reports it and the derived lever state so
  the setup step can present the current arrangement. `api.md` updated.

## 6. Testability

- **Channel host test**: drive a `console_channel_c` with a scripted producer and
  synthetic clients — a late-joining client receives the exact retained snapshot
  then the live stream in order; the ring drops oldest past 256 KB; a client that
  reports dead on the snapshot is removed. Rides the host harness from the
  configuration-model plan.
- **console_type derivation** unit-tested: each value yields the right
  `external_console`/DL11-enable pair, and the two conflicting combinations are
  rejected.
- Frontend mount/unmount is exercised by a route-change test asserting the
  terminal reconnects and repaints (the navigation bug regression).

## 7. Decisions

Resolved:

- History is an **in-memory 256 KB ring per channel**, raw bytes, replayed in full
  on every connect; snapshot-and-insert is atomic under the channel lock.
- **One shared `console_channel_c`** backs the external console, the emulated
  DL11s, and the serial-mux ports.
- The navigation bug is fixed by **dispose-and-recreate** under the router; the
  terminal leaves module state.
- The console ring and the logger journal are **separate** artifacts.
- The setup step stores a high-level **`console_type`**; the backend derives the
  `external_console` source and the 777560 DL11 enable, with the conflict guard in
  one place.

Open:

- Whether the setup step is a first-run wizard, an always-available settings
  panel, or both — a UI placement decision for the dashboard/settings work.
