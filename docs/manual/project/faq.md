---
title: FAQ
description: Which BeagleBone, which backplane, powering the card outside a machine, and the usual reasons a machine will not start.
---

## Which BeagleBone will work?

The classic **BeagleBone Black** is what is supported. Others:

| | |
|---|---|
| **Black Wireless** | Community-tested. |
| **Green** | Not fully compatible and does not work out of the box — the GPIO impedance differs, which matters for nanosecond-timed PRU signals. It can be made to run with different terminator resistors on the PCB, but never reaches the Black's speed or safety margin. |
| **Enhanced (Blue)** | Would not make a network connection on a standard image. Probably needs an adapted Debian. |
| **White** | Not tested. Do not. |

## Can I power the card outside a machine?

Yes, and it is worth doing for software work rather than spending backplane hours
on it. The card runs on **+5 V only**, drawing well under 1.5 A — though
consumption rises sharply when it is driving bus terminator arrays.

| Option | |
|---|---|
| Bus +5 V — the silver capacitor leads or the `5VUB` pins | **Best.** Everything is powered and you should hear the delay relay click. |
| The BeagleBone's 5 V jack | Works, but the driver chips and the I²C panel power stay dead — they are fed from bus +5 V. |
| A strong (>2 A) USB supply into the BeagleBone's USB port | Works. |
| A PC's USB port | **No.** USB 2.0 data ports supply about 500 mA, enough for a bare bone but not for bone plus card. |

The green power LED indicates the BeagleBone's 3.3 V is good.

## Which backplanes take the card?

> **UNIBUS · UniBone**
>
> UniBone needs an **SPC** slot. Four kinds of backplane look identical from the
> front:
>
> 1. **Standard expansion backplanes** like the DD11-DK — SPC slots in every socket
>    row on slots C–F. What you want.
> 2. **CPU backplanes** — special slots for processor boards, then perhaps rows
>    wired for core memory (11/05) or local memory (11/44, 11/84). Whatever is left
>    over is SPC, enough for a console and a boot device.
> 3. **Specially wired controller backplanes** for older multi-board controllers
>    (RK11, DL11). No SPC slots at all.
> 4. **QBUS backplanes.** Do not even think about it.

> **QBUS · QBone**
>
> Any quad QBUS slot. The complication is not which backplane but the C/D rows and
> the slot ordering — see
> [Fitting it to a backplane](../hardware/fitting-the-card.md#backplane-geometry).

## The bus latch test fails on SACK

> **UNIBUS · UniBone**
>
> The stress test drives every bus line to a random state and reads them all back.
> An M9302 or any terminator with active SACK logic will fail it — the terminator
> is driving the line. Use a **passive** terminator: resistors only, no logic and
> no boot ROM.

## Will it be damaged if I pull the power without shutting down?

In practice, no. The journalled filesystem tolerates it and SD cards have some
protection of their own; QUniLators have been power-cycled hundreds of times without
harm. A clean shutdown is still tidier.

## It boots, but I do not see the QUniLator software

The BeagleBone has an on-board eMMC carrying a factory Debian, and if anything is
wrong with the SD card or the boot selection it will come up on that instead.

**When the BeagleBone is unplugged from the card it loses the resistors that encode the
boot device, and will always boot from eMMC.** So the BeagleBone must be on the card to
boot your image at all. Holding the **S2** button while applying power forces SD
boot.

## My machine will not start with the card fitted

The card closes the grant chain **in software**, by watching the pins and
forwarding the signals. So while QUniLator is not running, the chain through its
slot is open — and an open chain with an active terminator raises SACK, which
allocates the bus and stops the machine.

The usual checks:

- Is the card actually running? Watch the LEDs
  ([Reading the LEDs](../start/install.md#reading-the-leds)).
- Are the continuity cards seated properly in every unoccupied slot?

> **UNIBUS · UniBone**
>
> Is the NPG chain closed at CA1–CB1 on every unoccupied row, and **open** in the
> slot UniBone occupies?

You can close the grant chain on the card with jumpers permanently — but then it
can do no interrupts and no DMA, which rules out device emulation.

A UniProbe or QProbe terminator shows open BG/NPG signals and a raised SACK on its
LEDs, which turns this from guesswork into a glance.

## XXDP boots from an emulated drive but nothing else does

XXDP's disk drivers — the RL11 one at least — use DMA but **no interrupts**. So a
fault on the BG4–7 interrupt grant chain does not affect it.

In other words: XXDP is tolerant of missing interrupt continuity cards, and
intolerant of a broken DMA chain. If XXDP boots and your operating system does
not, look at the interrupt grant chain first.

## A disk image boots and then crashes

Two popular causes beyond the grant chain:

1. **The operating system does not match the machine.** RSX in particular is
   generated for target hardware — an RSX built for an 11/34 will not run on an
   11/84. Model your physical machine in SimH and boot the image there first.
2. **Missing line-time clock.** Unixes will not boot without one, and RSX needs it
   for task scheduling. See
   [The line-time clock](../hardware/fitting-the-card.md#the-line-time-clock).

## Do I need a boot ROM in the machine?

Not necessarily. QUniLator can emulate the bootstrap — an M9312, an MRV11-D or an
MXV11-B2 — so a machine whose ROM is missing or wrong for the device can still
boot. A configuration from the
[catalogue](https://qunilator.com/configurations/) brings whatever it needs with
it.

## Where do I ask something not answered here?

- [GitHub issues](https://github.com/QUniBone/QUniLator/issues) for the software.
- The [UniBone Google group](https://groups.google.com/forum/#!forum/unibone) for
  the hardware and general discussion.
