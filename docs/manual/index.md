---
title: QUniLator
description: The software that turns a UniBone or QBone card into emulated DEC hardware, on a real UNIBUS or QBUS backplane.
template: splash
hero:
  tagline: Emulated DEC hardware on a real backplane. One system, two cards.
  image:
    file: ./assets/photos/qbone-upward.jpg
  # Relative, so the hero works at the site root and under a base path alike.
  actions:
    - text: What QUniLator is
      link: start/what-it-is/
      icon: right-arrow
    - text: Configuration catalogue
      link: configurations/
      icon: open-book
      variant: minimal
---

## One name for the software, two for the cards

**QUniLator** is the software. It runs on a BeagleBone Black and drives a DEC bus
in real time, presenting emulated controllers, drives, memory and terminal lines
to the machine around it.

It runs on two cards, and the card is what your backplane sees:

| | |
|---|---|
| **UniBone** | A quad SPC card for **UNIBUS**. Plugs into a PDP-11 with a quad SPC slot, and into PDP-10 and VAX expansion backplanes. |
| **QBone** | A quad Flip-Chip card for **QBUS**, on the A/B fingers. Plugs into any QBUS card cage, from an LSI-11/03 to a MicroVAX. |

The cards differ in the bus they drive and very little else: the same BeagleBone,
the same software, the same web interface, the same disk images. So this is one
manual. Where UNIBUS and QBUS genuinely part company the page says so, and the
**I have a** control in the sidebar keeps the navigation to the card you own.

> [!NOTE]
> **Where the other names come from**
>
> The GitHub organisation is called `QUniBone`, and the source repository
> `QUniLator`. "QUniBone" was the combined name for the two cards while the
> software had no name of its own. The distinction to hold on to is the one
> above: QUniLator is the software, UniBone and QBone are the cards it runs on.

## What it is for

| | |
|---|---|
| **Keep a broken machine running** | Emulate the subsystem that failed — memory, a disk controller, a terminal line — and the rest of the machine carries on working. |
| **Supply what you never had** | An RL02 pack, an MSCP disk, a DELQA on the LAN. Media are SimH-compatible files, so images move by network rather than by SD card. |
| **Diagnose the bus** | Trace bus traffic, stimulate a standalone device, dump a pack — the work a 56-channel logic analyser would otherwise be doing. |
| **Build a machine that never was** | Emulate cards in parallel up to a whole system, and hand the result to somebody else as one file. |

## Where to go next

- New to the project — [What QUniLator is](start/what-it-is.md), then
  [Choose your card](start/choose-your-card.md).
- Card in hand — [Installing the software](start/install.md) and the
  [Acceptance test](start/acceptance-test.md).
- Looking for a machine to run — the
  [configuration catalogue](https://qunilator.com/configurations/).
- Wondering what is not written yet — [What is coming](project/roadmap.md).
