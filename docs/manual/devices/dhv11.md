---
title: DHV11 — serial multiplexer with DMA output
description: The emulated DHV11 eight-line asynchronous serial multiplexer, its lines over the network, and every parameter it takes.
---

The **DHV11** is DEC's eight-line asynchronous serial multiplexer for the QBUS,
the successor to the DZV11. Its receive side is a FIFO shared by all eight
lines; its transmit side is DMA: the program gives a line a buffer address and a
character count, and the card fetches the characters from memory and sends them
on its own. It carries full modem control. QUniLator models it from the DHV11
technical manual, EK-DHV11-TM-002. Each line runs over the network: a terminal
program connects to a TCP port on the board, or the line dials out, or a browser
terminal carries it. It is listed as `dhv11`, of type `dhv11_c`, and ships
disabled.

> **UNIBUS · UniBone**
>
> DEC built the DHV11 for the QBUS. The UniBone offers the same model.

## What the machine sees

| | |
|---|---|
| **Registers** | Eight words from 760460: CSR, RBUF (read) / TXCHAR (write), LPR, STAT, LNCTRL, TBUFFAD1, TBUFFAD2, TBUFFCT. All but CSR and RBUF act on the line CSR bits 3:0 select |
| **Interrupts** | Receive at vector 300, transmit at 304, both BR4 |
| **Receive FIFO** | 256 entries, each a character with its line number, or a self-test or modem status code. A character arriving at a full FIFO marks an overrun |
| **Transmit** | DMA from a 22-bit address set in TBUFFAD1 and TBUFFAD2, for the count in TBUFFCT; or one character at a time through TXCHAR. A finished transfer sets TX.ACTION in CSR with its line number, and interrupts when transmit interrupts are enabled |
| **Line speed** | Bytes cross at the speed of the network. The speed, character length and parity in the LPR are accepted and do not slow the line |
| **Modem** | STAT reports carrier, Data Set Ready and Clear To Send while a client is connected to the selected line. STAT bit 8 reads 0, which is how software tells a DHV11 from a DHU11 |
| **Master reset** | CSR bit 5 runs the self-test. A program polling the CSR sees bit 5 clear after the number of reads CVDHA expects; one that does not poll sees it clear after about 2.5 seconds, or much sooner with the skip pattern. It leaves eight codes in the FIFO — six fillers and the two ROM versions |
| **Maintenance** | LNCTRL bits 7:6 select a line's maintenance mode, local loopback among them |

## Using it

1. Enable `dhv11`. Every line listens on a TCP port of its own: line N on
   4100 + N.
