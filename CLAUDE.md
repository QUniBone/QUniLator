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

State lives in `/var/lib/bone`: `images/` for disk images, `configs/` for saved
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
re-applies the selected config's DIP binding, re-jumpers the devices, and drops
the CPU at a clean boot ROM entry — so a fresh boot dialog is available. Do not
try to resume a machine you have left `halt`ed or that has fallen into the boot
ROM's ODT (`@`) prompt; power cycle and drive the boot from the top instead.

## The console

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

`./crossbuild.sh` builds for the board in Docker; `-d` deploys the binary.
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

## Warnings and diagnostics

Keep both the build and the editor clean. A change is not done while it leaves
compiler warnings or spurious language-server errors behind.

- **No compiler warnings.** Fix them at the source, not by silencing the
  compiler — e.g. bounded `snprintf(buf, sizeof buf, …)` rather than `sprintf`.
  This holds for the board build (`./crossbuild.sh`), the host tests
  (`10.05_web/tools/*`), and CI.
- **No spurious editor diagnostics.** The IDE's clangd has none of the makefile's
  `-I` paths or `-D` defines, so without help it reports false "file not found"
  errors (`civetweb.h`, `logger.hpp`, …) that cascade into undeclared-identifier
  noise. The committed **`.clangd`** at the repo root gives clangd the QBUS
  build's view so those clear. Keep it in sync when include paths or defines
  change; add a path there rather than leaving a real header unresolved.
- **Vendored third-party code** under `91_3rd_party/` is upstream; prefer a
  scoped, documented handling (a per-file suppression or an upstream-safe patch)
  over editing it casually, but the goal is still a clean build.

## Hardware notes

The 11/73 CPU board carries no memory; a Q-bus memory card supplies it, and the
test rig uses a **2 MB card**. So the low 2 MB of the 22-bit space is backed by
that card, and the range from 2 MB up to the I/O page is nonexistent memory that
answers with a bus timeout. QBone does not fill that range under `qbone.service`:
`emulate_memory()` runs only from the interactive device-exerciser menus, never at
service startup. An emulated device that needs a window in bus address space is
therefore clear of memory above 2 MB and collides with the card below it; a
different-sized card moves that boundary.

An **external line clock** drives BEVNT on the Q-bus, so the line-clock interrupt
(vector 100, BR 6) is present without enabling QBone's own emulated `KW11`
(`ltc_c` at 777546).
