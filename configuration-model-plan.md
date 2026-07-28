# Configuration model — implementation plan

Makes the *current configuration* a first-class object: the running machine
always represents a named configuration that can be edited, saved back, renamed,
and designated as the default. Implements the requirements in
[`docs/planning/configuration-model.md`](docs/planning/configuration-model.md).
The web UI, dashboard drive-swap, and MCP config tools all build on this, so it
is planned and built first.

## 1. What exists today

`webconfigs.cpp` already treats a configuration as a JSON snapshot of the device
set under `$QUNILATOR_DIR/configs/<name>.json` (`/var/lib/qunilator/configs` on the
board). A snapshot lists the enabled devices and, of those, only the parameters
that differ from the values captured at construction (`capture_parameter_defaults()`
at startup). The endpoints cover list, read, save (`PUT`), apply (`POST
…/apply`), delete, per-drive image editing, and `GET /api/configs?current=1`,
which renders the live setup in snapshot form so a caller can compare it against
the saved ones.

`snapshot_devices_now()` already polls both the operations and device-registry
mutexes to a deadline, answering `503` rather than wedging a worker — the
mechanism a live comparison needs.

What is missing is any notion of *which* configuration the machine represents:

- No **current configuration** pointer — "current" is inferred client-side by
  comparing `?current=1` against every saved file.
- No **default** — startup applies whatever `--config <name>` the service is
  launched with (`main_qbone_web.cpp`), not a stored designation.
- No **modified/dirty** signal, no **rename**, and delete does not protect the
  current or default.

## 2. The model

- **Current configuration** — a runtime pointer, `current_config_name`, held in
  `webconfigs.cpp`. It always names a saved configuration. It is set to the
  default at startup and updated whenever a configuration is applied or the live
  setup is saved under a name. It is not persisted: startup always re-establishes
  it from the default.
- **Default configuration** — a persisted `default_config` name in
  `settings.json`. Applied at startup. Protected from deletion.
- **Modified (dirty)** — computed by comparing the live snapshot
  (`snapshot_devices_now()`) against the saved snapshot of `current_config_name`.
  No write-time bookkeeping; the comparison is the source of truth and is cheap
  against the small device set.
- **Board settings stay separate.** A configuration captures the device set only.
  The console bridge (`external_console`), address width, and admin password
  remain in `settings.json` and are applied independently, so switching
  configurations never disturbs the `ttys2` console bridge.

## 3. Persistence and startup

- `settings.json` gains a top-level `default_config` string, read and written by
  `websettings.cpp` alongside `external_console`.
- Startup applies `default_config` instead of the `--config` argument:
  `main_qbone_web.cpp`, after the server registers (which is what locates the
  configs directory and captures parameter defaults), calls `webconfigs_apply()`
  with the default and sets `current_config_name` to it. `--config` remains as an
  explicit override for bring-up and testing.
- **Initial default.** A bundled **empty configuration** (`{"devices": []}`,
  shipped in `configs/` as `default.json`) is always present. On first run, if
  `default_config` is unset or names no existing file, the service adopts this
  empty configuration as the default and records it. An empty config is trivially
  valid, and applying it yields a bare board — the apply path already switches
  every unnamed device off and back to defaults, so an empty device list means
  "nothing enabled".

## 4. REST API changes

Free to reshape (the web UI, MCP, and board version together). `api.md` is
updated to match.

- **`GET /api/configs`** returns an object, not a bare array:

  ```json
  {"current": "211bsd", "default": "211bsd", "modified": false,
   "configs": [{"name": "211bsd", "mtime": "…", "enabled": […],
                "default": true}, …]}
  ```

  `modified` is the live dirty state of the current configuration; it answers
  `503` only if the busy machine blocks the comparison, in which case `modified`
  is omitted and the list still returns.

- **`GET /api/configs?current=1`** — unchanged; the live setup in snapshot form,
  used for the dirty comparison and the current-config view.

- **`PUT /api/configs/<name>`** — saves the live setup under `<name>` (Save and
  Save As are one call). After a successful save, `<name>` becomes the current
  configuration, clearing the modified state.

- **`POST /api/configs/<name>/apply`** — applies the snapshot and sets the
  current configuration to `<name>`. A **Revert** is `apply(current_config_name)`;
  the client reaches it through this endpoint, no separate route.