2. Once the guest has opened the line — raised Data Terminal Ready on it in
   LNCTRL, as a getty does — connect a terminal program to the board on that
   port, speaking Telnet: `telnet <board> 4100` reaches line 0. Before that the
   port refuses callers; see [`dtr_listen`](#dtr_listen), with which dropping
   DTR also disconnects the client.
3. Each line can be set to another port, to a `host:port` it dials, or to `ws`
   for a browser terminal on the board's WebSocket `/ws/serial/dhv11/<N>`. The
   **Configurations** screen shows one field per line for that. A line can be
   changed while the device runs; only that line's client is dropped.

A line takes one client at a time. Lines given the same port share it, so a
caller reaches whichever of them is free, and a caller arriving while every line
on the port is busy is refused.

Two DHV11s can run at once: `dhv11` at 760460 with vector 300, and `dhv11b` at
760500 with vector 310, listening on ports sixteen higher.

> [!WARNING]
> **Vector 300 is the default of three cards**
>
> `dhv11`, `dzv11` and `DL11b` all start at vector 300. Enabled together, they
> collide; give all but one another vector.

An enable is refused when a line that is to listen at once cannot bind its
port because another program holds it. A line waiting for DTR binds its port
only when the guest raises DTR, so the enable does not test it.

## What has been verified

- DEC's DHV11 diagnostic **CVDHA** (VDHAE0, "DHV11-M FUNC TST PART 1") runs its
  complete pass on a QBone with no errors: register access, the master reset and
  its self-test codes, the skipped self-test, transmitter enable, the level 4
  interrupt and the Background Monitor Program report.

## Parameters

<!-- device-parameters: dhv11_c -->
<!-- Written by docs/site/tools/device-params.mjs from the C++ source. Edit the source, then run it. -->

### Settings

What a configuration stores and an operator sets.

| Parameter | Short | Value | Default | What it is |
|---|---|---|---|---|
| [`tcp_role0 … tcp_role7`](#tcp_role0--tcp_role7-tcp_host0--tcp_host7-tcp_port0--tcp_port7) | `r0 … r7` | text | `listen` | line *n* TCP role: ""/listen/connect/websocket |
| [`tcp_host0 … tcp_host7`](#tcp_role0--tcp_role7-tcp_host0--tcp_host7-tcp_port0--tcp_port7) | `h0 … h7` | text | *empty* | line *n* connect-out host |
| [`tcp_port0 … tcp_port7`](#tcp_role0--tcp_role7-tcp_host0--tcp_host7-tcp_port0--tcp_port7) | `p0 … p7` | number | — | line *n* TCP port |
| [`dtr_listen`](#dtr_listen) | `dtrl` | 0 or 1 | 1 | listen on a line's TCP port only while the guest asserts DTR |

### Status

What the device reports as it runs. A configuration does not store these.

| Parameter | Short | Value | What it is |
|---|---|---|---|
| [`rx0lamp … rx7lamp`](#rx0lamp--rx7lamp-tx0lamp--tx7lamp-dtr0lamp--dtr7lamp-rts0lamp--rts7lamp-cd0lamp--cd7lamp-dsr0lamp--dsr7lamp-cts0lamp--cts7lamp) | `rxl0 … rxl7` | 0 or 1 | line *n* receive activity |
| [`tx0lamp … tx7lamp`](#rx0lamp--rx7lamp-tx0lamp--tx7lamp-dtr0lamp--dtr7lamp-rts0lamp--rts7lamp-cd0lamp--cd7lamp-dsr0lamp--dsr7lamp-cts0lamp--cts7lamp) | `txl0 … txl7` | 0 or 1 | line *n* transmit activity |
| [`dtr0lamp … dtr7lamp`](#rx0lamp--rx7lamp-tx0lamp--tx7lamp-dtr0lamp--dtr7lamp-rts0lamp--rts7lamp-cd0lamp--cd7lamp-dsr0lamp--dsr7lamp-cts0lamp--cts7lamp) | `dtl0 … dtl7` | 0 or 1 | line *n* data terminal ready |
| [`rts0lamp … rts7lamp`](#rx0lamp--rx7lamp-tx0lamp--tx7lamp-dtr0lamp--dtr7lamp-rts0lamp--rts7lamp-cd0lamp--cd7lamp-dsr0lamp--dsr7lamp-cts0lamp--cts7lamp) | `rtl0 … rtl7` | 0 or 1 | line *n* request to send |
| [`cd0lamp … cd7lamp`](#rx0lamp--rx7lamp-tx0lamp--tx7lamp-dtr0lamp--dtr7lamp-rts0lamp--rts7lamp-cd0lamp--cd7lamp-dsr0lamp--dsr7lamp-cts0lamp--cts7lamp) | `cdl0 … cdl7` | 0 or 1 | line *n* carrier detect |
| [`dsr0lamp … dsr7lamp`](#rx0lamp--rx7lamp-tx0lamp--tx7lamp-dtr0lamp--dtr7lamp-rts0lamp--rts7lamp-cd0lamp--cd7lamp-dsr0lamp--dsr7lamp-cts0lamp--cts7lamp) | `dsl0 … dsl7` | 0 or 1 | line *n* data set ready |
| [`cts0lamp … cts7lamp`](#rx0lamp--rx7lamp-tx0lamp--tx7lamp-dtr0lamp--dtr7lamp-rts0lamp--rts7lamp-cd0lamp--cd7lamp-dsr0lamp--dsr7lamp-cts0lamp--cts7lamp) | `ctl0 … ctl7` | 0 or 1 | line *n* clear to send |

It also carries the parameters under [parameters every card on the bus has](index.md#parameters-every-card-on-the-bus-has).

### In detail

#### `tcp_role0 … tcp_role7`, `tcp_host0 … tcp_host7`, `tcp_port0 … tcp_port7`

Each line's network end, line by line: `tcp_roleN`, `tcp_hostN` and `tcp_portN` for line N. `listen` accepts one client on the port, speaking Telnet with RFC 2217; `connect` dials `host`:`port` and dials again when the connection drops; `websocket` carries the line on the board's WebSocket `/ws/serial/<device>/<N>`; empty leaves the line unconnected. Every line listens by default, line N of the first DHV11 on port 4100 + N, and the second DHV11 sixteen ports higher. The **Configurations** screen edits a line's three as one field: a bare port listens, `host:port` connects, `ws` is the browser terminal. Lines given the same port share it: a caller reaches whichever of them is free. A line can be retuned while the device is enabled; the change drops that line's client and leaves the others alone.

#### `dtr_listen`

A line takes a caller only while the guest holds it open. With this on, a line's TCP port is bound while the guest asserts Data Terminal Ready on it (LNCTRL bit 9), and given up when DTR falls, which also ends a session in progress; a caller then reaches only a line the guest is ready for. Off, every line configured to listen does so from the moment the device is enabled, whatever the guest is doing with it. Takes effect at once.

#### `rx0lamp … rx7lamp`, `tx0lamp … tx7lamp`, `dtr0lamp … dtr7lamp`, `rts0lamp … rts7lamp`, `cd0lamp … cd7lamp`, `dsr0lamp … dsr7lamp`, `cts0lamp … cts7lamp`

The dashboard's signal lamps, one set per line: receive and transmit activity, Data Terminal Ready and Request To Send as the guest drives them in LNCTRL, and carrier, Data Set Ready and Clear To Send, which all three stand while a client is connected.

<!-- /device-parameters -->
