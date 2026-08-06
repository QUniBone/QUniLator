# Web: dashboard

**Status:** Ready — implementation plan drafted in
[`web-dashboard-plan.md`](../plans/web-dashboard-plan.md).

Refinements to the dashboard: the control row, the front panel, disk widgets,
and LED rendering.

## Current state

The dashboard shows status tiles, an 80×24 console, a control row, RL02 disk
panels, and a log. Halt and continue are separate buttons, alongside Pulse INIT
and Power cycle. Run/halt/init state, activity LEDs, and DIP switches sit as
separate top tiles. Disk widgets show a graphical state but no words. LEDs render
with a pronounced glow.

## Requirements

### Control panel — PDP-11/03 console

The control section is rendered as the **PDP-11/03 front-panel bezel** (two LEDs
and three rocker switches), driving QBone's control actions:

- **PWR OK LED** — lit when the machine is powered (DC present).
- **RUN LED** — lit when the CPU is running and executing an instruction; off
  when halted or waiting. This is the run-state display, now living in the
  control section.
- **RESTART switch** (momentary) — pulses BINIT and restarts the processor from
  its power-up/boot sequence (INIT, then resume at the restart vector), matching
  the 11/03's "system reset before restarting execution". More than a bare INIT
  pulse.
- **HALT / ENABLE switch** (two-position) — HALT halts the processor (into the
  console/ODT state), ENABLE lets it run. It reflects the **live run state** and
  sets it — this is the single halt/continue control (no separate halt and
  continue buttons). Maps to QBone halt / continue. During a brief unknown or
  transitional moment it **holds its last-known position** rather than flickering.
- **DC ON/OFF switch** — a real toggle with a **persistent off state**: OFF leaves
  the emulated machine powered down (RUN and PWR OK dark) until switched ON. An
  OFF→ON transition powers it up.

The two LEDs are the bus/run display moved into the control section; the
HALT/ENABLE switch is the single run-state toggle.

### Front panel — activity LEDs and DIP switches

- The activity LEDs and DIP switches stay as a **separate element** beside the
  11/03-styled control panel, forming a **fixed-size grid**, tightened and
  visually improved, in the same LED visual language as the control panel.
- The **DIP switches are the QUniBone cape's physical switches**; the panel only
  **shows their current state**. No function is assigned to them — the user may
  use them as they wish — so the dashboard reads and displays their position and
  nothing more.

### Disk widgets

- The RL02 widget, and the other disk widget, gain a **verbal status** with these
  meanings:
  - `off` — device disabled
  - `idle` — enabled but no image
  - `loaded` — image attached, drive coming online
  - `ready` — online and mounted, available for I/O
  - `busy` — actively transferring
- **Any disk drive's image can be selected from its widget**, without leaving the
  dashboard. The widget opens the **same image-assignment picker** used on the
  configuration-management screen
  ([web-config-management.md](web-config-management.md)), for consistency.
- No **fixed/removable chip** on the disk widgets.

### Serial mux widget

- Adding a serial mux ([serial-ports.md](serial-ports.md)) adds a dashboard
  widget: an additional terminal with **tabs, one per port of the mux**.

### LED rendering

- **All LEDs use the same rendering** — the status tiles in the top-right, the
  control-panel LEDs, and the front-panel LEDs render the same way.
- Slightly **larger** LEDs.
- **Less pronounced glow**.
- **Real-LED look throughout** — realistic physical LED colors (the reddish/amber
  of period DEC panels, matching the 11/03 photo), regardless of what each LED
  signals.

### Chrome

- The "QBUS web interface" caption in the lower-left corner is removed.

## Decisions

- **Control panel modelled on the PDP-11/03 console** — PWR OK / RUN LEDs and
  RESTART / HALT-ENABLE / DC-ON-OFF switches, mapped to QBone's init, halt,
  continue, and powercycle actions. (Reference: the 11/03 bezel photo.)
- **DIP switches display-only**, kept as a separate grid beside the control panel.
- **Any disk drive** can have its image selected from its widget.
- **RESTART = reset + restart from boot** (INIT then resume from the power-up
  vector).
- **DC ON/OFF has a persistent off state** (the machine can sit powered down).
- **Disk verbal status** uses the five-state mapping above
  (off/idle/loaded/ready/busy).
- **Real-LED look throughout** for all LEDs.
- **DIP switches show the cape's physical switch state** only, no assigned
  function.
- **HALT/ENABLE holds last-known** during transitional moments.

## Open questions

- The persistent power-off state needs QBone to hold an unpowered machine — does
  the emulator support that today, or is it new backend work? What is dark/frozen
  while off (console, device widgets, activity LEDs)?
- Reading the cape DIP switch state for display — is it already exposed (a GPIO
  read), or does that path need adding?
- Disk verbal status: which device signals distinguish `loaded` (coming online)
  from `ready` (available) — is there a spin-up/online state QBone exposes, or is
  `loaded` momentary? (Non-spinning drives may never show `loaded`.)
- VCB01 keyboard reliability is tracked separately in [vcb01.md](vcb01.md).
