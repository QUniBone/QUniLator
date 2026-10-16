---
title: RX11, RX211 — RX01/RX02 floppy subsystem
description: The emulated RX11/RXV11 and RX211/RXV21 floppy controllers, the dual-drive box behind them with its RX01 or RX02 drives, and every parameter they take.
---

The **RX01** and **RX02** are DEC's 8-inch floppy subsystems: a box holding two
drives and the microcontroller that runs them, joined to the bus by a small
interface card. The **RX11** (RXV11 on the QBUS) drives an RX01, moving every
byte of a sector through its data register. The **RX211** (RXV21 on the
QBUS) drives an RX02 in single or double density and moves sectors by DMA.
QUniLator emulates both subsystems, each as three parts:

| | RX01 subsystem | RX02 subsystem |
|---|---|---|
| Interface card | `rx` | `ry` |
| Drive box, with its microcontroller | `rxbox` | `rybox` |
| Drives | `rx0`, `rx1` | `ry0`, `ry1` |

The box's type reads `RX0102uCPU`. The `ry` card's type reads `RY211` on a
UniBone and `RXV12` on a QBone.

> **UNIBUS · UniBone**
>
> The cards are an **RX11** and an **RX211**: base address 777170, vector 264,
> bus request level 5. The RX11 arbitrates in backplane slot 16, the RX211 in
> slot 17. The RX211 addresses 18 bits of memory.

