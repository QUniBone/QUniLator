---
title: DEUNA — UNIBUS Ethernet controller
description: The emulated DEUNA Ethernet controller, how it reaches the LAN, and every parameter it takes.
bus: [unibus]
---

The **DEUNA** is DEC's Ethernet controller for the UNIBUS. QUniLator presents
its four port registers and its port commands to the processor, works through
the transmit and receive descriptor rings the driver lays out in memory, and
moves the frames to and from the board's own network. It is listed as `deuna`,
of type `DEUNA`.

> **UNIBUS · UniBone**
>
> The DEUNA is a UNIBUS card. A QBone carries the QBUS equivalent, the
> [DELQA](delqa.md).

[VMS on an emulated VAX-11/780](../walkthroughs/vax-vms.md) builds a machine with
one.

## What the machine sees

| | |
|---|---|
| **Registers** | Four words, PCSR0 to PCSR3, at 774510, the address of the first DEUNA |
| **Interrupt** | BR5, vector 120 |
| **Station address** | `mac`. Each board has its own, starting `08:00:2b` |
| **Transfers** | The port control block, the descriptor rings and the frames move by DMA. Behind a VAX's UNIBUS adapter they go out on the bus and through its map registers, as every device's transfers do |
| **Network** | The board's interface `interface` |

## Using it

1. Add `deuna` on the **Configurations** screen. Its `interface` starts as
   `veth-pdp`, which the board joins to the bridge that carries its own
   Ethernet, so the machine reaches the LAN and the board itself. `eth0` reaches
   the LAN only.
2. Enable it. Enabling opens the interface, and is refused when the interface
   cannot be opened; the log says why.

`interface`, `mac`, `rx_slots` and `tx_slots` can only be changed while the
controller is disabled.

## ULTRIX-32 V1.1

ULTRIX-32 V1.1 recognises one IP broadcast address: its network's address
with the host part zero, `10.0.0.0` for the 10.0.0.200 its disk image comes
configured with, which is where its own rwho and routed broadcasts go. Every
other IP broadcast — 255.255.255.255, the all-ones `10.255.255.255`, the
broadcast of another network on the same LAN — it takes for a packet to
forward, and its forwarding of it corrupts kernel memory: the system panics at
the first such packet, with `mfree`, `Segmentation fault` or
`Protection fault`. The crash dump shows the network interrupt running through
`ip_forward` and `ip_output`, and a single UDP broadcast from another host
brings it down. A LAN with an ordinary mix of hosts carries such broadcasts
every few minutes.

Set `ultrix11_broadcast` to ULTRIX's broadcast address. The controller then
drops every IPv4 broadcast sent to another address before the driver sees it.
ULTRIX's own broadcasts, ARP and every other protocol still arrive, and ULTRIX
runs on.

## What has been verified

- VMS V4.7 on the emulated [KA780](ka780.md) boots to a login prompt with the
  DEUNA enabled on the bus, ten boots out of ten.
- That VMS system disk does not name a DEUNA in its configuration, so its
  driver, `XEDRIVER`, does not touch the controller during those boots.
  Network traffic under VMS has not been tested.

## Parameters

<!-- device-parameters: deuna_c -->
<!-- Written by docs/site/tools/device-params.mjs from the C++ source. Edit the source, then run it. -->

### Settings

What a configuration stores and an operator sets.