- **`POST /api/configs/<name>/rename`** with `{"name": "<new>"}` — renames the
  file. If `<name>` is the current or default, those pointers follow. Rejects a
  name that already exists or is invalid.

- **`PUT /api/configs/<name>/default`** — designates `<name>` the default,
  writing `settings.json`.

- **`DELETE /api/configs/<name>`** — refuses (`409`) when `<name>` is the current
  configuration or the default; the caller must switch current away, or designate
  a different default, first.

- **`PUT /api/configs/<name>/devices/<device>/image`** — unchanged; per-drive
  image editing on a stored config.

## 5. Events

`/ws/events` gains a `config` event so the dashboard's configuration tile and the
management screen update live:

```json
{"t":"config","current":"211bsd","default":"211bsd","modified":true}
```

It is published when the current configuration changes (apply, save), when the
default changes, and when the modified state flips. Modified is recomputed on the
same 10 Hz poll that already drives the `state` event, comparing the live
snapshot against the current config; a flip publishes the event.

## 6. Concurrency

Reads and comparisons use the existing `snapshot_devices_now()` deadline-polled
locking, so a status query never wedges a worker. Apply and save take the
`operations_mutex` as they do now. `current_config_name` and the cached modified
flag are guarded by a small dedicated mutex; the poll reads the live snapshot
under the registry lock, then compares outside it.

## 7. Testability and CI

New API work carries automated tests (Forgejo Actions). The config logic operates
on the `device_c`/`parameter_c` abstraction, not the PRU, so the testable seam is
a **host build of the web+config layer over a synthetic device set** — a handful
of stub devices with real `parameter_c` members, no hardware. Cases:

- save captures enabled devices and only non-default params;
- apply switches off unnamed devices, resets to defaults, sets current;
- modified is false immediately after apply/save, true after a parameter change;
- rename moves the file and carries the current/default pointers;
- delete refuses the current and the default;
- default protection: deleting the default is refused until reassigned;
- startup applies the default and sets current; a missing default adopts the
  fallback.

Building this host harness (a stub `device_c` set and a way to invoke the
handlers without civetweb, or against a loopback server) is itself a task in this
plan, since it is the first automated API test and there is no harness yet.

## 8. Migration

- Existing `configs/*.json` are already device-set snapshots and work unchanged.
- The empty `default.json` is shipped with the package (installed into
  `configs/` if absent, never overwriting an operator's edits).
- `default_config` is new; on first start after the upgrade it is unset, so the
  fallback-adoption path (§3) runs once and records the empty default.
- The systemd unit's `--config` argument is dropped in favour of the stored
  default; it stays available as an override.
- `211bsd.json` lists a DL11 that never actually enables on this board; migrating
  is the moment to drop it, so the saved set matches what runs.

## 9. Out of scope (built on this later)

- **Full offline editing of a stored configuration** beyond its drive images
  (device enable/disable and parameter edits without applying) — the
  configuration-management screen builds that on this model
  ([`docs/planning/web-config-management.md`](docs/planning/web-config-management.md)).
- **UI** — the dashboard configuration tile with its modified indicator and the
  master/detail management screen
  ([`docs/planning/web-dashboard.md`](docs/planning/web-dashboard.md)).
- **MCP config tools** — apply/save/switch over the same endpoints
  ([`docs/planning/mcp-server.md`](docs/planning/mcp-server.md)).

## 10. Decisions and open questions

Resolved here:

- Modified state is **computed by comparison**, not tracked at write time.
- The default lives in **`settings.json`**; the current pointer is **runtime**,
  re-established from the default at each start.
- **Save As makes the saved name current** and clears modified.
- **Revert** is `apply(current)`.
- The bundled fallback is an **empty configuration** (`default.json`,
  `{"devices": []}`), shipped with the package.

Open:

- May a configuration be **renamed while it is the current one and modified** —
  does rename operate on the file only, leaving the live dirty state intact?
- Does **Revert** re-init devices even for parameters that only differ in
  memory, and how does it treat a device added since the last save (apply already
  switches off unnamed devices, so it is removed — confirm that is the wanted
  behaviour)?
