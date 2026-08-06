# Serial ports over TCP

**Status:** Ready — implementation plan drafted in
[`serial-ports-plan.md`](../plans/serial-ports-plan.md).

Additional emulated serial ports, each mapped to a TCP port so a host program
can connect to it.

## Current state

The 11/73's console SLU is real hardware bridged over `/dev/ttyS2` and carried on
the `/ws/console/ext` WebSocket. QBone can emulate DL11s (777560, 776500) exposed
on `/ws/console/0` and `/ws/console/1`, but this board leaves them disabled. There
is no way to attach an emulated serial line to a TCP endpoint.

## Requirements

- QBone can present **additional emulated serial ports** across three device
  kinds:
  - a **DZV11/DZQ11** 4-line async mux,
  - a **DHV11/DHQ11** 8-line async mux,
  - **independent DL11 single lines**.
- Each port is **mapped to a TCP endpoint** speaking **Telnet with RFC2217**, so
  a client can query and set line parameters (baud, bits). Per line, QBone either
  **listens** (a host program dials in) or **connects out** to a host-provided
  address — the role is configurable per line.
- A mux gets a **dashboard widget**: an additional terminal with tabs, one per
  port; the independent DL11 lines are single terminals
  ([web-dashboard.md](web-dashboard.md)).
- Each device follows the
  [device implementation standard](device-implementation-standard.md): built from
  its DEC manual (kept in the repo) and validated under XXDP, with any loopback
  the diagnostic needs in place.

## Decisions

- **Three device kinds supported** — DZV11/DZQ11, DHV11/DHQ11, and independent
  DL11 lines.
- **Per-line TCP role is configurable** — each line is either listen or
  connect-out.
- **Telnet / RFC2217** framing, so clients can negotiate line parameters.
- **Single client per port** — a line is point-to-point; a second connection is
  **refused** while one is active, protecting the session in progress.

## Open questions

- A refused second connection can lock a line if a client hangs — is there a way
  to see and drop a stale connection (from the dashboard, or an idle timeout)?
- The DHV11/DHQ11 does DMA output; the DZ and DL11 are character-at-a-time. Does
  the emulation implement each device's real register/DMA interface, or a common
  byte-pipe backend behind three register facades?
- How many instances of each, and are their bus addresses/vectors fixed or
  configurable?
- RFC2217 carries line parameters — does the emulated mux act on them (real baud
  pacing), or accept and ignore them since the TCP link has no real UART timing?
- How does a serial port's TCP mapping appear in the configuration model and the
  web UI — is it a device parameter?
- Relationship to the existing `/ws/console/<n>` WebSocket taps — do these new
  ports also get a WebSocket view (for the dashboard widget), or only TCP?
