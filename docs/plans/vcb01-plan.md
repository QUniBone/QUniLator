# VCB01 (QVSS) emulation plan

Emulate the DEC VCB01 / QVSS (M7602) Qbus video subsystem on QBone, rendering
to an X server over TCP. The board is register-compatible with the real M7602;
keyboard and mouse are carried on its DUART as on the original.

References: appendix C of AZ-GLFAB-MN (VAXstation II Technical Manual, BA23
Enclosure) for the register set, and SIMH `VAX/vax_vc.c`, `vax_2681.c`,
`vax_lk.c`, `vax_vs.c` for behavioural detail.

## The device

### I/O page

32 word registers (0100 bytes), default base 17777200.

| word | register |
|------|----------|
| 0 | CSR |
| 1 | cursor X |
| 2 | mouse position |
| 4 | CRTC address pointer, plus VB / light-pen-full / update-strobe status |
| 5 | CRTC data — 18 eight-bit 6845 registers behind the pointer |
| 6 | interrupt controller data (ICDR) |
| 7 | interrupt controller command/status (ICSR) |
| 16..19, 21, 24..27 | SCN2681 DUART |

CSR bits: MOD(0) monitor size, VID(2) video enable, FNC(3) cursor function
(0 = AND, 1 = OR), VRB(4) video readback, TST(5), IEN(6) interrupt enable,
CUR(7) cursor active, MSA/MSB/MSC(8..10) mouse buttons, MA(11..14) memory bank.
Writable bits are IEN, TST, VRB, FNC, VID.

The DUART carries the LK201 keyboard on channel A and the VSXXX mouse on
channel B.

### Video memory

256 KB (64K longwords) in Q22 address space, at `base = MA << 18`.

**MA is a switch on the board, which software reads and cannot set.** It
occupies CSR bits 11..14 and is absent from the register's writable bits, so a
driver learns where video memory is by reading the CSR rather than by placing
it. Boards differ: a VAXstation II puts the bank at the top of Qbus memory
space, MA = 017, while the board in the RSXstation write-up answered at
16000000, MA = 016. Here it is a device parameter.

Screen is 1024 x 864. Layout by longword index within the bank:

| longword index | content |
|----------------|---------|
| 0x0000..0xFDFF | bitmap: 32 longwords (128 bytes) per buffer line, bit 0 of each longword is the leftmost pixel of its group of 32 |
| 0xFE00..0xFFF7 | scanline map: 1024 entries of 11 bits, two per longword, even screen line in the low half |
| 0xFFF8..0xFFFF | 16 x 16 cursor bitmap, 256 bits |

The scanline map says which buffer line appears on each screen line, which is
how the board scrolls. Several screen lines may point at one buffer line, so a
change to a buffer line invalidates every screen line that maps to it.

Cursor position is `X = curx & 0x3FF`, `Y = crtc[CAH] * (crtc[MSCN] + 1) +
crtc[CSCS]`; visible when `crtc[CSCS] & 0x20` is clear. OR composites the
cursor bits into the picture, AND masks them out.

### Interrupts

An eight-source priority interrupt controller with a separately programmable
vector per source: DUART(0), VSYNC(1), MOUSE(2), cursor-start(3), mouse buttons
A/B/C(4..6), spare(7). The host programs IRR, IMR, ISR, ACR and the vector
table through ICDR/ICSR with a register-preselect field in the mode register.

## Address space

The board occupies 256 KB of Q22 space exclusively, so the machine's RAM has to
end below it. This is the same trade the real hardware imposes: a VCB01 in a
Qbus backplane costs a quarter megabyte of the four the bus can address.

The 22-bit space runs 0 .. 0o17777777, with the I/O page taking the top 8 KB
from 0o17760000. A memory card configured for the full 4 MB covers everything
below the I/O page and leaves no bank free — with such a card the framebuffer
has nowhere to live and both boards would answer the same addresses. The card
has to be reconfigured for less.

Bank placement follows MA, at 256 KB granularity:

| MA | bank | note |
|----|------|------|
| 016 | 16000000 .. 16777776 | RAM limited to 3.5 MB |
| 015 | 15000000 .. 15777776 | RAM limited to 3.25 MB |
| 017 | 17000000 .. 17777776 | unusable: overlaps the I/O page |

