# Working on QUniBone

## The board

**`build.env` names the board to work against, and it is the only place that
does.** `QUNILATOR_HOST` is an ssh destination, `<user>@<host>`, and both halves
are needed: the host is where the board answers, and the user is the account on
it. Never write a board's name into a command from memory or from an older note
in this file — read `build.env` and use what it says.

    eval "$(grep -E '^QUNILATOR_HOST=' build.env)"
    BOARD_USER=${QUNILATOR_HOST%@*}
    BOARD_HOST=${QUNILATOR_HOST#*@}

ssh goes to `$QUNILATOR_HOST` on the operator's key, which the first-run dialog
installed; no password is involved and none should be typed. Deploys
(`crossbuild.sh -d`) use exactly that.

A `.local` host name is mDNS, which does not resolve inside the tool sandbox.
Either run the command with the sandbox off or use the address it resolves to.

`build.env` also records the bus that board carries and where a deploy puts
what it built; `build.env.example` documents every setting. It is not in git —
it describes one person's board — and `crossbuild.sh` creates it from the
example on its first run and stops so it can be filled in.

## Driving it: use the web API

The emulator runs as `<name>.service` (`/usr/bin/<name>` — `qbone` or `unibone`,
whichever bus the board is), serving the web interface and API on port 80.
**Drive the board through that API.** The API is documented in
`10.05_web/docs/api.md` — read it rather than guessing endpoints.

Authentication is HTTP basic, and **the user name is part of it**: one identity
is both the BeagleBone account and the web login, so the name is the one in
`QUNILATOR_HOST` and the password is in `~/.qbone-pw` on the workstation.

    curl -s -u "$BOARD_USER:$(cat ~/.qbone-pw)" http://$BOARD_HOST/api/devices

Requests without it answer `401`, and so does the right password under another
name. Both halves are mandatory: a board carries exactly one operator, created
either when its SD card was prepared or in the first-run dialog, and a stored
password with no name beside it is not in force — such a board asks to be set
up. `<name> --setup-operator <user>` on the board (service stopped, password on
stdin) is the way in when the credentials are gone.

`~/.qbone-pw` is the web password. It is also the account password, which only
matters where no key is installed — with the key in place nothing should ask
for it.

The service applies `settings.json` on startup, which is where the console
bridge and other machine settings live. Stopping it to run `<name>-demo` by hand
loses that, so the interactive menu is for hardware-level work the API does not
cover, not for ordinary device configuration.

State lives in `/var/lib/qunilator`: `images/` for disk images, `configs/` for saved
device snapshots, `settings.json` for board settings.

Useful endpoints, all under `http://$BOARD_HOST/api`:

| | |
|---|---|
| `GET /devices` | every device and its parameters |
| `PUT /devices/<dev>/params/<param>` | `{"value": "..."}`; `enabled` switches the device |
| `POST /control` | `{"action": "powercycle"}`, also `init`, `halt`, `continue`, `dc_on`, `dc_off` |
| `GET /configs`, `POST /configs/<name>/apply` | saved device snapshots |
| `GET /images`, `POST /images` | disk images |
| `GET /notice`, `POST /notice/dismiss` | what the board did unattended, until acknowledged |

**A board that has just started serves a machine that is switched off.** The
service loads the configuration the DIP switches name without putting any of it
on the bus - no card is installed, no register window answers, no emulated
processor takes the bus over - because a board is fitted to a machine and
configured afterwards, and what it carries may describe a backplane it is no
longer in. `POST /control {"action":"dc_on"}` is what brings the machine up, and
until it has been given, `GET /devices` shows the cards the machine *carries*
rather than what is live. A configuration marked `autostart` switches itself on
at startup instead, and says so afterwards in the journal and in the standing
notice (`GET /notice`).

**Begin every test run with a power cycle.** `POST /control {"action":"powercycle"}`
re-jumpers the devices of the running configuration and drops the CPU at a clean
boot ROM entry — so a fresh boot dialog is available. It keeps whatever
configuration is loaded: the DIP switches are read only when the backend starts,
so a power cycle (or `dc_on`, the AUX ON/OFF switch) never re-selects from them.
To load the configuration the switches name, change them and restart the backend.
Do not try to resume a machine you have left `halt`ed or that has fallen into the
boot ROM's ODT (`@`) prompt; power cycle and drive the boot from the top instead.

## The console

**Send console input one character at a time, waiting for its echo before the
next.** The DL11/SLU has no receive FIFO: a burst of bytes overruns the one
RBUF register and the program sees only a few of them — "ra(0,0,0)unix" arrives
as "r,)x". This holds for every console path (emulated DL11 channels and the
external bridge alike) and for every program driving it (ODT, boot blocks, the
OS). Pace each character on the echo; where nothing echoes (a password prompt),
fall back to a delay per character.

**Which WebSocket carries the console depends on the CPU in the box**, and the
CPU is swapped from time to time. Read `GET /api/devices` and
`GET /api/settings` before assuming either channel — a silent channel usually
means the console is on the other one, not that the machine is dead.

### A CPU with its own SLU — the 11/53 `qbone` carries now, and the 11/73

The console is real hardware on `/dev/ttyS2`, carried by `/ws/console/ext` — the
raw tty bridge, with no emulated device behind it:

    AUTH=$(printf "%s:%s" "$BOARD_USER" "$(cat ~/.qbone-pw)" | base64)
    websocat --binary -H="Authorization: Basic $AUTH" ws://$BOARD_HOST/ws/console/ext

`external_console.source` is `ttys2` (its other values are `webserial` and
`off`) and the baud must match what the CPU's own line is jumpered for — **38400
on the 11/53 here**. **`DL11` stays disabled**: its `serialport` is `ttyS2` too,
so enabling it both duplicates the CPU's own SLU at 777560 and fights the bridge
for the port. Asking for it anyway **answers `200` with `"value": false`** — the
bridge holds the port, the device stays off, and only the journal says so
(`Another process has locked the comport`). A configuration that lists `DL11` as
enabled on such a rig works only because the enable quietly fails.

The CPU's console connector has to be **cabled to the cape's UART2** for any of
this to reach the board; only GND, TXD and RXD are connected, so it takes a
null-modem.

### A CPU with no SLU, such as the KDJ11-A

There `/dev/ttyS2` reaches nothing and `/ws/console/ext` stays silent. The
console is QBone's **emulated `DL11`** at 777560, carried by **`/ws/console/0`**:

    node -e 'const w=new (require("ws"))("ws://qbone/ws/console/0",
      {headers:{Authorization:AUTH}}); w.on("message",d=>process.stdout.write(d))'

