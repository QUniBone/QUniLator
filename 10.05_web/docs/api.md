# QBone Web API

The `demo` application serves this API when started with `--web [port]`
(default port 80). All request and response bodies are JSON unless noted.
Errors use HTTP status codes with a body of `{"error": "message"}`.

With `WEBUI_PASSWORD` set in the environment, every request requires HTTP
basic auth: any user name, the password must match. Browsers replay the
credentials on the WebSocket handshakes. Unset, access is open.

## State

### `GET /api/state`

Identifies the bridge.

```json
{"platform": "QBUS", "api_version": 0}
```

## Admin password

Every request needs HTTP basic auth once a password is set - static files and
the WebSocket handshakes included. Any user name is accepted. A board with no
password answers everything, which is how a new one is reached in order to set
one.

### `GET /api/auth`

```json
{"configured": false, "source": "none", "min_length": 8}
```

`source` is `none`, `settings` for a password set through this endpoint, or
`environment` for one given as `WEBUI_PASSWORD`.

### `PUT /api/auth`

```json
{"password": "...", "current": "..."}
```

`current` is required once a password exists, and refused with 403 if it does
not match. A password shorter than `min_length` is refused with 422, as is any
attempt to change one that came from the environment. Answers `{"ok": true}`.

## Devices and parameters

Devices are the emulated hardware — controllers, drives, serial lines.
Infrastructure singletons (bus adapter, panel driver) are not exposed.

### `GET /api/devices`

Snapshot of every device and its parameters.

```json
[{"name": "uda0", "type": "RA81", "label": "MSCP disk 0 (RA81)",
  "enabled": false, "parent": "uda",
  "params": [
    {"name": "address", "shortname": "addr", "type": "unsigned",
     "value": 63944, "base": 8, "bitwidth": 18,
     "readonly": false, "info": "controller address"},
    ...]}]
```

Parameter `type` is one of `string`, `bool`, `unsigned`, `unsigned64`,
`double`. Unsigned parameters carry `base` (usually 8) and `bitwidth`.
Drives reference their controller through `parent`.

Disk drives (category `disk`) additionally carry `removable`, `locked`, and a
computed, read-only `status` — the drive's verbal runtime state, one of:

| value | meaning |
|---|---|
| `off` | device disabled |
| `idle` | enabled, no medium attached |
| `loaded` | medium present, drive still coming online (spin-up / seek / load) |
| `ready` | online, ready for I/O |
| `busy` | actively transferring |

It is derived per drive type from the parameters the drive already exposes
(`enabled`, `image`, the drive state machine, the ready lamp, the access lamp),
so the dashboard and the MCP server read one field and never drift. Drives with
no modelled spin-up (RK05, RX, MSCP/RA81) report `ready` as soon as a medium is
present; the RL01/RL02 pass through `loaded` while the pack spins up and reach
`ready` at lock-on. Non-disk devices omit the field.

`label` is a computed, read-only friendly name in the form `<role> (<code>)`,
drawn from a static per-type table keyed by `type`. Instanced drives append
their unit (`MSCP disk 0 (RA81)`); the two serial lines carry their CSR
address (`Serial line unit @777560 (DL11)`); internal devices with no DEC
code show the bare role (`Front panel`); an unrecognised type falls back to
the raw handle. The field is derived at response time and held nowhere on the
device — there is no setter and no persistence. `type` keeps carrying the raw
DEC code.

### `PUT /api/devices/<device>/params/<param>`

Sets a parameter. Device and parameter names are case-insensitive;
`<param>` accepts the full name or the short name.

```json
{"value": "174400"}
```

The value is parsed exactly as the CLI's `p <param> <value>` command
parses it; `enabled` is switched like the CLI's `en`/`dis`. Responds with
the parameter (as in the snapshot) after the write. Validation failures
respond `422` with the device's message. Attaching a disk image is a
write to the drive's `image` parameter; an empty value detaches.

## Bus control

### `POST /api/control`

```json
{"action": "init"}
```

Actions:

| action | effect |
|---|---|
| `init` | pulse bus INIT |
| `powercycle` | simulated DCOK/POK power-fail cycle |
| `restart` | reboot from the power-up vector: release the HALT line, then power cycle so the CPU restarts execution |
| `halt` / `continue` | QBUS HALT line |
| `dc_on` | logical power on: set `powered`, release HALT, then power cycle the machine up running |
| `dc_off` | logical power off: halt the CPU and clear `powered` |

The HALT line is released before any power-up and asserted after it, so a
machine brought up by `dc_on` or `restart` comes up **running** from the
power-up vector rather than halted into micro-ODT. A `dc_off` leaves the CPU
halted; the following `dc_on` clears that HALT as part of the power-up.

