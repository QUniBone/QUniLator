---
title: Device reference
description: One page per emulated card, with every parameter it takes, generated from the source.
---

Each card QUniLator emulates has a page here: what the card is, what the machine
sees of it, and every parameter it takes. The parameter tables are written from
the source code, so they list exactly what the software accepts.

The same parameters are on the **Configurations** screen, under a device's
**Parameters** button, and in [`GET /api/devices`](https://github.com/QUniBone/QUniLator/blob/main/10.05_web/docs/api.md).
The short name beside each one is what the board's command-line menu accepts.

## The devices

| | |
|---|---|
| **Processors** | |
| [KA780](ka780.md) | The VAX-11/780 processor and its UNIBUS adapter. UNIBUS only. |
| [KA11, KD11-EA](pdp11-processors.md) | The PDP-11/20 and PDP-11/34 processors. UNIBUS only. |
| [KE11-A](ke11.md) | The extended arithmetic element of the PDP-11/20. UNIBUS only. |
| **Memory and bootstrap** | |
| [MS11, MSV11](memory.md) | Memory the board supplies out of its own. |
| [M9312](m9312.md) | The UNIBUS bootstrap card with five ROM sockets. |
| [MRV11-D](mrv11d.md) | The QBUS PROM card carrying the MXV11-B2 bootstrap. |
| **Disks** | |
| [UDA50](uda50.md) | The MSCP disk controller, with RA, RD and RX drives. |
| [RL11, RLV12](rl.md) | The RL01/RL02 cartridge disk controller. |
| [RK11, RKV11](rk.md) | The RK05 cartridge disk controller. |
| [RF11](rf11.md) | The fixed-head disk controller with its RS11 platters. |
| **Floppies and tapes** | |
| [RX11, RX211](rx.md) | The RX01/RX02 floppy subsystem. |
| [TS11, TSV05](ts11.md) | The TS05 magnetic tape subsystem. |
| [TQK50](tqk50.md) | The TMSCP cartridge tape controller. |
| **Serial lines and clocks** | |
| [DL11, KW11](dl11.md) | A single serial line, and the line clock. |
| [DZ11, DZV11](dz11.md) | The DZ eight- or four-line multiplexer. |
| [DHV11](dhv11.md) | The eight-line multiplexer with DMA output. |
| [KW11-P](kw11p.md) | The programmable real-time clock. |
| **Network and display** | |
| [DELQA](delqa.md) | The QBUS Ethernet controller. |
| [DEUNA](deuna.md) | The UNIBUS Ethernet controller. |
| [VCB01](vcb01.md) | The QVSS bitmap display with keyboard and pointer. QBUS only. |

## Parameters every device has

Every device carries these, besides its own: a card on the bus, and a drive
behind a controller alike.

Three more describe the device rather than set it, and the **Configurations**
screen shows them as the row itself: `name`, the device's name on this board
(`uda`, `rl0`); `type`, what it emulates (`UDA50`, `RL02`); and `enabled`, whether
it is in the machine. Writing `enabled` through the API is what switches a device
on or off.

<!-- device-parameters: device_c -->
<!-- Written by docs/site/tools/device-params.mjs from the C++ source. Edit the source, then run it. -->

### Settings

What a configuration stores and an operator sets.

| Parameter | Short | Value | Default | What it is |
|---|---|---|---|---|
| [`emulation_speed`](#emulation_speed) | `es` | number | 1 | 1 = original speed, > 1: faster |
| [`verbosity`](#verbosity) | `v` | number | the board's log level | 1 = fatal, 2 = error, 3 = warning, 4 = info, 5 = debug |

### In detail

#### `emulation_speed`

How fast the device's mechanics run against the original: a disk's seeks and rotation, a tape's motion. 1 is the original speed, 10 ten times as fast. A processor shows its own speed here and takes no setting.

#### `verbosity`

How much the device writes to the board's log: 1 only fatal errors, up to 5 for everything it can say. The board keeps each device's level in its own settings, not in a configuration, and sets it again whenever a configuration is applied; `/api/logging` (the MCP server's `set_log_level`) is the way to change it for good.

<!-- /device-parameters -->

## Parameters every card on the bus has

A card that answers on the bus — a controller, a processor, a serial line — has
a place there, which these say.

<!-- device-parameters: qunibusdevice_c -->
<!-- Written by docs/site/tools/device-params.mjs from the C++ source. Edit the source, then run it. -->

### Settings

What a configuration stores and an operator sets.

| Parameter | Short | Value | Default | What it is |
|---|---|---|---|---|
| [`base_addr`](#base_addr-slot-intr_vector-intr_level) | `addr` | octal | DEC's, for the device type | controller base address in IO page |
| [`slot`](#base_addr-slot-intr_vector-intr_level) | `sl` | number | DEC's, for the device type | backplane slot #, interrupt priority within one level, 0 = next to CPU |
| [`intr_vector`](#base_addr-slot-intr_vector-intr_level) | `iv` | octal | DEC's, for the device type | interrupt vector address |
| [`intr_level`](#base_addr-slot-intr_vector-intr_level) | `il` | octal | DEC's, for the device type | interrupt bus request level: 4,5,6,7 |

It also carries the parameters under [parameters every device has](#parameters-every-device-has).

### In detail

#### `base_addr`, `slot`, `intr_vector`, `intr_level`

Where the card sits on the bus: the address of its first register in the I/O page, the interrupt vector and bus request level it interrupts with, and its slot, which orders devices that request at the same level — 0 is nearest the processor. Each starts at DEC's standard values for the device type, and the Configurations screen offers the type's other standard addresses and vectors. They can be changed while the device is disabled. A device with no registers on the bus has 0 here.

<!-- /device-parameters -->

## Parameters every drive has

A drive — a disk, a tape, a floppy — sits behind its controller and holds a
medium, which is an image file in the board's image tree.

<!-- device-parameters: storagedrive_c -->
<!-- Written by docs/site/tools/device-params.mjs from the C++ source. Edit the source, then run it. -->

### Settings

What a configuration stores and an operator sets.

| Parameter | Short | Value | Default | What it is |
|---|---|---|---|---|
| [`unit`](#unit) |  | number, read-only | 0 | Unit # of drive |
| [`capacity`](#capacity) | `cap` | number, byte, read-only | 0 | Storage capacity |
| [`image`](#image) | `img` | image file | *empty* | Path to binary image file. Empty to detach. ".gz" archive also searched. |
| [`shared_dir`](#shared_dir-shared_filesystem) | `shd` | text | *empty* | Path to directory with shared files. Created on demand, empty to disable sharing. |
| [`shared_filesystem`](#shared_dir-shared_filesystem) | `shfs` | text | *empty* | Encode shared dir in this file system (empty, RT11, XXDP). |
| [`lock`](#lock) | `lk` | 0 or 1 | 0 | Host lock: medium cannot be changed while set |
| [`use_overlay`](#use_overlay) | `ovl` | 0 or 1 | 1 | Write through a copy-on-write overlay, keeping the base image pristine |

### Status

What the device reports as it runs. A configuration does not store these.

| Parameter | Short | Value | What it is |
|---|---|---|---|
| [`activityled`](#activityled) | `al` | number | Number of LED to used for activity display. |
| [`accesslamp`](#accesslamp) | `acl` | 0 or 1 | State of ACCESS lamp |

It also carries the parameters under [parameters every device has](#parameters-every-device-has).

### In detail

#### `unit`

The drive's unit number on its controller, which follows from its place under the controller.

#### `capacity`

The size of the medium the drive holds, in bytes, which follows from the drive type or the image.

#### `image`

The medium in the drive: an image file, named by its path in the board's image tree. Empty takes the medium out. When the file is not there but the same name with `.gz` is, it is expanded first.

A disk writes through an overlay beside the image unless `use_overlay` is 0; a tape writes the image itself.

#### `shared_dir`, `shared_filesystem`

A directory on the board presented to the machine as a volume, in place of an image: `shared_filesystem` names the layout, `RT11` or `XXDP`, and `shared_dir` the directory, which is created when it is not there. Both are needed; the volume is built from the files in the directory. Only drive types whose volume layout the board can build take it.

#### `activityled`

Which of the card's LEDs the drive lights while it transfers.

#### `accesslamp`

Lit while the drive transfers, as the ACCESS lamp of a real drive.

#### `lock`

Keeps the medium in the drive: while it is 1, `image` cannot be changed.

#### `use_overlay`

A disk writes through a copy-on-write overlay beside the image while this is 1, so the image itself stays as it was and an unclean shutdown can only damage the overlay; the Storage screen discards the overlay or folds it into the image. At 0 the disk writes the image itself. It is read when the image is set.

<!-- /device-parameters -->
