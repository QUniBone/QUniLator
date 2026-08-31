# QUniLator Web API

The service - `/usr/bin/<name>`, run by `<name>.service` - serves this API on
port 80. All request and response bodies are JSON unless noted. Errors use HTTP
status codes with a body of `{"error": "message"}`.

Once an operator is set up - preparing the SD card does it, or the first-run
dialog, and `PUT /api/auth` changes it afterwards - every request requires HTTP
basic auth. **The user name is part of it**: one identity is both the operator's
account and the web login, and the right password under another name answers
`401`. A browser may carry a session cookie instead of the credentials, which is
what signing in once gets it; both ride the WebSocket handshakes. A QUniLator
nobody has set up answers everything.

## State

### `GET /api/state`

Identifies the bridge.

```json
{"platform": "QBUS", "api_version": 0}
```

### `GET /api/version`

What this QUniLator runs.

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

Every request needs HTTP basic auth once an operator is set up - static files
and the WebSocket handshakes included, and the user name is half of it: the
right password under another name answers 401. An installation nobody has set
up answers everything, which is how a new one is reached in order to set it up.

The operator is created either when the SD card is prepared (`--setup-operator`
on the emulator binary) or in the first-run dialog, and there is exactly one.
The name is an OS account as well, made beside the `qunilator` service account
with a home under `/home` and a login shell, and given the web password, so the
same pair reaches the image library over SMB, FTP and SFTP and a shell over ssh.
What it may do follows two group memberships - `qunilator` carries the image
tree, `qunilator-admin` carries sudo and the shell sshd would otherwise confine
to an SFTP session. The service account takes no login of its own. Changing the
name creates the new account and removes the old one.

### `GET /api/auth`

```json
{"configured": true, "user": "operators", "min_length": 8}
```

`configured` is false until an operator exists, and `user` is empty with it.

The answer also carries the session cookie, below — this request is what a page
renews it with, and the frontend makes it on every load and every twelve hours
thereafter.

### The session cookie

A browser holds basic credentials only as long as it cares to, and each time it
forgets them the sign-in dialog is back in the middle of somebody's work. So an
answer from `GET /api/auth` — and from `PUT /api/auth`, which invalidates what
came before it — carries

```
Set-Cookie: qunilator_session=1.<user>.<expiry>.<mac>; Path=/; Max-Age=432000;
            HttpOnly; SameSite=Lax
```

and a request carrying that cookie needs no password. The token is a claim this
QUniLator signed: the operator's name, the unix second the session ends at, and
an HMAC-SHA256 over both. Nothing in it is secret and nothing in it can be
edited.

The session lasts **five days**, and every answer from `GET /api/auth` pushes
that out again, so an interface in daily use never asks a second time and one
untouched for five days asks once. The signing key is derived from a secret in
`settings.json` — so restarting the service, or the board, leaves every session
open — and from the stored password digest, so changing either half of the
credentials closes all of them at once.

Scripts want none of this: `curl -u` and `websocat` with an `Authorization`
header authenticate on every request, exactly as before, and are handed a cookie
they are free to ignore.

### `PUT /api/auth`

```json
{"user": "operators", "password": "...", "current": "...",
 "hostname": "shed-11", "ssh_key": "ssh-ed25519 AAAA… you@workstation"}
```

Both `user` and `password` are required while nothing is set up. Afterwards, at
least one of them: a body without `user` leaves the name in force, one without
`password` keeps the password, so either half changes on its own. An empty
`user` is refused - there is no state in which this endpoint leaves an
installation without an operator.

`current` is required once an operator exists, and refused with 403 if it does
not match, which holds for a change of name as much as of password. A password
shorter than `min_length` is refused with 422, as is a user name that is not a
portable one (1 to 32 characters: a lower case letter or underscore, then lower
case letters, digits, underscores and hyphens), one this QUniLator reserves, or
one that already belongs to an account the service did not create - that last
one is adopted deliberately at the machine, with `--setup-operator <name>
--adopt-account`, rather than over the API.

`hostname` and `ssh_key` are what the first-run dialog asks for beside the
credentials, and are applied after the account exists - the key has nowhere to go
until then. Both are optional, and neither takes the credentials back if it
fails: the refusal is reported as a warning and the same settings are offered
again by their own endpoints below.

Answers `{"ok": true, "user": "operators", "warnings": []}`.

## The installation itself

The appliance around the emulator: the host name, and the key that reaches a
shell.

### `GET /api/hostname`

```json
{"hostname": "shed-11"}
```

The first label of the host name. `<name>.local`, the DNS-SD entry, the DHCP
lease and the login banner all follow it, so several QUniLators on one network are
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
interface offers and what the account answers to are the same thing. The line is
one an OpenSSH `.pub` file carries - a type, a base64 key and an optional
comment; options before the type are refused, as is a type OpenSSH does not
offer, both with 422. An installation with no operator answers 409.

Answers `{"ok": true, "user": "operators"}`.

### `GET /api/serialports`

```json
{"ports": [
   {"device": "ttyS0", "where": "debug header, J1 pins 4/5 - needs a 3.3 V TTL adapter",
    "login": true,  "kernel_console": true,  "used_by": ""},
   {"device": "ttyS1", "where": "cape connector",
    "login": true,  "kernel_console": false, "used_by": ""},
   {"device": "ttyS2", "where": "cape connector",
    "login": false, "kernel_console": false, "used_by": "DL11"}],
 "reboot_required": false}
```

