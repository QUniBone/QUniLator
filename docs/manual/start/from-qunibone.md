---
title: Coming from QUniBone Classic
description: What a machine looks like now that it is a saved configuration rather than a menu script, and how to keep running the .cmd scripts you already have.
sidebar:
  order: 6
---

You have a card that ran Jörg's `demo` application, a folder of `.cmd` scripts —
one per machine — and an `autostart.sh` that picked one from the DIP switches.

All of that still runs, on the same command-line program, from the same scripts.
What changed is where a machine is **described**: a machine is now a document
QUniLator applies, rather than a sequence of commands that builds one.

## What moved where

| QUniBone Classic | QUniLator |
|---|---|
| `demo` from a source tree, started with `sudo ./demo.sh` | a package: the service and the menu in `/usr/bin`, named for the card — `qbone` and `qbone-cli`, or `unibone` and `unibone-cli` |
| a `.cmd` script per machine, one shell wrapper each | a saved **configuration** per machine, in the web interface |
| `autostart.sh` reading the switches and running the matching wrapper | the service reading the switches once at startup and applying the configuration bound to that value |
| the menu, on a terminal, as the operator's surface | the web interface, on port 80 |
| images wherever you kept them | `/var/lib/qunilator/images`, also reachable over SMB, FTP and SFTP |
| the menu, for bus latches and exercisers | the same menu, as `qbone-cli` |

## A machine is a document

A **configuration** is the set of devices a machine carries and the parameters
they carry them with. QUniLator applies it as a set when the machine is switched
on. There is no order to it and nothing to step through:

```json
{
  "title": "KDJ11-A",
  "devices": [
    { "name": "rl",     "enabled": true, "params": {} },
    { "name": "rl0",    "enabled": true, "params": {
        "image": "images/dl/xxdp25.rl02",
        "powerswitch": "1", "runstopbutton": "1" } },
    { "name": "DL11",   "enabled": true, "params": {} },
    { "name": "MEM",    "enabled": true, "params": { "size": "1 MB" } },
    { "name": "MRV11D", "enabled": true, "params": {} }
  ]
}
```

They live in `/var/lib/qunilator/configs`, and the **Configurations** screen is
where they are built, saved, renamed and applied. Three things follow from a
machine being a document rather than a program:

- **The switches select it, not a script.** A configuration binds to a DIP value
  1–15 under **Power-on DIP**; the service reads the switches once, at startup,
  and applies the one that claims that value. Setting 0 brings back the machine
  that was last running, unsaved changes and all. Changing machines means
  changing the switches and restarting the service.
- **The machine still comes up dark.** Applying a configuration loads it; it does
  not put it on the bus. Switching the machine on is a separate, explicit act —
  or a standing one, if the configuration is marked **Start this machine at
  power-on**.
- **It travels as a file.** **Export ▾** offers the document, the same set as a
  menu command script, and an archive carrying the document with every image it
  names, so a whole working machine moves as one file.

## A script is a program

Here is the shape of a real Classic script, trimmed:

```
d                       # device test menu
pwr
.wait 3000              # wait for PDP-11 to reset
m i                     # install max UNIBUS memory
m ll dl.lst             # deposit bootloader into memory

en rl                   # enable RL11 controller
en rl0
sd rl0
p powerswitch 1         # power on, now in "load" state
p image xxdp25.rl02     # mount image file
p runstopbutton 1       # press RUN/STOP, will start

.wait 6000              # wait until drive spins up
.print RL drives ready.
```

Two kinds of line are mixed in there, and the difference is the whole of this
page:

- **`sd`, `p`, `en` describe the machine.** Select a device, set a parameter,
  put the card in. Those are exactly what a configuration holds, and the
  translation is mechanical.
- **`pwr`, `m i`, `m ll`, `.wait`, `.print` perform an action** at a point in a
  sequence. A configuration is applied all at once and has no sequence to put
  them in.

So each of those has somewhere else to be:

