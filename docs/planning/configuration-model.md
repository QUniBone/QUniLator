# Configuration model

**Status:** Ready — implementation plan drafted in
[`configuration-model-plan.md`](../../configuration-model-plan.md).

The concept of a *current configuration* as a first-class object, so the running
machine's setup can be edited, saved back, named, and designated as the default.

## Current state

Configurations are saved device snapshots under `/var/lib/qunilator/configs`, applied
with `POST /api/configs/<name>/apply`. The live machine is a set of devices with
parameters; there is no notion of *which* configuration it currently represents.
"Current configuration" is inferred by comparing live devices against saved
snapshots, which cannot express an edited-but-unsaved state.

## Requirements

- There is always a **current configuration** — a named object the running
  machine represents, not something derived by comparison.
- The current configuration can be edited (devices and parameters change) and
  **saved back** under its own name.
- A configuration can be **renamed**.
- Any configuration other than the current one can be **deleted**.
- One configuration is the **designated default**, applied on startup.
- The default is **protected from deletion** — the user must designate another
  default before deleting the current one, so a valid default always exists.
- A bundled **empty configuration** (no devices) ships as the always-present
  fallback default, so a board that has never had one set still has a valid
  default to apply on startup.

## Decisions

- **Startup always applies the designated default configuration.** The current
  configuration is therefore always a named, saved config — there is no implicit
  unsaved "working" state. Edits are made against the current (named) config and
  saved back, or saved under a new name.
- **Edits are tracked as a dirty state.** When live devices diverge from the
  current config's last-saved form, the config is marked *modified*; the user
  clears it with **Save** (write back), **Save As** (new name), or **Revert**.
  Nothing auto-persists.
- **A configuration captures the device set only.** Board-level settings (the
  console bridge, `external_console=ttys2`, and other `settings.json` entries)
  stay separate and are applied independently of which configuration is current,
  so switching configurations never disturbs the `ttys2` console bridge.
- **Revert re-applies the saved form to the live machine.** It restores the
  current config's last-saved device set and re-initialises the running machine
  to match, so a device added since the save is dropped.
- **Existing config files migrate into the new model.** `211bsd.json` and the
  other saved snapshots become configurations directly; the migration is small
  and is where the DL11 that `211bsd.json` lists but never enables gets dropped.

## Open questions

- The dirty/modified state must be exposed by the API for the UI to read. Is it
  computed by comparing live devices against the saved config, or tracked
  explicitly as edits happen?
