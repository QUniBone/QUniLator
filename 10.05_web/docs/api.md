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
    ...],
  "statusparams": [
    {"name": "activityled", "shortname": "al", "type": "unsigned",
     "value": 0, "base": 10, "bitwidth": 8,
     "readonly": false, "info": "Number of LED to used for activity display."},
    ...]}]
```

A device's parameters come in two collections, split by what the value
represents:

- `params` — the configuration an operator sets and a saved configuration
  captures (address, image, vector, baud rate, mode, …).
- `statusparams` — running state the emulator drives on its own as the machine
  runs: panel lamps, activity LEDs, a drive's state machine and rotation,
  network counters, mirrored registers. Read-only for display; never part of a
  configuration snapshot or the `modified` comparison.

The split is by kind, not by writability: a status value can still be writable
(a settable activity-LED index) yet stays in `statusparams`, so it never leaks
into the configuration. Both collections use the same entry shape. Parameter
`type` is one of `string`, `bool`, `unsigned`, `unsigned64`, `double`. Unsigned
parameters carry `base` (usually 8) and `bitwidth`. Drives reference their
controller through `parent`.

Disk drives (category `disk`) additionally carry `removable`, `locked`, and a
computed, read-only `status` string — the drive's verbal runtime state (distinct
from the `statusparams` collection), one of:

| value | meaning |
|---|---|
| `off` | device disabled |
| `idle` | enabled, no medium attached |
| `spinning up` | RL pack coming up to speed / loading heads |
| `spinning down` | RL pack unloading heads / stopping |
| `loaded` | medium present, drive coming online (seek / load), not spinning |
| `ready` | online, ready for I/O |
| `busy` | actively transferring |

It is derived per drive type from the parameters the drive already exposes
(`enabled`, `image`, the drive state machine, the ready lamp, the access lamp),
so the dashboard and the MCP server read one field and never drift. Drives with
no modelled spin-up (RK05, RX, MSCP/RA81) report `ready` as soon as a medium is
present; the RL01/RL02 pass through `loaded` while the pack spins up and reach
`ready` at lock-on. Non-disk devices omit the field.

A device that plugs into the bus (a `qunibusdevice`) also carries
`address_options` and `vector_options` — the standard `base_addr` and
`intr_vector` values for its type (raw numbers, ascending), gathered from the
construction defaults of every instance of that type. The config editor offers
these as the address/interrupt menus rather than a free octal field. A type
with a single instance yields a single-entry list.

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

A device's **bus placement** — `base_addr`, `intr_vector`, `intr_level`,
`slot` — is locked while the device is installed, since moving it re-registers
the device on the bus. Changing one on an enabled device is refused with `409`
unless the CPU is halted; when halted the device is unplugged, re-jumpered, and
re-plugged at the new placement. A new `base_addr` that would overlap another
enabled device's register window is refused with `409` (its owner is named)
rather than colliding. On a disabled device these fields are freely settable.

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
`$QUNIBONE_DIR/configs/<name>.json`. It may also carry a `title`, an
operator-friendly label that reads more naturally than the file name; the name
stays the identity used to address and rename the configuration. The title is
file metadata, so it survives a save whose document names only devices.

The running machine always represents one named configuration, the **current**
one. It is updated whenever a configuration is applied or the live setup is
saved under a name. The machine is **modified** when the live device set differs
from the saved form of the current configuration; this is computed by
comparison, not tracked. Configurations capture the device set only — the
console bridge and other board settings stay separate, so switching
configurations never disturbs them. A device's `verbosity` (log level) is
likewise not part of a snapshot; it belongs to the board's logging settings (see
[Logging](#logging)).

At **power-on** the configuration is chosen by the board's four DIP switches
(read as a value 0..15): the one whose `dip_value` matches the switches is
applied. When no configuration claims that value the bundled empty configuration
is applied, leaving the machine passive on the bus. The selection runs at
service startup and again on a power cycle or `dc_on` (see
[`POST /api/control`](#post-apicontrol)), so changing the switches and cycling
power switches machines. A configuration binds itself to a value with
[`PUT /api/configs/<name>/dip`](#put-apiconfigsnamedip); at most one may claim a
given value.

### `GET /api/configs`

```json
{"current": "rt11", "modified": false,
 "configs": [{"name": "rt11", "title": "RT-11 bench", "mtime": "2026-07-16 20:52",
              "enabled": ["RL11", "rl0"], "dip_value": 3}]}
