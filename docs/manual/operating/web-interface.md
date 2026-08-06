---
title: The web interface
description: What QUniLator serves on port 80, how you get in, and what each of the six screens is for.
sidebar:
  order: 1
---

Everything an operator does day to day happens here: switch the machine on, put
a pack in a drive, watch the console, save the machine you built. It is served on
port 80 by the emulator itself, so there is nothing to install on your
workstation and nothing to keep running there.

## Getting in

The first-run dialog creates **one identity that is both your account on the
BeagleBone and your web login**, so the same name and password reach the web
interface, the file shares of the image library, and an `ssh` session.

Every request takes it, and **the name is part of it** — the right password under
the wrong name is refused. [System](system.md) is where it is changed.

## The status bar

The bar along the top belongs to the whole interface, not to any one screen.

| | |
|---|---|
| **DCOK**, **POK** | the bus power signals. Both green is a machine with power on it. |
| **addr 22-bit** | the address width the bus is running at. |
| **connected** | the event stream. When it says *disconnected*, the page is showing you the past — everything on every screen is pushed over that one connection. |

## The six screens

| | |
|---|---|
| [**Dashboard**](dashboard.md) | The machine as it is now: the control panel, the console, and a widget per device. |
| [**Storage**](storage.md) | The library of disk and tape images, what is inside them, and their copy-on-write overlays. |
| [**Configurations**](configurations.md) | The machines this QUniLator knows — the device set as a saved document. |
| [**Machine**](machine.md) | Address width, and where the real machine's console line is read from. |
| [**Diagnostics**](diagnostics.md) | The live log. |
| [**System**](system.md) | Who reaches this QUniLator, what it is called, and updating it. |

## The machine comes up dark

A QUniLator that has just started serves a machine that is **switched off**. It
loads the configuration the DIP switches name, but puts none of it on the bus: no
card is installed, no register window answers, no emulated processor takes the
bus over.

That is deliberate — a card is fitted to a machine and configured afterwards, and
what it carries may describe a backplane it is no longer in. Switching the
machine on is an explicit act, on the [dashboard](dashboard.md#control-panel).

## What is not here

The hardware-level work — bus latches, master/slave transfers, interrupt tests,
the device exercisers — has no web equivalent and belongs to the interactive menu
application. See [Coming from QUniBone Classic](../start/from-qunibone.md).

Per-device **log levels** are not set here either; [Diagnostics](diagnostics.md)
shows the log but does not change what is emitted into it.