So `DL11` is **enabled**, with `serialport=ttyS2` (an empty serialport refuses to
enable), and `external_console.source` is **`off`** in `PUT /api/settings` so the
bridge releases the port for it.

### Telling a dead line from a wrong one

A **baud mismatch still produces characters** — `ôôôôWWô` and the like. Total
silence in every CPU state means the line is not connected.

Waiting for the machine to speak first is the slow way to find out. Assert HALT
and send CR instead: micro-ODT answers `?\r\n@`, and `R0/` then examines a
register and proves the whole path. That test works at any moment, with no boot
and no guest.

Either way, nothing else may hold `/dev/ttyS2`: a stray reader steals the bytes
and makes the bridge fail to open with "Another process has locked the comport".

`/ws/console/1` taps the second emulated DL11 at 776500, and `/ws/console/vax`
the emulated VAX-11/780's own console.

## Building

`./crossbuild.sh` builds for the board in Docker; `-d` deploys the binary, and
`-g` builds unoptimised with debug symbols into its own object directory
(`4_deploy<suffix>_dbg`), so a debug and a release tree can live side by side.
`-h` lists every option.

Which bus it builds for comes from `QUNILATOR_BUS` in `build.env` — the board
that is going to run it — and `-u`/`-q` override that for one run. An appliance
deploy installs the result as `/usr/bin/<name>` whatever bus it was built for,
so building for the wrong one and deploying is a bad afternoon; that is why the
setting is required rather than defaulted.

The builder image is Debian trixie, the same distribution the appliance image
carries, and its tag is a hash of the recipe in the script, so editing the
recipe builds a new image rather than reusing the old one.

Every run reads `build.env` and refuses to build while `QUNILATOR_HOST`,
`QUNILATOR_BUS`, `QUNILATOR_DEPLOY_MODE` or `QUNILATOR_REMOTE_DIR` has no value,
naming the ones that do not. The last line of a run says what came out:

    Built UNIBUS (unibone): 10.03_app_demo/4_deploy_u/qbone-web

`./compile.sh` is the same build **on the board**, out of a checkout there, and
it installs what it built: `qbone-web` as `/usr/bin/<name>`, `demo` as
`/usr/bin/<name>-cli` (setuid root, group `qunilator-admin`), then restarts the
service — so it costs the running machine exactly what a deploy does. `-n`
installs without the restart, `-N` builds only. Off a board — no
`<name>.service` — it builds and installs nothing.

