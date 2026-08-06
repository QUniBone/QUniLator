# Logging control — implementation plan

Makes the log level adjustable at runtime from the web UI: a global default plus
per-target overrides, for devices and non-device subsystems alike, persisted in
settings and restored on restart. Implements the requirements in
[`logging.md`](../planning/logging.md).

## 1. What exists today

The logger (`90_common/src/logger.hpp`) defines five levels — `LL_FATAL` (1),
`LL_ERROR` (2), `LL_WARNING` (3), `LL_INFO` (4), `LL_DEBUG` (5) — and a global
`default_level`, `LL_WARNING`. Every message goes through `logger->ignored()`,
which compares the message level against the **source's own level**
(`*(logsource->log_level_ptr)`) before it reaches either sink, so a level change
governs both the web event stream (`message_sink`) and the journal/file
(`file_sink`) at once.

Each log source is a `logsource_c` with a `log_label` and a `log_level`. The
logger keeps them all in one `logsources[]` vector:

- **Devices** point `log_level_ptr` at their **`verbosity` parameter**, so a
  per-device level already exists and is settable through
  `PUT /api/devices/<dev>/params/verbosity`.
- **Non-device subsystems** register their own sources — `web` and `websrv`
  (web layer), `PRU`/`PRURP`/`PRUSS`, `QUNIBUS`/`QUNAPT`/`REQ` (the PRU and bus
  layer), `GPIOS`, `DDRMEM`, `APP`, and others — each with a plain `log_level`
  member and no path to set or persist it.

What is missing: a runtime control for the **global default**, a settable and
**persistent** level for the **non-device subsystems**, an independent
persistence home so a debug level is not tied to which configuration is saved,
and an API that reads the current levels back for the UI.

## 2. The model

- **Log levels live in `settings.json`, independent of the configuration.** A new
  `log_levels` object holds the global default and per-target overrides keyed by
  `log_label`:

  ```json
  "log_levels": {
    "default": "warning",
    "sources": { "delqa": "debug", "PRU": "info" }
  }
  ```

  This is the persistent authority. It is applied at startup and re-asserted after
  every configuration apply, so switching configurations never disturbs the levels
  and a debug bump is not accidentally baked into a saved config.

- **`verbosity` leaves the configuration snapshot.** Because log levels are now
  owned by settings, the device `verbosity` parameter is excluded from config
  snapshots (`webconfigs.cpp` capture), removing the second source of truth. The
  parameter stays as the live per-device knob; its persistent value comes from
  `log_levels.sources`.

- **Everything persists.** The global default and every per-target override are
  written to settings and restored on restart.

- **Levels are the logger's five**, exposed as lowercase names
  `fatal`/`error`/`warning`/`info`/`debug` at the API, mapped to the `LL_`
  constants.

## 3. Applying and re-asserting

- `websettings.cpp` reads `log_levels` at startup, sets `logger->default_level`
  from `default`, and walks `logger->logsources[]` applying each matching
  override through `*(source->log_level_ptr)`.
- Because a config apply resets device `verbosity` params to the config's values,
  the same walk runs again at the end of `webconfigs_apply()`, so device levels
  return to what `log_levels` says. A target with no override falls to the global
  default.
- A source that appears with no stored override inherits `default`; a stored
  override for a label not currently registered is retained in settings and
  applied if that source later appears.

## 4. REST API

A dedicated logging surface, since the levels are no longer device parameters:

- **`GET /api/logging`** — the whole picture for the UI:

  ```json
  {"default": "warning",
   "sources": [
     {"label": "delqa", "level": "debug", "kind": "device"},
     {"label": "PRU",   "level": "warning", "kind": "subsystem"},
     … ]}
  ```

  The source list is `logger->logsources[]` enumerated under the logger's lock,
  each with its current effective level and whether it is a device (its
  `log_level_ptr` is a device `verbosity` param) or a subsystem. This needs a
  small accessor on `logger_c` to iterate the private vector.

- **`PUT /api/logging/default`** `{"level": "info"}` — sets `default_level`,
  applies it to every source with no explicit override, persists to settings.

- **`PUT /api/logging/sources/<label>`** `{"level": "debug"}` — sets one target's
  level (writing through its `log_level_ptr`), records the override in settings.
  `{"level": null}` clears the override back to the default.

`api.md` documents the surface. The per-device `verbosity` param stays writable
through the existing devices endpoint for compatibility, but `GET /api/logging`
is the surface the UI uses.

## 5. Events

Level changes are infrequent and operator-driven, so the UI re-reads
`GET /api/logging` after a write rather than needing a push. No `/ws/events`
addition; if live coordination between multiple open tabs is wanted later, a
`logging` event can be added on the same pattern as the `config` event.

## 6. Web UI

A logging panel (reachable from the dashboard log area / a settings view) with:

- a **global default** selector;
- a list of **targets** from `GET /api/logging`, each a level selector, grouped
  devices vs. subsystems, showing the friendly device name
  ([device-metadata.md](../planning/device-metadata.md)) for the device rows;
- an override indicator where a target differs from the default, with a clear
  (reset-to-default) action.

## 7. Testability

Host-build tests over the apply logic with a synthetic set of `logsource_c`s and
a stub settings store:

- default applied to all sources with no override;
- an override sets one source and leaves the rest at default;
- clearing an override returns a source to the default;
- a config apply that resets a device `verbosity` is followed by re-assertion, so
  the stored override wins;
- an override for a not-yet-registered label is retained and applied when the
  source appears.

Rides the host harness the configuration-model plan introduces.

## 8. Decisions

Resolved:

- Log levels persist in **`settings.json` `log_levels`**, independent of the
  configuration; `verbosity` is dropped from config snapshots to keep one source
  of truth.
- **Named target per non-device subsystem** — every registered `logsource_c` is
  addressable, not just devices.
- **Everything persists** — global default and all overrides restored on restart.
- Levels are the existing five (`fatal`…`debug`); a change governs both the web
  stream and the journal, as the single `ignored()` filter already does.

Open:

- Whether to collapse the noisier infrastructure sources (timeouts `TO`/`FTO`,
  `REQ`) under a single coarse group in the UI, or list every registered source
  verbatim — a presentation choice settled when the panel is built.