| Script line | Where it went |
|---|---|
| `pwr` | the power switch on the dashboard's control panel, and `POST /api/control` |
| `m i` | the **MEM** device, which claims an address range by parameter and is sized in the configuration |
| `m ll <listing>`, `m lp <tape>` | a bootstrap ROM card in the machine — **MRV11-D** on QBUS, **M9312** on UNIBUS — or the menu, for a loader no ROM carries |
| `.wait`, `.print`, `.input` | nothing: there is no sequence to pace or narrate |

## Turning a script into a configuration

By hand, once per machine:

1. Take the `sd` / `p` / `en` lines. Build that device set on the
   **Configurations** screen and save it under a name.
2. Bind it to the DIP value its wrapper answered to, under **Power-on DIP**.
3. Give the leftover lines a home from the table above. A `pwr` at the top is
   the power switch; `m i` is the MEM device; a `m ll` bootloader is usually a
   bootstrap ROM card you can simply add to the machine.
4. Mark it **Start this machine at power-on** if the wrapper used to boot it
   unattended.

Reading a `.cmd` file straight into a configuration is
[planned](https://github.com/QUniBone/QUniLator/issues/82). Going the other way
already works: **Export ▾ → Menu command script** writes any configuration out
as `sd`/`p`/`en` commands.

## Running the scripts you have

The menu application is installed beside the service as **`qbone-cli`** — the
name follows the card, so `unibone-cli` on a UniBone. It is the same program,
with the same menus, and it reads a command file exactly as `demo` did:

```sh
qbone-cli --cmdfile xxdp.cmd
```

> [!NOTE]
> **You do not have to stop the service**
>
> `qbone-cli` asks the service for the board. The service switches its machine
> off, hands the hardware over, and locks the web interface for the length of
> the session — every page says so and why. When the session ends the service
> takes the board back and rebuilds the machine from the configuration that was
> current, **switched off**, because what a menu session leaves behind is not
> something the service should start a machine on.
>
> One session runs at a time. A second is refused, naming the process that holds
> the board.

It drives the bus, which is root's work, so it is installed set-user-id root and
executable only by the **`qunilator-admin`** group — the group that already
carries sudo and a login shell. `Permission denied` means your account is not in
it:

```sh
sudo usermod -aG qunilator-admin $USER   # log out and back in
```

> **QBUS · QBone**
>
> Pass the CPU's address width; it cannot be probed from the backplane.
>
> ```sh
> qbone-cli --addresswidth 22 --cmdfile xxdp.cmd
> ```

### What the command file may contain

Lines are processed as if typed at the menu. `#` starts a comment, leading and
trailing space is trimmed, and blank lines are skipped. Beyond the menu's own
commands, the reader handles these directives itself:

| | |
|---|---|
| `.wait <ms>` | pause |
| `.print <text>` | print a line to the terminal |
| `.input` | wait for the operator to press ENTER |
| `.ifeq <a> <b>` … `.endif` | skip the enclosed lines unless the two strings match |
| `.end` | stop reading the file; whatever follows is ignored |

### Things worth knowing

- **The menus nest, so quitting does too.** `q` leaves the device menu and a
  second `q` leaves the program. A script that ends with one `q` lands you at
  the main menu.
- **The script runs out and you keep the session.** When the file ends, input
  falls through to your terminal at whatever menu the script reached — which is
  how the Classic scripts left an operator sitting at the device menu.
- **`d` is the device menu**, and the emulated processors are devices in it like
  any other card, so Classic's separate `dc` entry is gone. Enabling a processor
  there is what makes the board the machine.
- **An unrecognised command at the main menu is ignored silently.** A typo looks
  like nothing happening.

## What only the menu does

The hardware-level work has no web equivalent and is what `qbone-cli` is for:
bus latches, master/slave transfers, interrupt tests, the device exercisers, and
the GPIO and I²C panel tests. The [acceptance test](acceptance-test.md) is the
main one you will want.

## Where your files went

| | |
|---|---|
| `/var/lib/qunilator/images` | disk and tape images, in `dk/`, `dl/`, `du/`, `mu/`, `rx/` and `roms/`. Also reachable over SMB, FTP and SFTP, so images move by network. |
| `/var/lib/qunilator/configs` | the saved configurations |
| `/var/lib/qunilator/settings.json` | board settings — the console bridge and the rest — applied by the service at startup |