```

Each entry's `title` is the operator label; it falls back to the `name` when the
configuration stores none. `dip_value` is the DIP setting that selects the
configuration at power-on, or `-1` when it binds to none.

`modified` is the live dirty state of the current configuration. It is omitted
(the list still returns `200`) when the busy machine blocks the comparison.

### `GET /api/configs?current=1`

The live setup in the same shape a saved snapshot has, so a caller can compare
it against the saved ones and tell which — if any — is the configuration
currently loaded. Answers `503` while the machine is busy, the snapshot being a
status query that gives up rather than waiting on the device registry.

### `GET /api/configs/<name>`

The full snapshot. `title`, `dip_value` and `layout` are present only when the
configuration stores them:

```json
{"title": "RT-11 bench", "dip_value": 3,
 "layout": {"console": {"x": 0, "y": 6}},
 "devices": [{"name": "RL11", "enabled": true,
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

Renames the file. If `<name>` is the current one, that pointer follows; the DIP
binding travels with the file. Refused with `409` when the target name already
exists or is invalid. The live device set is untouched, so a machine modified
against `<name>` stays modified against the new name. Answers `{"ok": true}`.

### `PUT /api/configs/<name>/title`

```json
{"value": "<title>"}
```

Sets the configuration's operator-friendly title, writing the file directly. It
is metadata only: the current pointer and the running machine are untouched, so
the machine's `modified` state does not change. An empty value clears the title
back to the name. A title over 128 characters is refused with `422`. Answers
`{"ok": true, "title": "<effective title>"}`; `404` for an unknown
configuration.

### `PUT /api/configs/<name>/dip`

```json
{"value": 3}
```

Binds `<name>` to a DIP-switch value (`0`..`15`), so the board loads it at
power-on when the switches read that value. `null` clears the binding. It is
file metadata: the current pointer and the running machine are untouched. At
most one configuration may claim a value — one another configuration already
holds is refused with `409`; a value outside `0`..`15` with `422`. Answers
`{"ok": true, "dip_value": <value or -1>}`; `404` for an unknown configuration.

### `PUT /api/configs/<name>/layout`

```json
{"value": {"controlpanel": {"x": 0, "y": 0}, "rl0": {"x": 6, "y": 0, "hidden": true}}}
```

Stores the dashboard arrangement for `<name>`: a map from card key (a device
name, or `controlpanel` / `frontpanel` / `console`) to its top-left grid cell
`{x, y}` and optional `hidden`. It is per-configuration metadata — switching
configurations switches the dashboard layout — and is opaque to the backend,
which neither validates nor interprets it. The current pointer and the running
machine are untouched. A `null` value clears the layout. Answers `{"ok": true}`;
`404` for an unknown configuration.

### `DELETE /api/configs/<name>`

Removes the snapshot. Refused with `409` when `<name>` is the current
configuration — switch the current away first.

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

### `GET /api/log?before=<id>&limit=<n>`

A page of the persisted log journal, **newest first**. Every log line is
appended to `$QUNIBONE_DIR/log.jsonl` with a monotonic `id` and a server
timestamp, so the diagnostics view reloads its history and pages older entries
in — the log survives a page reload and a service restart. The file is bounded
(trimmed to the most recent ~20000 lines at startup).

```json
{"entries": [{"id": 4210, "time": "20:52:04", "level": 4,
              "label": "web", "text": "configuration \"rt11\" applied"}],
 "more": true}
```

Entries are newest-first. `before` returns entries with a smaller `id` (omit for
the latest); `limit` defaults to 200, max 1000. `more` is true when older
entries remain to page in. Live lines continue to arrive on `/ws/events` with
the same `id`/`time`, so a client merges the two by `id`.

## WebSockets

### `/ws/events`

Text frames, one JSON event each, pushed to every connected client:

| event | payload |
|---|---|
| `{"t":"param","dev":…,"param":…,"value":…}` | committed parameter change (includes enable/disable, image attach, panel lamps) |
| `{"t":"log","id":n,"time":…,"level":n,"label":…,"text":…}` | log message; levels 1 FATAL … 5 DEBUG. `id` and `time` (server clock) match the journal ([`GET /api/log`](#get-apilogbeforeidlimitn)), so a client merges live lines with a fetched page by `id` |
| `{"t":"state","halt":…,"powered":…,"leds":[…],"switches":[…],"init":…,"dcok":…,"pok":…}` | activity LEDs, DIP switches, HALT, the logical power flag, bus INIT/DCOK/POK — published on change (10 Hz poll); a full snapshot opens every connection. `powered` is the runtime power flag driven by `dc_on`/`dc_off`; the dashboard derives RUN from `!halt && powered` and PWR OK from `powered`. Transitions may arrive as partial `state` frames (e.g. `{"t":"state","powered":false}`), which the client merges onto the last snapshot |
| `{"t":"config","current":…,"modified":…}` | current configuration and the live modified flag — published on apply, save, rename, and whenever the modified flag flips (10 Hz poll); a snapshot opens every connection |

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

### `/ws/serial/<dev>/<line>`

Binary frames bridged to any mux or SLU serial line whose `tcp_role` is
`websocket` — the WebSocket is that line's backend, carrying its bytes in place
of a telnet or serial-port transport. `<dev>` is the device handle and `<line>`
the line index (`0` for a single-line DL11), e.g. `/ws/serial/dzv11/0`. Same
byte-transparent shape and retained-history replay as `/ws/console/<n>`. A line
that is not in `websocket` mode is refused, so a telnet or serial-port line is
never tapped. A serial line's backend is one of:

- **`listen`** — a bare port; a host program dials in (telnet / RFC2217).
- **`connect`** — `host:port`; the line dials out and reconnects on drop.
- **`websocket`** — no socket; carried over this WebSocket.
- **a serial port** (`serialport`, DL11 only) — a real `/dev/ttyS*` UART.
