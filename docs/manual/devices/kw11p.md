---
title: KW11-P — programmable real-time clock
description: The emulated KW11-P programmable interval clock, what it does and does not pass, and every parameter it takes.
---

The **KW11-P** (M7228) is DEC's programmable real-time clock: a 16-bit counter
that counts up or down at a rate the program picks, and interrupts when it
passes zero — once, or again and again from a preset. Programs use it for
intervals finer than the line clock's 50 or 60 a second, and diagnostic
supervisors use it to time what they test. QUniLator models it from the KW11-P
manual, DEC-11-HPWB-D. It is listed as `kw11p`, of type `kw11p_c`, and ships
disabled.

## What the machine sees

| | |
|---|---|
| **Registers** | CSR 772540, count set buffer 772542, counter 772544 |
| **Interrupt** | Vector 104, BR6 |
| **Rates** | 100 kHz, 10 kHz, line frequency (`Line frequency`), external. The board has no external input, so a clock set to that rate does not count |
| **Modes** | Single interrupt: the clock stops at zero, clearing counter and buffer. Repeat: the counter reloads from the buffer and keeps going; a second interrupt before the program has read the CSR sets ERROR (bit 15) |
| **Buffer** | Write-only. Writing it loads both buffer and counter; reading it gives 0 |
| **FIX** | CSR bit 5 steps the counter by one count |
| **Bus INIT** | Clears the CSR, the counter and the buffer |

The interrupt is placed at the instant the count reaches zero, computed from
the time the clock started. The counter the program reads while the clock runs
is brought up to date at least every 20 ms; on a stopped clock it reads exactly.

## Using it

Enable `kw11p`. It needs nothing else, and has no line or file behind it. A
program that selects the line-frequency rate counts at `Line frequency`, which is
60 unless set otherwise.

## What has been verified

- With the clock enabled, XXDP boots from an RL02 and the RLV12 diagnostic
  CZRLG (ZRLGE0) reaches its end of pass with no errors.
- DEC's own KW11-P test **ZKWB** (`ZKWBJ1.BIC`, CZKWBJ) **does not pass**. It
  hangs in its counter tests, which write the counter and read it straight back
  tens of thousands of times: one read in all of them returns a stale value, at a
  different count on every run. The cause is not established. The project's
  findings are in [`docs/kw11p-zkwb-findings.md`](https://github.com/QUniBone/QUniLator/blob/main/docs/kw11p-zkwb-findings.md).

ZKWB prints its banner once at the start and again on every pass it completes,
so a banner followed by silence is the hang, not a pass.

## Parameters

<!-- device-parameters: kw11p_c -->
<!-- Written by docs/site/tools/device-params.mjs from the C++ source. Edit the source, then run it. -->

### Settings

What a configuration stores and an operator sets.

| Parameter | Short | Value | Default | What it is |
|---|---|---|---|---|
| [`Line frequency`](#line-frequency) | `freq` | number | 60 | line-frequency rate: 50/60 Hz |

It also carries the parameters under [parameters every card on the bus has](index.md#parameters-every-card-on-the-bus-has).

### In detail

#### `Line frequency`

The rate the counter counts at when the program selects the line-frequency rate (CSR bits 2:1 = 10): 50 or 60 Hz; another value is accepted with a warning, and 0 counts at 60. The 100 kHz and 10 kHz rates do not use it. A change takes effect at once, also on a clock that is running.

<!-- /device-parameters -->
