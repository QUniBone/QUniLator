# The VAX host

Where the VAX UNIBUS host of [`vax-unibus-plan.md`](vax-unibus-plan.md) stands.
Stages 0 and 1 are complete: the processor runs as a device of the application
on a backplane-less UniBone, and boots VMS over the web console.

## Stage 0

The harvest: the simh VAX-11/780 is vendored, it builds on the
workstation and cross-builds for the board, DEC's own processor diagnostics
pass on it, and it boots VMS both as the stock simulator and with a shim in
place of simh's command interpreter - which is the form it takes when it moves
into the QUniLator application in stage 1.

## What is in the tree

| | |
|---|---|
| `91_3rd_party/simh_vax` | the vendored simh sources and their provenance |
| `10.07_vax/2_src/makefile` | the build, three targets |
| `10.07_vax/2_src/shim` | the stand-in for simh's command interpreter |
| `10.07_vax/3_tests` | the runs that judge the stage |
| `10.07_vax/4_deploy` | binaries and test output, not committed |

`make -C 10.07_vax/2_src -j` builds all three, clean under gcc and clang.
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
the run, so the caller's volume is untouched.

```
10.07_vax/3_tests/vax-diagnostics.sh
```

runs DEC's own processor diagnostics, which is to this core what the MAINDEC
tapes of `10.06_cputest` are to the KA11 and the KD11: EVKAA standalone, then
EVKAB, EVKAC and EVKAD under the VAX Diagnostic Supervisor - basic
instructions, floating point and compatibility mode. All four pass. The three
Supervisor tests read their results with regular expressions and so want a
build with PCRE2, which the makefile takes when pkg-config finds it and reports
skipped when it does not.

The Supervisor attaches its UDA50 at 772150, where the emulated one answers.
Once the bus stages are done this same suite becomes a test of `uda.cpp`.

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

Two of simh's facilities are deliberately absent rather than stubbed:

- **Asynchronous I/O.** The shim build omits `SIM_ASYNCH_IO`, so the core has no
  reader thread of its own.
- **Idling.** An embedded core must not put its host thread to sleep, because
  that thread has a bus to serve.

The clock calibration is kept, measured against the elapsed time the embedding
supplies rather than against the workstation's own clock. On the board that
will be the time source the other device models already share.

```
10.07_vax/3_tests/shim-console.sh
```

assembles a few instructions, loads them at address 0 and runs them. They write
`OK` to the console and halt, which exercises the loader, instruction execution,
the console channel, the event queue that completes each character, and the stop
path. `shim-rate.sh` runs a five-instruction loop against the clock and reports
the instruction rate.

## vax780-shim-disk — the shimmed core carrying an operating system

The shimmed core with simh's own MSCP controller and disk layer inside it, and
`shim-boot-vms.sh` to boot VMS on it. It is a test configuration and not a
device: its controller answers at 772150, where the emulated UDA50 answers, so
the two are alternatives and nothing in the application's configuration model
reaches this binary.

It earns its place as the control for the stages that put the emulated device
on the bus. When VMS will not boot from that one, the fault could be in the UBA
register window, the interrupt path, the map translation, `mscp_server.cpp` or
the shim; a core that still boots from the disk inside it puts everything above
the bus beyond suspicion. It is also the only thing that works the shim hard -
a boot is order 10^8 instructions with a real interrupt load, against the 400
of the console test.

Bringing it up found five faults in the shim, all of them things scp does that
an embedded core still needs:

- `sim_finit()` was never called, so simh's file layer never worked out that
  large files are supported, and every disk format probe answered no.
- A unit's `dptr` back pointer was never filled in, which scp does when it walks
  the device list at startup.
- `load_cmd()` ignored the switches in the command it was given, so `-O` was
  read as a file name and the bootstrap landed at address 0 instead of at its
  origin.
- `SET` matched a modifier's name exactly, where scp matches the head of it, and
  called the setter only for the extended kind - so `FORMAT=SIMH` matched
  nothing and `CPU 8M` set a flag without resizing memory.
- The interval clock was left uncalibrated and coscheduling counted ticks as
  instructions, which between them ran the guest's clock some tens of times too
  fast: VMS reached its banner and then spent every instruction in its own timer
  routine.

The last of those also settled how batching works. A bounded run is a unit
queued for the end of the batch whose service routine stops the processor,
because the event queue's accounting rests on `sim_interval` being the time
remaining to the entry at its head: shortening it behind the queue's back makes
the next update charge the difference as though those instructions had run, and
the simulated clock then runs away from the work actually done.

