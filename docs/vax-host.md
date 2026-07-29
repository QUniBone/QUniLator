# The VAX host, stage 0

Where the VAX UNIBUS host of [`vax-unibus-plan.md`](vax-unibus-plan.md) stands.
Stage 0 is complete: the simh VAX-11/780 is vendored, it builds on the
workstation, it boots VMS, and the core also builds and runs with a shim in
place of simh's command interpreter, which is the form it takes when it moves
into the QUniLator application in stage 1.

Nothing here touches a board. Everything below is a host build and a host run.

## What is in the tree

| | |
|---|---|
| `91_3rd_party/simh_vax` | the vendored simh sources and their provenance |
| `10.07_vax/2_src/makefile` | the host build, two targets |
| `10.07_vax/2_src/shim` | the stand-in for simh's command interpreter |
| `10.07_vax/3_tests` | the runs that judge the stage |
| `10.07_vax/4_deploy` | binaries and test output, not committed |

`make -C 10.07_vax/2_src -j` builds both targets, clean under gcc and clang.
The vendored tree is compiled with `-Wall` and a suppression per diagnostic it
raises, each named and explained in the makefile; the shim, being this
project's own code, is held to `-Wall -Wextra` with nothing suppressed.

## vax780 — the stock simulator

The whole harvest, built as simh builds it: the 780 processor and system files
plus the UNIBUS peripherals the PDP-11 simulator supplies, driven by simh's own
`scp.c`. It is the reference, and the thing that says the file set is complete.

```
10.07_vax/3_tests/boot-vms.sh ~/vax/rd54-vms47-clean.dsk
```

boots VMS 4.7 from an MSCP volume, answers the startup dialog and stops at the
`Username:` prompt. The system disk is supplied by the caller and copied before
the run, so the caller's volume is untouched. A run takes about three minutes on
an Apple workstation.

## vax780-shim — the core as it will be embedded

The same processor and the same 780 system files, with `10.07_vax/2_src/shim` in
place of `scp.c`, `sim_console.c`, `sim_timer.c` and `sim_tmxr.c`, and without
the UNIBUS peripherals: on the board those are the emulated devices of
`10.02_devices/2_src`, reached over the real bus, and a second set inside the
CPU model would be two devices at one address.

`simh_shim.h` states the seam. The embedding supplies a console byte channel and
an elapsed-time source, and drives the processor in batches:

```c
simh_shim_bind (&host);
simh_shim_reset ();
for (;;)
    simh_shim_run (batch_size);
```

`shim_main.c` is the first embedding, on the workstation terminal. The CPU
device of stage 1 is the second.

What the shim implements for real is what an executing processor uses: the event
queue, device reset, unit attach and detach, the bootstrap loader, the console
byte stream, and the diagnostic output. What it stubs is what a processor
reaches only through a typed command, and each stub says which entry point was
reached, so a core that grows a new dependency on scp reports it rather than
failing quietly.

Three of simh's facilities are deliberately absent rather than stubbed:

- **Asynchronous I/O.** The shim build omits `SIM_ASYNCH_IO`, so the core has no
  reader thread of its own.
- **The clock calibration.** simh measures a simulator's instruction rate
  against the workstation's wall clock. On the board the emulation has its own
  time source, which the other device models already share, and the calibration
  belongs there; the shim gives a device the interval it asked for.
- **Idling.** An embedded core must not put its host thread to sleep, because
  that thread has a bus to serve.

```
10.07_vax/3_tests/shim-console.sh
```

assembles a few instructions, loads them at address 0 and runs them. They write
`OK` to the console and halt, which exercises the loader, instruction execution,
the console channel, the event queue that completes each character, and the stop
path. `shim-rate.sh` runs a five-instruction loop against the clock and reports
the instruction rate.

## On the board

The core is portable C, and both targets cross-build for the AM335x under the
armhf toolchain `crossbuild.sh` carries — the makefile header gives the command.
`SIMH_USE_NETWORK=no` turns off simh's own ethernet, which a board build does
not want: its ethernet is the emulated DEUNA on the bus.

Measured on `unibone.huebner.org`, a backplane-less UniBone (AM335x, 474 MB),
against the same workstation figures:

| | workstation | board |
|---|---|---|
| `shim-rate.sh` | 38.9 M instructions/s | 0.97 M instructions/s |
| `boot-vms.sh` to the login prompt | 13 s | 55 s |

A VAX-11/780 is about half a million instructions per second, so the board runs
the machine at roughly twice the speed of the original. The board figure was
taken while the board's own PDP-11/34 emulation was running and holding about a
third of the processor, so an idle board gives more.

These are the stage 1 baseline: a later stage that moves them by an order of
magnitude has changed something structural.

What runs there today is the simulator standing alone, not a VAX under
QUniLator. The step from one to the other is stage 1 below.

A backplane-less board reaches the rest of the plan by a shorter road than the
plan assumes, because `internal_bus` (see `unibone-bringup-issues.md` §20) makes
the PRU answer its own bus cycles. Stages 2 to 4 exist to put device registers
and DMA on real bus wires and to prove the map lookup fits the UNIBUS slave
timing budget; with the bus internal the same PRU path carries them with no
external deadline to meet and no third-party controller to arbitrate with. The
DW780 register window and the interrupt path of stage 2 are still needed, and
stage 5 then runs against the emulated devices.

## What stage 1 needs next

- Wrap the shim as a `unibuscpu_c` subclass beside `cpu_c`, with the batching of
  `simh_shim_run()` driving `worker()`.
- Point the shim's elapsed-time source at
  `the_flexi_timeout_controller`, as `cpu.cpp` does for the KA11.
- Route the console byte channel to the web console channel.
- Cross-build the vendored tree for the AM335x and record the instruction rate,
  which is the baseline every later stage is measured against.
