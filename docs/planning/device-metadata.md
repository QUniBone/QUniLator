# Device metadata (friendly names)

**Status:** Ready — implementation plan drafted in
[`device-metadata-plan.md`](../plans/device-metadata-plan.md).

Devices carry a friendly name shown in the web UI, alongside their terse
identifier.

## Current state

Devices are identified by their internal handle (`uda0`, `DL11`, `delqa`). The
web UI shows these handles directly. A newcomer has to know the hardware to read
them.

## Requirements

- Each device has a **friendly name** shown in the web UI — for example `uda0`
  presents as its real controller name (MSCP disk controller).
- The friendly name comes from a **static built-in table keyed by device type**.
  It is not user-editable. Where an instance number matters (`uda0` vs. `uda`),
  the name reflects it.
- The internal handle remains available (it names the device in the API and
  logs).

## Decisions

- **Static per-type table.** Built-in, not user-editable; instance number
  reflected in the presented name.

## Open questions

- Assemble the exact table for the current device set: `uda`/`uda0`, `DL11`,
  `KW11`, `delqa`, RL02, VCB01, and any others.
- Does the API expose the friendly name as a new field, and does anything besides
  the web UI consume it?
