---
title: UDA50 — MSCP disk controller
description: The emulated UDA50 MSCP disk controller, its RA, RD and RX drives, and every parameter they take.
---

The **UDA50** is DEC's MSCP disk controller: the host talks to it through two
registers and a pair of command and response rings in its own memory, and the
controller carries out each command against the drive it names. QUniLator
implements the MSCP protocol in full on top of those two registers, in the manner
of the third-party MSCP controllers of the time, and backs each drive with a
disk image. It is listed as `uda`, of type `UDA50`, with eight drives `uda0` to
`uda7` behind it.

> **UNIBUS · UniBone**
>
> UNIBUS addresses are 18 bits wide, and `22_bit_dma` is off. The emulated
> [VAX-11/780](ka780.md) boots from this controller.

> **QBUS · QBone**
>
> The same controller answers on a QBUS. `22_bit_dma` is on there, so the host
> may place its buffers anywhere in the 22-bit address space.

[2.11BSD on an MSCP disk, on the LAN](../walkthroughs/211bsd-network.md) and
[VMS on an emulated VAX-11/780](../walkthroughs/vax-vms.md) each build a machine
around it.

## What the machine sees

| | |
|---|---|
| **Registers** | IP at 772150 and SA at 772152 (17772150 and 17772152 on a QBone) |
| **Interrupt** | BR5, at the vector the host gives the controller when it initialises it |
| **Transfers** | Commands, responses and data move by DMA to and from the host's memory |
| **Drives** | Eight units, 0 to 7. Each takes a drive type and an image; a unit without an image is reported as not available |
| **Blocks** | 512 bytes. The replacement and caching table each drive type carries is kept in memory, starts empty and is not saved: an image has no bad blocks to replace |
| **Identity** | Controller class 1, model 2 (UDA50). With `type` set to `RQDX3`, the fourth initialisation step reports the RQDX3's microcode version |

The drive types, with the block counts they announce when `useimagesize` is off:

| Type | Blocks | | Type | Blocks |
|---|---|---|---|---|
| RX50 | 800 | | RA60 | 400176 |
| RX33 | 2400 | | RA70 | 547041 |
| RD51 | 21600 | | RA81 | 891072 |
| RD31 | 41560 | | RA82 | 1216665 |
| RC25, RC25F | 50902 | | RA71 | 1367310 |
| RD52 | 60480 | | RA72 | 1953300 |
| RD32 | 83236 | | RA90 | 2376153 |
| RD53 | 138672 | | RA92 | 2940951 |
| RA80 | 237212 | | RA73 | 3920490 |
| RD54 | 311200 | | | |

A new drive is an RA81. The RX50, RX33, RC25, RC25F and RA60 take removable
media, and the drive reports that in its unit flags, which is how a boot ROM tells
a floppy or cartridge from a fixed disk.

## Using it

1. On the **Configurations** screen, enable `uda`. The defaults are right for a
   UDA50 at its standard address.
2. Give each drive a `type` and an `image`, then enable it. Enabling the
   controller rebuilds the drives under it, so assign the images after the
   controller is on, and check that each drive shows a non-zero `capacity`.
3. The host finds the drives when it initialises the controller. A boot ROM that
   knows MSCP boots unit 0 as `DU0`; the VAX console calls it `RQ0`.

A drive goes offline at every bus reset and power-up, and comes online when the
host brings it online.

## What has been verified

- **VMS V4.7** boots from the emulated UDA50 on the emulated VAX-11/780, over the
  UniBone's bus, to a login prompt. The project's hardware test runs that boot on
  a UniBone every night and before each release.
- **2.11BSD** runs multi-user from an RA81 on a real KDJ11-D (PDP-11/53) in a
  QBUS backplane, verified with QUniLator 1.16.0-1.
- **RSX-11M-PLUS V4.6** boots on a real PDP-11/73: the KDJ11 boot ROM finds `DU0`
  over the emulated UDA50.
- The **MXV11-B2** bootstrap ROM runs separate boot passes for removable and
  fixed units and tells them apart by the removable-media flag alone; both find
  their units.

## Parameters every MSCP controller has

The UDA50 and the [TQK50](tqk50.md) share the MSCP port: the two registers, the
initialisation handshake and the rings. What they have in common is listed here.

The host sets the interrupt vector during initialisation, as MSCP prescribes, so
`intr_vector` reads 0 until it has and does not take a value of its own.

<!-- device-parameters: mscp_port_c -->
<!-- Written by docs/site/tools/device-params.mjs from the C++ source. Edit the source, then run it. -->

### Settings

What a configuration stores and an operator sets.

| Parameter | Short | Value | Default | What it is |
|---|---|---|---|---|
| [`22_bit_dma`](#22_bit_dma) | `dma22` | 0 or 1 | 1 on a QBUS board, 0 on a UNIBUS board | Enable 22-bit DMA |

It also carries the parameters under [parameters every card on the bus has](index.md#parameters-every-card-on-the-bus-has).

### In detail

#### `22_bit_dma`

Whether the controller tells the host, at the first step of its initialisation, that it transfers to all 22 address bits of a QBUS: bit 9 of SA, the flag a Q22 MSCP controller sets there. A host that sees it hands the controller buffer addresses anywhere in the 4 MB space. On a UNIBUS board it stays 0, since UNIBUS addresses are 18 bits wide. The value is read each time the host initialises the controller, so a change takes effect at its next initialisation.

<!-- /device-parameters -->

## Parameters

<!-- device-parameters: uda_c -->
<!-- Written by docs/site/tools/device-params.mjs from the C++ source. Edit the source, then run it. -->

It has no parameters of its own.

It also carries the parameters under [parameters every MSCP controller has](#parameters-every-mscp-controller-has).

On this device, `base_addr` can be set.

<!-- /device-parameters -->

## Drive parameters

<!-- device-parameters: mscp_drive_c -->
<!-- Written by docs/site/tools/device-params.mjs from the C++ source. Edit the source, then run it. -->

### Settings

What a configuration stores and an operator sets.

| Parameter | Short | Value | Default | What it is |
|---|---|---|---|---|
| [`useimagesize`](#useimagesize) | `uis` | 0 or 1 | 1 | Determine unit size from image file instead of drive type |

It also carries the parameters under [parameters every drive has](index.md#parameters-every-drive-has).

### In detail

#### `useimagesize`

How many blocks the drive announces to the host. On, it is the size of the image in 512-byte blocks, so the host sees exactly what the image holds, whatever the drive type. Off, it is the block count of the drive type set in `type` (an RA81 has 891072): a larger image is cut at that count, and every block beyond it answers as lying past the end of the drive, which corrupts a file system laid out across them. Turn it off only for a guest that needs the drive type's exact size. The geometry the drive reports (sectors per track, tracks per group) is the drive type's either way.

<!-- /device-parameters -->
