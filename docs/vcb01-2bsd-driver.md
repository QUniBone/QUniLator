# A graphics surface for 2.11BSD on the emulated VCB01

Investigation for the "2.11BSD driver — a graphics surface" round of
`vcb01-plan.md`: does the board's 2.11BSD carry a `qv`/QVSS driver, and if not,
what does giving it a program-drawable framebuffer take? Investigate-first; no
kernel was rebuilt and the system pack was not modified.

## Conclusion

**2.11BSD carries no `qv`/QVSS driver, and its device model has no place to
configure one.** The QVSS `qv` driver is a VAX 4.3BSD / Ultrix device; it was
never part of the PDP-11 2.11BSD tree. There is nothing to enable. A graphics
surface means **writing a new character driver** — and on 2.11BSD it is a
`read`/`write`/`ioctl` character device that copies pixels into the bank, not a
VAX-style `mmap` of the framebuffer into the program's address space, which the
PDP-11 memory model cannot do.

The emulated VCB01 is not the obstacle: it already presents the full QVSS
register and interrupt interface a driver would target (assessed below).

## Evidence that no driver exists

Checked offline against the pack `/var/lib/qunilator/images/2.11BSD_qbone.dsk` on the
board, without booting (the console is char-at-a-time fragile). Two independent
lines agree.

**1. Source scan of the raw image.** A `strings`/`grep` scan for driver source:

- `qv.c` as a literal filename: **0 occurrences.** (A regex `qv.c` matched 3
  spots — all wildcard hits in binary, not the filename.)
- `qv0` / `QVSS`: the only real hits are **prose** — the X11 distribution notes
  describing "the DEC b/w VAXStation II (QVSS)" and a keyword list. The two
  `qv0` byte hits are random binary.
- Method validation: real 2.11BSD driver filenames are abundant in the same
  image — `if_qe.c` 30 hits, `dhu.c` 61 — so a present `qv.c` would have shown.

**2. The kernel config vocabulary.** The build config
(`images/211bsd/QBONE` in this repo, the `IDENT QBONE` kernel this pack runs)
enumerates every device 2.11BSD's `config` understands: disks (`ra`, `rl`,
`rk`, `rx`, `xp`, `si`…), tapes, terminal muxes (`KL`/`DL`, `DH`, `DM`, `DHU`,
`DHV`, `DZ`), line printer, and network interfaces (`ec`, `de`, `il`, `sl`,
`qe`, `qt`, `vv`…). **There is no graphics or framebuffer device of any kind.**
2.11BSD's `config` has no keyword that would place a QVSS in the kernel.

## Register-compatibility assessment

If a driver existed, could it drive this emulation? Yes — the emulated board
(`10.02_devices/2_src/vcb01.cpp`) implements the M7602/QVSS programming
interface a driver expects, at base `17777200`:

| QVSS facility | emulated | note |
|---|---|---|
| CSR (reg 0) | yes, DATI/DATO | read-only MA bank field composed from the `bank` param, MOD monitor size, live mouse-button bits; writable bits IEN/TST/VRB/FNC/VID as on hardware |
| cursor X (reg 1) | yes | masked to 10 bits |
| CRTC pointer/data (reg 4/5) | yes | 18 6845 registers behind the pointer; window height derived from the programmed timing |
| interrupt controller ICDR/ICSR (reg 6/7) | yes | 8 sources, per-source vectors, arbitration before `INTR()` |
| SCN2681 DUART (reg 16..27) | yes | LK201 keyboard chan A, VSXXX mouse chan B |
| 256 KB video bank | yes | bank 016 at `16000000`, served from DDR at bus speed |

So the emulation is driver-ready. One known gap a real driver would meet, from
`VCB01_STATUS.md`: **CRTC read-back returns a stale value** — the DATI latch is
refreshed on a data write, not when the address pointer changes, so a driver
that reads the 18 CRTC registers back reads wrong. A driver that only writes the
CRTC (the usual case, as `setlin.mac`/`vcbdemo.mac` do) is unaffected. Fix noted
there: reload the latch from `crtc[pointer]` on a pointer write.

## Why "enable the driver" is not the path, and why `mmap` is not either

The plan's branch 2 (configure an existing driver) does not apply — none exists.
Branch 3 (write one) is the path, but the plan's shorthand of "a character /
`mmap` device onto the bank" needs a 2.11BSD-specific correction:

- **No device `mmap`.** VAX BSD exposes a framebuffer by an `mmap` entry in
  `cdevsw` (`d_mmap`) that the VAX pmap uses to map device physical pages into a
  process. 2.11BSD's `cdevsw` has no such entry — a scan of the whole pack finds
  **0 occurrences of `d_mmap`**. Device `mmap` is a VAX-era feature the PDP-11
  port never had.
- **The address space forbids it anyway.** A 2.11BSD user process is split I/D
  with at most **64 KB of data space** through 8 APRs. The bank is **256 KB**.
  You cannot map the whole framebuffer into a program at once; at best a driver
  hands out an 8 KB (one-APR) window and the program pans it — clumsy and not
  what "draw pixels" wants.