| Parameter | Short | Value | Default | What it is |
|---|---|---|---|---|
| [`interface`](#interface) | `if` | text | `veth-pdp` | host Ethernet interface shared with the emulated controller |
| [`mac`](#mac) |  | text | derived from the board's Ethernet address | station address, 6 hex bytes separated by ":", empty = device default |
| [`activityled`](#activityled) | `al` | number | 3 | Number of the LED used for activity display. |
| [`rx_slots`](#rx_slots-tx_slots) | `rx` | number | 0 | RX ring scan limit (0 = no limit) |
| [`tx_slots`](#rx_slots-tx_slots) | `tx` | number | 0 | TX ring scan limit (0 = no limit) |
| [`trace`](#trace) | `tr` | 0 or 1 | 0 | Trace CSR/ring events to log |
| [`ultrix11_broadcast`](#ultrix11_broadcast) | `u11b` | text | *empty* | ULTRIX-32 V1.1 workaround: the guest's IP broadcast address; other IPv4 broadcasts are dropped |

### Status

What the device reports as it runs. A configuration does not store these.

| Parameter | Short | Value | What it is |
|---|---|---|---|
| [`activitylamp`](#activitylamp) | `actl` | 0 or 1 | State of activity lamp |
| [`rx_frames`](#rx_frames-tx_frames-rx_errors-tx_errors) | `rxf` | number | Received frames count |
| [`tx_frames`](#rx_frames-tx_frames-rx_errors-tx_errors) | `txf` | number | Transmitted frames count |
| [`rx_errors`](#rx_frames-tx_frames-rx_errors-tx_errors) | `rxe` | number | Receive error count |
| [`tx_errors`](#rx_frames-tx_frames-rx_errors-tx_errors) | `txe` | number | Transmit error count |

It also carries the parameters under [parameters every card on the bus has](index.md#parameters-every-card-on-the-bus-has).

### In detail

#### `interface`

The board's network interface the controller is attached to. It starts as `veth-pdp`, the interface the QUniLator package creates for an emulated controller: its other end is joined to the bridge `br0` together with the board's own `eth0`, so the machine reaches both the LAN and the board itself. `eth0` attaches to the uplink directly, which reaches the LAN but not the board. Enabling the controller opens the interface and is refused when it is empty or cannot be opened; it can be changed only while the controller is disabled.

#### `mac`

The station address the controller answers to: six hex bytes separated by `:`. Each board derives its own the first time - DEC's prefix `08:00:2b` and the low three bytes of the board's own Ethernet address - and keeps it in `deuna.mac` in the board's state directory (`/var/lib/qunilator`), so it stays the same across boots and differs from board to board. A value set here overrides it and is stored with the configuration; setting it empty puts the derived address back. It can be changed only while the controller is disabled.

#### `activityled`

Which of the board's four activity LEDs, 0 to 3, traffic lights; 4 or more lights none. Drives take the LEDs by unit number, so 3 keeps the controller clear of a machine with one drive.

#### `activitylamp`

On while frames move in either direction, held a moment past each frame so the web interface sees it.

#### `rx_slots`, `tx_slots`

The most descriptors of the driver's receive and transmit rings the controller works through each time it scans them. 0 takes the ring length the driver gave, which is what a working machine wants; a smaller number is a debugging aid. Both can be changed only while the controller is disabled.

#### `trace`

Writes the controller's port commands, interrupts and ring events to the log at WARNING level, so they appear without raising the log level. For following a driver's conversation with the controller; it is verbose, and takes effect at once.

#### `ultrix11_broadcast`

A workaround for ULTRIX-32 V1.1: the IP broadcast address the guest uses, such as `10.0.0.0`. ULTRIX-32 V1.1 recognises one broadcast, its network's address with the host part zero, and takes every other IP broadcast - 255.255.255.255, the all-ones broadcast of its own network, the broadcast of another network on the LAN - for a packet to forward. Its forwarding of it corrupts kernel memory, so the system panics (`mfree`, `Segmentation fault`, `Protection fault`) at the first such packet. Set to ULTRIX's broadcast address, the controller drops every received IPv4 frame sent to the Ethernet broadcast address with another IP destination, before the driver sees it. ULTRIX's own broadcasts - rwho, routed - ARP and every other protocol still arrive. Empty leaves every frame to the driver, which suits every other guest. Takes effect at once.

#### `rx_frames`, `tx_frames`, `rx_errors`, `tx_errors`

The controller's own counters, the ones a driver reads with the port's read-counters function: frames received, frames sent, frames received with errors or lost for want of a buffer, and transmissions that failed. They follow the counters as frames move.

<!-- /device-parameters -->
