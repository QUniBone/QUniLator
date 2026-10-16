---
title: Device reference
description: One page per emulated card, with every parameter it takes, generated from the source.
device: qunibusdevice_c
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
| [KA780](ka780.md) | The VAX-11/780 processor. UNIBUS only. |

## Parameters every device has

Every card on the bus carries these, besides its own.

Three more describe the device rather than set it, and the **Configurations**
screen shows them as the row itself: `name`, the device's name on this board
(`uda`, `rl0`); `type`, what it emulates (`UDA50`, `RL02`); and `enabled`, whether
it is in the machine. Writing `enabled` through the API is what switches a device
on or off.

<!-- device-parameters -->
<!-- Written by docs/site/tools/device-params.mjs from the C++ source. Edit the source, then run it. -->

### Settings

What a configuration stores and an operator sets.

| Parameter | Short | Value | Default | What it is |
|---|---|---|---|---|
| [`emulation_speed`](#emulation_speed) | `es` | number | 1 | 1 = original speed, > 1: faster |
| [`verbosity`](#verbosity) | `v` | number | the board's log level | 1 = fatal, 2 = error, 3 = warning, 4 = info, 5 = debug |
| [`base_addr`](#base_addr-slot-intr_vector-intr_level) | `addr` | octal | DEC's, for the device type | controller base address in IO page |
| [`slot`](#base_addr-slot-intr_vector-intr_level) | `sl` | number | DEC's, for the device type | backplane slot #, interrupt priority within one level, 0 = next to CPU |
| [`intr_vector`](#base_addr-slot-intr_vector-intr_level) | `iv` | octal | DEC's, for the device type | interrupt vector address |
| [`intr_level`](#base_addr-slot-intr_vector-intr_level) | `il` | octal | DEC's, for the device type | interrupt bus request level: 4,5,6,7 |

### In detail

#### `emulation_speed`

How fast the device's mechanics run against the original: a disk's seeks and rotation, a tape's motion. 1 is the original speed, 10 ten times as fast. A processor shows its own speed here and takes no setting.

#### `verbosity`

How much the device writes to the board's log: 1 only fatal errors, up to 5 for everything it can say. The board keeps each device's level in its own settings, not in a configuration, and sets it again whenever a configuration is applied; `/api/logging` (the MCP server's `set_log_level`) is the way to change it for good.

#### `base_addr`, `slot`, `intr_vector`, `intr_level`

Where the card sits on the bus: the address of its first register in the I/O page, the interrupt vector and bus request level it interrupts with, and its slot, which orders devices that request at the same level — 0 is nearest the processor. Each starts at DEC's standard values for the device type, and the Configurations screen offers the type's other standard addresses and vectors. They can be changed while the device is disabled. A device with no registers on the bus has 0 here.

<!-- /device-parameters -->
