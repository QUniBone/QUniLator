# Web: dashboard — implementation plan

Reworks the dashboard: a PDP-11/03-styled control panel, a separate activity/DIP
front panel, verbal disk-status widgets with image selection, a serial-mux
widget, and a unified real-LED rendering. Implements the requirements in
[`web-dashboard.md`](../planning/web-dashboard.md). Built on the
Vite + Preact frontend, the [device metadata](device-metadata-plan.md) labels,
and the shared image picker from the
[config-management plan](web-config-management-plan.md).

## 1. What exists today

The dashboard shows status tiles, an 80×24 console, a control row (separate Halt,
Continue, Pulse INIT, Power cycle), RL02 panels, and a log. The `/ws/events`
`state` event already publishes `halt`, `leds[]` (activity LEDs, read active-low
from GPIO), and `switches[]` (the cape DIP switches, read from GPIO on the 10 Hz
poll). `/api/control` takes `init`, `powercycle`, `halt`, `continue`. There is no
"powered off" concept — nothing keeps the machine unpowered. Disk widgets show
graphical state with no words. LEDs render with a pronounced glow.

## 2. Control panel — PDP-11/03 bezel

Rendered as the 11/03 front-panel bezel: two LEDs and three switches, mapped to
control actions.

- **PWR OK LED** — lit when the machine is powered (the new power flag, §3).
- **RUN LED** — lit when the CPU is running (`!halt` and powered); off when halted
  or unpowered. This is the run-state display, moved into the control section.
- **RESTART switch** (momentary) — a **reset-then-restart**: pulse BINIT and
  resume from the power-up/boot sequence, more than a bare INIT. Backend
  `POST /api/control {"action":"restart"}` performs INIT then a resume at the
  restart vector (a small addition beside `init`/`powercycle`).
- **HALT / ENABLE switch** (two-position) — the single run-state control. HALT maps
  to `halt`, ENABLE to `continue`. It reflects the live `halt` state and sets it;
  during a transitional/unknown moment it **holds its last-known position** rather
  than flickering (client keeps the last definite value until a new one arrives).
- **DC ON/OFF switch** — a real toggle with a persistent off state (§3).

## 3. Power flag (new backend)

A **logical power state** gives the DC switch meaning without touching the
PRU/bus:

- A runtime `powered` flag (in the web layer / control path). OFF **halts the CPU
  and sets `powered=false`**; ON sets `powered=true` and **powercycles**.
- `POST /api/control` gains `{"action":"dc_off"}` and `{"action":"dc_on"}`.
- While `powered=false`, `restart`, `halt`, and `continue` are refused (and their
  controls disabled in the UI); `dc_on` is the only transition.
- The `state` event gains **`powered`**; RUN derives from `!halt && powered`,
  PWR OK from `powered`.
- **Runtime only** — a service restart comes up powered on and applies the default
  configuration as normal; the flag is not persisted.

While off the dashboard shows a **frozen, dark** machine: the console keeps its
last screen with no new bytes, disk widgets read `off`, and the RUN, PWR OK, and
activity LEDs are dark.

## 4. Front panel — activity LEDs and DIP switches

A separate element beside the control panel, a **fixed-size grid** in the same LED
visual language:

- **Activity LEDs** from the `state` event `leds[]`.
- **DIP switches** from `switches[]` — the cape's physical switches, **display
  only**, no assigned function. The panel reads and shows their position and
  nothing more. (Both arrays already flow from the backend; this is a
  presentation rework.)

## 5. Disk widgets

- **Verbal status** with five states, derived per drive type from the parameters
  the devices already expose (`enabled`, `image`, the drive `state` machine —
  e.g. RL02's `power_off`/`load_cartridge`/`seek`/`lock_on` — the ready/load
  lamps, and the activity signal):
  - `off` — device disabled;
  - `idle` — enabled, no image;
  - `loaded` — image attached, drive coming online (a spin-up/seek state);
  - `ready` — online, ready for I/O (lock-on / ready lamp);
  - `busy` — actively transferring (activity signal).

  A drive that never models spin-up (a fixed or non-spinning unit) may pass
  `loaded` instantly; the mapping tolerates that, going straight to `ready`. The
  derivation lives in the **backend and is exposed as a `status` field** in
  `GET /api/devices`; the dashboard and the MCP per-device status
  ([mcp-server-plan.md](mcp-server-plan.md)) both read that one field, so the two
  never drift.
- **Image selection from the widget** opens the **shared image-assignment picker**
  ([web-config-management-plan.md](web-config-management-plan.md)); against the
  live machine it sets the drive's image immediately. No fixed/removable chip.

## 6. Serial-mux widget

A serial mux ([serial-ports.md](../planning/serial-ports.md)) adds a widget: an
additional terminal with **tabs, one per port**, each a console channel with its
own history replay ([console-plan.md](console-plan.md)); independent DL11 lines are
single terminals.

## 7. LED rendering

One **shared LED component** used everywhere — the top-right status tiles, the
control-panel LEDs, and the front-panel LEDs:

- **slightly larger**, **less pronounced glow**;
- a **real-LED look throughout** — the reddish/amber of period DEC panels
  (matching the 11/03 photo), regardless of what each LED signals. The color is
  the physical LED color, not a semantic green/red.

## 8. Chrome

The "QBUS web interface" caption in the lower-left corner is removed.

## 9. Testability

- **Power flag**: host test that `dc_off` halts and clears `powered`, `restart`/
  `halt`/`continue` are refused while off, and `dc_on` powercycles and sets
  `powered`; the `state` event carries `powered`. Rides the configuration-model
  host harness.
- **Verbal-status helper**: unit-tested over the parameter combinations for each
  drive type → the expected state, including the non-spinning fast path.
- Frontend: HALT/ENABLE holds last-known across a transitional gap; the widget
  picker sets the live image.

## 10. Decisions

Resolved:

- Control panel is the **11/03 bezel** (PWR OK / RUN LEDs, RESTART / HALT-ENABLE /
  DC-ON-OFF) mapped to `init`/`restart`/`halt`/`continue`/`powercycle` and the new
  power flag.
- **DC OFF is a logical power flag** (halt + `powered=false`); ON powercycles;
  **runtime only**; while off the machine shows **frozen and dark**.
- **RESTART = reset + restart from boot** (a new `restart` action).
- **DIP switches display-only** from the already-published `switches[]`; activity
  LEDs from `leds[]`.
- **Disk verbal status** (off/idle/loaded/ready/busy) derived **per type in the
  backend and exposed as a `status` field**, read by both the dashboard and the
  MCP server; **image selectable from the widget** via the shared picker; no
  fixed/removable chip.
- **One LED component** everywhere — larger, less glow, real-LED amber/red.
- The corner caption is removed.

Open:

- The exact per-type signal→state table for `busy`/`loaded` on the MSCP (RA81) and
  RK05 drives is settled when the helper is written against each device's state
  machine.
