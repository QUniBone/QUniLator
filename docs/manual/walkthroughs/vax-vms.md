---
title: VMS on an emulated VAX-11/780
description: Run VAX/VMS on a UniBone with no machine around it at all — QUniLator supplies the processor, the memory, the disk and the Ethernet.
sidebar:
  order: 3
---

> [!NOTE]
> **Verified on real hardware**
>
> QUniLator **1.16.0-1**, 2026-08-06, on a UniBone with **no backplane** — the
> card on a bench, driving its own internal bus. VMS **V4.7** booted from the
> emulated UDA50 to a login prompt.

## What you will have at the end

VAX/VMS V4.7, booted from a disk that is a file, on a processor that is
software, on a bus with nothing else on it — running on a BeagleBone with a
UniBone cape and no PDP-11 in sight.

The other walkthroughs put QUniLator *into* a machine. This one is the machine.

## The machine

| | |
|---|---|
| **Processor** | **Emulated** VAX-11/780 (`cpuvax`), 4 MB |
| **Backplane** | None. The card runs on its **internal bus** — no cage, no cards, no terminator |
| **From QUniLator** | Everything: processor, memory, a UDA50 with one drive, a DEUNA, a DZV11 |

This is the configuration to reach for when you have a card but no machine yet —
it exercises the whole emulation stack and needs nothing but power and a
network cable.

> **UNIBUS · UniBone**
>
> An emulated processor is UNIBUS-only. A QBone drives a real CPU board beside
> it and ships no processor emulation, so this walkthrough has no QBUS
> equivalent.

## What you need

- A UniBone with QUniLator on it. It need not be in a backplane.
- A **VMS system disk image** — `vms47.dsk` here, about 147 MB.
- Nothing else. There is no console cable: the emulated VAX has its own console,
  and the browser is the terminal.

## 1. Tell the card it has no bus

On **Machine**, the card must be running on its **internal bus** — the emulated
processor and the emulated devices talk to each other inside the software, and
nothing is driven onto the physical connector.

Set **External console** to **Off**. The VAX's console is its own, not a serial
line on the cape.

## 2. Build the machine

On **Configurations**, five devices:

| Device | Setting |
|---|---|
| `cpuvax` | the VAX-11/780 processor, `memory` = 4 (MB) |
| `uda` | the UDA50 MSCP controller |
| `uda0` | the system disk, holding `vms47.dsk` |
| `deuna` | Ethernet, on interface `veth-pdp` |
| `dzv11` | a serial mux, for terminals beyond the console |

The saved configuration on this card is called `vax`.

## 3. Switch it on

Press **AUX ON/OFF**. The console to watch is the VAX's own — the console widget
on the dashboard shows it.

VMS boots straight through and stops to ask the time:

```
   VAX/VMS Version V4.7 28-Oct-1987 13:00

PLEASE ENTER DATE AND TIME (DD-MMM-YYYY  HH:MM)
```

> [!TIP]
> **It always asks, every boot**
>
> There is no battery-backed clock behind an emulated processor, so VMS has
> nothing to read the time from and asks. Answer in exactly the format shown —
> `06-AUG-2026 08:00`. A malformed answer is asked again.

Startup then runs to completion:

```
%%%%%%%%%%%  OPCOM   6-AUG-2026 08:01:14.40  %%%%%%%%%%%
Logfile has been initialized by operator _OPA0:
Logfile is SYS$SYSROOT:[SYSMGR]OPERATOR.LOG;23

%SET-I-INTSET, login interactive limit = 64, current interactive value = 0
  SYSTEM       job terminated at  6-AUG-2026 08:01:17.07
```

`SYSTEM job terminated` is the startup batch job finishing normally, not an
error. Press **Return** and you have a login:

```
Username:
```

The system disk here carries the traditional `SYSTEM` account.

## What can go wrong

| | |
|---|---|
| **Nothing on the console at all** | The console widget is showing the wrong line. An emulated VAX talks on its own console channel, not on `ttyS2`; check *External console* is **Off**. |
| **The date prompt returns immediately** | The format is exact: two-digit day, three-letter month, four-digit year, two spaces before the time is not required but the dashes are. |
| **It boots about half the time** | Fixed. An interrupt granted with a zero vector at batch entry used to wedge the boot; if you see it, you are on a build older than the fix. |
| **The card is in a backplane and behaves oddly** | An emulated processor takes the bus over. Do not leave a real CPU in the cage with it. |

## Where this one goes next

This configuration is the project's own proving ground for the UNIBUS side, and
the emulated processors are validated against DEC's original MAINDEC diagnostics
on every build. What has *not* been exercised much is a UniBone in a live,
terminated backplane driving real cards — if you have UNIBUS hardware, that is
the report the project most wants.