`sudo qunilator-devkit` is what puts that checkout on a flashed board: build
prerequisites (including the TI PRU tools, without which no build on the board
can produce PRU firmware), the repository fetched into `/root` at the tag
matching the installed package, `qunibone-platform.env` written for the board's
bus, and `qunibone-platform.sh` run — which merges `5_applications_u|_q` into
`10.03_app_demo/5_applications`, links `4_deploy`, and puts a shortcut to every
example in the tree's root. `packaging/tests/devkit-test.sh` exercises all of
that against a stubbed board, so it needs neither hardware nor root.

## Deploying to the board

`./crossbuild.sh -d` sends the build to `QUNILATOR_HOST`. In `appliance` mode —
a board flashed from the release image, which is the usual one — it replaces
`/usr/bin/<name>` and **restarts `<name>.service`**, so the machine the board is
running goes down with it. `QUNILATOR_DEPLOY_FRONTEND=1` adds the web bundle,
unpacked into `/usr/share/qunilator/frontend`. It is a `build.env` setting, and
works on the command line for one run because the file leaves it unset:

    QUNILATOR_DEPLOY_FRONTEND=1 ./crossbuild.sh -d

The web root is served from disk, so the frontend by itself would need no
restart — but there is no frontend-only path: the swap happens inside the
appliance deploy, after the binary has been replaced and the service bounced.
A UI change therefore costs the running machine, and a board running something
that matters is asked about first.

ssh and scp go on the key; nothing prompts for a password. An appliance deploy
writes below `/usr` and restarts a unit, so the account needs sudo there.

## Building a release image

`packaging/build-release.sh` builds a whole card-ready release image on an
x86_64 Linux workstation: it stages the pinned Debian base image (downloading
and checksum-checking it into `dist/` when it is not there), the package, and
the sample disk images, then runs `packaging/build-image.sh`. See
`docs/distribution.md` for the options and the environment it reads.

Two linker settings that have to stay:

- **Dynamic, never `-static`.** glibc loads its name service backends with
  `dlopen()` even from a static binary, and picks up the ones belonging to the
  glibc the board runs rather than the one the binary carries. A static ARM
  binary built on glibc 2.36 dies with SIGFPE inside `getaddrinfo()` on the
  bone, which runs 2.41. The package names its libraries in `Depends`, written
  by `packaging/build-deb.sh` - `packaging/debian/control` is documentation,
  since the build uses plain `dpkg-deb` and substitutes no `${shlibs:Depends}`.
- **`-no-pie`.** The vendored `libprussdrv.a` holds no position-independent
  code, so a toolchain defaulting to PIE rejects its relocations.

## Testing the emulated CPU

`make -C 10.06_cputest/2_src -j test` runs the original DEC MAINDEC diagnostics
against both emulation cores — the KA11 (11/20) and the KD11-EA (11/34 with
KT11-D memory management). It builds with the **host** compiler and runs on the
build machine: the cores reach the world only through the `unibone_*()` functions
of `cpu_bus_adapter.h`, and `10.06_cputest/2_src/testbus.cpp` implements those on
a word array with KL11 and KW11 stubs, so no board and no bus hardware take part.
CI runs the same command, and so does `./crossbuild.sh -u` before it builds the
binary — a failing core stops the build and any deploy. A QBUS build does not
run them: a QBone drives a real CPU board and ships no emulated CPU.
`./crossbuild.sh -t` skips them when iterating on something else.

Change a core and the tapes for it re-run; a stamp per (core, tape) pair keeps an
unrelated build from re-running anything. Drop a tape into `3_tapes/both`,
`3_tapes/cpu20` or `3_tapes/cpu34` and it is picked up by wildcard. The suite is
35 passes and one documented skip — see `10.06_cputest/3_tapes/README.md` for
what each tape covers and how a run is judged.

The bus the emulator drives on the board is the real thing; this suite says
nothing about it. It validates instruction and MMU behaviour only.

## Testing the web interface

**Every change to the web interface is verified in a real browser before it is
called done.** `tsc --noEmit` and `vite build` catch type and syntax errors;
they say nothing about layout, colour, focus, drag behaviour, the WebSocket
streams, or whether a widget draws at all. Only the live page does.