MA = 017 is where a VAXstation II puts the bank, because a Qbus tells memory
space from I/O space by BBS7 rather than by address alone, so video memory at
017000000 and the I/O page at 017760000 do not collide. `ddrmem->set_range()`
compares plain addresses and warns when a range reaches the I/O page, so
whether QBone can serve that bank depends on how the PRU encodes BBS7 into the
address it decodes. Until that is established the bank stays below it, which
is where the board in the RSXstation write-up sat anyway.

Memory sizing routines walk upward until an address stops answering, so they
count the framebuffer as RAM if it sits directly above the last memory card
address. Leaving a gap between the top of RAM and the bank keeps sizing honest:
with 3 MB of RAM ending at 0o13777776 and the bank at 16000000, the probe stops
at 3 MB and the framebuffer stays out of the operating system's page pool.

Effective ceiling: usable RAM is whatever ends below the bank base, and 3.5 MB
is the most a machine with this board can carry.

## Fit to QBone

The framework already provides everything this needs.

**Registers.** A `qunibusdevice_c` with 32 registers. CSR, CRTC pointer/data,
ICDR/ICSR and the DUART ports are active on DATO; CSR and the DUART status
ports are also active on DATI. Asynchronous status — mouse buttons, vertical
blank — is pushed into the DATI latch by the worker with
`set_register_dati_value()`, so a polling loop is answered by the PRU without
an ARM round trip.

**Video memory.** `ddrmem` is indexed by absolute bus address into the 8 MB DDR
reservation, and the PRU's `emulated_addr_read`/`_write` serve any address
inside `memory_start_addr .. memory_limit_addr` at full bus speed in ordinary
device-emulation mode. So `ddrmem->set_range(fb_base, fb_base + 0x40000 - 2)`
makes the bank live, and the ARM reads the same bytes through
`ddrmem->base_virtual->memory.bytes[fb_base ...]`. Writing the CSR MA field
moves the range.

The 11/73 supplies its own main memory, so the single emulated memory range
belongs to the framebuffer.

**No write notification on memory**, so the renderer polls. The scanline map
and cursor image live in the same bank and are picked up by the same scan.

**Interrupts.** One `intr_request_c` whose vector is set from the emulated
controller's arbitration before each `INTR()`, as the DELQA already does for
its two vectors.

## New files

Under `10.02_devices/2_src/`, mirroring how SIMH factors the same device:

- `vcb01.{hpp,cpp}` — the bus device: registers, CRTC, interrupt controller,
  memory bank management, refresh worker.
- `x11display.{hpp,cpp}` — Xlib wrapper: window, XImage, `XPutImage` of dirty
  rectangles, event pump, pointer grab.
- `scn2681.{hpp,cpp}` — the DUART, two channels.
- `lk201.{hpp,cpp}` — keyboard model: X keysyms to LK201 make/break codes, and
  the host-to-keyboard command set (LEDs, auto-repeat, mode setting) which must
  be acknowledged or drivers hang waiting.
- `vsxxx.{hpp,cpp}` — VSXXX-AA mouse: incremental reports and prompt mode.

## Refresh

A worker thread at a configurable rate, 30 Hz by default:

1. Copy the whole 256 KB bank out of DDR into a shadow buffer.
2. Diff the scanline map against the previous frame; changed entries invalidate
   their screen lines.
3. Diff each 128-byte buffer line; a changed line invalidates every screen line
   mapping to it.
4. Expand invalidated screen lines from 1 bpp into the XImage, compositing the
   cursor with AND or OR per CSR FNC when visible.
5. `XPutImage` the changed spans, then `XFlush`.

Measured on the board, 2026-07-22, rendering from the reserved region itself:

| | |
|---|---|
| read the 256 KB bank out of DDR | 1.11 ms, 226 MB/s |
| a whole pass, nothing changed | 3.47 ms |
| a whole pass, one line repainted | 3.48 ms |

At 30 Hz that is a tenth of one core, so the refresh loop needs no tuning. The
read is a third of it; the rest is the line-by-line comparison, and painting a
changed line costs nothing measurable on top. Should more be wanted later, the
comparison can be restricted to the buffer lines the scanline map references
rather than all 2032.

