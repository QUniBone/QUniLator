# Logging control

**Status:** Ready — implementation plan drafted in
[`logging-plan.md`](../plans/logging-plan.md).

The log level is controllable from the web interface.

## Current state

The logger feeds the dashboard log and the systemd journal. The verbosity is not
adjustable at runtime from the web UI.

## Requirements

- The **log level is controllable from the web interface** at runtime.
- Levels are settable **per device instance**, over a **global default** — so
  one device (e.g. the DELQA worker) can be made verbose without raising the
  noise everywhere.
- The configured levels **persist** in settings and are restored on restart.

## Decisions

- **Levels are set per device instance** (uda0, DL11, delqa, KW11, …) over a
  **global default**.
- **Persisted in settings**, restored on restart.

## Open questions

- Non-device subsystems (web server, event hub, PRU/bus layer) also log — do they
  get their own targets alongside the per-device ones, or only the global default?
- Which levels exist, and do they match what the logger already defines?
- Do per-subsystem overrides persist too, or only the global default (with
  overrides being transient debug bumps)?
- Does changing the level affect only the dashboard log stream, the journal, or
  both?
- Is the current level surfaced through the API for the UI to read back?