The board's three UARTs. `login` says a `serial-getty` answers there, which is
what keeps the emulator off the port; `used_by` names the enabled DL11 or the
console bridge that holds it, and is empty for a free port. `kernel_console` is
the port printk writes on, read from the command line the kernel booted with.
`reboot_required` says that setting has been moved and takes effect at the next
boot.

### `PUT /api/serialports`

```json
{"logins": ["ttyS0"]}
```

Names every port that is to carry a Linux login; the rest are freed for the
emulator. An empty list is refused with 422, since one login is the way back onto
a board whose network has gone, as is a name that is not a port of this board. A
port an enabled device holds is refused a login with 409. A host with no systemd
answers 501.

The kernel console follows the first port in the list, so printk never writes
into a line an emulated device has. That is a boot setting (`console=` in
`/boot/uEnv.txt`), so the answer reports `reboot_required` rather than treating
it as done.

Answers the same body as the GET, with a `warnings` array for any unit that did
not start or stop.

## Machine settings

Settings that belong to the whole machine rather than to any one device, and are
therefore not part of a configuration snapshot: the CPU's address width, where
the real machine's console line is read from, and which bus the card drives.
`external_console` and `internal_bus` are persisted in the board's
`settings.json`; `address_width` is a live property of the bus whose boot value
comes from the launch flag.

### `GET /api/settings`

```json
{"platform": "QBUS", "address_width": 22, "internal_bus": false,
 "external_console": {"source": "ttys2", "port": "ttyS2", "baud": 38400}}
```

`platform` is `QBUS`, `UNIBUS` or `HOST`, fixed at build time and read-only.

| `external_console.source` | |
|---|---|
| `ttys2` | the BeagleBone's own UART backs the console, carried by [`/ws/console/ext`](#wsconsoleext) |
| `webserial` | a serial port on the machine running the browser backs it instead |
| `off` | nothing does, and the port is free for an emulated `DL11` to take |

`port` is a bare tty name — `rs232_c` prepends `/dev/`, matching the SLU
convention. `baud` is the line speed, and must match what the CPU's own console
line is jumpered for.

### `PUT /api/settings`

```json
{"address_width": 22, "internal_bus": false,
 "external_console": {"source": "ttys2", "port": "ttyS2", "baud": 38400}}
```

Every member is optional; only those present are applied. Answers
`{"ok": true, "warnings": [...]}`.

**`address_width`** must be 18, or 16 or 22 on QBUS — anything else is `422`.
Changing it re-bases the I/O page, so it is applied **only while the bus is
halted**. Asked for on a running machine, the request still answers `200` and the
width is left alone, with the reason in `warnings`.

**`external_console`** takes `source` (validated against the three values above;
anything else is `422`), `port` and `baud`. Applying it opens or closes the
`ttyS2` bridge, and a refusal — most often the port being held by something else
— comes back in `warnings` rather than as an error.

**`internal_bus`** chooses what the machine's peripherals are: the cards in a
real backplane, or the board's own emulated devices. It is settled when the PRU
firmware is loaded, so it is stored and **takes effect at the next start of the
service**, which the warning says.

A change that is actually applied raises a `settings` event on
[`/ws/events`](#wsevents); a PUT that changes nothing does not.

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

A parameter whose value names a **file of the image tree** says so with
`content`, which is absent for an ordinary value:

| `content` | what it names | empty value means |
|---|---|---|
| `image` | the medium a drive holds | no medium in the drive |
| `rom` | the file a PROM card's socket is programmed from | an empty socket |

The device declares this, so a caller offers the file browser for such a
parameter whatever the device called it — a drive's `image`, the MRV11-D's
`romfile`, the M9312's five `bootromN_file` sockets — rather than recognising
particular names. A `PUT` of one resolves its value against the image tree the
way a drive's medium is resolved (a subpath, `images/…`, or an absolute path
inside the tree); an absolute path outside the tree is stored unchanged, which
is what keeps a configuration naming a file under `/usr/share` working.

The M9312's `bootaddress_label` names a MACRO-11 label of one of those
listings, and a label no plugged ROM defines is a machine that comes up and
does nothing. So a `PUT` of it is checked against the code labels of the
sockets that carry a listing: a label none of them defines is refused `409`,
the message naming the labels there are to choose from, and with every socket
empty it is refused as naming nothing — program a socket first. An applied
configuration therefore sets a device's ROM sockets before the rest of its
parameters, whatever order the document lists them in.

