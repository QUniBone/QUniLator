# VCB01 support

**Status:** Ready — implementation plan drafted in
[`vcb01-plan.md`](../plans/vcb01-plan.md) (device background plus the current-round
web-keyboard, 2.11BSD-driver, and performance work).

Reliable VCB01 use: keyboard input from the web UI, and a working 2.11BSD driver
for the graphics device.

## Current state

The VCB01 graphics device renders into the console phosphor and accepts keyboard
input through the web UI. Keyboard input is not delivered reliably. Whether
2.11BSD has a driver that binds the VCB01 (QVSS/`qv`-class framebuffer and its
LK201 keyboard) is unconfirmed. Background on the device is in
[`vcb01-plan.md`](../plans/vcb01-plan.md) and [`vcb01-status.md`](../vcb01-status.md).

## Requirements

### Keyboard input

- VCB01 keyboard input works reliably from the web UI.

### 2.11BSD driver

- 2.11BSD drives the VCB01: enable the driver if the kernel already carries one,
  or write one if it does not.

### Implementation standard

- The VCB01 follows the
  [device implementation standard](device-implementation-standard.md) — built
  from its original manual (kept in the repo, OCR'd if needed). The VCB01/QVSS
  may have no XXDP diagnostic; if so, its stand-in validation is an open question
  in that standard.

### Performance

- The VCB01 graphics path is fast enough to run the demos at their intended
  speed. The RSX-11 graphics demo currently runs much slower than expected.

## Open questions

### Keyboard input

- What is the failure — dropped keystrokes, wrong scan codes, timing, focus, or a
  lost/idle connection?
- Where in the path does it break — browser key capture, the WebSocket transport,
  or the emulated LK201 keyboard delivering to the guest?
- Does the guest expect LK201 semantics the emulation does not yet provide
  (auto-repeat, key-up events, the make/break protocol)?

### 2.11BSD driver

- Does the 2.11BSD source tree already contain a QVSS/`qv` driver, and is it just
  not configured into this kernel?
- Does the emulated VCB01 present the register and interrupt interface such a
  driver expects, or does the emulation need to match a specific board the driver
  targets?
- What is the end goal for the driver — console framebuffer, an X-like graphics
  surface, or just a working terminal on the VCB01 screen?
- How is this validated — a diagnostic, a getty on the VCB01 console, or a guest
  program drawing to the framebuffer?

### Performance

- Where is the RSX-11 graphics demo's time going — the emulated framebuffer bus
  accesses (PRU/DMA throughput), the web-side phosphor rendering, or the guest
  code itself?
- What is the expected speed to compare against — real VCB01 hardware, or a known
  frame rate?
- Is the slowness specific to the RSX-11 demo, or does any heavy framebuffer
  drawing show it?
