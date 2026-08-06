# VCB01 (QVSS) emulation — status

Emulating a DEC VCB01 / QVSS monochrome video subsystem on QBone for the real
11/73, rendering to a remote X display. The whole path works end to end: the
bus device, the framebuffer, the refresh worker, the X renderer, and a
standalone PDP-11 program that drives the board and reads its keyboard and
mouse.

## What works

- **The device** (`10.02_devices/2_src/vcb01.{cpp,hpp}`, `vcb01_render.*`,
  `vcb01_input.*`): registers, CSR (read-only bank field), 6845 CRTC, 8-source
  interrupt controller, SCN2681 DUART (LK201 keyboard on chan A, VSXXX mouse on
  chan B), VSYNC, refresh worker, CRTC-derived height with window resize.
- **The memory API** (`10.05_web/2_src/webapi.cpp`): `GET`/`POST /api/memory`
  reads and writes bus memory by DMA, CPU running or halted, including I/O-page
  device registers (the test card enables video by writing the CSR this way).
- **The test card** (`tools/vcb01_testcard.py`): DMAs a full-screen picture
  straight into video memory with no PDP-11 program running — QBone is bus
  master. Layout constants: `XSIZE=1024`, 128 bytes/line, scanline map at byte
  `0o774000` of the bank, CSR at `0o17777200`, video bank base `0o16000000`.
- **The standalone demo** (`tools/vcbdemo.mac`): brings the board up from a
  halted CPU, sets up the MMU itself, programs the 6845, lays down a scanline
  map, draws the test pattern (border, both diagonals, centre crosshair, cursor)
  through APR 6, enables video, then loops on the keyboard and pointer. Verified
  on the real 11/73: the pattern appears on the X display, a keystroke draws its
  LK201 code as a column of bits along the top, and pointer motion over the
  window leaves a trail behind the hardware cursor. `tools/vcbtest.mac` is the
  simpler exerciser it grew from — it draws a static pattern into one 8K window
  and halts.

## Driving a standalone program onto the board

`tools/dmaload.py` parses a MACRO-11 listing and DMA-loads it through
`/api/memory`; `tools/odt.py` sends micro-ODT lines over the console WebSocket.
The cycle used here:

1. clear low memory `0..0o777` via `/api/memory` (kills stale vectors),
2. DMA-load the assembled `.lst` with `dmaload.py`,
3. `halt` → `continue` (release BHALT) → `1000G` over the console —
   **`continue` must come before `1000G`**,
4. read progress markers back with `/api/memory`.

Two things a standalone program and its loader have to get right:

- **PC-relative operands.** A MACRO-11 listing shows the resolved absolute
  address for a PC-relative operand (`jsr pc,X(pc)`, i.e. `call`), flagged with
  a trailing apostrophe (`002412'`), while the object word holds the *offset*
  from the following word. `dmaload.py` converts these back to offsets; loading
  the displayed value verbatim sends the first `call` into garbage. Programs
  with no subroutine calls (like an early `vcbtest`) never expose it.
- **`setpix` clobbers r2–r5.** It preserves only r0 and r1 (the x,y it draws), so
  a loop that calls it across a counter must keep the counter in r0 or r1.

## The screen height

`vcbdemo` draws 1024 × 800 — 50 displayed 6845 rows of 16 scan lines, which is
what its CRTC timing (the values from setlin.mac) programs. The device derives
the window height the same way, `crtc[VDSP] * (crtc[MSCN] + 1)`, so the drawn
pattern fills the window exactly. The renderer's untimed default is 864.

## Board / workflow reference

- Drive the board through the web API (docs in `10.05_web/docs/api.md`); auth is
  HTTP basic, password in `~/.qbone-pw`, any user name.
- Console is the 11/73's on-board SLU on the bone's `/dev/ttyS2`, carried by
  `/ws/console/ext`. DL11 stays disabled. Micro-ODT is reachable there;
  register examine (`R7/`) did not work, so progress markers in low memory are
  how a run is traced.
- The VCB01 needs a free 256 KB bank in Q22 space, so the machine's memory card
  has to end below the bank base. The 11/73 currently carries a 2 MB card at 0,
  which leaves bank 016 (at `0o16000000`) free; a full 4 MB card would collide
  with it.
- Enabling the device opens the X window (`display` parameter, e.g.
  `192.168.2.120:0`, see [[qbone-x-display]]); a display it cannot reach refuses
  the enable rather than leaving a controller on the bus with nowhere to draw.
- Capture the actual window with `xwd -id <win> -out f.xwd` (a `-root` capture
  does not composite windows under XQuartz); `xwdtopnm | magick` turns it into a
  PNG.

## Open items

- **CRTC readback.** The CRTC data register's DATI latch is refreshed only on a
  write, not when the address pointer changes, so reading the 18 registers back
  over the bus returns a stale value. A driver that reads CRTC registers would
  see it; the fix is to reload the latch from `crtc[pointer]` when the pointer
  is written.
- **Interrupt vector timing.** Real hardware selects the vector during the IAK
  cycle; QBone commits it when `INTR()` is scheduled. Two sources becoming ready
  between the schedule and the acknowledge would deliver the earlier vector.
- **Integration.** Web UI device page, packaging (`libx11`), and documentation
  — phase 5 of `docs/plans/vcb01-plan.md`.
