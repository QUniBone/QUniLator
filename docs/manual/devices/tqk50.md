---
title: TQK50 — TMSCP tape controller with TK50
description: The emulated TQK50 tape controller, its TK50 cartridge drives, and every parameter they take.
---

The **TQK50** is DEC's controller for the TK50 cartridge tape. It speaks TMSCP,
the tape dialect of MSCP, through the same two registers and command and response
rings as the [UDA50](uda50.md) disk controller, and QUniLator builds it on the
same MSCP port. Each TK50 drive behind it holds a tape image in SIMH's `.tap`
format. It is listed as `tqk50`, of type `TQK50`, with four drives `tqk500` to
`tqk503`, and ships disabled.

> **UNIBUS · UniBone**
>
> The same model answers on a UniBone, at 774500. DEC's UNIBUS card for the
> TK50 is the TUK50.

## What the machine sees

| | |
|---|---|
| **Registers** | IP at 774500 and SA at 774502 (17774500 and 17774502 on a QBone) |
| **Interrupt** | BR5, at the vector the host gives the controller when it initialises it |
| **Transfers** | Commands, responses and data move by DMA to and from the host's memory |
| **Drives** | Four TK50 units, 0 to 3. A unit without a tape is reported as not available |
| **Identity** | Controller class 1, model 9 (TQK50) |
| **Tape** | Records and tape marks, read, written, spaced over and rewound. A cartridge holds 94 MB, set by the drive's `capacity` |

When a command leaves the tape past `capacity`, the drive reports end of tape
once for that crossing, which is the signal a host writing a full tape turns
around on. A write that reaches the end also latches a serious exception, which
the host clears before it carries on.

## Using it

1. Enable `tqk50` on the **Configurations** screen.
2. Set a drive's `image` to a `.tap` file and enable the drive. A tape that does
   not exist yet is created empty; an image file without write permission mounts
   write-locked, and the drive's WRITE PROTECT lamp lights.
3. The host brings the unit online and uses it. Changing the image unloads the
   tape and takes the unit offline.

The drive writes straight into its image file. A tape that has to stay as it is
belongs in a file without write permission.

A smaller `capacity` models a shorter tape, which bounds the run time of a
diagnostic that writes the whole cartridge.

## What has been verified

- DEC's TKxx data reliability diagnostic **CZTKBD0** runs under XXDP on a QBone pass after
  pass with 0 errors: it fills the tape to the end, reads every record back and
  compares it. The runs used a shortened `capacity`.

## Parameters

<!-- device-parameters: tqk50_c -->
<!-- Written by docs/site/tools/device-params.mjs from the C++ source. Edit the source, then run it. -->

It has no parameters of its own.

It also carries the parameters under [parameters every MSCP controller has](uda50.md#parameters-every-mscp-controller-has).

On this device, `base_addr` can be set.

<!-- /device-parameters -->

## Drive parameters

<!-- device-parameters: mscp_tape_c -->
<!-- Written by docs/site/tools/device-params.mjs from the C++ source. Edit the source, then run it. -->

### Status

What the device reports as it runs. A configuration does not store these.

| Parameter | Short | Value | What it is |
|---|---|---|---|
| [`writeprotectlamp`](#writeprotectlamp) | `wpl` | 0 or 1 | State of WRITE PROTECT lamp |

It also carries the parameters under [parameters every drive has](index.md#parameters-every-drive-has).

On this device, `capacity` can be set.

### In detail

#### `writeprotectlamp`

The drive's WRITE PROTECT lamp: lit while the cartridge is write-locked, because its image file is read-only or the host has locked the unit in software. The dashboard shows it beside the ACCESS lamp, `accesslamp`, which lights while the tape moves or transfers.

<!-- /device-parameters -->
