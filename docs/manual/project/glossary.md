---
title: Glossary
description: What this manual means by QUniLator, UniBone, QBone, card and BeagleBone, and the handful of other words that carry a particular meaning here.
---

Three of the names in this project end in the same syllable, and several ordinary
words — card, device, machine — mean something narrow here. This page fixes them.

## The four names

| | |
|---|---|
| **QUniLator** | The software, and a running installation of it. It drives a DEC bus in real time and presents emulated hardware to the machine around it. Countable, because an installation has an address and a name of its own: *two QUniLators on the same LAN*. |
| **UniBone** | The card that drives UNIBUS. |
| **QBone** | The card that drives QBUS. |
| **card** | Either module, where the bus does not matter — a quad-height PCB that occupies a backplane slot and carries the BeagleBone. |
| **BeagleBone** | The BeagleBone Black single-board computer the card carries, and the computer QUniLator runs on. Written in full. |

Named by what each one is: QUniLator is what runs, the card is what plugs in, the
BeagleBone is what it plugs into.

## Words with a narrow meaning

| | |
|---|---|
| **machine** | The PDP-11, PDP-10 or VAX the card is fitted to — the computer around the card, never the card itself. |
| **device** | One piece of emulated DEC hardware: a controller, a drive, a terminal line, a block of memory, a bootstrap ROM. An RL11 is a device; so is each RL02 hanging off it. |
| **widget** | One element of the web interface's dashboard — the console, a drive's status, the front panel. |
| **configuration** | The set of devices a machine carries, with their parameters: what QUniLator installs on the bus when the machine is switched on. |
| **bundle** | A configuration packaged with its media as a `.qcfg.zip`, so a whole working machine travels as one file. |
| **catalogue** | A published list of bundles that a QUniLator can subscribe to and import from. |
| **SD card** | The memory card the release image is written to. Always named in full, so it is never confused with the card in the backplane. |
| **bus** | UNIBUS or QBUS — the backplane wiring the card drives. Most of this manual applies to both. |

## Names you will meet elsewhere

| | |
|---|---|
| **QUniBone** | The GitHub organisation, and the older combined name for the two cards, from when the software had no name of its own. |
| **board** | Older issues, mailing-list postings and the original retrocmp articles use this for a running QUniLator, and sometimes for the bare PCB. |
| **demo** | The interactive menu application (`unibone-demo`, `qbone-demo`) that the web interface replaced. The [acceptance test](../start/acceptance-test.md) still uses it, because that is hardware-level work. |