> **QBUS · QBone**
>
> The cards are an **RXV11** and an **RXV21**: base address 17777170 (177170 as
> a program names it), vector 264, bus request level 4, both in backplane slot
> 16. The RXV21 addresses 18 bits of memory, or 22 with
> [`mxv22`](#mxv22) on.

Both cards answer the same address, as the real ones do at their factory
settings, so a machine carries one of them. Enabling the second while the first
is on the bus is refused and leaves it off. To carry both, give one of them its
own base address and vector, and on a QBone its own slot.

## What the machine sees

| | |
|---|---|
| **Registers** | RXCS at the base address, RXDB at base + 2 (RX2CS and RX2DB on the RX211) |
| **Functions** | Fill Buffer, Empty Buffer, Write Sector, Read Sector, Read Status, Write Sector with Deleted Data and Read Error Code; the RX211 adds Set Media Density |
| **Transfers** | RX11: the sector buffer passes byte by byte through RXDB. RX211: Fill and Empty Buffer move it by DMA, with word count and bus address loaded through RX2DB |
| **Drives** | Two per box, units 0 and 1. 77 tracks of 26 sectors: 128-byte sectors and 256,256 bytes a disk in single density, 256-byte sectors and 512,512 bytes in the RX02's double density |
| **Timing** | The disk turns at 360 rpm, and a sector read or written waits for it to pass. The head steps at 5 ms a track and settles for 25 ms after a seek |
| **Power-up** | Both heads go to track 0, and drive 0 reads track 1, sector 1 |

A drive is ready once its `image` is open. An image the board cannot write
turns on the drive's WRITE PROTECT lamp, and the drive then drops every write
without reporting an error to the machine.

A sector written with the deleted-data mark keeps the mark while the image stays
in the drive. The image file has no room for it, so attaching an image starts
with no marks set.

## Using it

1. Add the interface card, `rx` or `ry`, on the **Configurations** screen, and
   its box, `rxbox` or `rybox`.
2. Enable the card first, then the box. Enabling the box switches it on, and
   the drives in it with it; their own `enabled` follows the box.
3. Set a drive's `image` to a floppy image. An RX01 image is 256,256 bytes; an
   RX02 drive takes its density from the size of the image it is given.

A floppy image normally holds the sectors as they lie on the disk, track by
track from track 0. A drive also serves the two other forms floppy images come
in: one that leaves out track 0 ([`imagetrack0`](#imagetrack0)), and one that
holds an RT-11 volume's blocks in order ([`layout`](#layout)). Left at `auto`,
the drive tells an RT-11 volume in block order from one in sector order by
itself.

### 2.11BSD on a QBone

2.11BSD drives a stock RXV21 through 18-bit addresses, so on a machine with
more than 256 KB a transfer to a buffer above that misses the memory it is
meant for. Turn on [`mxv22`](#mxv22) on `ry` in the configuration that
boots it: the driver then finds a 22-bit controller when it attaches the
RXV21, and RX02 floppies read and write.

## What has been verified

- 2.11BSD on the QBone reads and writes RX02 floppies through the RXV21 with
  `mxv22` on, and what it writes is in the image afterwards.
- The RX01 diagnostic ZRXAF0 (CZRXA) does not pass on the QBone's RXV11: it
  reports NO INTERRUPT AT DONE. The cause is in the controller model, which can
  take back a DONE interrupt it has just raised.

## Interface card parameters

### For the RX01

<!-- device-parameters: RX11_c -->
<!-- Written by docs/site/tools/device-params.mjs from the C++ source. Edit the source, then run it. -->

It has no parameters of its own.

It also carries the parameters under [parameters every card on the bus has](index.md#parameters-every-card-on-the-bus-has).

<!-- /device-parameters -->

#### RXV11

<!-- device-parameters: RXV11_c -->
<!-- Written by docs/site/tools/device-params.mjs from the C++ source. Edit the source, then run it. -->

It has no parameters of its own.

It also carries the parameters under [for the RX01](#for-the-rx01).

<!-- /device-parameters -->

### For the RX02

<!-- device-parameters: RX211_c -->
<!-- Written by docs/site/tools/device-params.mjs from the C++ source. Edit the source, then run it. -->

#### Settings

What a configuration stores and an operator sets.

| Parameter | Short | Value | Default | What it is |
|---|---|---|---|---|
| [`mxv22`](#mxv22) |  | 0 or 1 | 0 | 22-bit DMA (DSD MXV-22): enables Q22 addressing; needed by 2.11BSD |

It also carries the parameters under [parameters every card on the bus has](index.md#parameters-every-card-on-the-bus-has).

#### In detail

##### `mxv22`

Makes the controller a 22-bit DSD MXV-22. RX2CS then reads with bit 10 (RX_Q22) set, and a command that sets that bit takes its bus address as two RX2BA writes: the low 16 bits, then bits 21:16. At 0 it is a stock 18-bit RX211/RXV21, which is what XXDP and other guests expect.

2.11BSD needs it on a QBUS machine with more than 256 KB: its driver checks the bit when it attaches the controller at boot, and takes the 22-bit path when it is set. Without it the driver addresses a buffer above 256 KB through an 18-bit address, and the transfer lands in the wrong memory. The change shows in RX2CS at once, but the guest only looks at boot, so keep it in the configuration that boots 2.11BSD.

<!-- /device-parameters -->

#### RXV21

<!-- device-parameters: RXV21_c -->
<!-- Written by docs/site/tools/device-params.mjs from the C++ source. Edit the source, then run it. -->

It has no parameters of its own.

It also carries the parameters under [for the RX02](#for-the-rx02).

<!-- /device-parameters -->

## Drive box parameters

<!-- device-parameters: RX0102uCPU_c -->
<!-- Written by docs/site/tools/device-params.mjs from the C++ source. Edit the source, then run it. -->

### Settings

What a configuration stores and an operator sets.

| Parameter | Short | Value | Default | What it is |
|---|---|---|---|---|
| [`powerswitch`](#powerswitch) | `pwr` | 0 or 1 | 0 | State of POWER switch |

It also carries the parameters under [parameters every device has](index.md#parameters-every-device-has).

### In detail

#### `powerswitch`

The POWER switch of the drive box, one for both drives. Enabling the box closes it, and the box runs its power-up initialize: both heads go to track 0 and drive 0 reads track 1, sector 1. Opened, the controller reads DONE and ERROR together, as with a box that is switched off. Disabling the box opens it.

<!-- /device-parameters -->

## Drive parameters

<!-- device-parameters: RX0102drive_c -->
<!-- Written by docs/site/tools/device-params.mjs from the C++ source. Edit the source, then run it. -->

### Settings

What a configuration stores and an operator sets.

| Parameter | Short | Value | Default | What it is |
|---|---|---|---|---|
| [`density`](#density) | `d` | text | `SD` on an RX01, `DD` on an RX02 | SD for RX01 & RX02 FM; DD for RX02 MFM |
| [`imagetrack0`](#imagetrack0) | `it0` | 0 or 1 | 1 | true: File image contains track 0-76 (std), else only 1..76 |
| [`layout`](#layout) | `lay` | text | `auto` | Sector order in the image file: physical, logical or auto |

### Status

What the device reports as it runs. A configuration does not store these.

| Parameter | Short | Value | What it is |
|---|---|---|---|
| [`layoutinuse`](#layoutinuse) | `lyu` | text | Sector order the drive serves the mounted image in |
| [`track`](#track) | `tr` | number | Track # of current head position |
| [`writeprotectlamp`](#writeprotectlamp) | `wpl` | 0 or 1 | State of WRITE PROTECT lamp |

It also carries the parameters under [parameters every drive has](index.md#parameters-every-drive-has).

### In detail

#### `density`

The recording density of the floppy: `SD` is single density, 128-byte sectors and 256,256 bytes to the disk; `DD` is the RX02's double density, 256-byte sectors and 512,512 bytes. An RX01 is always `SD`. On an RX02, attaching an image sets it from the file's size: anything larger than a single-density disk is `DD`.

#### `imagetrack0`

Whether the image file starts with track 0. At 1, the usual form, the file holds tracks 0 to 76. At 0 it starts with track 1, as some images do; track 0 then reads back as zeros and takes no data. A `logical` image always starts at track 1, whatever this says.

#### `layout`

The order the image file holds the floppy's sectors in. `physical` is the surface as it was written, track by track with sectors 1 to 26 in place. `logical` is the volume's blocks in order from block 0, the form an archived RT-11 distribution comes in: the file starts at track 1, and the drive maps every access through the 2:1 interleave with six sectors of skew per track that an RT-11 handler uses. `auto` looks at the image as it is attached and takes the order the RT-11 home block turns up in; an image without one is taken as physical.

#### `layoutinuse`

The order the drive serves the image in, which is what `auto` settled on.

#### `track`

The track the head is on, 0 to 76. It moves at 5 ms a track, and the head takes 25 ms to settle after a seek.

#### `writeprotectlamp`

Lit while the image in the drive is read-only. The drive then drops every write without reporting an error to the guest.

<!-- /device-parameters -->
