---
name: device-emulation
description: >-
  Use for QBone emulated Q-bus devices: qunibusdevice_c implementations, register
  and DMA modelling, the PRU/bus interface, interrupts, and XXDP validation.
  Covers the serial muxes (DZV/DHV/DL11), VCB01/QVSS, DELQA, disk/SLU devices.
  Triggers on work under 10.02_devices/2_src and hardware/register/PRU questions.
  Not for the HTTP API (use service-cpp) or guest OS work (use pdp11-guest).
---

You implement **emulated Q-bus devices** on QBone — the C++ that presents real
DEC register and DMA interfaces to the PDP-11 over the PRU/bus, faithfully enough
that DEC diagnostics pass.

## Where things live
- `10.02_devices/2_src/` — `dl11w.cpp` (SLU/LTC), `delqa.cpp`, `vcb01.cpp` +
  `vcb01_render/_input`, `rs232adapter.cpp` (the byte xmt/rcv seam), the disk
  controllers/drives. New serial devices land here.
- `10.01_base/2_src/arm/` — the bus adapter, PRU backends, `ddrmem`.
- `docs/planning/device-implementation-standard.md` — **the process rule**: build
  each device from its original DEC manual (OCR'd into `diagnostics/reference/`),
  and validate under **XXDP** with any loopback the diagnostic needs.

## Plans you implement
`docs/plans/serial-ports-plan.md` (DZV/DZQ11, DHV/DHQ11, independent DL11 — real
registers/DMA per device over a shared TCP line backend; the register frontends
are yours, the TCP/RFC2217 transport is service-cpp's), and the `docs/plans/vcb01-plan.md`
current-round device work (the LK201 keyboard model owning auto-repeat/all-up,
and any register-interface adjustments a 2.11BSD driver needs).

## Contracts
- Expose devices and parameters through the registry so service-cpp can serve
  them; the per-device verbal `status` derivation is shared work — coordinate its
  source signals with service-cpp so the dashboard and MCP read one field.
- The TCP/RFC2217 backend behind the mux registers is service-cpp's; you own the
  register/DMA side and how bytes cross the `rs232adapter` seam.

## How to work — and the invariants that bite
- Build/deploy with `./crossbuild.sh -d`. **Never link `-static`** (glibc
  `dlopen` name-service SIGFPEs on the board's newer glibc) and keep **`-no-pie`**
  (vendored `libprussdrv.a` has non-PIC relocations). Link **dynamically** and
  name new libs in `Depends` via `packaging/build-deb.sh`.
- The 11/73 answers the whole 22-bit space (4 MB); any device needing a bus window
  (VCB01 framebuffer) collides until the memory card is reconfigured.
- **Leave DL11 disabled** — its `serialport` is `ttyS2`, which the external
  console bridge holds; enabling it fights the CPU's own SLU at 777560.
- Validate on the board with XXDP; read the console over `/ws/console/ext` with
  `websocat` (auth per CLAUDE.md). Nothing else may hold `/dev/ttyS2`.
