---
title: Switch and display register
description: The console switch and display register at 777570, the dashboard card that shows it, and every parameter it takes.
---

A PDP-11 console carries a **switch register** and a **display register** at
one address, 777570. A program reads the switches the operator has set and
writes a word to the display lights. XXDP diagnostics take their options from
the switches, and many programs show progress or an error code on the display.
A Q-bus processor has neither, so QUniLator supplies both on the bus. The
device is listed as `dispreg`, of type `dispreg_c`, and ships disabled.

## What the machine sees

| | |
|---|---|
| **Register** | 777570. A read returns `switches`, a write sets `display` |
| **Interrupt** | None |
| **Bus INIT** | Leaves both standing, so a diagnostic's last code stays readable after it stops |
| **Power down** | Clears the display |

Enabling it probes 777570 first and refuses when something already answers
there. An emulated PDP-11 processor on a UNIBUS board carries its own switch
register at 777570 and answers it without a bus cycle, so with one of those
enabled the device refuses as well.

## Using it

Enable `dispreg` and add its card to the dashboard. The card shows the display
as sixteen lamps grouped by octal digit, with the octal value beside them, and
the switches as sixteen toggles with their value in a field: click a toggle, or
type an octal word and press Enter.

## Parameters

<!-- device-parameters: dispreg_c -->
<!-- Written by docs/site/tools/device-params.mjs from the C++ source. Edit the source, then run it. -->

### Settings

What a configuration stores and an operator sets.

| Parameter | Short | Value | Default | What it is |
|---|---|---|---|---|
| [`switches`](#switches) | `sw` | octal | 0 | Console switch register, read at 777570. |

### Status

What the device reports as it runs. A configuration does not store these.

| Parameter | Short | Value | What it is |
|---|---|---|---|
| [`display`](#display) | `dsp` | octal | Console display register, written at 777570. |

It also carries the parameters under [parameters every card on the bus has](index.md#parameters-every-card-on-the-bus-has).

### In detail

#### `switches`

What a program reads at 777570: the console switches, set by the operator from the dashboard or the API. A change reaches a running program at its next read.

#### `display`

The word a program last wrote to 777570, as the display lights show it. Bus INIT leaves it standing, so a diagnostic's last code stays readable after the program has stopped; powering the machine down clears it.

<!-- /device-parameters -->
