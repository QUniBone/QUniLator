---
title: MRV11-D — universal PROM module
description: The emulated MRV11-D bootstrap ROM card with the MXV11-B2 set, its two ways onto the bus, and every parameter it takes.
bus: [qbus]
---

The **MRV11-D** (M8578) is DEC's universal PROM module for the QBUS: sixteen
sockets of byte-wide ROM, an address decoder and a page control register, and
nothing else. A machine whose processor has no boot ROM gets one by carrying
it. QUniLator lists it as `MRV11D`, of type `MRV11-D`, with its sockets filled
with the **MXV11-B2** bootstrap set DEC supplied for it, which boots MSCP disks,
the RL01/RL02, the RX01/RX02 and the TU58. The model follows EK-MRV1D-UG-001.

> **QBUS · QBone**
>
> The MRV11-D is a QBUS card. A UNIBUS machine takes its bootstrap from the
> [M9312](m9312.md).

## What the machine sees

| | |
|---|---|
| **Bootstrap windows** | Window 0 at 773000 and window 1 at 765000, 512 bytes each, each showing one page of the array |
| **Page control register** | 777520: the low byte selects the page window 0 shows, the high byte window 1's. Power-up and INIT clear it, so both windows show page 0 |
| **Direct addressing** | With `directmode`, the whole array in memory space from `startaddr` |
| **Array** | 16 KB of MXV11-B2 code in 32 pages, or what `romfile` programs |

A page beyond the installed array answers nothing, as on the real module. Page
mode addressing, through a second page control register in memory space, and the
DATO-inhibit jumper are not modelled.

## Using it

1. Enable `MRV11D` on the **Configurations** screen. As it comes, it answers the
   bootstrap windows with the MXV11-B2 set.
2. Start the processor at 173000. A KDJ11-A whose power-up option starts it at
   173000 runs the bootstrap by itself on every restart.

A processor that answers 773000 and 777520 itself needs `bootstrap` off and the
array in memory space with `directmode`.

The sockets and the strapping can be changed only while the card is disabled.

## What has been verified

- On a KDJ11-A, which has no boot ROM of its own, the MXV11-B2 set boots the
  machine from an emulated MSCP disk with no console input.

## Parameters

<!-- device-parameters: mrv11d_c -->
<!-- Written by docs/site/tools/device-params.mjs from the C++ source. Edit the source, then run it. -->

### Settings

What a configuration stores and an operator sets.

| Parameter | Short | Value | Default | What it is |
|---|---|---|---|---|
| [`chipsize`](#chipsize-chipsets) | `cs` | number, bytes | 8192 | size of each device: 2048 to 32768 |
| [`chipsets`](#chipsize-chipsets) | `cn` | number | 1 | populated chip sets, 1 to 8 |
| [`romfile`](#romfile) | `rf` | ROM file | *empty* | ROM image or MACRO-11 listing from roms/; empty = MXV11-B2 set |
| [`bootstrap`](#bootstrap) | `bs` | 0 or 1 | 1 | answer the bootstrap windows at 773000 and 765000 |
| [`directmode`](#directmode-startaddr) | `dm` | 0 or 1 | 0 | answer the whole array in memory space at startaddr |
| [`startaddr`](#directmode-startaddr) | `sa` | octal | 0 | first address of the array in memory space |

### Status

What the device reports as it runs. A configuration does not store these.

| Parameter | Short | Value | What it is |
|---|---|---|---|
| [`accesslamp`](#accesslamp) | `acl` | 0 or 1 | machine is reading the module |
| [`contents`](#contents-arraysize-windows-range) | `co` | text | what the sockets hold |
| [`arraysize`](#contents-arraysize-windows-range) | `as` | text | size of the array |
| [`windows`](#contents-arraysize-windows-range) | `wi` | text | pages the bootstrap windows show |
| [`range`](#contents-arraysize-windows-range) | `ra` | text | addresses the array answers in memory space |

It also carries the parameters under [parameters every card on the bus has](index.md#parameters-every-card-on-the-bus-has).

### In detail

#### `chipsize`, `chipsets`

The sockets: how large each device is, in bytes, and how many of the eight chip sets are populated. A chip set is a low-byte and a high-byte device, so it holds as many words as one device holds bytes, and the array is `chipsize` × `chipsets` words. The device size is 2048, 4096, 8192, 16384 or 32768. The default, one set of 8192-byte devices, is the MXV11-B2 set: 16 KB in 32 pages of 512 bytes.

#### `romfile`

What the sockets are programmed with: a file in the board's image tree, usually in `roms/`. A `.lst` file is read as a MACRO-11 listing, and its lowest address becomes the first word of the array, so a bootstrap assembled at 173000 is page 0. Any other file is a dump of the array, low and high byte of each word in order, as the two devices of a chip set supply them; a short file leaves the rest unprogrammed, reading 177777. A file larger than the array is refused. Empty: the MXV11-B2 bootstrap set DEC supplied for this module, which boots MSCP disks, the RL01/RL02, the RX01/RX02 and the TU58.

#### `accesslamp`

Lit while the machine reads the module, through either window or in memory space: the bootstrap running.

#### `bootstrap`

The bootstrap: two 512-byte windows in the I/O page, window 0 at 773000 and window 1 at 765000, and the page control register at 777520 whose low byte selects the page of the array window 0 shows and whose high byte selects window 1's. Power-up and INIT clear the register, so the machine comes up looking at page 0 through both, and starting it at 173000 runs the bootstrap. A page beyond the array answers nothing. Turn it off for a processor that answers 773000 and 777520 itself, and reach the array through `directmode`. The card refuses to be enabled with both off, since it would answer nothing.

#### `directmode`, `startaddr`

Direct addressing: the whole array in memory space, from `startaddr`, which has to be on a 4 KB boundary, with the array ending below the I/O page. Enabling the card probes the range first and refuses when anything on the bus answers there. The array is served out of the board's memory through the range a VCB01 framebuffer also takes, so a machine carries one of the two. The range answers writes too.

#### `contents`, `arraysize`, `windows`, `range`

What the module carries: `contents` names what the sockets hold, the MXV11-B2 set or the file they were programmed from; `arraysize` the size of the array and how many bootstrap pages it makes; `windows` the pages the two bootstrap windows show, or `off`; `range` the addresses the array answers in memory space, or `off`.

<!-- /device-parameters -->
