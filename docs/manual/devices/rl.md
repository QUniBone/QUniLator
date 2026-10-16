---
title: RL11, RLV12 — RL01/RL02 disk controller
description: The emulated RL11 and RLV12 controllers, the RL01 and RL02 cartridge drives behind them, and every parameter they take.
---

The **RL11** is DEC's controller for the RL01 and RL02 cartridge disks: up to
four drives on one controller, each holding a removable pack of 5 MB (RL01) or
10 MB (RL02). QUniLator emulates the controller and its four drives. The
controller is listed as `rl`, the drives as `rl0` to `rl3`, and each drive's
pack is an image file on the board.

> **UNIBUS · UniBone**
>
> The controller is an **RL11**: base address 774400, vector 160, bus request
> level 5, backplane slot 15. It moves data to and from 18-bit addresses, with
> the top two address bits in the control and status register.

> **QBUS · QBone**
>
> The controller is an **RLV12**: base address 17774400 (174400 as a program
> names it), vector 160, bus request level 4, backplane slot 15. It carries a
> fifth register, the bus address extension, which gives it the full 22-bit
> address range.

[Boot XXDP from an emulated RL02](../walkthroughs/xxdp-rl02.md) builds a machine
around it on a real PDP-11/53, step by step.

## What the machine sees

| | |
|---|---|
| **Registers** | CS, BA, DA and MP at the base address and the three words above it; on the RLV12 also BAE, the fifth word |
| **Drives** | Four drive slots, units 0 to 3. Each enabled drive is an RL01 (256 cylinders, 5,242,880 bytes) or an RL02 (512 cylinders, 10,485,760 bytes), set by its `type` |
| **Disk format** | 2 heads, 40 sectors of 256 bytes a track. Read Header returns the sector headers a real pack carries, with their CRC |
| **Drive status** | The drive state, heads, cover, drive type, volume check and write lock, in the Get Status word a real drive returns |
| **Timing** | A seek takes the time a real positioner does, 5 ms for one track to 100 ms across the pack, and every seek ends with the 6.5 ms settle before the drive is ready |
| **Empty slot** | A drive slot with no enabled drive answers with neither Drive Ready nor Drive Error, and a function addressed to it ends in Operation Incomplete. That is how a program finds out which drives the machine has |

A seek given to a drive that is still settling from the previous one waits for
it, and a read or write waits for the selected drive to come ready, both up to
the controller's own operation-incomplete timeout. A program that overlaps seeks
with other work sees the drive where it sent it.

## Using it

1. Add `rl` on the **Configurations** screen, with a drive — `rl0` — whose
   `image` names a pack. The board ships the XXDP 2.5 pack as
   `dl/xxdp25.rl02.dsk`.
2. Enabling a drive switches it on, and a drive holding an image spins up by
   itself and comes READY. The drive's four lamps are among its status
   parameters.
3. An image can be changed while the pack spins: the drive spins the old one
   down, takes the new one and spins it up.

A drive is an RL02 unless its `type` is set to `RL01`.

A blank pack has no factory bad-sector file on its last track, which DEC's
drive diagnostics look for. Before running them on a scratch pack, copy the
last cylinder from a pack that has one. Run the diagnostics that write on a
drive other than the one the system booted from.

The RLV12's maintenance mode is outside the model.

## What has been verified

- On a QBone in a PDP-11/53, the RLV12 and an RL02 pass DEC's XXDP
  diagnostics ZRLG and ZRLH (controller), and ZRLI, ZRLJ and ZRLN (drive), to
  the end of pass with no errors. The few tests that time the drive against a
  clock skip themselves when the supervisor has no clock to use. The random exerciser ZRLK runs for many
  minutes with no error. The drive tests were run on drive 1, with a scratch
  pack carrying a copied factory bad-sector file.
- The PDP-11/53's own boot ROM boots XXDP from an emulated RL02, as the
  walkthrough shows.
- On a UniBone, the emulated PDP-11/20 boots XXDP from an emulated RL02 through
  the RL11, with the M9312's DL boot ROM.

## Controller parameters

<!-- device-parameters: RL11_c -->
<!-- Written by docs/site/tools/device-params.mjs from the C++ source. Edit the source, then run it. -->

It has no parameters of its own.

