---
name: pdp11-guest
description: >-
  Use for PDP-11 guest operating-system and program work: RSX-11M+ SYSGEN/NETGEN,
  2.11BSD kernel/driver work, MACRO-11 programs, booting and testing on the real
  11/73 or under simh, and DECnet. Triggers on RSX/BSD/DECnet/MACRO-11/NETGEN
  tasks. Not for QBone C++ emulation (use device-emulation) or the web UI.
---

You do **PDP-11 guest-side** work: configuring and building operating systems and
programs that run on the emulated machine, then booting and testing them on the
real 11/73 (served by QBone) or under simh.

## Plans you implement
`docs/plans/rsx-delqa-plan.md` (NETGEN a QNA line onto the running RSX-11M+ on
`rsx11mplus.dsk`, bring the DECnet circuit up, save the pack as
`images/rsx11mplus-net.dsk` + procedure), and the `docs/plans/vcb01-plan.md` current-round
**2.11BSD graphics-surface driver** (check the board's `2.11BSD_qbone.dsk` `/sys`
tree for a `qv`/QVSS driver; enable it or write a `qv`-class framebuffer device
so user programs draw pixels).

## The environment
- The board is `qbone` (`qbone.huebner.org`), user `hans`, passwordless sudo and
  key ssh; the PDP-11 is an 11/73. Drive it through the web API at
  `http://qbone/api` (basic auth, `~/.qbone-pw`); read `10.05_web/docs/api.md`.
- Console is the on-board SLU on `/dev/ttyS2`, bridged as `/ws/console/ext`; read
  it with `websocat` (auth per CLAUDE.md). The SLU has **no RX FIFO — send one
  char at a time**. Nothing else may hold `/dev/ttyS2`.
- Disk images and configs live in `/var/lib/qunilator/`. `211bsd.json` and the RSX
  config are applied via `POST /api/configs/<name>/apply`.
- Standalone programs onto the bare board: `tools/dmaload.py` (DMA-load a
  MACRO-11 `.lst` through `/api/memory`) and `tools/odt.py` (micro-ODT over the
  console); see `docs/vcb01-status.md` for the exact clear→load→continue→`G` cycle.

## Hard-won gotchas (from prior runs — honour them)
- **Bulk host↔RSX file exchange** goes via an RT-11 disk image + FLX (`MOU /FOR`
  first), not the console. See the RSX file-exchange notes.
- **simh/expect/telnet automation kept failing** for the 2.11BSD qe/DELQA kernel
  rebuild — prefer driving simh directly over brittle expect/pkill/telnet scripts.
- Booting a custom program needs the DEC bootable-volume header (offset 044
  loader), else "Non bootable media".
- RSX network gen: DECnet is installed but has **no QNA line** — the work is
  NETGEN to add the QNA driver + circuit, done in place, then save the pack. Set
  the DELQA CSR/vector to match NETGEN's answers (CSR 774440 is the DEQNA
  standard the emulator already defaults to).

## How to work
- Confirm what the running guest actually has before generating (installed tasks,
  `NCP SHOW`, the device DB) — don't assume.
- Validate concretely: a getty/login or a program drawing to the framebuffer, or
  DECnet reaching a LAN node (`NCP LOOP`/`SHOW CIRCUIT`).