`dc_on`/`dc_off` drive a **runtime logical power flag**, `powered`, reported in
the `state` event. It is runtime only — a service restart comes up powered on —
and does not touch the PRU or bus. While `powered` is false the machine is
frozen and dark: `restart`, `halt`, and `continue` are refused with `409`, and
`dc_on` is the only transition back up.

## Memory

The board is bus master, so it reads and writes the machine's memory - its own
card or an emulated range - by DMA, without the CPU. Word values and addresses
are octal, as on the console. Loading a program this way and starting it from
the console is far faster and more reliable than depositing it by hand.

### `GET /api/memory?address=<octal>&count=<n>`

Reads `count` words (default 1, max 4096) from `address`.

```json
{"address": 3670016, "words": [5386, 1024, ...]}
```

`address` is echoed as a number; the word values are decimal in the JSON.

### `POST /api/memory`

```json
{"address": "1000", "words": [5386, 1024, ...]}
```

Writes the words consecutively from `address`, which is a number or an octal
string and must be even. 1..4096 words. A bus timeout answers `502`.
Answers `{"ok": true, "address": …, "count": …}`.

## Disk images

Image files live in `$QUNIBONE_DIR/images/`.

### `GET /api/images`

```json
[{"name": "rt11v53.rl02", "path": "/home/.../images/rt11v53.rl02",
  "size": 10485760, "mtime": "2026-07-16 20:51", "attached": ["rl0"]}]
```

`attached` lists the drives whose `image` parameter points at the file.

### `POST /api/images`

Multipart upload (`multipart/form-data`, one file field). The client-side
file name becomes the image name; names must be plain file names.

### `GET /api/images/<name>`

Downloads the image.

### `DELETE /api/images/<name>`

Removes the image file. Refused with `409` while the image is attached to
a drive or referenced by a saved configuration.

## Configurations

A configuration is a named JSON snapshot of the device setup — every
device's enabled state and writable parameter values — stored in
`$QUNIBONE_DIR/configs/<name>.json`.

The running machine always represents one named configuration, the **current**
one. It is set to the **default** at startup and updated whenever a
configuration is applied or the live setup is saved under a name. The machine is
**modified** when the live device set differs from the saved form of the current
configuration; this is computed by comparison, not tracked. The default is a
board setting (in `settings.json`), applied at startup and protected from
deletion. Configurations capture the device set only — the console bridge and
other board settings stay separate, so switching configurations never disturbs
them. A device's `verbosity` (log level) is likewise not part of a snapshot; it
belongs to the board's logging settings (see [Logging](#logging)).

### `GET /api/configs`

```json
{"current": "rt11", "default": "rt11", "modified": false,
 "configs": [{"name": "rt11", "mtime": "2026-07-16 20:52",
              "enabled": ["RL11", "rl0"], "default": true}]}
```

`modified` is the live dirty state of the current configuration. It is omitted
(the list still returns `200`) when the busy machine blocks the comparison.

### `GET /api/configs?current=1`

The live setup in the same shape a saved snapshot has, so a caller can compare
it against the saved ones and tell which — if any — is the configuration
currently loaded. Answers `503` while the machine is busy, the snapshot being a
status query that gives up rather than waiting on the device registry.

### `GET /api/configs/<name>`

The full snapshot:

```json
{"devices": [{"name": "RL11", "enabled": true,
              "params": {"address": "174400", ...}}, ...]}
```

### `PUT /api/configs/<name>`

Writes a configuration document to `<name>`, atomically. The body is the whole
device set in snapshot shape:

```json
{"devices": [{"name": "RL11", "enabled": true,
              "params": {"address": "174400", ...}}, ...]}
```

One endpoint stores every configuration; what differs is whether the body is the
live setup or an offline edit, signalled by the `from=live` query flag:

- **`?from=live`** — the body is the live setup being saved under `<name>` (the
  client obtains it from `GET /api/configs?current=1`). `<name>` becomes the
  current configuration, clearing `modified`. Save and Save As are this call.
- **no flag** — the body is an offline edit of a stored configuration. The file
  is written; the current pointer and the running machine are left untouched,
  even when `<name>` is the current configuration — editing the stored file is
  distinct from the live dirty state.

The document is validated against the known devices and their parameters before
it is written: a device the machine does not have, a parameter a device does not
have, or a parameter an operator may not set is refused with `422` naming the
offending device/param, and the file is left unchanged. A body that is not a
JSON object with a `devices` array answers `400`. On success, `{"ok": true}`.

### `POST /api/configs/<name>/apply`