## On the board

The core is portable C, and both targets cross-build for the AM335x under the
armhf toolchain `crossbuild.sh` carries — the makefile header gives the command.
Measured on `unibone.huebner.org`, a backplane-less UniBone (AM335x, 474 MB),
against the same workstation figures:

| | workstation | board |
|---|---|---|
| `shim-rate.sh` | 38.9 M instructions/s | 0.98 M instructions/s |
| `boot-vms.sh` to the login prompt | 13 s | 55 s |
| `shim-boot-vms.sh` to the login prompt | 19 s | 45 s |
| `vax-diagnostics.sh` | 15 s, all four pass | 31 s, EVKAA passes |

A VAX-11/780 is about half a million instructions per second, so the board runs
the machine at roughly twice the speed of the original. The board figure was
taken while the board's own PDP-11/34 emulation was running and holding about a
third of the processor, so an idle board gives more.

These are the stage 1 baseline: a later stage that moves them by an order of
magnitude has changed something structural.

What runs there today is the simulator standing alone, not a VAX under
QUniLator. The step from one to the other is stage 1 below.

The board build leaves out simh's ethernet and its regular expressions -
`SIMH_USE_NETWORK=no SIMH_USE_REGEX=no` - so it wants no package the appliance
image does not carry. The board's ethernet is the emulated DEUNA on the bus, and
the diagnostics that want a pattern matcher are a workstation test.

A backplane-less board reaches the rest of the plan by a shorter road than the
plan assumes, because `internal_bus` (see `unibone-bringup-issues.md` §20) makes
the PRU answer its own bus cycles. Stages 2 to 4 exist to put device registers
and DMA on real bus wires and to prove the map lookup fits the UNIBUS slave
timing budget; with the bus internal the same PRU path carries them with no
external deadline to meet and no third-party controller to arbitrate with. The
DW780 register window and the interrupt path of stage 2 are still needed, and
stage 5 then runs against the emulated devices.

## Stage 1 — the processor as a device

`cpuvax_c` is the VAX inside the application. It descends from `unibuscpu_c`
rather than from `cpu_base_c`, which is shaped for the PDP-11 and its sixteen
bit program counter, console switch register and power-fail trap through vector
24; a VAX has none of those.

The emulation core lives in `10.02_devices/2_src/cpuvax`, beside the KA11 and
the KD11-EA, because it is one of them. `makefile_u` compiles it and the
vendored simh as C with the VAX configuration into their own object directory,
so this application's C++ and its defines never reach them. The QBUS build
carries none of it: a VAX has no Q-bus.

The device gives the seam its three things. The console is an `rs232adapter`,
the type a DL11 line uses, so the web console bridge carries it the same way
and reaches it at `/ws/console/vax`. The time source is whichever clock the
emulation is running on. And `worker()` runs the processor in batches, charging
the emulated clock two microseconds an instruction - what a real 780 spends.

Its parameters are the machine: `memory` in megabytes, `bootimage` and
`bootdevice` for the volume it boots from, `batch` for how long a pass runs
before the switches are looked at again. `run_led`, `PC`, `PSL` and
`cycle_count` are what it is doing. Enabling the device builds the machine and
leaves it stopped; `start_switch` boots it.

The volume is attached to the controller inside the core, and that is
scaffolding: it answers at 772150, where the emulated UDA50 answers, so the two
are alternatives. Stage 3 is where the disk moves onto the bus and this one
goes. The interrupt path waits for the UNIBUS adapter of stage 2, so nothing on
the bus can interrupt this processor yet.

Measured on `unibone.huebner.org`: VMS 4.7 boots to `Username:` over
`/ws/console/vax`, and runs at **2.5 M instructions per second** with the
system idle at that prompt - about five times a real 11/780. The synthetic
figure below is lower because its loop reads and writes memory on every pass,
where an idle VMS mostly does not; both are worth keeping, one for the machine
and one for the emulator.

## What stage 2 needs next

- The DW780's register window, with VAX access to UNIBUS I/O space going
  through `unibone_dati()` and `unibone_dato()` onto the bus.
- `on_interrupt()` widened to carry the bus request level, and BR4-7 mapped to
  VAX IPL 14-17 with the adapter's vector offset applied.
- A bus timeout becoming a UBA error and a VAX machine check.

A backplane-less board shortens what follows, because `internal_bus` makes the
PRU answer its own cycles: the same path carries the register window and the
DMA of stages 3 and 4 with no external timing deadline to meet.