Video memory is read exactly once per pass, into a buffer the comparison and
the painting then work from: reading it twice - once to compare, once to keep -
would double the only uncached traffic in the loop.

VSYNC fires from the same worker at 60 Hz — SIMH records that drivers notice
when it is slower.

## Input

The X event pump feeds the two DUART receivers. Key press and release become
LK201 make and break codes on channel A. Pointer motion becomes VSXXX
incremental reports on channel B; buttons additionally set CSR MSA/MSB/MSC and
raise their interrupt controller sources.

The mouse is relative, so the window grabs the pointer and warps it back to the
centre each frame, feeding the deltas. A `capture` parameter controls the grab
so the pointer can be released.

## Parameters

Beyond the standard `enabled`, `base_addr` (default 17777200), `slot`,
`intr_level` and `intr_vector`:

- `display` — X display, e.g. `192.168.2.10:0`
- `capture` — grab pointer and keyboard in the window
- `updatefreq` — refresh rate in Hz

## Build

The emulator links dynamically against the distribution the appliance image
carries, and the package names what it needs in its `Depends`.

- Add `libx11-dev:armhf` and its dependencies to the crossbuild image
  (`dpkg --add-architecture armhf` first). Done.
- Add `-lX11` to `makefile_q` and `makefile_u` when the device is linked in.
- Add `libx11-6` to the `Depends` written by `packaging/build-deb.sh`, and
  `libx11-dev` to the on-bone build in `docs/debian-installation.md`.

**Do not link statically.** glibc resolves host names through name service
modules it loads with `dlopen()` even in a static binary, and those modules
belong to the glibc the board runs rather than the one the binary carries.
Measured on the bone: a statically linked ARM binary built against glibc 2.36
dies with SIGFPE inside `getaddrinfo()`, for a good name and a bad one alike,
while the same source linked dynamically resolves correctly. The builder and
the appliance are both Debian trixie so their libraries agree.

The X server needs to accept TCP connections and the bone's address
(`xhost +<bone>` under XQuartz).

## Display names

`display` takes a host name. The name is resolved to a literal address before
Xlib sees it, so what reaches `XOpenDisplay()` is always numeric:

1. Take `display`, or `$DISPLAY` when it is empty.
2. Split off a leading transport prefix at the first `/`, and the
   `:number[.screen]` suffix at the last `:` — after the closing bracket when
   the host is a bracketed IPv6 literal.
3. Pass the name through unchanged when the host part is empty, `unix` or
   `localhost`, or when `inet_pton()` already accepts it as an address.
4. Otherwise resolve it with `getaddrinfo()` and format the result with
   `inet_ntop()`, preferring an IPv4 answer and bracketing an IPv6 one.
5. Reassemble prefix, literal and suffix, and log the name alongside what it
   resolved to.

A failed lookup names the host in the error and leaves the device disabled
rather than retrying into a stall. Resolution repeats whenever `display`
changes and on every reconnect, so a server that moves between addresses is
picked up without editing the configuration.

Resolution uses `getaddrinfo()`, which is why the emulator must not be linked
statically; see the build section.

## Phases

**1 — X output, off the bus. Done.** `vcb01_render` and `x11display`, with
`tools/vcb01_selftest` driving them: 23 checks over bit order, the scanline map
including its 11-bit range and out-of-range entries, dirty tracking across
shared buffer lines, cursor AND and OR compositing, and display name
resolution. `--dump` writes a frame to an image, `--display` animates a scroll,
and `--ddrmem` renders from the board's own video memory and times it. All 23
pass on the workstation and on the board.

**2 — The bus device. First light reached 2026-07-23.** `vcb01.cpp`: the 32
registers, the CSR with its read-only bank switch and monitor size, the CRTC
behind its address and data ports, the eight-source interrupt controller with
its command set and per-source vectors, vertical sync at 60 Hz with blanking
readable in the CRTC pointer, and the refresh worker driving the renderer. The
screen is opened in `on_before_install()` and dropped in `on_after_uninstall()`,
so a display that cannot be reached refuses the enable rather than leaving a
controller on the bus with nowhere to draw.

