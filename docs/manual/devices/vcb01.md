---
title: VCB01 — QVSS monochrome graphics
description: The emulated VCB01 (QVSS) bitmap display with its keyboard and pointer, where its video memory goes, and every parameter it takes.
bus: [qbus]
---

The **VCB01** (M7602), also called the QVSS, is the monochrome video board of
the VAXstation II: a 1024 × 864 bitmap in 256 KB of video memory on the QBUS, a
6845 CRT controller, a hardware cursor, an interrupt controller, and a serial
chip carrying an LK201 keyboard and a mouse. QUniLator presents its registers
and its video memory to the processor and draws the screen in the browser, and
on an X server when one is named. It is listed as `vcb01`, of type `VCB01`.

> **QBUS · QBone**
>
> The VCB01 is a QBUS card. A UniBone has no emulated display.

## What the machine sees

| | |
|---|---|
| **Registers** | 32 words at 17777200: the CSR, cursor position, pointer position, the CRT controller's address and data, the interrupt controller, and from base+40 the SCN2681 serial chip |
| **Video memory** | 256 KB at `bank` × 256 KB — 16000000 to 16777776 with the default bank 016. The bank reads back in CSR bits 11 to 14 |
| **Screen** | 1024 pixels wide. The height follows what the driver programs into the CRT controller, 864 lines until it does |
| **Keyboard** | An LK201 on channel A of the serial chip |
| **Pointer** | A mouse on channel B, its buttons in the CSR. Base+4 and base+6 also give the pointer's absolute position on the screen, which the original board does not have; bit 15 of base+4 marks it present |
| **Interrupts** | BR4, through the board's interrupt controller, which gives each source — serial chip, vertical sync, pointer, buttons — the vector the driver programs. Vertical sync comes 60 times a second |

## Using it

1. Find room for the video memory. The bank needs a free 256 KB in the 22-bit
   address space. The default, 016, sits just below the I/O page, so the
   machine's memory must end below 16000000 (3.5 MB). A memory card that fills
   the whole 4 MB space collides with every bank; the board's own `MEM` device
   and the video memory are served out of two separate ranges that may not
   overlap. `GET /api/memory/map` shows what answers where.
2. Add `vcb01` on the **Configurations** screen. To have the screen on an X
   server as well as in the browser, set `display` to that server, such as
   `myhost:0`.
3. Enable it. The **VCB01** card on the dashboard shows the screen; click it to
   give it keyboard and pointer.

`bank` and `display` can only be changed while the card is disabled. Enabling
is refused when the bank cannot be served or a named display cannot be reached,
and the log says which.

2.11BSD carries no driver for it. The QVSS driver, `qv`, is part of the VAX
BSD and Ultrix systems.

## What has been verified

- A standalone PDP-11 program, [`vcbdemo.mac`](https://github.com/QUniBone/QUniLator/blob/main/tools/vcbdemo.mac), run on a real PDP-11/73,
  programs the CRT controller, draws a test pattern into video memory and turns
  the video on; the pattern appears on the X display. A key pressed shows its
  LK201 code on the screen, and moving the pointer over the window leaves a
  trail behind the hardware cursor.
- DEC wrote no XXDP diagnostic for the VCB01, so programs that drive it are its
  test.

## Parameters

<!-- device-parameters: vcb01_c -->
<!-- Written by docs/site/tools/device-params.mjs from the C++ source. Edit the source, then run it. -->

### Settings

What a configuration stores and an operator sets.

| Parameter | Short | Value | Default | What it is |
|---|---|---|---|---|
| [`display`](#display) | `disp` | text | *empty* | X display carrying the screen, e.g. "host:0" |
| [`bank`](#bank) | `bk` | octal | 16 | video memory bank, 256 KB each: address = bank * 1000000 |
| [`refreshrate`](#refreshrate) | `rr` | number, Hz | 30 | screen updates per second |
| [`largemonitor`](#largemonitor) | `lm` | 0 or 1 | 1 | report a 19-inch monitor rather than a 15-inch one |

It also carries the parameters under [parameters every card on the bus has](index.md#parameters-every-card-on-the-bus-has).

### In detail

#### `display`

An X server to show the screen on as well, as an X display name such as `host:0`; a host name is resolved on the board before connecting. Empty, the screen is drawn to the web interface only, where the VCB01 card on the dashboard shows it and takes keyboard and pointer. Named, the screen opens in an X window when the card is enabled, and keyboard and pointer work there too; a display that cannot be reached refuses the enable. Can be changed only while the card is disabled.

#### `bank`

The card's memory bank switch: the 256 KB of video memory answer at `bank` x 256 KB, 0 to 016, and a driver reads the setting back from CSR bits 11 to 14. 016 puts the bitmap at 16000000..16777776, the highest bank below the I/O page. The bank needs a free stretch of bus address space: the machine's own memory has to end below it, and the `MEM` device's range may not overlap it - enabling the card refuses a bank that collides with `MEM`. Enabling clears the video memory. Can be changed only while the card is disabled.

#### `refreshrate`

How often the screen is redrawn, 1 to 60 times a second. Each pass sends only the lines that changed, and keyboard and pointer input are taken in at the same pace. The vertical sync interrupt a driver sees runs at 60 Hz whatever this is set to.

#### `largemonitor`

The monitor size the card reports in CSR bit 0: on, a 19-inch monitor that shows all 864 lines; off, a 15-inch one. Set it while the card is disabled.

<!-- /device-parameters -->
