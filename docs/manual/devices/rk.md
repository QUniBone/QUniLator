---
title: RK11, RKV11 — RK05 disk controller
description: The emulated RK11 and RKV11 controllers, the RK05 cartridge drives behind them, and every parameter they take.
---

The **RK11** is DEC's controller for the RK05 cartridge disk, the 2.5 MB
removable pack of the early PDP-11 systems: up to eight drives on one
controller. QUniLator emulates the controller and its eight drives. The
controller is listed as `rk`, the drives as `rk0` to `rk7`, and each drive's
pack is an image file on the board.

> **UNIBUS · UniBone**
>
> The controller is an **RK11**: base address 777400, vector 220, bus request
> level 5, backplane slot 10. The two memory extension bits in RKCS carry a
> transfer to 18-bit addresses.

> **QBUS · QBone**
>
> The controller is an **RKV11**: base address 17777400 (177400 as a program
> names it), vector 220, bus request level 4, backplane slot 13. It addresses
> memory with the 16 bits of RKBA, so its transfers reach the first 64 KB; the
> memory extension bits in RKCS read and write but do not move the transfer.

## What the machine sees

| | |
|---|---|
| **Registers** | RKDS, RKER, RKCS, RKWC, RKBA, RKDA at the base address and the five words above it, RKDB at base + 16 |
| **Drives** | Eight drive slots, units 0 to 7. Each is an RK05: 203 cylinders, 2 surfaces, 12 sectors of 512 bytes a track, 2,494,464 bytes |
| **Functions** | Control Reset, Write, Read, Write Check, Seek, Read Check, Drive Reset and Write Lock, with the format bit on Read and Write |
| **Seek** | A Seek or Drive Reset ends with a search-complete interrupt when interrupts are enabled, and the drive's sector counter turns while it is idle |
| **Timing** | A seek takes from 3 ms for a short move to about 27 ms across the whole pack, and each sector read or written takes 10 ms |

A drive is ready once its `image` is open: setting the image is loading the pack,
and there is no front panel to work. A function given to a drive that is not
ready ends with Drive Error. The drive reports no write protection, and the
Write Lock function leaves the pack writable.

## Using it

1. Add `rk` on the **Configurations** screen, with a drive — `rk0` — whose
   `image` names a pack.
2. Enable the controller and the drive. The drive is ready for the machine as
   soon as the image is open.

An RK05 image is the pack's sectors in order, cylinder by cylinder, surface 0
before surface 1: the layout SimH and other emulators use.

## What has been verified

No DEC diagnostic run on this controller is recorded in the project yet.

## Controller parameters

<!-- device-parameters: rk11_c -->
<!-- Written by docs/site/tools/device-params.mjs from the C++ source. Edit the source, then run it. -->

It has no parameters of its own.

It also carries the parameters under [parameters every card on the bus has](index.md#parameters-every-card-on-the-bus-has).

<!-- /device-parameters -->

### RKV11

<!-- device-parameters: rkv11_c -->
<!-- Written by docs/site/tools/device-params.mjs from the C++ source. Edit the source, then run it. -->

It has no parameters of its own.

It also carries the parameters under [controller parameters](#controller-parameters).

<!-- /device-parameters -->

## Drive parameters

<!-- device-parameters: rk05_c -->
<!-- Written by docs/site/tools/device-params.mjs from the C++ source. Edit the source, then run it. -->

It has no parameters of its own.

It also carries the parameters under [parameters every drive has](index.md#parameters-every-drive-has).

<!-- /device-parameters -->
