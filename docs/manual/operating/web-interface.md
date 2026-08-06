---
title: The web interface
description: A tour of the six screens QUniLator serves on port 80 — the dashboard, the image library, configurations, machine settings, the log and the system page.
sidebar:
  order: 1
---

Everything an operator does day to day happens here: switch the machine on, put
a pack in a drive, watch the console, save the machine you built. It is served
on port 80 by the emulator itself, so there is nothing to install on your
workstation and nothing to keep running there.

Six screens, in the sidebar, in the order you meet them.

## Getting in

The first-run dialog creates **one identity that is both your account on the
BeagleBone and your web login**, so the same name and password reach the web
interface, the file shares of the image library, and an `ssh` session. Every
request takes it, and the name is part of it — the right password under the
wrong name is refused.

The bar along the top belongs to the whole interface, not to any one screen:

| | |
|---|---|
| **DCOK**, **POK** | the bus power signals. Both green is a machine with power on it. |
| **addr 22-bit** | the address width the bus is running at. |
| **connected** | the event stream. When it says *disconnected*, the page is showing you the past. |

## Dashboard

![The dashboard, with XXDP booted from an emulated RL02](../assets/screenshots/dashboard-xxdp.jpg)

The machine as it is now, built from **widgets** you place yourself. The heading
names the configuration that is loaded — its title, its short name, and a
**MODIFIED** badge when the live machine has drifted from what was saved.

- **Control panel** — the lamps and buttons of the machine's own front panel.
  *PWR OK* and *RUN* say what the CPU is doing; **RESTART**, **HALT** and **AUX
  ON/OFF** drive it. AUX ON/OFF is the power switch: a QUniLator that has just
  started serves a machine that is switched *off*, and this is what brings it up.
- **Front panel** — the four LEDs and four DIP switches on the card itself. The
  switches are read once, when the service starts, to choose which configuration
  to load.
- **Console** — the machine's serial console as a terminal in the page. The badge
  says which line it is showing (**TTYS2** here, the real console of a CPU that
  has its own serial line). **Record** captures the session, typing included, to
  a file you can replay later.
- **Drives** — one widget per drive, carrying the lamps the real unit had:
  *LOAD*, *READY*, *FAULT*, *WRITE PROT*, and the image the drive currently
  holds. Swapping a pack is editing that field.

**Edit layout** moves and resizes the widgets; the arrangement is saved with the
configuration, so a machine remembers how you like to look at it.

## Storage

![The image library](../assets/screenshots/storage.jpg)

The library of disk and tape images, in `/var/lib/qunilator/images`. Drag a file
onto the page to upload it — `.rl02`, `.rl01`, `.rk05`, `.rx2`, `.dsk` and
`.tap`, gzipped or not.

The folders are conventions, one per device family: `dl` for RL packs, `dk` for
RK, `du` for MSCP volumes, `mu` for tapes, `rx` for floppies, `roms` for
bootstrap images.

Two columns earn their place. **USED BY** says which saved configurations name
this image and which drive they put it in, and marks the ones a running machine
has **MOUNTED** — so you can see before deleting anything whether it is in use.
**READ-ONLY** marks an image the emulator may not write, which is how a master
pack survives a diagnostic that would otherwise scribble on it.

**Contents** looks inside an image and lists the files on it without booting
anything.

> [!TIP]
> The same tree is reachable over **SMB, FTP and SFTP** under the identity you
> created, so images can be moved by network rather than by SD card.

## Configurations

![The configuration screen](../assets/screenshots/configurations.jpg)

A **configuration** is the set of devices a machine carries and the parameters
they carry them with — the whole machine as one document. The list on the left
is every machine this QUniLator knows; the panel on the right is the one you
picked.

Devices are grouped under the controller they hang off, each with an **enabled**
box, its image, and **Parameters** for everything else — addresses, vectors, bus
slots. **+ Add device** brings in a card the machine does not yet have.

Above them, **Power-on DIP** binds this configuration to a switch setting, 1–15.
The service reads the switches once at startup and loads the configuration that
claims that value; setting 0 brings back the machine that was last running,
unsaved changes and all.

**Save** writes the live machine back. **Export** hands it to you three ways —
the document as JSON, the same device set as a script for the interactive menu,
or an archive carrying the document *with every image it names*, so a whole
working machine travels as one file.

> [!NOTE]
> **The machine still comes up dark**
>
> Applying a configuration loads it; it does not put it on the bus. Switching
> the machine on stays an explicit act, because a card is fitted to a machine and
> configured afterwards, and what it carries may describe a backplane it is no
> longer in.

## Machine

![Machine-wide settings](../assets/screenshots/machine.jpg)

The handful of settings that belong to the whole machine rather than to any one
device.

**Address width** is the CPU's, and changing it re-bases the I/O page — so it
only applies while the bus is halted.

**External console** is where the real machine's console line is read from:
`/dev/ttyS2` on the BeagleBone, a **Web Serial** port on the machine running the
browser, or **Off**. Which you want depends on the CPU in the box. A CPU with its
own serial line — an 11/53, an 11/73 — has that line cabled to the card and read
here. A CPU with no console of its own leaves this **Off**, and QUniLator's own
emulated DL11 supplies the console instead.

The **Baud** must match what the CPU's own line is set to; a mismatch gives you a
screen of garbage rather than silence, which is the useful clue.

## Diagnostics

![The log stream](../assets/screenshots/diagnostics.jpg)

The live log, newest first, with the severity chips at the top filtering it.
Every line carries its source — `web`, `QUNAPT`, a device's own name — and each
source has its own level, so one device can be made talkative without drowning
the rest.

Sources sit at **warning** by default. Raise the one you are chasing to *debug*,
read, and put it back: a source left at debug costs the running machine.

## System

![The system page](../assets/screenshots/system.jpg)

Who reaches this QUniLator, what it is called, and keeping it current.

- **Access** — the name and password from the first-run dialog. Changing either
  takes effect on the next request, so the browser will ask again.
- **Board** — the network name (`qbone.local`, the DNS-SD entry and the DHCP
  lease all follow it) and an SSH public key to install.
- **Serial ports** — which of the BeagleBone's three UARTs carry a Linux login.
  One always keeps its login: it is the way back onto a card whose network has
  gone. A port the emulator holds cannot take one — `ttyS2` here is held by the
  external console bridge.
- **Console recordings** — sessions captured with **Record** on the dashboard.
- **Updates** — what the installed package is and what the repository offers.

## What is not here

The hardware-level work — bus latches, master/slave transfers, interrupt tests,
the device exercisers — has no web equivalent and belongs to the interactive menu
application. See [Coming from QUniBone Classic](../start/from-qunibone.md).

## Next

Put it to work: the [walkthroughs](../walkthroughs/xxdp-rl02.md) take a real
machine from a bare card to a running system.