`bootaddress_info` is the resolved power-on PC. `DISABLED` means no autoboot
was asked for; `UNRESOLVED` means a label was and the plugged ROMs no longer
define it — swapping a ROM out from under a resolved address leaves it there.

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
the `state` event. It is runtime only and does not touch the PRU or bus. A
service restart comes up with the machine **dark**: the board loads its
configuration without putting any of it on the bus (see
[Configurations](#configurations)). While `powered` is false the machine is
frozen and dark: `restart`, `halt`, and `continue` are refused with `409`, and
`dc_on` is the only transition back up.

## Performance

What the machine is doing, as rates measured over the last second: an emulated
processor's instruction rate, a drive's throughput and access count, an
Ethernet board's traffic in each direction.

Devices count what they have done and nothing else - a total, never reset,
meaningless on its own because it only says how long the board has been up. The
service samples those totals once a second and publishes the difference; the
rate is the only number here, and there is no endpoint that reports a total.

**Only an enabled device on a machine that is switched on reports.** A card the
machine carries but has not been given power is off the bus and its counters
cannot move, so it is absent rather than reported as zero - the difference
between "not running" and "running and doing nothing" is one an operator needs.
A device that stops reporting simply leaves the set; the frame is always the
whole set, so a client replaces rather than merges.

There is no rate until a metric has been sampled twice, so a device that has
just been enabled is absent for one interval. A total that falls between two
samples - an emulated processor's opcode count restarts at every HALT - drops
the interval it fell across rather than reporting the difference, which would be
a large negative rate.

### `GET /api/metrics`

```json
{"devs": [
  {"dev": "CPU34", "type": "PDP-11/34", "kind": "cpu",
   "metrics": [
     {"name": "instructions", "unit": "instruction", "label": "Instructions",
      "rate": 412345.2, "pct": 103.1, "reference": 400000}]},
  {"dev": "RL0", "type": "RL02", "kind": "disk",
   "metrics": [
     {"name": "read_bytes",  "unit": "byte",  "label": "Read",     "rate": 61440},
     {"name": "write_bytes", "unit": "byte",  "label": "Written",  "rate": 26624},
     {"name": "transfers",   "unit": "count", "label": "Accesses", "rate": 42}]},
  {"dev": "XQ0", "type": "DELQA", "kind": "network",
   "metrics": [
     {"name": "tx_frames", "unit": "count", "label": "Frames sent",     "rate": 12},
     {"name": "tx_bytes",  "unit": "byte",  "label": "Sent",            "rate": 1433},
     {"name": "rx_frames", "unit": "count", "label": "Frames received", "rate": 240},
     {"name": "rx_bytes",  "unit": "byte",  "label": "Received",        "rate": 31744}]}]}
```

| field | |
|---|---|
| `dev` | the device handle, as [`GET /api/devices`](#get-apidevices) names it |
| `type` | its type name (`PDP-11/34`, `RL02`, `DELQA`) |
| `kind` | the device's category, as `GET /api/devices` reports it: `cpu`, `disk`, `tape`, `network` |
| `name` | the metric's key, stable across versions; match on this, not on the label |
| `unit` | `instruction`, `byte` or `count`. What one count is, which is what decides how the rate reads: bytes per second, or things per second |
| `label` | a couple of words for a person, in the device's own terms. Not a key |
| `rate` | units per second over the last sampling interval, a floating-point number |
| `pct` | the rate as a percentage of what the original machine ran at. **Present only for an emulated processor**; a drive's or a board's rate is set by what the guest asks of it and has nothing to be a percentage of |
| `reference` | what `pct` is against, in units a second |

This answers with what the once-a-second poll last measured rather than
sampling on the spot: a rate exists only between two samples, and a request
arriving a millisecond after the last one has no interval to measure. Polling
this endpoint faster than once a second returns the same numbers again.

#### What the percentage means

`pct` is an emulated processor's instruction rate against an approximation of
what the model it emulates ran at: 285,000 instructions a second for a
PDP-11/20, 400,000 for a PDP-11/34, 500,000 for a VAX-11/780. Those are averages
over an ordinary instruction mix taken from the processor handbooks' timings,
and no two programs average the same instruction - the figures are a yardstick
and the panel says "about" for that reason. Each is set in one place, its
model's constructor (`cpu20.cpp`, `cpu34.cpp`, `cpuvax.cpp`), where the
derivation is written down.

A percentage over 100 is not an error: a BeagleBone executing PDP-11 opcodes can
outrun the machine it is emulating, and on a quiet guest it usually does.

## Memory

The board is bus master, so it reads and writes the machine's memory - its own
card or an emulated range - by DMA, without the CPU. Word values and addresses
are octal, as on the console. Loading a program this way and starting it from
the console is far faster and more reliable than depositing it by hand.

### `GET /api/memory?address=<octal>&count=<octal>`

Reads `count` words (default 1, max 4096) from `address`.

```json
{"address": 3670016, "words": [5386, 1024, ...]}
```

`address` is echoed as a number; the word values are decimal in the JSON.
**Both query values are parsed as octal**, `count` included — a count carrying
an 8 or a 9 is not a number here, and reads one word rather than failing.

**A word no address answered is `null`**, not an error. Walking the I/O page or
the top of a machine's memory means walking past addresses that belong to
nobody, and which ones those are is the answer rather than a failure of the
read. The run is made as one transfer, which is what a range backed by memory
costs; only a run that hit a hole is re-read a cycle per word to find out where
the holes are, so the common case pays nothing for this.

`address` must lie inside the machine's own address space — 18 bit on a UNIBUS
machine, 16, 18 or 22 on a QBUS one — and a `count` reaching past the end of it
is shortened to the words that are there, so the answer may be shorter than
asked for. A read starting past the end answers `400` naming the last address
the machine has.

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

A machine that is switched off, or one whose processor is not arbitrating,
grants the board no bus cycle at all: nothing can answer and the sweep has
nothing to measure. That answers `504` — "the board asked for the bus and was
not granted it" — and leaves what `/api/memory/map` reports from an earlier
probe alone, rather than filing the machine as one with no memory.

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

## Debugging the machine

### `GET /api/latency`, `POST /api/latency`

How long the PRU was left holding the bus. The PRU leaves RPLY asserted from
raising a device register event until the ARM acknowledges it, so a bus cycle is
stretched for exactly as long as Linux takes to schedule the bus worker — the one
place where the emulation depends on the kernel rather than on its own code.

```json
{"available": true, "count": 38817, "max_us": 810, "mean_us": 75,
 "histogram": [{"from_us": 16, "count": 13602}, {"from_us": 32, "count": 14562},
               {"from_us": 256, "count": 2382}, {"from_us": 512, "count": 50}]}
```

**The maximum is the number that matters**, not the mean: one late wakeup
stretches a QBUS cycle far enough for the processor to call it a timeout, and an
average buries it. The histogram says whether the tail is a cliff or a slope.
Buckets double — under 1 µs, 1–2, 2–4, and so on to "16 seconds or more" — and
`from_us` is a bucket's lower edge. Empty buckets are omitted.

`available` is false when the backend cannot reach PRU1's cycle counter, in which
case nothing is recorded and every field reads zero.

A passing diagnostic on an idle board says nothing about this: the tail only
appears under load and over hours. **`POST`** resets the counters and starts a
fresh measurement, then answers the same body.

### `GET /api/debug/pru`

Where the PRU's main loop is spending its passes. For one question above all:
when the emulated processor reports

    ERR cpu34] unibone_grant_interrupts(): PRU arbitration pending for >100ms
               - PRU stopped or hung?

the flag it waits on is cleared at the bottom of the PRU's arbitration worker,
so all the message says is that the worker was not reached — and there are three
different reasons for that, with three different fixes.

```json
{"available": true, "sample_ms": 50, "looping": true, "arbitrating": true,
 "loop_passes": 6294296, "loop_passes_delta": 35219,
 "arbitration_passes_delta": 33521, "master_passes_delta": 1698,
 "blocked_passes_delta": 0, "arbitration_pending": false,
 "events": {"deviceregister_signaled": 206, "deviceregister_acked": 206,
            "dma_signaled": 136, "dma_acked": 136}}
```

Answering needs two samples, so the request takes both — `sample_ms` apart — and
reports the differences. A board in this state is being looked at by somebody
with one `curl` and a problem.

The three pass counters partition every pass of the loop, so which of them moves
names the case:

| | |
|---|---|
| `looping` false | the PRU is not running its loop at all |
| `master_passes_delta` carrying the loop | stuck in a bus master cycle that never ends |
| `blocked_passes_delta` carrying the loop | held on a device register event the ARM has not acknowledged |

`blocked_passes_delta` is what is left of the loop after the other two, and the
`events` block is the other half of that picture: a count signalled and not
acknowledged is the ARM owing the PRU an answer, with the PRU holding a bus
cycle open (`deviceregister`) or a transfer's result (`dma`) until it comes. The
two should never sit apart for long.

For scale, this rig: an idle dark board turns about 56000 passes in 50 ms, one
running XXDP about 35000, and a directory listing puts some 5% of them in the
master state. Absolute `loop_passes` is free-running and wraps; only differences
mean anything.

`available` is false on a firmware built without the counters, and then `magic`
reports what was found in their place instead. `0x00000000` is a PRU that has
not reached its loop; anything else is the ARM and the PRU disagreeing about the
mailbox layout, which would make every other field here fiction.

### `GET /api/debug/cpu`

What the processor holds. One document whichever kind of processor runs the
machine; `source` says where the answer came from.

| `source` | where it came from |
|---|---|
| `emulated` | a core of the board's own (CPU20, CPU34), read where its registers lie: no bus cycle, nothing disturbed, and cheap enough to poll |
| `bus` | a processor that lays state out in the I/O page, read by DATI — a cycle per location |
| `none` | neither; `reason` says why |

**Registers are reported only while the CPU is halted**, and `available` is
false with a `reason` while it runs or sits in a WAIT. Read one at a time out of
a processor executing millions of instructions a second, they would be a set of
numbers that were never all true at once; and answering is not free, because the
emulation runs one instruction per pass of a worker taking whatever processor
time is left on the board, so a caller asking repeatedly slows the machine it is
watching. `run_state` and `cycle_count` are there either way — the second is a
rate rather than a value, and is how a caller sees the machine is getting on
without asking it anything else. This is a property of the API, not of the panel:
poll it and you will slow the machine down whatever is reading.

An emulated processor answers whether or not the machine is switched on: a dark
board still carries the cards its configuration names, its CPU is halted, and
the registers it would start from are real.

```json
{"source": "emulated", "available": true, "device": "CPU34",
 "model": "PDP-11/34", "run_state": "halted",
 "registers": [{"name": "R0", "value": 0}, …, {"name": "SP", "value": 1000},
               {"name": "PC", "value": 65036}],
 "stackpointers": [{"name": "KSP", "value": 1000}, {"name": "USP", "value": 0}],
 "psw": {"value": 224, "priority": 7, "t": false, "n": false, "z": false,
         "v": false, "c": false, "has_modes": true, "mode": "kernel",
         "previous_mode": "kernel"},
 "ir": 0, "bus_addr": 0, "bus_data": 0, "cycle_count": 0,
 "mmu": {"enabled": false, "mmr0": 0, "mmr1": 0, "mmr2": 0,
         "kernel": {"par": [0, 0, 0, 0, 0, 0, 0, 8128],
                    "pdr": [0, 0, 0, 0, 0, 0, 0, 77406]},
         "user":   {"par": [0, …], "pdr": [0, …]}},
 "powered": true, "halted": true, "addr_width": 18}
```

Every value is a number, decimal in the JSON, as elsewhere in this API.
`registers` is R0..R5, SP and PC — SP is the stack pointer of the mode the CPU
is in, and `stackpointers` carries one per mode so a reader wanting the other
stack does not have to work out from the mode which of the two SP is showing.
`psw.has_modes` is false on a model that has none: PSW\<15:12\> there are not
bits reading as "kernel", they are bits that do not exist, and `mode` and
`previous_mode` are absent. `mmu` is absent on a model without memory
management, and `run_state` is one of `halted`, `running`, `waiting`.

`mmu.kernel` and `mmu.user` are the page registers of each mode, eight per
array, page 0 first — a KT11-D holds one set per mode and relocates through the
set PSW\<15:14\> names. **Both sets are reported whatever mode the CPU is in**:
which one is in force follows from `psw.mode`, and a debugger reading a machine
that has just trapped out of user mode wants the other set as much as this one.
The PAR and the PDR of a page are separate registers at separate addresses and
are reported as two arrays rather than paired, though they are read together.
They are internal to the processor, so this is the only way to see them — the
KT11-D window in the I/O page belongs to a real 11/34, not to an emulated one.

A running processor answers with the identity and the counter alone:

```json
{"source": "emulated", "available": false, "device": "CPU34",
 "model": "PDP-11/34", "run_state": "running", "cycle_count": 19448491,
 "reason": "the processor is running: its registers change with every instruction…",
 "powered": true, "halted": false, "addr_width": 18}
```

**The general registers of a real processor are not among what `bus` reports.**
A processor decodes its own register file, and the addresses the big machines
give it (777700..777717) are one apart — not a spacing a bus master can select
between, since a DATI carries a word address and bit 0 is the byte select. So
the probe reports, by address, which locations answered a cycle and with what,
and leaves it to the reader to say which register a machine of that model keeps
there. The processor state that *is* laid out as ordinary bus words is probed by
name: the status word at 777776 and the memory management registers at
777572..777576. The window itself is walked at word spacing.

```json
{"source": "none", "available": false,
 "reason": "no processor state answered on the bus. …",
 "probe": [{"address": 262010, "name": "MMR0", "value": null},
           {"address": 262080, "value": null}, …],
 "registers": [], "powered": true, "halted": false, "addr_width": 18}
```

`probe[].value` is `null` where the cycle timed out. The addresses follow the
machine's address width, so the same points are 0777572… on an 18-bit machine
and 017777572… on a 22-bit one.

A negative probe is remembered, so a page polling this does not put a dozen
cycles that are expected to time out on a running machine's bus every second;
`?probe=1` asks again. A switched-off machine is not probed at all — nothing
would answer, and the cycles would be made against a bus with no arbitrator,
which is where a probe waits rather than fails.

Registers of a real Q-bus processor are reachable over its console micro-ODT
(halt the CPU, read `R0/`…`R7/`, `RS/`), and of a real UNIBUS machine from its
front panel. Neither is driven from here.

`probe[].info` and the disassembler's `known_addresses` come from the same map
of what an address means on a PDP-11 (`pdp11disas_address_info()`), so a bare
number is named wherever one is shown.

### `GET /api/debug/disassemble?address=<octal>&count=<n>&model=<name>`

The machine's memory read over the bus and turned back into instructions. Works
whichever kind of processor runs the machine: the board is bus master, so this
asks the processor nothing.

`count` is **decimal** — it counts instructions rather than naming a place in
the machine, and `count=10` meaning eight is a trap not worth setting. 1..200,
default 10. `address` is octal and even.

```json
{"address": 261638, "next": 261672, "model": "11/34",
 "options": "eis mmu mfps sxs mark rtt", "complete": true,
 "instructions": [
   {"address": 261638, "words": [5313, 63744], "mnemonic": "mov",
    "operands": "#174400,r1", "known": true, "available": true,
    "truncated": false,
    "known_addresses": [{"address": 63744, "info": "rl11 rlcs (control/status)"}]},
   {"address": 261652, "words": [62980], "mnemonic": "subf", "operands": "ac4,ac0",
    "known": true, "available": false, "truncated": false,
    "comment": "fp11 not on pdp-11/34"}]}
```

**Which machine it is decoded for matters.** An 11/20 has fewer instructions
than an 11/70, and a disassembler told the wrong model invents instructions the
CPU would trap on. The default is the processor the machine carries — its device
type, so an emulated 11/34 needs no saying — and `model` overrides it
(`11/34`, `1134` and `34` all name the same machine; `cpu_model_list()` has the
rest). An instruction outside the model's set is still disassembled, with
`available: false` and a `comment` saying what it would need.

`next` is where a following listing continues, which is what a "more" button
asks for. `complete` is false when memory stopped answering before the count was
met — a listing running into the end of what a card answers ends there, with
`reason` saying where.

## Hardware self-tests

The interactive program's test menus — bus latches and signals, panel and
board, the machine's memory — reachable without a shell. The service does not
run a test itself: it starts `<name>-cli --selftest <test>` as a child, and the
child takes the board claim exactly as the interactive menu does. The service
therefore yields the machine (halt, power off, device set down) before the test
touches a pin, and rebuilds it when the child exits — **the machine comes back
switched off**, like after any hand-over.

While a test runs the board is held (`held_by` in the state frame), and every
other mutating request answers `409`. The `/api/selftest` endpoints themselves
are exempt from that refusal — the hold is on for the whole run, and stopping
the run must still work.

The bus tests drive raw bus signals with the DS8641 drivers enabled. **They
belong on an empty bus**: a machine full of cards, or a live CPU, sees arbitrary
SYNC/DIN/DOUT/GRANT traffic, and the latch tests short the grant chain with the
loopback jumpers they need. **They are never run with the board in a real
machine** — only the tests the catalog marks `machine_safe` are (the panel tests
and the memory tests, which are about what the machine carries). The board
belongs in an empty, terminated backplane for the rest; the acceptance-test
procedure —
[UNIBUS](https://retrocmp.com/projects/unibone/287-unibone-acceptance-test),
[QBUS](https://retrocmp.com/projects/qbone/320-qbone-acceptance-test) — says
which backplane, which terminator and which jumpers, and the web interface links
to the one for the board's bus.

### `GET /api/selftest`

```json
{"tests": [{"id": "latch-multi", "label": "Bus latches, all at once",
  "category": "bus", "description": "The PRU exercises all 8 latch registers…",
  "warning": "Drives raw bus signals: run only on an empty bus.",
  "setup": "Fit the 5 loopback jumpers on BG4, BG5, BG6, BG7 and NPG…",
  "machine_safe": false, "unbounded": true, "default_seconds": 10}],
 "running": null,
 "last": {"test": "latch-multi", "verdict": "failed", "hint": "every error was on
   the grant lines (BG4, BG5, BG6, BG7 and NPG) and on nothing else…",
  "exit_code": 1, "started_at": 1766140000, "ended_at": 1766140010}}
```

The catalog is platform-specific (the M9302 SACK test exists only on UNIBUS, and
the latch tests name that bus's loopback jumpers — BG\*/NPG IN-OUT on UNIBUS,
IAKI-IAKO and DMGI-DMGO on QBUS). `warning` is what a run costs, `setup` what
has to be fitted before it, `machine_safe` whether it may be run with the board
in a machine rather than on the bench; each is `""`/`false` where it does not
apply. `unbounded` marks a test that loops until stopped and takes a `seconds`
bound; `default_seconds` is the suggested bound, `0` a test that ends by itself.

`hint`, on both `running` and `last`, is a likely cause the test named for
itself, or `""`. A failure shape a test recognises says so on a `HINT: ` line,
which the service lifts out of the stream as it passes — the latch tests
recognise the one that matters, every error on the grant lines and nowhere
else, which is what a board tested without its loopback jumpers does. The line
stays in the output as well, for anyone reading the run in a terminal. A hint
can appear before the verdict, and publishes a `selftest` event when it does;
the last one of a run stands.

`running` is the test in progress or `null`; `last` is the previous run's
outcome, in memory only — a service restart forgets it. `verdict` is `passed`
(exit 0 — an operator stop with no errors counts, these are endurance tests),
`failed` (exit 1, errors found), `error` (the test could not run: no memory
found, no panel fitted, no bus grant), or `aborted` (the child was killed).

### `POST /api/selftest/run`

```json
{"test": "latch-multi", "seconds": 10}
```

Answers `202` and starts the child; the run's progress is the `/ws/selftest`
stream and the `selftest` event. `seconds` bounds an unbounded test, `0` (or
omitting it on a self-bounded one) runs until stopped; omitted on an unbounded
test, the catalog's `default_seconds` applies. `400` for an unknown test, `409`
while a test already runs or while something else holds the board (the
interactive menu, a power-up's checks). On QBUS the memory tests need the
machine's address width, which the service passes from its settings; unset, the
run is refused with `409`.

### `POST /api/selftest/stop`

Answers `202` and sends the child SIGINT, which every test loop honours; a
child that has not exited five seconds later is killed and the run ends
`aborted`. `409` when nothing runs.

## Disk images

Image files live in a folder hierarchy under `$QUNILATOR_DIR/images/`. The
package seeds one folder per media type, named by DEC device mnemonic — `dl/`
(RL), `du/` (MSCP), `rx/` (RX floppy), `mu/` (TMSCP tape), `dk/` (RK05), `rf/`
(RF11/RS11) — plus `roms/` for the ROM images a PROM card is programmed from.
The operator may nest their own folders freely below.

This is where every image is, whoever mounts it: `qunilator-fetch-images` files
the sample disks here by medium, and the example command files of
`10.03_app_demo/5_applications` name the same paths the API returns.

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
that reference it. `writable` reports the file's owner write bit — cleared, the
image is a write-protected medium.

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

### `GET /api/roms` · `POST /api/roms`

The M9312 console/diagnostic and boot PROM listings the package installs under
`/usr/share/qunilator/roms` (overridable with `QUNILATOR_ROMS_DIR` for a build
tree). They are package content, not operator state — every upgrade rewrites
them — so nothing references them by path: they are offered as a **source** to
copy from, and the copy in the images tree is what a card is programmed from and
what the operator may edit.

`GET` lists what is on offer. `title` is the listing's MACRO-11 `.title` line,
which is what makes a part number recognisable; it is empty for a file that
carries none:

```json
{"roms": [{"name": "23-751A9.lst", "size": 21902,
           "title": "M9312 'DL' BOOT prom for RL11 controller"}]}
```

`POST` body `{"name": <file>, "dir": <subpath>}` copies one into the images
tree; `dir` defaults to `roms`, and its folders are made as needed. `name` is a
single path segment naming a listed file — anything else is `400`, an unknown
one `404`. Answers `{"ok": true, "path": "roms/23-751A9.lst", "size": …}`.
Refused `409` when the target file exists, so a copy the operator has since
edited is never overwritten; delete or rename it to take a fresh one.

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

Whichever configuration is selected, the board comes up **dark**: the machine is
loaded — the cards it names and the media they hold are what it carries — and
none of it is on the bus. Nothing is installed, no register window answers, and
no emulated processor takes the bus over. A board is fitted to a machine and
configured afterwards, so what it carries at power-on describes a backplane it
may no longer be in, and at boot there is nobody at the interface to be warned.
`dc_on` (see [`POST /api/control`](#post-apicontrol)) is what brings it up.

A configuration marked with
[`PUT /api/configs/<name>/autostart`](#put-apiconfigsnameautostart) switches
itself on instead. That is a standing instruction and cannot be made safe — the
board still cannot see the backplane — so it is made loud: the boot logs a
warning naming the cards it put on the bus, and raises the standing
[notice](#get-apinotice), which holds until somebody dismisses it.

### `GET /api/configs`

```json
{"current": "rt11", "modified": false,
 "configs": [{"name": "rt11", "title": "RT-11 bench", "mtime": "2026-07-16 20:52",
              "enabled": ["RL11", "rl0"], "dip_value": 3, "autostart": false}]}
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
{"ok": true, "errors": [], "warnings": []}
```

`errors` is the configuration failing to take: a parameter a device refused, a
device that does not exist. `warnings` is the opposite — the configuration
taking, and doing something the operator is owed a word about, such as putting
an emulated processor on a bus the board does not know it owns. `ok` reflects
`errors` alone, so a call can succeed and still carry warnings.

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

### `PUT /api/configs/<name>/autostart`

```json
{"value": true}
```

Whether the board switches this machine on by itself when it loads it at
power-on, instead of leaving it dark for `dc_on`. Absent or `false` is the
default, so every configuration written before this existed comes up dark. It is
file metadata: neither the current pointer nor the running machine is touched.
Answers `{"ok": true, "autostart": <bool>}`; `404` for an unknown configuration,
`400` for a value that is not a boolean.

The flag is a standing instruction, and no standing instruction can be safe: a
configuration that was right on one backplane may name cards — or a processor —
that the next one already has, and the board cannot read the backplane it was
fitted to. What the flag buys instead is that the board says what it did. It
does not travel: an import drops it, and says so in `autostart_note`.

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

## The standing notice

One thing the board did on its own that no request of the operator's would show
them. Today that is a configuration marked
[`autostart`](#put-apiconfigsnameautostart) coming up running at boot: cards, and
possibly an emulated processor, went onto a bus with nobody watching. It travels
in the `state` frame as `notice`, so it reaches a page that connects long
afterwards, and it stands until it is dismissed.

The dismissal is the point: it is the acknowledgement that a person read the
warning, and the only record of that. It is therefore a request to the board
rather than a flag in one browser.

### `GET /api/notice`

```json
{"notice": "autostarted \"rt11\" unattended: RL11 rl0 DL11"}
```

`null` when there is none.

### `POST /api/notice/dismiss`

Clears it, and answers the same body (with `notice` now `null`).

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

## Catalogues

A catalogue is a static JSON index at a URL, naming `.qcfg.zip` bundles — the
same bundles [`GET /api/configs/<name>?export=json`](#get-apiconfigsnameexportform)
and the web interface's export build: one configuration document plus
`images/<subpath>` entries for every image it names. The board subscribes to
catalogue URLs, fetches both index and bundle itself, verifies the bundle
against the index's sha256, streams the images into place — an image already
present is kept, never overwritten — and imports the document exactly the way
[`POST /api/configs/<name>/import`](#post-apiconfigsnameimport) does.

The index is the **`qunilator-catalog/1`** schema, specified in the manual's
[Catalogue format](../../docs/manual/configurations/format.md) page and
published by the project's own site at
`https://qunilator.com/catalog/v1/index.json`. The fields this reader acts
on: `schema` must be exactly `qunilator-catalog/1` (an index it does not
understand is refused, which is what lets the schema grow); each entry's `id`
obeys the configuration-name rule and prefills the import dialog; `bus` —
`qbus`, `unibus` or `any` — is checked against the board's platform;
`download.url` (absolute or relative to the index URL; `..` is not resolved),
`download.bytes` (the progress bar and the disk-space check) and
`download.sha256` (verified before anything is unpacked) describe the bundle;
the optional `images` list — images-root subpaths with sizes — is what says
which of a machine's images are already here and how much space an import
still needs. Everything else is display only.

One catalogue job runs at a time — a refresh of every subscribed index, or
the fetch-and-import of one entry. Its status is the `catalog` frame on
[`/ws/events`](#wsevents), broadcast on every change and sent to each new
connection; a service restart aborts a running job, and a retried fetch skips
the images the aborted one had already placed.

### `GET /api/catalog`

The subscribed catalogues in their configured order, each with what it
offered when it was last reachable, plus the current job:

```json
{"refreshed_at": "2026-08-31T08:43:21Z", "bus": "qbus",
 "sources": [
  {"url": "https://qunilator.com/catalog/v1/index.json",
   "ok": true, "error": "", "fetched_at": "2026-08-31T08:43:21Z",
   "index": {"schema": "qunilator-catalog/1", "name": "…", "configurations": [
     {"id": "211bsd", "…": "…",
      "imported": false, "bus_ok": true,
      "images_present": 0, "images_total": 1}]}},
  {"url": "http://…", "ok": false, "error": "the server answered 404"}],
 "job": {"state": "idle", "…": "…"}}
```

Each entry carries what only this board knows beside what the index said:
`imported` (a configuration of that name exists here), `bus_ok` (the entry's
bus matches this board), and `images_present` of `images_total` already in
the image tree. A source that stopped answering keeps its last good `index`
with `ok: false` and the error, so its listing goes stale rather than blank.

### `POST /api/catalog/refresh`

Fetches every subscribed index again. Answers `202` — the result arrives as
`catalog` frames (`refreshing`, then `idle`) — or `409` while a job runs.

### `POST /api/catalog/fetch`

```json
{"source": "https://…/v1/index.json", "entry": "211bsd", "config": "211bsd"}
```

Downloads and imports one entry under the name `config`. Refused before
anything is fetched: `409` while a job runs or when `config` is already a
configuration here, `422` for an unknown entry, a bus this board does not
drive, or a name an operator may not choose, `507` when the disk cannot hold
the bundle plus the images it still needs. Answers `202`; the job then walks
`downloading` → `verifying` → `extracting` → `importing` → `done` (or
`failed` with `error`) in the `catalog` frames, carrying byte progress for
the download and per-file progress for the extraction.

### `POST /api/catalog/cancel`

Asks the running job to stop; it ends in state `cancelled`. Images already
extracted whole are kept — they are valid media, and a retried fetch skips
them. `409` when no job runs.

### `GET /api/catalog/sources` · `PUT /api/catalog/sources`

```json
{"sources": ["https://qunilator.com/catalog/v1/index.json"]}
```

The subscription list, in the order the interface shows it — at most 32
http(s) URLs of at most 512 characters. It persists in `settings.json`; a
fresh board carries the project's own catalogue, and an emptied list stays
empty. A `PUT` also starts a refresh, unless a job is already running.

## WebSockets

### `/ws/events`

Text frames, one JSON event each, pushed to every connected client:

| event | payload |
|---|---|
| `{"t":"param","dev":…,"param":…,"value":…}` | committed parameter change (includes enable/disable, image attach, panel lamps) |
| `{"t":"status","dev":…,"status":…}` | a disk drive's verbal state — the same word [`GET /api/devices`](#get-apidevices) reports as `status`, published on change (10 Hz poll) so a state the machine reaches by itself (a pack spinning down, a transfer starting) reaches the client without a refetch |
| `{"t":"log","id":n,"time":…,"level":n,"label":…,"text":…}` | log message; levels 1 FATAL … 5 DEBUG. `id` and `time` (server clock) match the journal ([`GET /api/log`](#get-apilogbeforeidlimitn)), so a client merges live lines with a fetched page by `id` |
| `{"t":"state","halt":…,"powered":…,"leds":[…],"switches":[…],"init":…,"dcok":…,"pok":…,"held_by":…,"notice":…}` | activity LEDs, DIP switches, HALT, the logical power flag, bus INIT/DCOK/POK, and what holds the board — published on change (10 Hz poll); a full snapshot opens every connection. `powered` is the runtime power flag driven by `dc_on`/`dc_off`; the dashboard derives RUN from `!halt && powered` and PWR OK from `powered`. Transitions may arrive as partial `state` frames (e.g. `{"t":"state","powered":false}`), which the client merges onto the last snapshot. `held_by` is described below. `dcok` and `pok` are described below. `notice` is the standing notice (see [the standing notice](#the-standing-notice)), a string or `null` |
| `{"t":"config","current":…,"modified":…}` | current configuration and the live modified flag — published on apply, save, rename, and whenever the modified flag flips (10 Hz poll); a snapshot opens every connection |
| `{"t":"settings"}` | a machine setting changed. **No payload** — a client rereads [`GET /api/settings`](#get-apisettings), which is the one description of what the settings now are. This is how a page follows a change it did not make itself, and in particular how a console whose port has moved re-points itself instead of going quietly dead |
| `{"t":"metrics","devs":[…]}` | what each enabled device is doing, once a second: the same set [`GET /api/metrics`](#get-apimetrics) answers, described under [Performance](#performance). The frame is the whole set, so a client replaces rather than merges; an empty set is sent once when the last device stops reporting and then not again |
| `{"t":"update", …}` | the update status, the same object [`GET /api/update`](#get-apiupdate) answers. Published whenever the updater's status file changes (the service stats it once a second), and as a snapshot on every new connection — so a second tab, and a tab opened during an install, both know what is going on |
| `{"t":"catalog", …}` | the catalogue job's status — the same object [`GET /api/catalog`](#get-apicatalog) carries as `job`. Published on every change (byte progress included) and as a snapshot on every new connection, so a tab opened mid-download shows the bar at once |
| `{"t":"selftest","running":…,"last":…}` | the hardware self-test state, the same `running`/`last` members [`GET /api/selftest`](#get-apiselftest) answers. Published when a run starts or ends, and as a snapshot on every new connection. The run's output is not here — it streams on [`/ws/selftest`](#wsselftest) |

#### The bus power signals

`dcok` and `pok` are BDCOK and BPOK **as read off the backplane**, and they are
three-valued: `true` asserted, `false` negated, **`null` for a bus nothing is
reading**, whose signals therefore have no known state.

They are not the machine's power switch, and they do not follow it. A supply
drives these lines and every card reads them; the emulated processor and the
emulated cards never touch one. `dc_off` takes the cards out of the machine and
clears `powered` without putting an edge on the bus, so a machine switched off
through this API leaves `dcok`/`pok` exactly where the backplane holds them.
`powered` is the question "is the emulated machine switched on"; these two are
the question "what is on the bus", and they are answered apart.

What the board reads is not necessarily another supply's doing: a power cycle
drives the DCOK/POK sequence itself (see [what a power cycle
resets](#what-a-power-cycle-resets)), so afterwards the lines carry what the
board last put on them. That is still the state of the bus, which is all this
reports.

`null` is not a fault. The board reads these lines through the PRU, which
samples them once per pass of its main loop; the value is reported only while
that loop is turning, which is the same liveness
[`GET /api/debug/pru`](#get-apidebugpru) reports as `looping`. A stale sample is
a reading of an unknown moment, and publishing it as `true` would be the one
wrong answer indistinguishable from the right one.

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

### `/ws/selftest`

Binary frames carrying the running self-test child's stdout and stderr, raw.
One-way — client frames are ignored, Stop is [`POST
/api/selftest/stop`](#post-apiselfteststop). The channel retains the current
run's output and replays it on connect with the same `{"live":true}`
end-of-replay marker as the console channels, so a page opened mid-run sees the
whole run; the retained output is cleared when the next run starts. The bytes
are terminal-ish — the memory tests redraw a progress line with bare CR.

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