Restores the snapshot and makes `<name>` the current configuration. Parameters
are applied in stored order (controllers before their drives), unchanged values
are skipped, rejections are collected. A **Revert** is this call with the
current name: it re-initialises the live machine to the saved device set,
dropping any device enabled since the last save.

```json
{"ok": true, "errors": []}
```

### `POST /api/configs/<name>/rename`

```json
{"name": "<new>"}
```

Renames the file. If `<name>` is the current or the default, those pointers
follow. Refused with `409` when the target name already exists or is invalid.
The live device set is untouched, so a machine modified against `<name>` stays
modified against the new name. Answers `{"ok": true}`.

### `PUT /api/configs/<name>/default`

Designates `<name>` the startup default, writing `settings.json`. Answers
`{"ok": true}`; `404` for an unknown configuration.

### `DELETE /api/configs/<name>`

Removes the snapshot. Refused with `409` when `<name>` is the current
configuration or the default — switch the current away, or designate a
different default, first.

### `PUT /api/configs/<name>/devices/<device>/image`

```json
{"value": "<image>"}
```

Sets the medium a drive in the stored configuration starts with, without
disturbing the running machine. An empty value detaches. Refused with `409`
when another drive in the same configuration already names the file.

## Logging

The log level is adjustable at runtime: a global default plus per-target
overrides, for devices and non-device subsystems alike. The levels are the
logger's five, exposed as lowercase names — `fatal`, `error`, `warning`,
`info`, `debug`. A change governs both the dashboard log stream and the
journal.

The levels are persisted in the board's `settings.json` (a `log_levels`
object), independent of the configuration: switching configurations never
disturbs them. They are applied to the logger at startup and re-asserted after
every configuration apply. A stored override for a target that is not currently
registered is retained and applied if that target later appears.

A **target** is a registered log source: a `device` (its level is the device's
`verbosity` parameter, still writable through
`PUT /api/devices/<device>/params/verbosity`) or a `subsystem` (the web layer,
the PRU and bus layer, and others, which have no device parameter).

### `GET /api/logging`

The global default and every registered target with its current level:

```json
{"default": "warning",
 "sources": [{"label": "delqa", "level": "debug", "kind": "device"},
             {"label": "PRU", "level": "warning", "kind": "subsystem"}]}
```

### `PUT /api/logging/default`

```json
{"level": "info"}
```

Sets the global default and re-levels every target with no explicit override.
Persists. `422` for an unknown level name. Answers `{"ok": true}`.

### `PUT /api/logging/sources/<label>`

```json
{"level": "debug"}
```

Overrides one target's level. `{"level": null}` clears the override back to the
global default. Persists. `422` for an unknown level name. Answers
`{"ok": true}`.

## WebSockets

### `/ws/events`

Text frames, one JSON event each, pushed to every connected client:

| event | payload |
|---|---|
| `{"t":"param","dev":…,"param":…,"value":…}` | committed parameter change (includes enable/disable, image attach, panel lamps) |
| `{"t":"log","level":n,"label":…,"text":…}` | log message; levels 1 FATAL … 5 DEBUG |
| `{"t":"state","halt":…,"powered":…,"leds":[…],"switches":[…],"init":…,"dcok":…,"pok":…}` | activity LEDs, DIP switches, HALT, the logical power flag, bus INIT/DCOK/POK — published on change (10 Hz poll); a full snapshot opens every connection. `powered` is the runtime power flag driven by `dc_on`/`dc_off`; the dashboard derives RUN from `!halt && powered` and PWR OK from `powered`. Transitions may arrive as partial `state` frames (e.g. `{"t":"state","powered":false}`), which the client merges onto the last snapshot |
| `{"t":"config","current":…,"default":…,"modified":…}` | current/default configuration and the live modified flag — published on apply, save, default change, rename, and whenever the modified flag flips (10 Hz poll); a snapshot opens every connection |

### `/ws/console/0`, `/ws/console/1`

Binary frames, byte-transparent in both directions, bridged to the DL11
SLUs: `0` is the PDP-11 console at 777560, `1` the second line at 776500.
No echo, no line discipline; terminal emulation is the client's job. The
physical UART stays attached; the WebSocket is a parallel tap.

On connect the server replays the channel's retained history — the raw bytes
the line has emitted, up to a 256 KB in-memory ring — as one or more leading
binary frames, then continues with the live stream. A client that opens
mid-session reconstructs the current screen from the replay; xterm.js repaints
from the raw bytes with no server-side screen model. The ring is per channel and
does not persist across a service restart; it is unrelated to the log journal.

### `/ws/console/ext`

Binary frames bridged to the real console SLU on `/dev/ttyS2` (the external
console bridge); no emulated device sits behind it. Same shape and same
history replay on connect as `/ws/console/<n>`.
