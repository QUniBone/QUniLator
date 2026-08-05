---
title: Credits and licence
description: Who built this, whose photographs these are, and the terms everything comes under.
---

## The project

QUniLator began as **QUniBone**, designed and built by **Jörg Hoppe** — the
hardware, the PRU bus logic, the emulation framework and the original
documentation on [retrocmp.com](https://retrocmp.com/). Both cards are his design,
and his copyright is retained throughout.

The [`QUniBone/QUniLator`](https://github.com/QUniBone/QUniLator) repository is
the community-maintained mainline. Jörg's original reference codebase is preserved
at [`QUniBone/QUniBoneClassic`](https://github.com/QUniBone/QUniBoneClassic).

Device emulations contributed by others include the RK11/RK05 subsystem, the MSCP
disk subsystem, the RS11/RF11 DECdisk and the KE11 EAE, all by **Josh Dersch**,
and the PDP-11/20 processor emulation by **Angelo Papenhoff**.

The KiCad symbol for the quad Flip-Chip board came from Malcolm at
[avitech.com.au](http://avitech.com.au/).

## How the software is maintained

The code in the QUniLator repository is written and maintained by AI agents. The
maintainer directs and reviews the work; the code itself is largely
agent-authored. This is the ongoing method rather than a one-off experiment, and
it is stated plainly so that anyone who would rather not run software produced
that way can make that choice — Jörg's original codebase remains available at
QUniBoneClassic.

## Photographs

Every photograph on this site is by **Jörg Hoppe**, from the UniBone and QBone
articles on retrocmp.com, used with permission.

The oscilloscope traces of bus ringing and its correction are from DEC's
[UNIBUS Troubleshooting
manual](http://www.bitsavers.org/pdf/dec/unibus/UnibusTroubleshooting.pdf), via
bitsavers.

## Licence

Hardware and software are both **BSD 2-Clause**. You may forward, modify and sell
any of it, as long as the copyright notice remains visible. There is no warranty
of any kind.

This documentation is published under the same terms.

## Sources

Much of this manual descends from Jörg's original articles:

- [UniBone](https://retrocmp.com/projects/unibone) — 18 articles
- [QBone](https://retrocmp.com/projects/qbone) — 5 articles

Pages describing the software as it is now have been rewritten rather than
migrated: the originals document the `demo` menu application, which the web
interface has since replaced. Pages about the hardware, the buses and the machines
are closer to the originals, because that material has not changed.

Thanks are also owed to the [QBUS information
page](http://web.frainresearch.org:8080/projects/pdp-11/), without which the
backplane geometry documentation would not exist.
