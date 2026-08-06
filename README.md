# QUniLator

QUniLator bridges a DEC bus and Linux. It runs on a BeagleBone Black carried by
a card in a UNIBUS or QBUS backplane, and presents controllers, drives, memory,
terminal lines and network interfaces to a real PDP-11, PDP-10 or VAX as cards
that answer like the originals — device and controller together, at the bus
level, with nothing between the machine and the emulation but the backplane.

Four names run through the project: **QUniLator** is the software and a running
installation of it, **UniBone** the card that drives UNIBUS, **QBone** the card
that drives QBUS, and the **BeagleBone** the computer either card carries. One
codebase serves both buses, selected at compile time via `-DUNIBUS` / `-DQBUS`.

**The manual is at
[vaxbusters.org/qunilator](https://vaxbusters.org/qunilator/)** — what QUniLator
is, which card your machine takes, installing it, the acceptance test and the
configuration catalogue. This page is the repository's front door.

## Heritage

This is the community-maintained mainline of QUniBone, continuing the work of
**Jörg Hoppe** (retrocmp.com: [UniBone](http://retrocmp.com/projects/unibone/),
[QBone](http://retrocmp.com/projects/qbone/)). The original reference codebase
is preserved at
[`QUniBone/QUniBoneClassic`](https://github.com/QUniBone/QUniBoneClassic).
Licensed BSD 2-Clause; Jörg's copyright is retained.

## How this code is maintained

The code in this repository is written and maintained by AI agents, including
this README. The maintainer directs and reviews the work; the code itself is
largely agent-authored. This is the ongoing method, not a one-off experiment.

If you would rather not run or contribute to software produced this way, stop
here — Jörg Hoppe's original codebase is preserved at
[`QUniBone/QUniBoneClassic`](https://github.com/QUniBone/QUniBoneClassic).

## Status

**QBone (QBUS)** runs on real hardware and is exercised continuously. 2.11BSD
boots from an emulated MSCP volume, networks over the emulated DELQA and dumps
to an emulated TK50. The device models are validated against DEC's own XXDP
diagnostics — CZQNA for the DELQA, CVDZA for the DZV11, CVDHA for the DHV11,
and the RL, TS and TMSCP families among them.

**UniBone (UNIBUS)** now runs on a UniBone card. Twenty-one bring-up issues
were found and fixed against it, the emulated PDP-11/20 and 11/34 boot XXDP
from an emulated RL02, and a VAX-11/780 core boots VMS V4.7 from an emulated
UDA50 — see [`docs/unibone-bringup-issues.md`](docs/unibone-bringup-issues.md)
and [`docs/vax-host.md`](docs/vax-host.md). What is still untried is a UniBone
in a live, terminated backplane driving real cards: the emulated processors
have run against the card's internal bus, and whether one on a physical bus can
reach the card's own emulated devices is an open question. The DEUNA has been
carried through VMS boots on the emulated VAX, with three defects fixed there;
no guest's network stack has driven it on a real UNIBUS machine.

If you have UNIBUS hardware, reports are welcome.

## What the machine sees

| | |
|---|---|
| **Disk** | RL11 with RL01/RL02 · RK11 with RK05 · MSCP (UDA50 and friends) · RS11/RF11 DECdisk · RX11 and RX211 with RX01/RX02 floppies |
| **Tape** | TM11/TS11 · TMSCP |
| **Memory** | a card's worth of the BeagleBone's DDR, at an address range you name |
| **Serial** | DL11-W · DZV11 · DHV11 |
| **Network** | DELQA · DEUNA, bridged to the host LAN |
| **Bootstrap** | M9312 · MRV11-D · MXV11-B2 |
| **Clocks** | KW11-L line clock · KW11-P programmable clock |
| **Graphics** | VCB01 / QVSS framebuffer |
| **Processor** | KA11 (11/20) and KD11-EA (11/34 with KT11-D), validated against the original DEC MAINDEC diagnostics · a VAX-11/780 core — UNIBUS only, since a QBone drives a real CPU board beside it |
| **Other** | KE11 EAE · a device exposing the card's own LEDs and switches · lamps-and-switches panels over I²C |

## What this fork adds

- **Web interface** — the whole of operating a machine, in a browser: a
  dashboard of widgets (console, drives, front panel, memory, ROM), an image
  library, saved configurations, machine control, a log stream, and a system
  page that updates the software in place.
- **A debug workbench** — memory and disassembly views, as many as the work
  needs, held in the URL so a screen comes back on reload. The disassembler
  knows which processor the machine carries and names the I/O-page addresses an
  instruction refers to. It works on either kind of machine, because the card is
  bus master and asks the processor nothing.
- **Configurations** — the set of devices a machine carries, saved, applied,
  selected by DIP switch at startup, and checked before it is written so two
  devices cannot land on one backplane slot.
- **A machine that comes up dark** — the configuration is loaded at startup but
  nothing reaches the bus until the machine is switched on, because a card is
  fitted to a machine and configured afterwards. A configuration marked
  `autostart` overrides that, says so in the log, and raises a notice that
  stands until somebody acknowledges it.
- **Ethernet emulation** — DELQA on QBUS and DEUNA on UNIBUS, bridged onto the
  host LAN, with a station address derived from the BeagleBone's own MAC.
- **Debian 13 support** — runs on current Debian, loading the PRU firmware
  through `remoteproc`.
- **A distributable SD-card image** — a `.deb` package and image builder that
  self-configure on first boot: SSH host keys, the filesystem grown to fill the
  card, and per-installation personalization. Later releases arrive over apt.
- **One identity** — the name and password created on first use reach the web
  interface, the SMB, FTP and SFTP shares of the image library, and an ssh
  session.
- **Status LEDs** — emulator state on the BeagleBone user LEDs.
- **2.11BSD QBONE kernel configurations** for the emulated machine.
- **An MCP server** ([`mcp-server/`](mcp-server)) wrapping the API, so an agent
  can drive a machine — power, devices, console, XXDP diagnostics.

## Installing

The supported path is the ready-made card image. It carries Debian 13, the
emulator, the cape overlay and the boot settings, and configures itself on first
boot. These links always give the newest release:

- **[qbone-dist.img.xz](https://github.com/QUniBone/QUniLator/releases/latest/download/qbone-dist.img.xz)** — QBUS
- **[unibone-dist.img.xz](https://github.com/QUniBone/QUniLator/releases/latest/download/unibone-dist.img.xz)** — UNIBUS

Write it to a microSD card of 8 GB or larger — the image grows to fill the card
on first boot:

    xz -dc qbone-dist.img.xz | sudo dd of=/dev/sdX bs=4M status=progress conv=fsync

Replace `/dev/sdX` with the card — on macOS `/dev/rdiskN`, and check it twice,
`dd` will not ask. Then fit the card, apply power, and wait out the first boot:
it takes 2–3 minutes and includes a reboot of its own, so do not pull power in
between. The user LEDs say how far it has got, and a bouncing sweep across
`usr0`–`usr2` means the emulator is running.

It takes a DHCP address and answers on port 80 — try `http://qbone.local/` (or
`http://unibone.local/`), a service browser, or a USB cable, which puts the
BeagleBone at a fixed `192.168.7.2`. The interface asks you to create the
operator identity before anything else.

[**Installing the software**](https://vaxbusters.org/qunilator/start/install/)
in the manual covers all of this properly: reading the LEDs, the five ways of
finding it on the network, running more than one on a LAN, and renaming them.

## Updating

The card image ships with the package repository preconfigured, so an
installation keeps itself current — from the System page in the web interface,
or from a shell:

    sudo apt update && sudo apt upgrade

Every release also attaches the packages themselves —
`qbone_<version>_armhf.deb` and `unibone_<version>_armhf.deb` — beside the
images, for a BeagleBone built another way.

## Building from source

Cross-build with `crossbuild.sh` (Docker, no toolchain to install):

    ./crossbuild.sh        # the bus build.env records
    ./crossbuild.sh -q     # QBUS
    ./crossbuild.sh -u     # UNIBUS
    ./crossbuild.sh -h     # every option

The first run copies `build.env.example` to `build.env` and stops so it can be
filled in: it names the card a deploy goes to and the bus to build for. `-d`
deploys after a successful build. A UNIBUS build runs the emulated processor
cores against the MAINDEC diagnostics first, so a broken core stops the build.

For deploying and for building distributable images, see
[`docs/distribution.md`](docs/distribution.md) and
[`docs/debian-installation.md`](docs/debian-installation.md).

## Documentation

The user manual is published at
[vaxbusters.org/qunilator](https://vaxbusters.org/qunilator/) and lives in
[`docs/manual/`](docs/manual), readable as plain Markdown in a checkout;
[`docs/site/`](docs/site) is the generator that publishes it. Beside it,
[`docs/`](docs) holds the working record — requirements per area of work,
implementation plans, and the findings from bringing the cards up.
[`docs/README.md`](docs/README.md) indexes all of it.

## License

BSD 2-Clause. © 2019 Jörg Hoppe; © 2026 Hans Hübner and the QUniBone
contributors. See [`LICENSE`](LICENSE).
