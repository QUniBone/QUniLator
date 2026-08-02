# QBone Web API

The `demo` application serves this API when started with `--web [port]`
(default port 80). All request and response bodies are JSON unless noted.
Errors use HTTP status codes with a body of `{"error": "message"}`.

With credentials set - through `PUT /api/auth` or as `WEBUI_PASSWORD` in the
environment - every request requires HTTP basic auth. Browsers replay the
credentials on the WebSocket handshakes. With none set, access is open.

## State

### `GET /api/state`

Identifies the bridge.

```json
{"platform": "QBUS", "api_version": 0}
```

### `GET /api/version`

What this board runs.

```json
{"package": "qbone", "version": "1.13.0-1", "built": "2026-07-30T09:12:00",
 "api_version": 0}
```

`package` is the Debian package that owns the binary — `qbone` on a QBUS cape,
`unibone` on a UNIBUS one — which is also the unit, the binary under
`/usr/bin` and the apt source file. `version` is compiled in from
`packaging/debian/changelog`, the same source the package's version comes from,
so the two cannot disagree. `built` is the compile timestamp as ISO 8601 in the
build machine's local time (no zone).

The service also writes the version to `/run/qunilator/version` at startup, which
is how the updater confirms that the instance answering after an install is the
new one without authenticating.

A page compares the version it was built from with what this reports and reloads
when they differ, so an upgrade — through the interface or a hand-run
`apt upgrade` — carries every open page onto the matching bundle.

## Admin credentials

Every request needs HTTP basic auth once a password is set - static files and
the WebSocket handshakes included. A board with no password answers everything,
which is how a new one is reached in order to set some.

The user name is an account on the board as well: setting one creates it beside
the `qunilator` service account, with a home under `/home` and a login shell, and
gives it the web password, so the same pair reaches the image library over SMB,
FTP and SFTP. What it may do follows two group memberships - `qunilator` carries
the image tree, `qunilator-admin` carries sudo and the shell sshd would otherwise
confine to an SFTP session. Changing the name creates the new account and removes
the old one. While no name is set, any user name is accepted and the shares
answer to `qunilator` - which is what an installation made before the name
existed keeps doing until one is set.

### `GET /api/auth`

```json
{"configured": true, "source": "settings", "user": "operators", "min_length": 8}
```

`source` is `none`, `settings` for credentials set through this endpoint, or
`environment` for a password given as `WEBUI_PASSWORD`. `user` is the configured
name, empty when any name is accepted.

### `PUT /api/auth`

```json
{"user": "operators", "password": "...", "current": "...",
 "hostname": "shed-11", "ssh_key": "ssh-ed25519 AAAA… you@workstation"}
```

At least one of `user` and `password` is required. A body without `user` leaves
the name in force; `"user": ""` clears it, which puts the shares back on
`qunilator` and removes the operator's account. A body without `password` keeps
the password, so a name changes on its own.

`current` is required once a password exists, and refused with 403 if it does
not match - changing either half takes the password in force. A password shorter
than `min_length` is refused with 422, as is a user name that is not a portable
one (1 to 32 characters: a lower case letter or underscore, then lower case
letters, digits, underscores and hyphens), one the board reserves, or one that
already belongs to an account the service did not create. Credentials that came
from the environment refuse both halves with 422.

`hostname` and `ssh_key` are what the first-run dialog asks for beside the
credentials, and are applied after the account exists - the key has nowhere to go
until then. Both are optional, and neither takes the credentials back if it
fails: the refusal is reported as a warning and the same settings are offered
again by their own endpoints below.

Answers `{"ok": true, "user": "operators", "warnings": []}`.

## The board itself

The appliance around the emulator: what the board is called on the network, and
the key that reaches its shell.

### `GET /api/hostname`

```json
{"hostname": "shed-11"}
```

The first label of the board's name. `<name>.local`, the DNS-SD entry, the DHCP
lease and the login banner all follow it, so several boards on one network are
told apart by it rather than by the mDNS suffix boot order hands out.

### `PUT /api/hostname`

