---
title: System
description: Who reaches this QUniLator, what it is called on the network, which serial ports carry a login, and installing updates.
sidebar:
  order: 7
---

Who reaches this QUniLator, what it runs, and updating it.

![The system page](../assets/screenshots/system.jpg)

## Access

The name and password from the first-run dialog — one identity for the web
interface, the file shares and `ssh`. Changing either takes effect on the next
request, so the browser will ask again.

Changing the **user name** also moves the board's file-share account to the new
name. A name is 1 to 32 characters: a lower-case letter or underscore, then
lower-case letters, digits, underscores and hyphens.

> [!NOTE]
> **A board set up another way**
>
> If the credentials came from `WEBUI_PASSWORD` in the environment rather than
> from this page, the page says so and will not change them — that setting
> outranks it and is managed outside the interface.

## Board

**Name** is what this QUniLator answers to on the network. Renaming it moves
`qbone.local`, the DNS-SD advertisement, the DHCP lease and the login banner
together. Letters, digits and inner hyphens, up to 63 characters.

**SSH key** installs a public key for the account, which is how you reach a shell
and `sudo` there. Installing one replaces the key already present.

## Serial ports

Which of the BeagleBone's three UARTs carry a Linux login.

| | |
|---|---|
| `ttyS0` | the debug header, J1 pins 4/5 — needs a 3.3 V TTL adapter. Also the **kernel console**. |
| `ttyS1` | the cape connector. |
| `ttyS2` | the cape connector — and normally **held by the external console bridge**. |

**One port always keeps its login**: it is the way back onto a card whose network
has gone, so switching the last one off is refused. The USB port carries a login
of its own as well. A port the emulator is holding cannot take one either — free
it by turning the device or the
[console bridge](machine.md#external-console) off first.

> [!NOTE]
> **The kernel console moves with the login**
>
> Turning a login off moves where the kernel prints, and that takes a reboot to
> settle. Until then the old port keeps printing; the page says so when a reboot
> is owed.

## Console recordings

Sessions captured with **Record** on the [dashboard](dashboard.md#console) — both
what the machine printed and what was typed at it, whoever typed it. Each can be
downloaded or deleted.

## Updates

What the installed package is, and what the repository offers.

**Check now** asks; **Show what changed** fetches the changelog before you
commit. Installing replaces the emulator and **restarts the service, so the
running machine stops** — the page says so before it starts.

An update returns QUniLator to its **DIP-selected configuration**, so a
configuration applied by hand needs re-applying afterwards.

**Dismiss** stops a version being announced in the sidebar badge; **Announce it
again** undoes that.

**Operating system** is everything else on the board, the emulator excluded.
Packages **held back** are listed rather than forced — the kernel and the cape
support are pinned deliberately. Some upgrades want a reboot, and the page says
which.