With the machine cut to 256 KB the bank was free, and the halted 11/73's
micro-ODT drove the board over the Qbus: a DATI on the CSR read back 073401 -
the board's own bank 016 and monitor size - and DATO to video memory at
16000000 and to the CSR put pixels on the X window. The whole path ran end to
end: CPU cycle, PRU, DDR, refresh, render, X server (reached at the
workstation's VPN address, see [[qbone-x-display]]).

It found a renderer bug. Writing video memory while video was disabled and then
enabling it left the screen blank: the bitmap did not change across the
transition, so nothing was marked dirty. A driver that clears memory, enables
video, then draws is fine; one that enables video over content it has already
written showed nothing. Fixed by repainting the whole screen when video-enable
changes, with a self-test check for the sequence.

A standalone program then drove the board from the CPU rather than by hand
(2026-07-23). Running on the 256 KB machine with no operating system, it sets
up the MMU itself - kernel APRs identity-mapped, 22-bit mode, APR 7 aimed at
the real I/O page and APR 6 windowed onto video memory - programs the 6845 with
the setlin.mac timing, lays down a scanline map that repeats 64 buffer lines
down the whole screen, draws into the bitmap, and enables video. It ran to a
clean halt and the pattern appeared. The program is `vcbtest.mac`, kept with
the RSXstation sources.

Two of its own bugs, both found by writing a progress marker to low memory at
each phase: a 16-bit program cannot reach 16000000 at all, so the whole MMU
setup is load-bearing and APR 7 has to map the real I/O page rather than
identity; and an octal offset error put the scanline-map write past APR 6's
window into the I/O page, taking a bus-timeout trap.

Still to come: the same under RSX-11M+ - modelled on the RSXstation write-up's
clear, fill, line and banner programs - if the machine can carry enough memory
for it alongside the bank.

**3 — Keyboard. Done.** The SCN2681 DUART and the LK201 model, in
`vcb01_input`. Channel A carries the keyboard: an X key press becomes the
LK201 make code, releasing the last key sends the all-up code, and the host's
commands are answered where a driver waits on them (keyboard ID, power-up
self-test, mode-change acknowledgements). The DUART's interrupt is the
interrupt controller's source 0. Status registers answer from a mirrored value
so a polling loop does not wake the ARM; only a receive buffer and a mode
register need the ARM on read.

Verified on the bus: injecting a key into the window put its LK201 code in the
receive buffer for the CPU to read - 'a' arrived as 0302, with the receiver-
ready bit set. The protocol details are covered by `vcb01_selftest`.

**4 — Mouse. Done in the same module.** Channel B carries a VSXXX pointer:
motion becomes a three-byte report - a header with the buttons and the sign of
each axis, then the magnitudes - and the buttons show in the CSR, which reads
each as one when up. Prompt mode, where the pointer reports only when the host
asks, is honoured. The report format and button handling are covered by
`vcb01_selftest`; exercising it against a real pointer driver waits on an
operating system that drives the QVSS.

**5 — Integration.** Web UI device page, parameters through the web API,
packaging, and documentation.

## Current round — web use, driver, performance

The device works on the bus and renders to a web canvas over `/ws/vcb01`
(`webvcb01.cpp`), in addition to the X path. This round makes that web use
reliable, gives 2.11BSD a way to draw to the board, and settles whether the
graphics path is fast enough. Requirements in
[`vcb01.md`](../planning/vcb01.md).

### Keyboard input over the web UI

`/ws/vcb01` carries key events as `0x10 <down> <keysym:u32>`; the frontend
listens on `document` for keydown/keyup, gated on the canvas holding focus
(`activeElement === cv`), maps the physical `e.code` through `VCB01_KEYSYM`, and
sends make/break into the same LK201 model (`vcb01_input`) the X path drives. It
is unreliable in three ways, all on the browser side:

- **A release is lost when the canvas loses focus.** A `keyup` that arrives after
  focus moved away (alt-tab, a click elsewhere) fails the `activeElement` gate and
  is dropped, so the emulated LK201 never sees the key go up and the guest holds a
  stuck key.
- **Browser auto-repeat floods makes.** A held key fires repeated `keydown`s; each
  is sent as a fresh make, competing with the LK201's own auto-repeat.
- **Unmapped keys vanish silently** — any `e.code` absent from `VCB01_KEYSYM` is
  dropped with no feedback.

The fix keeps the **emulated LK201 authoritative for repeat**, as real hardware
is:

- Send **one make per physical press** — ignore `keydown` events with
  `e.repeat`.
- Send a release on `keyup`, and add a **window `blur` / visibility-loss handler
  that releases every held key** (the LK201 all-up), so nothing sticks when focus
  leaves the canvas. The frontend tracks the currently-down set to do this.
- **Complete `VCB01_KEYSYM`** for the full LK201 layout, and log (once) an
  unmapped code rather than dropping it silently.
- The emulated LK201 generates **auto-repeat and the all-up metronome** itself;
  the frontend only reports discrete press/release edges.

Validation: a held key auto-repeats at the guest-programmed rate and stops on
release; alt-tab or click-away while a key is down leaves no key stuck; the LK201
command set (ID, self-test, mode/LED, auto-repeat configuration) is answered so a
driver that configures the keyboard does not hang. Covered by `vcb01_selftest`
for the protocol and a manual focus-loss check on hardware.

### 2.11BSD driver — a graphics surface

The goal is a **program-drawable graphics surface**: user programs draw pixels to
the framebuffer, beyond a text terminal.

1. **Establish what 2.11BSD carries.** Check the board's `2.11BSD_qbone.dsk`
   `/sys` tree for a `qv`/QVSS driver (the QVSS is a PDP-11-era DEC board, so a
   `qv`-class driver may exist and simply not be configured into this kernel).
2. **If a driver exists**, configure it into the kernel and confirm the emulated
   VCB01 presents the register and interrupt interface it targets — the CSR
   (with the read-only bank field), the 6845 CRTC, the eight-source interrupt
   controller, and the SCN2681 DUART — adjusting the emulation to the specific
   board the driver expects where they differ.
3. **If none exists**, write a `qv`-class driver exposing the 256 KB video bank.
   The investigation ([`vcb01-2bsd-driver.md`](../vcb01-2bsd-driver.md))
   confirms 2.11BSD carries no `qv` driver (it is a VAX-era device) and that the
   surface cannot be an `mmap` device — 2.11BSD has no `d_mmap` and a split-I/D
   process has only 64 KB of data space against the 256 KB bank. The realistic
   form is a **kernel-mediated `/dev/qv0` character device** (`write`/`lseek`
   linear-framebuffer plus a `blit` ioctl), the kernel reaching the bank through
   a mapping APR as `vcbdemo.mac` already does; the LK201 keyboard is delivered
   through the DUART. Built from the QVSS register interface this emulation
   implements (register-ready, one stale-CRTC-read-back caveat) and `vcbdemo.mac`
   as the starting material.

Because the VCB01/QVSS may have no XXDP diagnostic, its stand-in validation (per
the [device implementation standard](../planning/device-implementation-standard.md))
is a guest program that opens the framebuffer device and draws a known pattern,
read back over `/ws/vcb01`, plus keyboard delivery into a reading program.

### Performance

The refresh worker is already cheap — a full 30 Hz pass measured at ≈3.5 ms
(§Refresh) — so the RSX-11 demo's slowness is most likely the **guest's per-pixel
bus writes** (each pixel is a DATO through the PRU/ARM), not the renderer. This
round **profiles before committing** to any optimisation:

- Measure on hardware: per-pixel DATO latency for framebuffer writes, the
  `/ws/vcb01` frame/stream rate, and the guest draw-loop time, to attribute the
  cost.
- **Optimise only a QBone-side cost the profile actually shows** (e.g. streaming
  or dirty-rectangle handling). A cost that lives in the guest's byte-at-a-time
  drawing over the bus is inherent to how the program writes video memory and is
  recorded, not chased. No speedup is pre-committed.

## What the layout was checked against

Lee K. Gleason's `setlin.mac`, written for the RSXstation and linked from that
write-up, drives a real board and agrees with every constant here:

| | the program | this emulation |
|---|---|---|
| CSR, cursor X | 177200, 177202 | registers 0, 1 |
| CRTC pointer, data | 177210, 177212 | registers 4, 5 |
| interrupt controller status | 177216 | register 7 |
| video memory | 16000000..16777777 | bank 016, 256 KB |
| scanline map | 0x3F800 into the bank | `MAP_LW` 0xFE00, longwords |
| cursor bitmap | 0x3FFE0 into the bank | `CURSOR_LW` 0xFFF8 |
| a map entry | one 16-bit word per screen line | two 11-bit entries per longword |

It programs the CRTC by writing the pointer and then the data, loads the map
linearly so screen line N shows buffer line N, and takes the top 8 KB of the
bank through KISAR6 to reach the map and the cursor.

Its CRTC values, which the write-up credits to the Vintage Computer Forum:
horizontal total 41, displayed 32, sync position 34, sync width 6; vertical
total 51, adjust 10, displayed 50, sync position 50; mode 4, maximum scan line
15, cursor start 0, cursor end 1.

## Test software

RSX-11M+ supplies the environment the RSXstation programs were written for:
MACRO-11, the task builder, `[1,1]EXEMC.MLB`, `[11,10]RSXMC`,
`[3,54]RSXVEC.STB` and `[1,1]EXELIB.OLB`. A system disk lives in
`qbus-front-panel/systems/rsx11mplus` — a 300 MB MSCP image, `PiDP11_DU0.dsk`,
booted by `boot.ini` under SIMH as an 11/70 with 4 MB.

Two ways to use it, and they compose:

- **Build under SIMH.** Assemble and task-build the test programs on the
  emulated machine, where the edit-build cycle costs nothing and needs no
  hardware.
- **Boot the real 11/73 from the same image.** QBone's MSCP emulation serves
  the disk, which puts RSX-11M+ on the real machine with the emulated VCB01 in
  its backplane. Graphics tasks then run where the RSXstation write-up ran
  them, and its findings — the working 6845 initialisation values, the APR
  mapping through KISAR6 — carry over directly.

The image boots on the real 11/73 as generated, verified 2026-07-22: the
KDJ11 ROM finds DU0 over the emulated UDA50 and RSX-11M-PLUS V4.6 BL87 comes up
on 2044 KW, reaching MCR and answering commands. No SYSGEN is needed, so phase 2
tests can be ordinary RSX tasks. The board configuration is saved as
`rsx11mplus`: `uda`, `uda0` with `useimagesize` set, and `KW11`.

Tasks reach the running machine over the emulated Ethernet, or by writing them
into the disk image on the bone.

## Optional work

Worth doing, but nothing depends on them.

- **Refuse to map over physical memory.** Before enabling a range, probe it
  from the bus master side while emulation for it is still off; anything that
  answers means real memory already lives there. Refusing the range with a
  clear message turns a silent double-drive of the bus into a startup error.
  This generalises past the framebuffer to `emulate_memory()` and to `m i`.
- **A display path that needs no X server.** Streaming dirty tiles to a canvas
  in the existing web interface reaches a Mac with nothing installed, and a VNC
  server would reach the built-in Screen Sharing client. Either would sit
  behind the same renderer as the X output.

## Open items

- **Hardware availability.** Testing on the bus waits until the 11/73 carries a
  memory card small enough to leave the bank free. Phases 1 and 3 need no bus
  and the RSX-11M+ image builds under SIMH, so the work up to first light on
  real hardware is unblocked.
- **Interrupt vector timing.** Real hardware selects the vector during the IAK
  cycle; QBone commits it when `INTR()` is scheduled. Two sources becoming
  ready between the schedule and the acknowledge would deliver the earlier
  source's vector.
- **How tall the screen is.** The renderer draws 864 lines, which is what SIMH
  uses. The RSXstation write-up calls the board 1024 x 768, and the CRTC values
  it loads give 50 displayed rows of 16 scan lines, or 800. The scanline map
  holds 1024 entries either way, so the number decides how large a window to
  open and how much of the map to honour, not how memory is read. Deriving it
  from the programmed timings would settle it.
- **Monitor size.** CSR MOD reports a 19-inch VR260 or a 15-inch monitor;
  decide which the emulated board claims.
- **Contention for the emulated memory range.** `application_c::emulate_memory()`
  backs every address between the top of physical memory and the I/O page with
  DDR, using the same single range the framebuffer needs. It runs from the
  devices menu's `m i` and its siblings, not at startup, so the two collide only
  when both are asked for. The device claims the range when enabled, and `m i`
  has to see that claim and stay below the bank.
