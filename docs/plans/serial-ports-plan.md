# Serial ports over TCP — implementation plan

Adds emulated async serial devices — a DZV11/DZQ11 4-line mux, a DHV11/DHQ11
8-line mux, and independent DL11 lines — each line mapped to a TCP endpoint
speaking Telnet/RFC2217. Implements the requirements in
[`serial-ports.md`](../planning/serial-ports.md). Each device
follows the [device implementation standard](../planning/device-implementation-standard.md).

## 1. What exists today

`slu_c` (`dl11w.cpp`) emulates the DL11 SLU but is bound to a tty `serialport` or
the emulated-console WebSocket; there is no DZ or DH device and no TCP/RFC2217
server anywhere. `rs232adapter_c` (`rs232adapter.cpp`) is a byte xmt/rcv seam that
routes a device's character stream to/from a backend with optional baud
throttling and pattern matching — the seam a TCP transport hangs off. The
`/ws/console/<n>` taps show how a device's byte stream reaches the web UI.

## 2. Two layers

The work splits into **faithful device frontends** and a **shared TCP line
backend**, so the register/DMA behaviour is real (XXDP passes) while the transport
is written once.

### Device frontends (real registers/DMA)

- **DZV11/DZQ11** — a 4-line character-at-a-time async mux, real CSR/RBUF/TCR/MSR
  register model.
- **DHV11/DHQ11** — an 8-line async mux with **DMA output**, modelling its real
  FIFO/DMA transmit path and register interface.
- **DL11** — independent single lines, reusing `slu_c`'s register model with the
  TCP backend in place of the tty.

Each is built from its DEC manual (kept in the repo, OCR'd if needed) and
validated under XXDP with any loopback the diagnostic needs, per the device
standard. Bus addressing is **configurable per instance** — `base_addr`, `slot`,
`intr_vector`, `intr_level` parameters like DELQA/DL11 — defaulting to the
DEC-standard floating-address values; several instances of a kind may be enabled.

### Shared TCP line backend

A `serial_tcp_line_c` behind the `rs232adapter_c` seam carries one line's bytes:

- **Per-line role**, a device parameter: **listen** (a host program dials in) or
  **connect-out** (QBone dials a configured host:port). 
- **Telnet with RFC2217** framing. Line parameters (baud, bits) are **negotiated,
  stored, and reported**, and bytes are delivered at **full speed** — the TCP link
  has no real UART timing, so the negotiated baud is cosmetic. (The rs232adapter's
  baud throttle stays available but is off by default.)
- **Single client per port.** A line is point-to-point; while one client holds it,
  a second connection is **refused**, protecting the session in progress.

## 3. Configuration and web surface

- A line's TCP mapping is expressed as **device parameters** on the mux/line —
  `tcp_role` (listen/connect), `tcp_port` (listen port) or `tcp_host`/`tcp_port`
  (connect-out) per line — so it lives in the configuration model and the
  config-management editor like any other parameter
  ([configuration-model.md](../planning/configuration-model.md),
  [web-config-management.md](../planning/web-config-management.md)).
- A mux gets a **dashboard widget** ([web-dashboard.md](../planning/web-dashboard.md)):
  a terminal with **tabs, one per port**; independent DL11 lines are single
  terminals.

## 4. Dashboard view and stale connections

The dashboard terminal is a **monitor tap**, not the single TCP client:

- Each line mirrors its bytes to a **console channel** ([console-plan.md](console-plan.md))
  — `/ws/console/mux<n>/<port>` — with the same 256 KB ring and history replay as
  the other console channels, so opening the widget mid-session shows the recent
  exchange.
- The widget may **send input only when no TCP client holds the line**, so the
  monitor never fights a connected host program for the single writer slot.
- The dashboard shows the **connected peer** (address) with a **disconnect
  button** to drop a hung client manually, and a **configurable idle timeout**
  drops a stale connection automatically, so a refused-second-connection lock is
  always recoverable.

## 5. Relationship to existing taps

The new `/ws/console/mux<n>/<port>` channels use the shared `console_channel_c`,
the same mechanism as `/ws/console/ext` and `/ws/console/<n>`. The TCP endpoint is
the line's data path; the WebSocket is the dashboard's monitor of it. They are
distinct: the TCP side enforces single-client, the WS tap is a broadcast monitor.

## 6. Testability

- **XXDP** per device — the DZ, DH, and DL11 line diagnostics, with the loopback
  each needs, per the device standard. This is the emulation-fidelity gate.
- **TCP backend** host tests: listen accepts one client and refuses a second;
  connect-out dials the configured host; RFC2217 negotiation is answered and the
  reported params reflect the request; the idle timeout drops a silent client;
  a disconnect frees the line for a new client.
- **Config round-trip**: the per-line `tcp_role`/`tcp_host`/`tcp_port` parameters
  save and restore through the configuration model.

## 7. Decisions

Resolved:

- **Three device kinds** — DZV/DZQ11, DHV/DHQ11, independent DL11 — each with a
  **real register/DMA interface** validated under XXDP, over a **shared TCP/RFC2217
  line backend**.
- **Addressing is configurable per instance** with DEC-standard defaults; multiple
  instances allowed.
- **Per-line role** (listen or connect-out) is a device parameter.
- **RFC2217 negotiated, reported, delivered full-speed**; baud pacing available but
  off.
- **Single client per TCP port**; the dashboard is a **monitor tap** (console
  channel with replay) that can send input only when no TCP client is connected,
  with a **manual disconnect** and a **configurable idle timeout** to clear stale
  connections.

Open:

- Exact default instance counts per kind (how many DZ/DH/DL lines ship enabled vs.
  available to enable) — a small config-default choice settled when the devices
  are built.
- Whether connect-out retries on a refused/closed host or requires a manual
  re-enable — a transport-policy detail for implementation.
