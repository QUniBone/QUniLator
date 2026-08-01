# Working on QUniBone

## The board

The development QBone is `qbone` (`qbone.huebner.org`), user `hans`, with
passwordless sudo and key-based ssh. The PDP-11 it sits in is an 11/73.

## Driving it: use the web API

The emulator runs as `qbone.service` (`/usr/bin/qbone`), serving the web
interface and API on port 80. **Drive the board through that API.** The API is
documented in `10.05_web/docs/api.md` — read it rather than guessing endpoints.

Authentication is HTTP basic, any user name, password in `~/.qbone-pw` on the
workstation:

    curl -s -u ":$(cat ~/.qbone-pw)" http://qbone/api/devices

Requests without it answer `401`.

The service applies `settings.json` on startup, which is where the console
bridge and other machine settings live. Stopping it to run `qbone-demo` by hand
loses that, so the interactive menu is for hardware-level work the API does not
cover, not for ordinary device configuration.

State lives in `/var/lib/qunilator`: `images/` for disk images, `configs/` for saved
device snapshots, `settings.json` for board settings.

Useful endpoints, all under `http://qbone/api`:

| | |
|---|---|
| `GET /devices` | every device and its parameters |
| `PUT /devices/<dev>/params/<param>` | `{"value": "..."}`; `enabled` switches the device |
| `POST /control` | `{"action": "powercycle"}`, also `init`, `halt`, `continue` |
| `GET /configs`, `POST /configs/<name>/apply` | saved device snapshots |
| `GET /images`, `POST /images` | disk images |

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

**The 11/73 has an on-board console SLU, wired to the bone's `/dev/ttyS2`.**
The console is therefore real hardware, and the WebSocket that carries it is
`/ws/console/ext` — the raw tty bridge, with no emulated device behind it. Read
it with `websocat`:

    AUTH=$(printf ":%s" "$(cat ~/.qbone-pw)" | base64)
    websocat --binary -H="Authorization: Basic $AUTH" ws://qbone/ws/console/ext

The bridge is enabled by `external_console` in `PUT /api/settings`, whose
`source` is `ttys2`, `webserial` or `off`. It must stay `ttys2` on this board.

`/ws/console/0` and `/ws/console/1` tap QBone's *emulated* DL11s at 777560 and
776500 instead, which this machine does not use for its console.

**Leave DL11 disabled.** Its `serialport` parameter is `ttyS2`, so enabling it
both duplicates the CPU's own SLU at 777560 and fights the external-console
bridge for the port. `211bsd.json` lists DL11 as enabled, but applying it
silently leaves the device off because the bridge already holds `ttyS2` —
which is the only reason that configuration works.

Nothing else may hold `/dev/ttyS2`: a stray reader steals the bytes and makes
the bridge fail to open with "Another process has locked the comport".

## Building

`./crossbuild.sh` builds for the board in Docker; `-d` deploys the binary, and
`-g` builds unoptimised with debug symbols into its own object directory
(`4_deploy<suffix>_dbg`), so a debug and a release tree can live side by side.
The builder image is Debian trixie, the same distribution the appliance image
carries, and its tag is a hash of the recipe in the script, so editing the
recipe builds a new image rather than reusing the old one.

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
bundle — `unibone` is free for this, `qbone` needs asking first. The loop is:

1. `cd 10.05_web/3_frontend && npm run build`
2. deploy the `dist/` tree to `/usr/share/qunilator/frontend` on the board (see
   the deploy notes; a frontend-only swap needs no service restart)
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

The 11/73 CPU board carries no memory; a Q-bus memory card supplies it, and the
test rig uses a **2 MB card**. So the low 2 MB of the 22-bit space is backed by
that card, and the range from 2 MB up to the I/O page answers with a bus timeout
until something claims it. A different-sized card moves that boundary.

The board can supply memory itself: the **`MEM` device** (type `MSV11`) has the
PRU answer one address range out of the board's DDR, with `startaddr`/`endaddr`
naming the range. It ships disabled, and enabling it probes the range first and
refuses when anything already answers there — a range claimed over the card
would put two slaves on one cycle. `GET /api/memory/map` shows the address space
and `POST /api/memory/probe` sizes what the machine carries.

The PRU holds two such ranges. `MEM` takes one and a device that needs a window
in bus address space (the VCB01 framebuffer) takes the other, so a machine can
carry both — but two ranges may not overlap, and a device window still has to
clear whatever memory answers below it.

An **external line clock** drives BEVNT on the Q-bus, so the line-clock interrupt
(vector 100, BR 6) is present without enabling QBone's own emulated `KW11`
(`ltc_c` at 777546).