Drive it with the `claude-in-chrome` tools against a board serving the built
bundle — the one `build.env` names. A board that is running something for
somebody else needs asking first. The loop is:

1. `cd 10.05_web/3_frontend && npm run build`
2. `QUNILATOR_DEPLOY_FRONTEND=1 ./crossbuild.sh -d` — see "Deploying to the
   board" above for what that costs the running machine
3. navigate to the screen the change touches, exercise it, and screenshot it

A change that alters what an operator sees carries a screenshot of the new
behaviour in its report. A backend change that feeds the UI — a new status
parameter, a changed API field — counts as a web-interface change: the point is
that the pixels were seen, not that the JSON was.

If the Chrome tools are unavailable, say so and stop rather than shipping a UI
change on a type check alone.

## Warnings and diagnostics

Keep both the build and the editor clean. A change is not done while it leaves
compiler warnings or spurious language-server errors behind.

- **No compiler warnings.** Fix them at the source, not by silencing the
  compiler — e.g. bounded `snprintf(buf, sizeof buf, …)` rather than `sprintf`.
  This holds for the board build (`./crossbuild.sh`), the host tests
  (`10.05_web/tools/*`, `10.06_cputest/2_src`), and CI.
- **No spurious editor diagnostics.** The IDE's clangd has none of the makefile's
  `-I` paths or `-D` defines, so without help it reports false "file not found"
  errors (`civetweb.h`, `logger.hpp`, …) that cascade into undeclared-identifier
  noise. **`./tools/gen-compile-commands.sh`** writes `compile_commands.json` by
  asking the makefiles what they would compile each source with, which is what
  clears them; run it after adding a directory of sources or changing an include
  path. The database is generated, not committed. `.clangd` is only the fallback
  for a file no makefile names yet.
- **Vendored third-party code** under `91_3rd_party/` is upstream; prefer a
  scoped, documented handling (a per-file suppression or an upstream-safe patch)
  over editing it casually, but the goal is still a clean build.

## Hardware notes

**What answers on the bus is a property of the backplane, not of QUniLator.**
`POST /api/memory/probe` sizes what the machine carries and `GET /api/memory/map`
shows the address space; ask them rather than working from this section, which
records what the rig held when it was written.

`qbone` currently holds an **11/53 (KDJ11-D)** and no other Q-bus cards. That
card brings three things the emulation therefore need not:

- **1.5 MB of on-board memory.** The probe answers `first_invalid: 1572864,
  physical_end: 1572862`, and a booted 2.11BSD agrees from the other side with
  `phys mem = 1572864`. **No `MEM` device.**
- **Its own boot ROM.** On power-up it prints a countdown `9 8 7 6 5 4 3 2 1`
  and then the device it is booting (`DL0`, `DU0`). **No `MRV11-D`.**
- **Its own console SLU** at 777560 — see the console section above.

So a configuration on this rig is just the peripherals: `rl`+`rl0` for XXDP,
`uda`+`uda0`+`delqa` for 2.11BSD. Both are saved on the board as `xxdp` and
`211bsd`.

A **KDJ11-A** brings none of them: the probe answers `first_invalid: 0`, nothing
runs until `MEM` supplies memory, and it answers neither 17773000 nor 17777520,
which is what the emulated **`MRV11-D`** bootstrap card is for — its power-up
mode jumps to 173000, so a `restart` with that card enabled runs the bootstrap
with no console input.

A CPU board with a separate Q-bus memory card beside it behaves differently
again: with a **2 MB card** the low 2 MB of the 22-bit space is backed by it and
the range from 2 MB up to the I/O page answers with a bus timeout until something
claims it. A different-sized card moves that boundary.

The board can supply memory itself: the **`MEM` device** (type `MSV11`) has the
PRU answer one address range out of the board's DDR, with `startaddr`/`size`
naming the range and `endaddr` following from the two. It ships disabled, and
enabling it probes the range first and refuses when anything already answers
there — a range claimed over a card would put two slaves on one cycle. On a
backplane with no memory of its own, probing the empty space is slow enough to
look like a hang, which is what `probe=false` is for.

The PRU holds two such ranges. `MEM` takes one and a device that needs a window
in bus address space (the VCB01 framebuffer) takes the other, so a machine can
carry both — but two ranges may not overlap, and a device window still has to
clear whatever memory answers below it.

An **external line clock** drives BEVNT on the Q-bus, so the line-clock interrupt
(vector 100, BR 6) is present without enabling QBone's own emulated `KW11`
(`ltc_c` at 777546).