It also carries the parameters under [parameters every card on the bus has](index.md#parameters-every-card-on-the-bus-has).

<!-- /device-parameters -->

### RLV12

<!-- device-parameters: RLV12_c -->
<!-- Written by docs/site/tools/device-params.mjs from the C++ source. Edit the source, then run it. -->

It has no parameters of its own.

It also carries the parameters under [controller parameters](#controller-parameters).

<!-- /device-parameters -->

### RLV11

The RLV11 is the 18-bit QBUS controller: the RL11's registers at bus request
level 4. Neither board offers it.

<!-- device-parameters: RLV11_c -->
<!-- Written by docs/site/tools/device-params.mjs from the C++ source. Edit the source, then run it. -->

It has no parameters of its own.

It also carries the parameters under [controller parameters](#controller-parameters).

<!-- /device-parameters -->

## Drive parameters

<!-- device-parameters: RL0102_c -->
<!-- Written by docs/site/tools/device-params.mjs from the C++ source. Edit the source, then run it. -->

### Settings

What a configuration stores and an operator sets.

| Parameter | Short | Value | Default | What it is |
|---|---|---|---|---|
| [`powerswitch`](#powerswitch-runstopbutton-loadlamp-readylamp-faultlamp-writeprotectlamp-writeprotectbutton) | `pwr` | 0 or 1 | 0 | State of POWER switch |
| [`runstopbutton`](#powerswitch-runstopbutton-loadlamp-readylamp-faultlamp-writeprotectlamp-writeprotectbutton) | `rb` | 0 or 1 | 0 | State of RUN/STOP button |
| [`writeprotectbutton`](#powerswitch-runstopbutton-loadlamp-readylamp-faultlamp-writeprotectlamp-writeprotectbutton) | `wpb` | 0 or 1 | 0 | Writeprotect button pressed |
| [`coveropen`](#coveropen) | `co` | 0 or 1 | 0 | 1, if RL cover is open |
| [`spinupdelay`](#spinupdelay) | `sud` | 0 or 1 | 0 | 1 = model the drive's spin-up/spin-down delay; 0 = ready at once |

### Status

What the device reports as it runs. A configuration does not store these.

| Parameter | Short | Value | What it is |
|---|---|---|---|
| [`rotation`](#rotation-state) | `rot` | number, rpm | Current speed of disk |
| [`state`](#rotation-state) | `st` | number | Internal state |
| [`loadlamp`](#powerswitch-runstopbutton-loadlamp-readylamp-faultlamp-writeprotectlamp-writeprotectbutton) | `ll` | 0 or 1 | State of LOAD lamp |
| [`readylamp`](#powerswitch-runstopbutton-loadlamp-readylamp-faultlamp-writeprotectlamp-writeprotectbutton) | `rl` | 0 or 1 | State of READY lamp |
| [`faultlamp`](#powerswitch-runstopbutton-loadlamp-readylamp-faultlamp-writeprotectlamp-writeprotectbutton) | `fl` | 0 or 1 | State of FAULT lamp |
| [`writeprotectlamp`](#powerswitch-runstopbutton-loadlamp-readylamp-faultlamp-writeprotectlamp-writeprotectbutton) | `wpl` | 0 or 1 | State of WRITE PROTECT lamp |

It also carries the parameters under [parameters every drive has](index.md#parameters-every-drive-has).

### In detail

#### `rotation`, `state`

What the drive is doing. `rotation` is the spindle speed: 2400 rpm with a pack spinning, 0 at rest. `state` is the drive state the RL01/RL02 reports to the controller's Get Status, as DEC's manuals number it: 0 load cartridge, 1 spin up, 2 brush cycle, 3 load heads, 4 seek, 5 lock on, 6 unload heads, 7 spin down. A drive that is powered off shows 255.

A seek to another cylinder holds the drive in state 4 for the time a real positioner takes, from 5 ms for one track to 100 ms across the whole pack; a seek to the cylinder the heads are on stays in state 5. Either way the drive drops READY for the 6.5 ms the positioner takes to settle before it accepts the next command.

#### `powerswitch`, `runstopbutton`, `loadlamp`, `readylamp`, `faultlamp`, `writeprotectlamp`, `writeprotectbutton`

The drive's front panel. `powerswitch` is the POWER switch: enabling the drive closes it and disabling it opens it, so it needs no attention in normal use. A drive switched off is not ready and its panel is dark.

`runstopbutton` is the LOAD button. Pressed (1), a drive holding a cartridge spins up, loads its heads and comes READY; released (0), it unloads its heads and spins down to LOAD. Setting `image` on a drive at rest presses it, and a new `image` on a spinning drive releases it and presses it again once the pack is at rest, so a drive with a medium comes READY without it being touched.

The four lamps follow the drive's state as DEC's RL01/RL02 manuals give them: LOAD with the spindle stopped, READY with the heads on a track and settled, FAULT on a drive fault or a write the drive refused, and WRITE PROT while the pack is write-protected.

`writeprotectbutton` is the WRITE PROT switch. Pressed, the drive refuses every write with a write gate error. An image the board can only read is write-protected the same way, whatever the switch says.

#### `coveropen`

The cartridge cover. It can be opened (1) only while the drive is in LOAD, and a drive with its cover open does not spin up. The drive reports the cover in its status word, and DEC's drive diagnostic ZRLI has it opened during its tests.

#### `spinupdelay`

With 1, a drive that is loaded takes the time a real one does to come READY: 25 seconds of spin-up, then the heads load. With 0, it comes READY as soon as it is loaded, which saves that wait on every boot. Seek and settle times are modelled either way.

<!-- /device-parameters -->
