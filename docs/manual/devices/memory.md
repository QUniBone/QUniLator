---
title: MS11 and MSV11 — memory
description: The memory card the board supplies out of its own memory, how it is placed, and every parameter it takes.
---

`MEM` is a memory card for a machine whose backplane carries none, or not
enough. It answers one range of addresses out of the board's own memory: the
board's bus engine serves every read and write there by itself, so the machine
sees memory at bus speed. It holds no registers and appears nowhere in the I/O
page; an operating system finds the top of memory by the bus timeout above it,
as it does with any memory card.

> **UNIBUS · UniBone**
>
> The card is of type `MS11`, in the 18-bit address space. Placed at 0 with the
> default size, it fills everything below the I/O page: 248 KB.

> **QBUS · QBone**
>
> The card is of type `MSV11`. On a 22-bit QBUS the top 128 KB below the I/O
> page, from 17360000, belong to the processor module: a KDJ11 answers its boot
> ROM there without a bus cycle, so nothing the board can ask the bus reveals a
> collision. A card placed there is written over the bus and read back as ROM,
> and the ROM's own memory test stops on a word it did not write. The card
> therefore ends at 17357776 at the most, and a placement reaching further is
> refused. Placed at 0 with the default size, it fills those 3960 KB.

## What the machine sees

| | |
|---|---|
| **Memory** | Word and byte reads and writes over the range `startaddr`..`endaddr` |
| **Registers** | None. Parity and the CSR at 772100 are not modelled |
| **Contents** | Kept across INIT and power cycles, so a program loaded and started from the console survives the restart that starts it. Cleared when the card takes up a range |

## Placing it

The card goes above the memory the machine already carries. Two cards answering
the same address drive the bus against each other, so the card reads its range
over the bus before it takes it and refuses when anything answers there. The
**Machine** screen's memory probe sizes the memory the machine carries, and the
memory map on the dashboard shows the ranges the board answers.

The range is set by `startaddr` and `size`. [`POST /api/memory/place`](https://github.com/QUniBone/QUniLator/blob/main/10.05_web/docs/api.md#post-apimemoryplace)
sets both as one placement, which is how a configuration names the range. Set
one parameter at a time, and a card moving from one range to another can pass
through a placement that is refused.

The board answers two ranges out of its memory: this card's and one device
window, such as a [VCB01](vcb01.md) framebuffer or the [MRV11-D](mrv11d.md)
array in memory space. The two may not overlap.

An [emulated processor](pdp11-processors.md) supplies its own memory when it is
enabled, everything below the I/O page that no card answers. A `MEM` card placed
before it keeps its range.

## What has been verified

- On a KDJ11-SD with 1.5 MB of its own, `MEM` at 06000000 with 2424 KB carries
  the machine to 17357776, and 2.11BSD reports `phys mem = 4055040`, 3960 KB.

## Parameters

<!-- device-parameters: memory_c -->
<!-- Written by docs/site/tools/device-params.mjs from the C++ source. Edit the source, then run it. -->

### Settings

What a configuration stores and an operator sets.

| Parameter | Short | Value | Default | What it is |
|---|---|---|---|---|
| [`startaddr`](#startaddr) | `sa` | octal | 0 | first address the card answers |
| [`size`](#size-endaddr) | `sz` | text | all the bus leaves to memory: 248 KB with 18-bit addresses, 3960 KB on a 22-bit QBUS | how much memory the card carries |
| [`probe`](#probe) | `pr` | 0 or 1 | 1 | check the range answers to nothing before claiming it |

### Status

What the device reports as it runs. A configuration does not store these.

| Parameter | Short | Value | What it is |
|---|---|---|---|
| [`endaddr`](#size-endaddr) | `ea` | octal | last address the card answers |
| [`readlamp`](#readlamp-writelamp) | `rdl` | 0 or 1 | machine is reading the card |
| [`writelamp`](#readlamp-writelamp) | `wrl` | 0 or 1 | machine is writing the card |

It also carries the parameters under [parameters every device has](index.md#parameters-every-device-has).

### In detail

#### `startaddr`

The first address the card answers, octal and even. Changing it, or `size`, moves the card: a card in the machine gives up its range and takes the new one, probed as `probe` says, and the range it takes up comes up cleared. A range that is refused leaves the card where it was.

#### `size`, `endaddr`

How much memory the card carries, written as a card is described — `256 KB`, `2 MB`, or a count of bytes — and read back that way. `endaddr`, the last address the card answers, follows from it and `startaddr`. The default covers everything the bus leaves to memory, which is the whole machine on a backplane that carries no memory of its own.

The range has to end below the I/O page. On a 22-bit QBUS it also has to end below 17360000: the top 128 KB under the I/O page belong to the processor module, where a KDJ11 answers its boot ROM without a bus cycle, so nothing on the bus can see the collision. A range that breaks either rule is refused, and the error says why.

#### `readlamp`, `writelamp`

Lit while the machine reads or writes the card, one lamp per direction.

#### `probe`

Before the card claims a range it reads the range over the bus, and refuses when anything answers there: a range claimed over memory the machine already carries puts two cards on one bus cycle. The probe reads both ends and words in between. A range found clear is remembered on the board, and enabling the card on the same range again goes in on that record; moving the card probes again. Set this to 0 only where the probe cannot work: a bus with nothing on it to grant the board the cycles it probes with.

<!-- /device-parameters -->
