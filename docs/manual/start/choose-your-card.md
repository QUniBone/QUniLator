---
title: Choose your card
description: UniBone or QBone — which card your machine takes, and what differs between them.
sidebar:
  order: 2
---

The card follows from the backplane, not from preference. Look at the machine you
mean to put it in.

## Which bus is in the box

| Your machine | Bus | Card |
|---|---|---|
| PDP-11/04, /05, /34, /35, /40, /44, /45, /55, /70 | UNIBUS | **UniBone** |
| PDP-10 and VAX expansion backplanes | UNIBUS | **UniBone** |
| LSI-11/03, PDP-11/23, /23+, /53, /73, /83, /93 | QBUS | **QBone** |
| MicroVAX I, II, 3000-series | QBUS | **QBone** |

If you are unsure, count the fingers. A UNIBUS SPC slot takes a card on all four
rows A–D; a QBUS card cage carries the bus on rows A and B, with C and D varying
by backplane — see [Fitting it to a backplane](../hardware/fitting-the-card.md).

Both cards are quad-height, both carry a BeagleBone Black, and both run the same
QUniLator software.

## Where they differ

Most of this manual applies to either card. These are the differences that reach
the operator:

### Address width

> **UNIBUS · UniBone**
>
> UNIBUS is fixed at **18 bits** — 256 KB of address space. The larger PDP-11s
> (11/44, 11/70) reach 22 bits only over a separate memory bus, which UniBone does
> not sit on.

> **QBUS · QBone**
>
> QBUS runs at **16, 18 or 22 bits**, multiplexed onto the DAL lines, and the CPU
> determines which. QBone cannot infer it from bus traffic, so it is configured —
> and on an LSI-11/03 the `DAL<21:18>` jumpers must come off, because those
> backplane lines carry CPU-internal signals there.
>
> Getting this wrong shows up as memory that answers at the wrong addresses, or an
> I/O page that does not answer at all.

### An emulated processor

> **UNIBUS · UniBone**
>
> UniBone can be the CPU. It emulates a **KA11** (11/20) and a **KD11-EA** (11/34
> with KT11-D memory management), both validated against the original DEC MAINDEC
> diagnostics. A backplane with no working processor still becomes a running
> machine.

> **QBUS · QBone**
>
> QBone ships no emulated processor — it drives a real CPU board in the cage beside
> it. Everything else QUniLator emulates is available on both cards.

### Grant continuity

> **UNIBUS · UniBone**
>
> UniBone doubles as a G727 grant continuity card, with an NPG-closing function.
> The slot it goes into wants an **open NPG chain at CA1–CB1**.

> **QBUS · QBone**
>
> QBone's C/D grant continuity is jumpered, and the correct setting depends on the
> backplane — an H9270 or H9275 wants the jumpers closed, an H9276 open, an H9278
> depends on the slot. Empty QBUS slots between the CPU and the last used slot
> still need G9047 grant continuity cards.

### The line-time clock

> **QBUS · QBone**
>
> QBUS carries **EVNT**, which DEC power supplies drive with a 50/60 Hz square wave
> to give the operating system a line-time clock. QBone can generate 50 Hz itself
> via a jumper, for a machine on a non-DEC supply — but only one source may drive
> EVNT at a time.

### Diagnostic companion

The signal adapter that pairs with the card differs by bus: **UniProbe** for
UNIBUS, **QProbe** for QBUS. Both are debugging terminators with signal LEDs and
logic-analyser connectors.

## A note on maturity

QBone runs on real hardware and is regularly exercised. **UniBone is currently
untested in this codebase** — it cross-compiles, CI builds and packages it, and
its image comes off the same script, but none of it has been run on a UniBone in
a live backplane. The DEUNA Ethernet emulation in particular has never been run
against an operating system.

If you have UNIBUS hardware, reports are very welcome — see
[the project](../project/roadmap.md).
