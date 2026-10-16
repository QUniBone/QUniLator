---
title: DELQA — QBUS Ethernet controller
description: The emulated DELQA (M7516) Ethernet controller, how it reaches the LAN, and every parameter it takes.
bus: [qbus]
---

The **DELQA** (M7516) is DEC's Ethernet controller for the QBUS. QUniLator
presents its register file, its buffer descriptor lists and its interrupt to the
processor as the card does, and moves the frames to and from the board's own
network, so the machine stands on the LAN with an Ethernet address of
its own. It is listed as `delqa`, of type `DELQA`.

> **QBUS · QBone**
>
> The DELQA is a QBUS card. A UniBone carries the UNIBUS equivalent, the
> [DEUNA](deuna.md).

[2.11BSD on an MSCP disk, on the LAN](../walkthroughs/211bsd-network.md) puts a
real PDP-11/53 on the network through it, step by step.

## What the machine sees

| | |
|---|---|
| **Registers** | Eight words at 17774440, the address of the first Ethernet controller: the station address PROM in the first six, which also take the receive and transmit list addresses, then the vector address register and the CSR |
| **Mode** | A DELQA in normal mode, which a driver may switch to the DEQNA register model. With `deqna_lock` set it is a DEQNA from power-up |
| **Station address** | `mac`, read out of the address PROM. Each board has its own, starting `08:00:2b` |
| **Interrupt** | BR4, at the vector the driver writes into the vector address register. Until a driver writes one, the controller does not interrupt |
| **Transfers** | Frames and descriptor lists move by DMA, over the full 22-bit QBUS address space |
| **Network** | The board's interface `interface`, `veth-pdp` unless set otherwise |

The vector is the driver's to choose, as on the card, so `intr_vector` takes no
setting on this device.

## Using it

1. Add `delqa` on the **Configurations** screen. The defaults suit a first
   Ethernet controller and need no change.
2. Enable it. The controller attaches to `veth-pdp` and the machine's driver
   finds it at 17774440.

The board joins `veth-pdp` to the bridge that carries its own Ethernet, so the
machine reaches every host on the LAN, DEC multicast included, and the board
itself. `interface` and `deqna_lock` are read when the controller is enabled; to
change one, disable the controller, change it, and enable it again.

For DEC's diagnostic, set `loopback`: the controller then behaves as one with a
loopback connector on its transceiver port, and is off the network until it is
cleared.

## What has been verified

- DEC's DELQA diagnostic CZQNA (`ZQNAJ0.BIC`, the DEQNA-DELQA-DESQA functional
  test on the XXDP 2.5 pack) passes with no errors on a QBone, run with
  `loopback` set and answering Y to "Test in External Loop Back Mode". Test 15
  needs a processor strapped for power-up mode 0 and is skipped.
- 2.11BSD on a real PDP-11/53 attaches it as `qe0`, goes multi-user on the LAN,
  and answers `ping` and `telnet` from other hosts.
- RSX-11M-PLUS V4.6 brings its DECnet line `QNA-0` up on it and sends DECnet
  router hellos and MOP loopback frames onto the LAN.

## Parameters

<!-- device-parameters: delqa_c -->
<!-- Written by docs/site/tools/device-params.mjs from the C++ source. Edit the source, then run it. -->

### Settings

What a configuration stores and an operator sets.

| Parameter | Short | Value | Default | What it is |
|---|---|---|---|---|
| [`interface`](#interface) | `if` | text | `veth-pdp` | host Ethernet interface shared with the emulated controller |
| [`mac`](#mac) |  | text | derived from the board's Ethernet address | station address, 6 hex bytes separated by ":" or "-" |
| [`deqna_lock`](#deqna_lock) | `dl` | 0 or 1 | 0 | DEQNA-lock mode: emulate a DEQNA (for RSTS/E and other DEQNA-only drivers) |
| [`loopback`](#loopback) | `lb` | 0 or 1 | 0 | loopback connector installed on the transceiver port |

### Status

What the device reports as it runs. A configuration does not store these.

| Parameter | Short | Value | What it is |
|---|---|---|---|
| [`activityled`](#activityled) | `al` | number | Number of LED to used for activity display. |
| [`activitylamp`](#activitylamp) | `actl` | 0 or 1 | State of activity lamp |

It also carries the parameters under [parameters every card on the bus has](index.md#parameters-every-card-on-the-bus-has).

### In detail

#### `interface`

The board's network interface the controller is attached to. `veth-pdp` is the one the QUniLator package creates for an emulated controller: its other end is joined to the bridge `br0` together with the board's own `eth0`, so the machine reaches both the LAN and the board itself. A raw attachment to the uplink reaches the LAN but cannot reach the board it runs on, which is why the controller has an interface of its own. Read when the controller is enabled; an interface that is not there yet is tried again every two seconds until it appears.

#### `mac`

The station address the address PROM gives the driver: six hex bytes separated by `:` or `-`. Each board derives its own the first time - DEC's prefix `08:00:2b` and the low three bytes of the board's own Ethernet address - and keeps it in `delqa.mac` in the board's state directory (`/var/lib/qunilator`), so it stays the same across boots and differs from board to board. A value set here overrides it and is stored with the configuration. A multicast address (low bit of the first byte set) is refused. Change it while the controller is disabled.

#### `activityled`

Which of the board's four activity LEDs, 0 to 3, traffic lights; 4 or more lights none. Drives take the LEDs by unit number, so 3 keeps the controller clear of a machine with one drive.

#### `activitylamp`

On while frames move in either direction, held a moment past each frame so the web interface sees it.

#### `deqna_lock`

The DEQNA-lock switch on the board. On, the controller powers up as a DEQNA and stays one, whatever a driver writes to the vector register, which is what RSTS/E and other DEQNA-only drivers need. Off, it powers up as a DELQA, and a driver that clears the mode bit in the vector register switches it to the DEQNA register model itself. Can be changed only while the controller is disabled.

#### `loopback`

A loopback connector plugged into the transceiver port, as DEC's diagnostics ask for. Every frame the controller transmits comes straight back to its own receiver and shows as carrier in the CSR while it passes; the network is disconnected in both directions. Set it to run the external loopback tests of the CZQNA diagnostic, and leave it off for a machine on the network. It takes effect at once.

<!-- /device-parameters -->