```json
{"hostname": "shed-11"}
```

Runs `qunilator-rename`, which sets the system hostname, fixes `/etc/hosts` and
restarts avahi and networkd so the new name is published. A name that is not one
DNS label - letters, digits and inner hyphens, at most 63 characters - is refused
with 422, as is one the tool will not take. A board that carries no such tool
answers 503.

Answers `{"ok": true, "hostname": "shed-11"}`.

### `GET /api/sshkey`

```json
{"user": "operators", "configured": true}
```

`user` is the account the key belongs to, empty while no user name is set.
`configured` says whether that account holds one.

### `PUT /api/sshkey`

```json
{"key": "ssh-ed25519 AAAA… you@workstation"}
```

Writes the key as the operator account's only `authorized_keys` line, so what the
interface offers and what the board answers to are the same thing. The line is
one an OpenSSH `.pub` file carries - a type, a base64 key and an optional
comment; options before the type are refused, as is a type OpenSSH does not
offer, both with 422. A board with no user name set answers 409.

Answers `{"ok": true, "user": "operators"}`.

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

`enabled` says the card is **in the machine**, not that it is answering the bus
this instant: a machine switched off at the panel still carries its cards, and
each drive still names the medium it holds (see
[what a power cycle resets](#what-a-power-cycle-resets)).

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

The DZV11 mux exposes one set of per-line signal lamps in `statusparams`, named
`<sig><line>lamp` for line `0`–`3`: `rx<n>lamp` and `tx<n>lamp` pulse with
receive/transmit traffic, `dtr<n>lamp` follows the DTR bit the guest drives, and
`cd<n>lamp` follows carrier (a connected TCP client on that line). The DZV11
carries no RTS/CTS/DSR and rings no RI, so those signals have no lamp. The
dashboard renders the four lines as a modem-signal LED panel.

A device that plugs into the bus (a `qunibusdevice`) also carries
`address_options` and `vector_options` — the standard `base_addr` and
`intr_vector` values for its type (raw numbers, ascending), gathered from the
construction defaults of every instance of that type. The config editor offers
these as the address/interrupt menus rather than a free octal field. A type
with a single instance yields a single-entry list.

`label` is a computed, read-only friendly name in the form `<code> <role>`,
drawn from a static per-type table keyed by `type`. The instance is appended
where a machine can carry more than one: a drive takes its unit number
(`RA81 disk 0`), several boards of one type take their ordinal in the registry
(`DZV11 serial mux 0`, `DL11 serial line 1`), and a machine that carries one of
something takes no number (`UDA50 disk controller`). Internal devices with no
DEC code show the bare role (`Front panel`); an unrecognised type falls back to
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

**Media follow the drive into the machine and out of it.** Attaching an image to
a disabled drive switches the drive on; a drive whose controller is off is
refused with `409` naming the controller. Setting `enabled` to `0` on a drive
takes its medium out — the `image` parameter is cleared — and on a controller
does that for every drive it carries, alongside switching those drives off. The
released files are then held by nobody, so they can be deleted and renamed
again.

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
| `powercycle` | simulated DCOK/POK power-fail cycle, and every card rebuilt with it |
| `restart` | reboot from the power-up vector: release the HALT line, then power cycle so the CPU restarts execution |
| `halt` / `continue` | QBUS HALT line |
| `dc_on` | logical power on: set `powered`, release HALT, configure the machine from what it carries, then power cycle it up running |
| `dc_off` | logical power off: halt the CPU, take the cards out of the machine and clear `powered` |

A card the machine will not take — a bus address another card answers, an image
file that will not open — stops the power-up: the bus edges are not driven, the
machine is left dark with `powered` false, and the answer is `409` naming the
card and the reason it gave. The configuration stands as it was, so the operator
changes the card and switches on again.

The HALT line is released before any power-up and asserted after it, so a
machine brought up by `dc_on` or `restart` comes up **running** from the
power-up vector rather than halted into micro-ODT. A `dc_off` leaves the CPU
halted; the following `dc_on` clears that HALT as part of the power-up.

#### What a power cycle resets

`powercycle`, `restart` and the `dc_off`/`dc_on` pair rebuild every enabled
device: each is taken out of the emulation and put back, the same teardown a
device gets when its `enabled` parameter is switched off and on again. So the
cycle drops everything a card loses when it loses its supply —

- the device's worker threads, stopped and started fresh;
- its registration on the bus, redone, with the DCLO cycle `install()` drives
  over it;
- its controller state machine, including a latched MSCP/TMSCP initialization
  step, controller flags and credits;
- a drive's mechanics: media at load point (tape) or track 0 (disk), latched
  exceptions and software write locks cleared, and the unit offline so the host
  brings it up again;
- the image file, closed and reopened, so a partially written medium is read
  afresh from disk.

The pack stays in the drive and the card stays in the machine: a drive comes
back holding the medium it held, and no route through a second configuration is
needed. `init` is not a power event — it pulses BINIT and resets registers,
leaving all of the above standing.

Two consequences an operator sees:

- **A serial mux's TCP sessions end.** A DZV11, DHV11 or DL11 line serving
  telnet or RFC2217 closes its client connection and releases its listening
  port with the rest of the teardown, and binds again as the card comes back.
  A session open across an AUX OFF/ON has to be reconnected.
- **A machine switched off reads as switched off.** While `powered` is false
  the cards are out of the emulation: lamps are dark, drives are spun down and
  units are offline. What the machine *carries* is unchanged, so
  [`GET /api/devices`](#get-apidevices) still reports each card as `enabled`
  with the medium its drive holds, and the machine still matches the
  configuration it is loaded with (`modified` stays as it was). A drive's
  computed `status` reads `off`, because that is the drive's live state.

#### Editing a machine that is switched off

What a dark machine carries is the machine: the card set and the medium in each
drive are held on the board, and `dc_on` configures the emulation from them.
That record is what the whole configuration surface reads and writes while
`powered` is false, so a dark machine is edited through the endpoints a running
one is edited through:

- `PUT /api/devices/<dev>/params/enabled` puts a card in the machine or takes it
  out. Taking a controller out takes the drives that hang off it with it.
- `PUT /api/devices/<dev>/params/image` puts a medium in a drive or takes it
  out, and a drive given a medium goes into the machine with it. The controller
  it hangs off must be in the machine already, answered `409` otherwise.
- Every other parameter is set on the device where it stands. A card out of the
  machine is on no bus, so the value is inert until the card goes in.
- [`POST /api/configs/<name>/apply`](#post-apiconfigsnameapply) loads the
  configuration into what the machine carries, leaving it dark. The devices it
  names are what the next `dc_on` brings up.

None of it reaches the emulation: no device is installed, no bus address is
claimed and no image file is opened until the machine comes up. So the checks a
running machine makes as it takes an edit — a colliding bus address, a card that
will not install — are made at power-up instead, where a card the machine
refuses refuses the power-up.

Edits made dark move `modified` the way edits to a running machine do, and a
save writes the machine as it stands. Power-down itself changes neither.

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

### `GET /api/memory/map`

The address space as the board sees it. No bus traffic: the emulated ranges are
what the board is configured to answer, and `physical_end` is what the last
probe found.

```json
{"addr_width": 22, "iopage_start": 4186112, "addr_space_bytes": 4194304,
 "cpu_reserved_start": 4055040, "memory_limit": 4055040,
 "emulated": [{"slot": "memory", "start": 1572864, "end": 4055038,
              "reads": 7482735, "writes": 391044}],
 "physical_end": 1572862, "probed_at": 1785408000, "rom_accesses": 0}
```

`slot` is `memory` for the memory card and `device` for a window a device serves
out of the board's memory (the VCB01 framebuffer). `reads` and `writes` count
the cycles the board has answered out of the range, and `rom_accesses` the
reads of I/O-page ROM cells; the counts wrap, so a reader compares against what
it saw last. `physical_end` and `probed_at` are `null` until a probe has run,
and `physical_end` is `null` on a machine whose own memory answers nothing at
all.

`cpu_reserved_start` is where the CPU module's own memory begins — on a 22-bit
machine the top 128 KB below the I/O page, which is where a KDJ11 answers its
boot ROM. The module answers it without a bus cycle, so a card placed there
takes the board's write by DMA and the CPU reads its own ROM back; the failure
appears as the ROM's RAM test stopping on a word it did not write. A card is
held below it, and `memory_limit` is the address a placement has to stay under:
`cpu_reserved_start` where there is one, `iopage_start` otherwise.
`cpu_reserved_start` is `null` on a machine that reserves nothing.

### `POST /api/memory/probe`

Sizes the memory the machine carries: DATI ascending from 0 until the bus times
out, or until the first address the board answers out of its own DDR. Answers
`{"ok": true, "first_invalid": …, "physical_end": …}` and updates what
`/api/memory/map` reports.

The board's own ranges bound the sweep because a bus probe cannot tell them from
memory the machine carries: a card placed directly above the machine's own
memory answers continuously with it, and an unbounded sweep would report the sum
— a machine that appears to fill the space already, leaving nowhere to put a
card. So `physical_end` is the machine's own memory whether or not a card is
placed over the rest.

This sweeps the whole address space and holds the bus for the length of the
sweep, so run it with the CPU halted.

### `POST /api/memory/place`

```json
{"startaddr": "10000000", "size": "2040 KB"}
```

Places the memory card: `startaddr` is a number or an octal string, `size` a
count of bytes or a count followed by `KB` or `MB`. The two describe one card
and are applied as one placement — set one parameter at a time, a card moving
from one range to another passes through placements it refuses, such as a start
address that carries the old size past the I/O page.

A card that is in the machine gives up its range and takes the new one. A
placement the machine refuses — a range something already answers, one reaching
into the I/O page, or one reaching into `cpu_reserved_start` — answers `409`
with the reason and leaves the card
where it was. Answers
`{"ok": true, "startaddr": …, "endaddr": …, "size": "…", "enabled": …}`.

### `POST /api/memory/fill`

```json
{"address": "10000000", "count": 1024, "value": 0}
```

Sets `count` words from `address`, which is a number or an octal string and must
be even. `value` defaults to 0. The whole run must lie inside one range the
board serves out of its own memory, else `409`; the words are written into that
memory directly rather than over the bus. Answers
`{"ok": true, "address": …, "count": …}`.

## Disk images

Image files live in a folder hierarchy under `$QUNILATOR_DIR/images/`. The
package seeds one folder per media type, named by DEC device mnemonic — `dl/`
(RL), `du/` (MSCP), `rx/` (RX floppy), `mu/` (TMSCP tape), `dk/` (RK05) — plus
`roms/` for the ROM images a PROM card is programmed from. The operator may nest
their own folders freely below.

Everything the API takes or returns for a specific image is its **subpath** — a
path relative to the images root, e.g. `du/2.11BSD.dsk`. A drive stores its
image as `images/<subpath>` (relative to `$QUNILATOR_DIR`), which it opens
through the working directory; the `image` parameter accepts a subpath and the
service prepends `images/`. The MRV11-D's `romfile` is resolved the same way, so
a PROM card is programmed from `roms/<name>`. A subpath may not contain `..`, a
leading `/`, or a dot-leading segment.

### `GET /api/images`

The whole tree — folders and files:

```json
{"dirs": ["dl", "du", "dl/systems"],
 "images": [
   {"name": "2.11BSD.dsk", "path": "du/2.11BSD.dsk", "dir": "du",
    "size": 1000090112, "writable": true, "mtime": "2026-07-16 20:51",
    "attached": ["uda0"], "used": [{"config": "211bsd", "device": "uda0"}]}]}
```

`path` is the subpath; `dir` its parent folder (`""` = root). `attached` lists
drives whose `image` points at the file; `used` the configuration/device pairs
that reference it. `writable` is false while the file is held read-only (an
image attached to a running machine).

### `POST /api/images?dir=<folder>`

Multipart upload (`multipart/form-data`) of one or more `file` fields. The
target folder is the `dir` query parameter (a subpath; absent/empty = root),
known before the body is parsed so it is independent of field order. Each file's
name is validated as a single path segment and written under `dir`, which is
created if missing. Answers `{"ok": true, "names": ["du/foo.dsk", …]}`.

### `POST /api/images` (JSON body)

A blank medium to write on, rather than a file to upload — the same resource, and
the `Content-Type` says which. Body
`{"name": <file name>, "dir": <subpath>, "kind": "disk"|"tape", "size": <bytes>}`;
`dir` defaults to the root and `kind` to `disk`. Answers
`{"ok": true, "name": <subpath>, "size": <bytes>}`, `409` if the file exists.

A **disk** is a sparse file of `size` bytes, so a scratch pack costs the blocks
written to it rather than its capacity (512 bytes to 4 GiB). How big a medium a
drive takes is the drive's own read-only `capacity` parameter, so a caller reads
the size off the drive it means; a controller that takes the size from the image
instead (MSCP with `useimagesize`) will accept any, which is why this is a byte
count and not a menu.

A **tape** ignores `size`: a reel at the load point with nothing on it still
carries a file mark, so the file is one SIMH marker of `0x00000000` and nothing
else — the drive reads a mark and then end of medium, rather than end of medium
straight away.

### `GET /api/images/<subpath>` · `DELETE /api/images/<subpath>`

Download, or remove the file. Delete is refused with `409` while the image is
attached to a drive or referenced by a saved configuration. The subpath may
contain slashes (`/api/images/du/2.11BSD.dsk`).

### `POST /api/move`

Body `{"from": <subpath>, "to": <subpath>}` — rename or move a file or folder
within the tree. Refused `409` if the source is attached/referenced or the
target already exists.

### `POST /api/folders` · `DELETE /api/folders/<subpath>`

Create a folder (body `{"path": <subpath>}`, parents made as needed), or remove
an **empty** folder (`409` if not empty).

### `GET /api/images/<subpath>/contents`

Read-only listing of the files inside a disk/tape image, decoded by the Python
decoders under `/usr/share/qunilator/decoders`. Returns the detected filesystem,
the volume/home-block metadata, and the file list:

```json
{"file": "rt11-rl0.img", "filesystem": "RT-11", "home": {…},
 "files": [{"name": "SWAP.SYS", "bytes": 12800, "blocks": 25, "date": "…"}, …]}
```

Recognized filesystems are **RT-11** and **Files-11 ODS-2**; anything else
(Files-11 ODS-1, XXDP, a Unix filesystem, …) reports `"filesystem": "foreign"`
or `"unknown"` with no file list rather than an error. A 256,256-byte RX01
floppy image is de-interleaved first; other images are read as linear blocks.

## Configurations

A configuration is a named JSON snapshot of the device setup — every
device's enabled state and writable parameter values — stored in
`$QUNILATOR_DIR/configs/<name>.json`. It may also carry a `title`, an
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
applied. **Setting 0 is not a slot** — it is what the switches read when nobody
has chosen a machine — and brings back *the machine that was last running*,
unsaved changes and all, from a mirror the board keeps of the live setup. The
mirror records the configuration it was derived from, so the current pointer is
restored with it: the modified flag reads as it did, and a save updates that
configuration rather than inventing a new one. A configuration therefore binds
to 1..15. When no configuration claims the setting, and when setting 0 finds no
mirror, the bundled empty configuration is applied, leaving the machine passive
on the bus. The selection runs at
service startup and nowhere else: a power cycle or `dc_on` (see
[`POST /api/control`](#post-apicontrol)) keeps whatever configuration is loaded,
so switching machines means changing the switches and restarting the service. A
configuration binds itself to a value with
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
configuration at power-on (1..15), or `-1` when it binds to none.

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

### `GET /api/configs/<name>?export=<form>`

The same document the plain `GET` returns, as a file to keep, or the setup as
commands. `Content-Disposition` names it, so a browser saves rather than
renders it.

| `export=` | | |
|---|---|---|
| `json` | `<name>.qcfg.json` | the configuration document, which is what an import reads |
| `script` | `<name>.cmd` | the device set as commands for the interactive menu: `sd <dev>`, `p <param> <val>`, then `en <dev>` for each card that is in the machine |

Carrying the media is the web interface's business, not the board's: it holds
the document and every image the configuration names in one zip, built in the
browser from this endpoint and `GET /api/images/<subpath>`.

### `POST /api/configs/<name>/import`

The body is a configuration document. `<name>` must be free — an import brings
in a machine the board did not have, and writing over one it did is what
[`PUT /api/configs/<name>`](#put-apiconfigsname) is for; an existing name is
refused `409`. The document is validated the way `PUT` validates one, so a
device the machine does not have, a parameter a device does not have, or one an
operator may not set is refused by name and nothing is written.

The **DIP binding travels but does not displace**: a `dip_value` another
configuration on this board already claims is dropped, and the answer says so —
two configurations answering one switch setting is the ambiguity the binding
exists to prevent. The title and the dashboard layout are restored as they
stand.

```json
{"ok": true, "name": "rsx-from-elsewhere",
 "note": "the DIP value 1 is claimed by \"211bsd\", so the import is unbound"}
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

Applied to a machine whose power is off, the snapshot becomes what that machine
carries and the emulation is left dark (see
[editing a machine that is switched off](#editing-a-machine-that-is-switched-off)).

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

Binds `<name>` to a DIP-switch value (`1`..`15`), so the board loads it at
power-on when the switches read that value. `null` clears the binding. It is
file metadata: the current pointer and the running machine are untouched. At
most one configuration may claim a value — one another configuration already
holds is refused with `409`; a value outside `1`..`15` with `422`. Setting 0
brings back the machine that was last running and cannot be claimed. Answers
`{"ok": true, "dip_value": <value or -1>}`; `404` for an unknown configuration.

### `PUT /api/configs/<name>/layout`

```json
{"value": {"controlpanel": {"x": 0, "y": 0},
           "rl0": {"x": 6, "y": 0, "hidden": true},
           "cpu": {"x": 0, "y": 3, "opts": {"status": false}}}}
```

Stores the dashboard presentation of `<name>`: a map from card key (a device
name, or `controlpanel` / `frontpanel` / `console`) to its top-left grid cell
`{x, y}`, an optional `hidden`, and an optional `opts` object holding the
switchable parts of that card's widget — for a CPU card, `status` carries the
live register readouts, and a card without them is shorter. It is
per-configuration display state — switching configurations switches the
dashboard — and is opaque to the backend, which neither validates nor interprets
it; the option keys belong to the widgets. The current pointer and the running
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
appended to `$QUNILATOR_DIR/log.jsonl` with a monotonic `id` and a server
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

## Self-update

The service does not run apt. `/usr/sbin/qunilator-update` does, in its own
systemd unit, because installing the emulator package restarts this very service:
a dpkg running as a child of it would be killed with it, mid-install. So the
updater owns `$QUNILATOR_DIR/updates/status.json` and writes each step to it,
and these endpoints publish that file and start the updater's units.

### `GET /api/update`

```json
{"state": "idle", "package": "qbone", "source_configured": true,
 "checked_at": "2026-07-30T09:12:00Z", "installed": "1.12.0-1",
 "candidate": "1.13.0-1", "rollback": true, "needs_repair": false,
 "dismissed": "", "os": {"count": 4, "packages": [], "held_back": [],
 "reboot_required": false}, "last": {"state": "done", "from": "1.11.0-1",
 "to": "1.12.0-1", "at": "2026-07-28T06:40:11Z"}, "error": "", "journal": []}
```

`state` is `idle`, `checking`, `ahead`, `downloading`, `installing`,
`verifying`, `os-upgrading`, `done`, `failed` or `rolled-back`. `ahead` means the
repository offers an older version than the board runs, which is what a
development board carrying a hand-built package sees.

`source_configured` is false on a board installed by hand with `dpkg -i`: it has
no apt source, so it has no self-update, and `error` says so. `rollback` is true
when a cached copy of another version is present, which
`qunilator-update --rollback` reinstalls. `needs_repair` reports a dpkg
interrupted by a power loss; the next install repairs it first. `journal` carries
the failed unit's last lines when an install did not work. `last` is the previous
install's outcome, so a page that reconnects after one is told how it went.

`dismissed` is the version an operator has told the interface to stop announcing.
It is kept in `settings.json` (`update.dismissed_version`), so it belongs to the
board rather than to one browser.

`os` is what else the board could upgrade, the emulator package excluded — see
[`POST /api/update/os`](#post-apiupdateos).

### `POST /api/update/check`

Starts `qunilator-update-check.service` and answers `202`. The result arrives as
an `update` event; nothing is installed. The same check runs from a timer five
minutes after boot and daily after that.

### `GET /api/update/changelog`

```json
{"changelog": "qbone (1.13.0-1) trixie; urgency=low\n\n  * ...\n"}
```

The candidate package's own changelog stanzas newer than the installed version —
the text that shipped, byte for byte. The updater downloads the candidate to read
it, which takes a moment; the download doubles as the staging step an install
wants anyway. `502` when the candidate could not be fetched.

### `POST /api/update/install`

```json
{"version": "1.13.0-1"}
```

Answers `202`; the install runs in `qunilator-update.service`, its own cgroup, and
reports through `update` events. The version must be the candidate the last check
recorded — anything else is refused with `422`, and `409` answers a board with no
candidate or an update already running. The version is written to a request file
for the updater to read, so no string from a request reaches a command line.

**This stops the emulated machine.** The device set is shut down in order, so the
images are flushed, but a running Unix or RSX has its buffer cache cut, and the
board comes back up in its startup configuration rather than the one it was
running. The install then waits for the new service to come up and stay up, and
reinstalls the previous version if it does not.

### `POST /api/update/os`

Upgrades the board's other packages, in `qunilator-update-os.service`. Answers
`202`, or `409` while another update runs.

**This path has no rollback** — apt cannot undo an upgrade. It holds the emulator
package back, so a running machine keeps running and is not stopped at all; it
uses `upgrade` rather than `full-upgrade`, so no package is removed to satisfy
another, and what that holds back is reported; and it never reboots the board.
`os.reboot_required` afterwards is the operator's own call.

### `POST /api/update/dismiss`

```json
{"version": "1.13.0-1"}
```

Stops announcing that version. An empty string clears the dismissal, and a later
version announces itself again.

## WebSockets

### `/ws/events`

Text frames, one JSON event each, pushed to every connected client:

| event | payload |
|---|---|
| `{"t":"param","dev":…,"param":…,"value":…}` | committed parameter change (includes enable/disable, image attach, panel lamps) |
| `{"t":"status","dev":…,"status":…}` | a disk drive's verbal state — the same word [`GET /api/devices`](#get-apidevices) reports as `status`, published on change (10 Hz poll) so a state the machine reaches by itself (a pack spinning down, a transfer starting) reaches the client without a refetch |
| `{"t":"log","id":n,"time":…,"level":n,"label":…,"text":…}` | log message; levels 1 FATAL … 5 DEBUG. `id` and `time` (server clock) match the journal ([`GET /api/log`](#get-apilogbeforeidlimitn)), so a client merges live lines with a fetched page by `id` |
| `{"t":"state","halt":…,"powered":…,"leds":[…],"switches":[…],"init":…,"dcok":…,"pok":…,"held_by":…}` | activity LEDs, DIP switches, HALT, the logical power flag, bus INIT/DCOK/POK, and what holds the board — published on change (10 Hz poll); a full snapshot opens every connection. `powered` is the runtime power flag driven by `dc_on`/`dc_off`; the dashboard derives RUN from `!halt && powered` and PWR OK from `powered`. Transitions may arrive as partial `state` frames (e.g. `{"t":"state","powered":false}`), which the client merges onto the last snapshot. `held_by` is described below |
| `{"t":"config","current":…,"modified":…}` | current configuration and the live modified flag — published on apply, save, rename, and whenever the modified flag flips (10 Hz poll); a snapshot opens every connection |
| `{"t":"update", …}` | the update status, the same object [`GET /api/update`](#get-apiupdate) answers. Published whenever the updater's status file changes (the service stats it once a second), and as a snapshot on every new connection — so a second tab, and a tab opened during an install, both know what is going on |

#### The board held

`held_by` names what has the board, and is `null` when nothing does. Two things
take it:

- a power-up, for the length of its checks — `validating configuration for power
  on`. Bringing the cards back checks each against the machine before it goes
  in, and the probing runs on the bus.
- the interactive program, for the length of a session — `<name>-cli is running;
  the web interface is unavailable until it is exited`. `<name>-cli` asks the
  service for the hardware; the service puts its machine down, hands the board
  over, and rebuilds the machine from the configuration that was current when
  the session ends.

While it is set, every request other than `GET` and `HEAD` answers `409` with
`{"error": …, "held_by": …}` naming the holder, and the state frame carries the
same string, so every connected page says the same thing about why it cannot be
used. Reads keep working. There is nothing to acknowledge: the hold clears with
a `state` frame carrying `"held_by": null`.

The reason is the whole message a page shows: it names what holds the board and
what ends the wait, which differs by holder, so a client displays it as given
rather than adding a line of its own.

Only one interactive session runs at a time, whether or not a service is there
to yield to it — a second one is refused, naming the pid that holds the board.

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

#### Control frames

Anything that is not a byte on the line travels as a **TEXT** frame holding one
small JSON object, so it can never be mistaken for terminal data. Binary frames
are always the byte stream.

Server to client:

| | |
|---|---|
| `{"live":true}` | the replayed history ends here; everything after is live |
| `{"answerer":true}` | you are the one client that answers the guest's terminal queries |

The `live` marker is sent after the replay and before the client joins the live
set, so the boundary is exact. A program driving the console anchors its pattern
matching there and never mistakes a prompt that scrolled past for the one it is
waiting for. Every channel sends it; only a console channel designates an
answerer.

Client to server:

| | |
|---|---|
| `{"break":true}` | assert a line BREAK |

BREAK is a line condition rather than a character, which is why it cannot ride
in the byte stream. On `/ws/console/ext` it holds the real tty spacing for
300 ms, ordered among the bytes already queued for the line. On an emulated DL11
(`/ws/console/0`, `/1`) the SLU reports a null data byte with a framing error,
which is what a UART gives its driver for a received BREAK. The VAX console
ignores it. A client that ignores TEXT frames is unaffected by any of this.

### Console recordings

A session someone drives by hand is recorded **on the board**, because that is
the only place both directions pass: output reaches every client, but each
client's input goes straight to the line, so no client can see what another
typed. The file is
[asciicast v3](https://docs.asciinema.org/manual/asciicast/v3/), which `qcon
render` turns into a page and an asciinema player replays.

Recording is off until asked for, and stops itself at 16 MB. Input events carry
whatever was typed, passwords included, so a recording is something an operator
starts deliberately rather than a file the board always keeps.

#### `GET /api/console/<channel>/recording`

```json
{"recording": true, "name": "console-ext-20260801-194245.cast", "bytes": 4197}
```

#### `POST /api/console/<channel>/recording`

```json
{"action": "start", "name": "install"}
```

`action` is `start` or `stop`. `name` is optional and gains a `.cast` suffix;
without one the board names the file after the channel and the time. Answers
`{"ok": true, "recording": …, "name": …}`. `404` for an unknown channel.

#### `GET /api/recordings`

```json
{"recordings": [{"name": "install.cast", "bytes": 4197,
                 "mtime": "2026-08-01 19:37"}]}
```

#### `GET /api/recordings/<name>`

The cast itself, as `text/plain`.

#### `DELETE /api/recordings/<name>`

Removes it. Answers `{"ok": true}`; `404` when there is no such recording.

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