So the realistic 2.11BSD graphics surface is a **kernel-mediated character
device**: the program hands the driver pixels/rectangles, the kernel writes them
into the bank over the bus.

## Recommended path: a `qv` character driver for 2.11BSD

A minimal `/usr/src/sys/pdpuba/qv.c` (Qbus device, so `pdpuba`), configured as
`NQV`, exposing a character device `/dev/qv0`:

**Reaching the bank from the kernel.** The bank lives in Q22 space at
`16000000` (bank 016), above the top of RAM. The kernel reaches it the same way
the standalone `vcbdemo.mac` does — a mapping APR. In the kernel that is a
supervisor/kernel APR (a `UBMAP`-style or a spare KDSA) pointed at a 8 KB slice
of `16000000`, moved across the bank as the driver copies. `copyin` the user's
pixels into a kernel buffer, then block-move into the mapped window.

**Entry points (`cdevsw`):**

- `qvopen`/`qvclose` — probe the CSR (reads back the board's bank/monitor bits),
  claim the device.
- `qvwrite` — `copyin` a span of framebuffer bytes and blit them into the bank
  at the current offset (a linear `/dev/fb` model: `lseek` sets the byte offset,
  `write` lays down bitmap bytes). This is the "draw pixels" primitive.
- `qvioctl` — the control surface a windowing/pixel program needs:
  - program the 6845 (the `setlin.mac` timing) and lay a linear scanline map,
  - `QVIOVIDEO` on/off (CSR VID), cursor position/enable (CSR + CRTC),
  - a blit call `{x, y, w, h, *bits}` that copies a rectangle in one syscall so
    a program is not doing a `write` per pixel,
  - keyboard: deliver LK201 input from the DUART (chan A) through the same
    device's read path or a companion `/dev/qvkb`, so a reading program gets
    keystrokes. The DUART raises interrupt-controller source 0; the driver's
    interrupt handler queues received bytes.
- `qvintr` — interrupt handler for the 8-source controller (VSYNC, DUART,
  mouse). Vector/BR per the `config` line; the emulation programs per-source
  vectors through ICDR/ICSR.

**config integration.** Add `NQV` to the machine config, a `qv` line to the
device tables (`ioconf`/`conf.c` — `cdevsw` slot, no `bdevsw`), a `MAKEDEV qv0`
entry, and the CSR address/vector. Rebuild per the QBONE kernel procedure
(`images/211bsd/README.md`).

**Validation** (per the device-implementation standard, since QVSS has no XXDP):
a guest program opens `/dev/qv0`, writes a known pattern, and it is read back
over `/ws/vcb01`; plus a program reading LK201 keystrokes delivered through the
DUART.

This is a real driver-writing task over the fragile console and was **not
attempted this pass** (guardrail). The sketch above plus the working
`vcbdemo.mac` (which already does the MMU-mapping, 6845 programming, scanline
map and bitmap draw from a bare CPU) are the starting material: the driver is
`vcbdemo.mac`'s setup and draw logic, moved into kernel C behind `cdevsw`.

## Performance observation (light)

The plan's suspicion — the refresh worker is cheap (≈3.5 ms/30 Hz pass, already
measured) so slowness is the **guest's per-pixel bus writes** — holds. Notes
from this pass:

- **The `/api/memory` DMA path is not a clean proxy for guest draw cost.**
  Measured on hardware (config vcb01test, bank 016): a 1-word `POST /api/memory`
  round-trips in **~24 ms**, and an 8 KB (4096-word) POST in **~63 ms**. That is
  **HTTP + JSON parse overhead**, not bus time — a full 256 KB bank took ~2.0 s
  across 32 POSTs (~0.13 MB/s), dominated by the fixed per-request cost. This
  path is fine for painting a test card once but says nothing about a guest's
  draw loop.
- **Where the guest cost lives.** The renderer reads the bank from DDR at
  226 MB/s and a full pass is 3.5 ms, so the QBone side is not the bottleneck.
  A guest draw is bounded by **the number of QBUS DATO cycles it issues**: a
  naive per-pixel loop over ~800 K pixels does a read-modify-write word cycle
  per pixel — ~800 K bus round-trips through the PRU. That cost is **inherent to
  how the program writes video memory**, not a QBone-side inefficiency, so per
  the plan it is recorded, not chased.
- **The guest-side mitigation** (for whoever writes the driver or the demo):
  write **whole words/longwords** — 16 or 32 pixels per DATO — instead of a
  per-pixel RMW. Filling by word cuts the bus-cycle count ~16–32×. A driver
  `blit` ioctl that lays down rectangles by word is the natural place for this;
  the `write`-a-byte-per-pixel model is what to avoid. No QBone-side change is
  indicated by the profile.

## Board state

Left in config **vcb01test** (current and default, unchanged throughout). The
only board interaction was reads of the pack and `/api/memory` writes into the
framebuffer bank for the throughput timing — no config change, no CPU control,
and `2.11BSD_qbone.dsk` was only read (grep), never written.
