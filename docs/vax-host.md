# The VAX host

Where the VAX UNIBUS host of [`vax-unibus-plan.md`](plans/vax-unibus-plan.md) stands.
Stages 0 to 4 are done: the processor runs as a device of the application on a
backplane-less UniBone, reaches the emulated devices' registers on the bus and
is reached by their interrupts, and boots VMS V4.7 from the emulated UDA50 -
with the disk's transfers going out as bus cycles and translated by the PRU
through the adapter's map registers, not by this program.

Two of stage 4's checks need hardware this board does not have: the added slave
latency is derived rather than measured, and a real third-party UNIBUS DMA
board has not been tried. Both want a UniBone in a backplane.

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
plan assumes, because `internal_bus` (see `docs/unibone-bringup-issues.md` §20) makes
the PRU answer its own bus cycles. Stages 2 to 4 exist to put device registers
and DMA on real bus wires and to prove the map lookup fits the UNIBUS slave
timing budget; with the bus internal the same PRU path carries them with no
external deadline to meet and no third-party controller to arbitrate with. The
DW780 register window and the interrupt path of stage 2 are still needed, and
stage 5 then runs against the emulated devices.

## On the dashboard

The processor has a card of its own, because the PDP-11's does not fit: a VAX
has a thirty-two bit program counter read in hex, a status longword laid out
differently, and no switch register. It shows the counter and the longword, the
mode and the level the processor runs at, the condition codes, and what the
UNIBUS adapter has carried - register accesses, interrupts and mapped transfers,
which is what says whether the adapter is doing anything. RUN, HALT and START
work as the PDP-11's do.

The console follows the processor. A VAX carries its console terminal inside
itself rather than on the bus, so a machine with one enabled has its console
there and nowhere else, and the dashboard's terminal connects to
`/ws/console/vax` without being told to.

Reading a register in the base its documentation uses is now the device model's
business rather than each card's: a parameter declared in base 16 arrives
rendered in hex, as one declared in base 8 always arrived in octal.

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

## Stage 2 — the I/O page on the bus

The DW780 model reaches a peripheral's registers through simh's dispatch tables,
which `build_ubus_tab()` fills in from the devices simh itself carries. An
emulated VAX on a UniBone wants the other kind, so after each reset every
address in the I/O page that no simh device claimed is pointed at the two
routines of `cpuvax/simh_shim_bus.c`, which hand the cycle to the device. simh's
own dispatch keeps its addresses, which is what lets a machine boot from the
controller inside the core while the devices on the bus are brought up around
it. Nothing in the vendored tree is edited.

`bus_iopage` turns it on. A cycle then goes through `qunibusadapter` as a DATI
or DATO like any other bus master's, and `bus_cycles` and `bus_timeouts` count
what happened.

`bus_examine` and `bus_deposit` are what a real 780's console does, and the only
way to reach a device register before an operating system has a driver for it.
They are asked for from the web interface and performed by the processor's own
thread, because a bus master's transfer is serviced against that thread's
request.

Against QUniLator's DL11 on `unibone.huebner.org`:

| | |
|---|---|
| `bus_examine 777564` | `000200` — the transmitter is ready, which is what an idle DL11 says |
| `bus_deposit 777564` of `000100` then examine | `000100` — the write reached the model and reads back |
| `bus_examine 777560` | `000000` — the receiver is idle, and untouched by the write |
| `bus_examine 764000` | `no answer from 764000`, and the machine keeps running |

That is registers read and written from the console with the values the device
model reports, and an unpopulated address answered with an error rather than a
hang.

### The interrupt path

The bus ranks its requests BR4 to BR7 and a VAX ranks its own IPL 14 to 17, so
the processor has to know which level a request was granted at. The PRU knew and
threw it away: `sm_arb_worker_cpu()` computes `requested_intr_level` when it
grants, SACK clears the grant mask before the vector arrives, and
`mailbox.events.intr_slave` carried the vector alone. The level is now kept in
the arbitration state and reported with the vector, `unibuscpu_c::on_interrupt()`
carries it, and a PDP-11 ignores it as it always did.

On the VAX side the adapter keeps a bitmask of pending requests per hardware IPL
and a vector for each bit in it. A request from the bus takes one reserved bit,
which is enough because the arbitration lets one interrupt through at a time.

A processor also has to say it is there: until `ARM2PRU_CPU_ENABLE`, the PRU runs
neither the arbitration a device's request needs nor the state machine that
catches the INTR which follows it, and a device raises a request and sees
nothing come of it. And it has to publish the level it is running at, so the
arbitration knows what to compare a request against - the VAX's IPL 14 to 17
mapped back to BR4 to BR7, and anything below that leaving every device free.

Against QUniLator's own devices, with VMS running:

| | |
|---|---|
| DL11 at BR4, vector 060 | armed by depositing `000100` in its RCSR and typing a character; the processor took the interrupt |
| KW11 line clock at BR6, vector 100 | armed by depositing `000100` in `777546`; the processor took the interrupt |

Two levels, and the second is what proves the level is carried: a BR6 request
that arrived as BR4 would have collided with the DL11's, still pending, and been
refused with a warning. None was logged.

## Stage 3 — the map and DMA, so far

A device on the bus names an eighteen bit address; the memory it means is the
processor's, and the adapter's map registers join the two. The vendored model
already does all of that - the map lookup, the byte offset bit that lets a
transfer start on an odd boundary, and the invalid map entry that must fail a
transfer rather than corrupt memory - so `simh_shim_bus_dma()` is one call into
`Map_ReadW()` or `Map_WriteW()`.

`qunibusadapter` offers a device's transfer to the installed processor before
scheduling it, through a new `unibuscpu_c::on_dma()`. A processor whose memory
is its own answers it and the cycles never reach the bus, which is what this
stage asks for; stage 4 moves the same translation into the PRU. `dma_words`
and `dma_failures` count what happened.

Booting from a controller on the bus needs simh's own MSCP controller out of the
way but still known: `SET RQ DISABLED` takes it out of the I/O page while
leaving it in the device list, so the boot command still reads from its
descriptor where it would have answered and tells the bootstrap. With no
`bootimage` set, that is what the processor does, and the address is left to
whatever answers it on the bus.

Bringing this up found one fault of the same family as stage 1's: a boot resets
the machine, `reset_all()` rebuilds the I/O page dispatch from the devices simh
carries, and that dropped everything the bus had claimed - so a bootstrap found
nothing where its controller should have been. Every path that resets now goes
through one place that takes the I/O page back afterwards.

### The claim has to be held, not just made

A device may reconfigure itself while the machine runs - simh's MSCP controller
ends its reset by running the auto-configuration - and that rebuilds the whole
I/O page dispatch from the devices the core carries, taking back whatever the
bus had claimed. Nothing announces it, and the count taken when the page was
claimed goes on reading 4096 while the boot device's own address has quietly
gone back to the controller inside.

That is what had VMB failing with no bus cycle at all: it was talking to simh's
controller the whole time, and with no volume attached to it there was nothing
to boot from. The claim is now witnessed by one of the slots it took and
re-asserted whenever that slot changes hands, checked where every device's work
passes through. `bootdev_on_bus` reports the answer live, because the count
never could.

### Where it stands

With the claim held, VMB drives the emulated UDA50 over the bus and the
controller moves data into VAX memory through the map registers:

| | |
|---|---|
| `bootdev_on_bus` | true - the controller's address is answered by the bus |
| `iopage_dispatches` | tens of thousands - VMB is reading and writing its registers |
| `dma_words` | over a thousand - **the map registers are carrying transfers** |

That is stage 3's mechanism working: a device model of this project, driven by
a bootstrap written for real hardware, reaching memory through the adapter's
map. It is the first point at which the device models are genuinely under test.

### The controller inside has to be put aside, not overwritten

Re-asserting a claim that a device keeps taking back is a fight, and a fight has
a loser: an access that lands while the controller inside the core owns its
address again reaches a controller with no volume in it. That is what made the
attempts alternate between failing to initialise the device and getting as far
as looking for the boot file.

The controller inside is now disabled outright when the peripherals are on the
bus - but only after the first reset, because the address it answers at is
assigned by the auto-configuration, and a device disabled before that takes no
part in it and keeps no address. Disabled afterwards it keeps the address in its
descriptor, which is what the boot command reads to tell the bootstrap where to
look, while taking no further part in building the I/O page.

The difference is plain in what follows:

| | before | after |
|---|---|---|
| register accesses | 17157, thrashing | 42, and it stops |
| words through the map | 754 | **11164** |

### The protocol runs

The MSCP conversation completes. The controller's own log, at debug, records the
initialisation walking STEP1 to STEP4 and then the commands themselves:

	Message size 0x30 opcode 0x4  ...   SET CONTROLLER CHARACTERISTICS
	Message size 0x30 opcode 0x9  ...   ONLINE
	Message size 0x30 opcode 0x21 ...   READ, 512 bytes, lbn 1
	MSCP RWE 0x21 unit 0 ... count 12288 lbn 6070
	MSCP RWE 0x21 unit 0 ... count 47104 lbn 164548
	cmd 0x0 st 0x0 fl 0x0

Every command answers with status zero, the bootstrap reads the home block, the
index file and the directories, and the two large reads are a bootstrap image
and the system image being loaded. A hundred and sixty thousand words have moved
through the map registers with no transfer refused.

The controller does not interrupt, and is right not to: the bootstrap gives it a
vector of zero when it initialises it, which is how a program that polls says it
wants none. So the interrupt count staying at zero here says nothing about the
interrupt path, which stage 2 exercised on its own.

### The arbitration level, and what it costs to publish it late

Two things were wrong in how the processor talks to the arbitration on the bus,
and both showed up as a boot that moved less data the faster the board ran.

The level published is the **adapter's**, not the processor's. A DW780 takes a
device's request whenever it has a slot for it, latches the vector, and posts
the request to the processor, which services it once its own IPL permits. A
bootstrap sits at IPL 31 for its whole life; publishing 31 to the arbitration
holds off every grant, so a device the processor is waiting for can never
announce itself. What the arbitration has to hold off is only a level whose slot
is still full, which is what `simh_shim_bus_interrupt_pending()` reports.

And the level is published **every pass**, not when it changes. Granting an
interrupt leaves the arbitration holding its grants until the processor writes
the level again - that is how it is told the vector has been taken - and a level
written only when it differs leaves the bus held after the first one. A device's
transfer waits on the same grants, so the symptom is not a lost interrupt but a
disk that stops.

The effect is large, and it scales the wrong way without the fix, because the
processor opens a grant window once per batch:

| instructions per batch | words moved in 30 s, before | after |
|---|---|---|
| 200 | 0 | |
| 2000 | 0 | |
| 10000 | 754 | 8526 |
| 100000 | 8840 | |

### The console's window on memory

A 780's console reads and writes the processor's memory by physical address, and
`mem_examine`, `mem_deposit` and `mem_data` are that, alongside the `bus_*` trio
that reaches the I/O page. It is what identified the loop the machine sits in,
and it is the tool the rest of this stage is worked out with.

The map registers are right, which it settled first. Each transfer's destination
is traced beside it, and the bootstrap reprogramming register 0 between reads
shows up as it should:

	MSCP RWE 0x21 ... count 512 lbn 1
	dma write 000030 -> 00100018, 256 words
	MSCP RWE 0x21 ... count 512 lbn 155658
	dma write 000000 -> 0000e000, 256 words

### Where it stands: the machine waits for a controller that is asleep

The processor sits at PC 107F, and the two instructions there are

	107F  B5 CF 57 FF   TSTW  0FDA
	1083  19 FA         BLSS  107F

which is the bootstrap polling the top half of the response ring descriptor -
the OWN bit - and looping while the controller still owns it. Memory says the
same:

	0FD4  00010001      the two ring transition words, both set
	0FD8  C003DE18      response descriptor, OWN set
	0FDC  C003DDE4      command descriptor, OWN set

So the host has handed the controller a command and a response slot and is
waiting. The controller has both and is doing nothing, because its polling
thread only runs when the host rings its doorbell, which is a *read* of IP.
Reading IP by hand from the console proves it - the machine runs on at once:

| | words moved | register cycles |
|---|---|---|
| stalled | 1132 | 69 |
| after one read of IP | 9532 | 91 |

and then stops the same way again. Every stall in this stage has been this one,
at a different command each time, which is what a race between the host's insert
and the controller going back to sleep looks like.

The comm area layout is not the difference. simh's own RQ, which this same
bootstrap drives to a login prompt, puts the response ring at the comm base, the
command ring after it, the command interrupt word at base-4 and the response
interrupt word at base-2, and the model here agrees with all four. Nor is it a
lost wakeup: a doorbell arriving while the thread is executing commands leaves
the state at InitRun, which the end of the pass turns back into Run rather than
Wait.

### Why the bootstrap stops ringing, and what a controller owes it

It does not stop. The doorbell is only owed to a controller that has *stopped*
polling, and the host takes the controller to be polling still while it is
answering: it writes the next command into the ring as soon as it has read the
response, and expects the controller to come round and find it. simh's own RQ,
which the same bootstrap drives to a login prompt, behaves that way by
construction - its queue service takes one command per scheduled pass and
reschedules itself, so the ring is looked at again after the host has had its
turn to run.

The controller here runs on its own thread and drained the ring in a tight loop
instead, so it read the ring empty microseconds after posting a response - long
before the host could answer it - and went back to sleep on a command that was
about to arrive. Neither side then moves: the host waits for a response, the
controller waits for a doorbell that is not owed.

So a pass that did work now watches the ring for a while before sleeping, and
the poll ends only when nothing more arrives. The window has to cover the host
seeing the response and answering it, which on an emulated processor executing
in batches is a few of those batches.

| linger | words moved in one boot |
|---|---|
| none | 2342 |
| 30 ms | 41860 |
| 500 ms | 113120, and VMS reaches its banner |

### VMS boots from the emulated UDA50

	  VAX/VMS Version V4.7 28-Oct-1987 13:00

on the console, from an image on a UDA50 that answers on the bus, with every
transfer carried through the UNIBUS adapter's map registers by the processor
itself. The interrupt path opens at the same moment: `bus_interrupts` leaves
zero for the first time, because VMS - unlike the bootstrap - asks the
controller for a vector and is served at the level the adapter grants.

### Where it stands: VMS's own driver re-initialises the controller forever

The banner is as far as it goes. What follows it is the handover from the
bootstrap to VMS's own disk driver, and that driver cannot get the controller
initialised. The loop is exact and repeats about every four seconds:

	Transition to Init state S1          the controller offers S1, SA = 004000
	DATO SA  ... 122377                  the host writes its S1 word, 0xA4FF
	  resp ring 0x10, cmd ring 0x10, vector 0x1fc, ie 1
	Transition to Init state S2          SA = 010244, and an interrupt
	INTR() req: dev uda, level/vector 5/774
	DATO IP                              the host writes IP - a hard init
	Reset due to IP write

The bootstrap asked for one-entry rings and no interrupts; VMS asks for sixteen
of each, interrupts enabled, and vector 0774. So this is the first time the
controller's interrupt path carries a driver's traffic rather than a poll.

What has been ruled out. The step-2 value is what simh's own RQ returns for the
same S1 word - `S2 | (s1dat >> 8)`, 010244 for 0xA4FF - and that controller
boots this same image to a login prompt. The adapter is configured and open:
`uba_cr` reads 7C, so IFS and BRIE are both set and `uba_get_ubvector` will
hand a device vector over. The vector fits: `UBA_VEC_MASK` is 0x1FC and the
requested vector is 0x1FC exactly. `uba_uiip` is zero, and BR5 is inside the
range `uba_eval_int` scans, so the nexus request is raised.

What changed while looking. SA was being published with the interrupt grant
rather than when the step was reached, which left it reading the previous step
for as long as the arbitration took; a controller loads SA and then requests
the interrupt. Fixing that ended the delayed grants and took the transfers the
map registers refused during the handover from three to none, but did not
change the loop.

### Reading the same boot against the controller that works

The core carries an MSCP controller of its own, and this VMS image boots from it
to the date prompt. That is the same host, the same image and the same
processor driving a different controller, so it says what ours should look like -
and `core_debug` turns on simh's own tracing for it to be read off.

It gave up the first fault at once. VMS's own initialisation, the one after the
bootstrap's, writes the same step-1 word to both:

	RQ rq_rd (SA) = 0x0940            the controller says what it can do
	RQ rq_wr (SA, 0xA4FF)             rings of 16, interrupts, vector 0774
	RQ rq_rd (SA) = 0x10A4            step 2
	RQ rq_wr (SA, 0xD0A9)             and the host carries on

Ours answered 0x0800 to that first read: a controller supporting nothing beyond
a host-settable vector. Extended diagnostics and host buffer mapping are what a
Unibus MSCP controller declares, and both hosts look for them - CZUDH's step-1
test wanted them too, and now gets past it and fails at step 4 instead.

### Where it stands: the interrupt is taken and nothing follows it

The loop survives that fix. What is now known about it, by measurement rather
than inference:

- The processor takes the interrupt and reads the vector. `intr_pending`, the
  level whose vector the adapter still holds, reads zero at every sample: the
  vector is consumed as fast as it is offered, which only the processor's
  acknowledge does.
- The vector is the one VMS asked for. It requests 0774 in the step-1 word, the
  adapter is open (`uba_cr` 7C, so IFS and BRIE are both set), `UBA_VEC_MASK` is
  0x1FC and the vector is 0x1FC exactly, and simh's own controller returns the
  same value from its acknowledge routine.
- Nothing follows it. The processor's register accesses are traced now, and
  between the interrupt and the reset nine seconds later there is not one - no
  read of SA, which is what the working controller's host does immediately.
  Then VMS polls SA, finds step 2, and starts the handshake again.

So VMS takes an interrupt it asked for, at the vector it asked for, and does not
run the driver that asked for it. What the processor executes when it takes it
is the next thing to see, and neither of the tools built here reaches it: the
console examines physical memory and the address is in system space, and the
adapter declares debug flags but has no trace points to switch on.

### How the two controllers differ

The question worth asking is what this controller does that the one inside the
core does not, since the same VMS boots from that one. The protocol is no longer
any of it. Every value the handshake exchanges is identical:

| the host writes | the core's controller answers | this one answers |
|---|---|---|
| (reads step 1) | 0940 | 0940 |
| A4FF | 10A4 | 10A4 |
| D0A9 | 20FF | 20FF |
| 0003 | 4063 | 4063 |

and the bootstrap, which drives the same four steps with a different step-1 word,
gets 1080, 2000 and 4063 out of this controller and completes.

What differs is everything around the protocol:

- **Where an interrupt is raised.** The core's controller raises one inside the
  core, on the thread running instructions, between two of them, at its own bit
  with an acknowledge routine. This one raises it on the bus; the PRU arbitrates
  and grants it, and it is put into the core from the thread that watches the
  bus, at a reserved bit with a vector.
- **What thread the controller runs on.** The core's is a scheduled event in the
  processor's own thread, so a command and the instruction that issued it cannot
  overlap. This one is a pthread of its own, so every exchange is between
  threads - and `uba_eval_int()`, called from ours, clears every nexus request
  the adapter holds and re-sets them from scratch, while the processor's thread
  is clearing the one it has just taken.
- **How long a step takes.** The core's answers within the guest's instruction
  stream, a couple of hundred instructions. This one answers after real-time
  waits - half a millisecond per initialisation step, and a polling linger of
  half a second - which is milliseconds of guest time.
- **What a register read reaches.** SA here is write-only as far as the state
  machine is concerned: the callback takes the DATO flip-flops without looking
  at the cycle, so a read never enters it. The core's read path is a full one.

The second of those was tried as a fix - handing the vector to the processor's
thread and putting it in between batches, where the core's own devices raise
theirs. It did not get the boot further and the controller stopped raising
interrupts at all, so it was backed out rather than kept on the strength of the
argument for it. The race it describes is real and worth closing, but it is not
what is holding the boot up.

### What the instruction history says

`core_history` keeps the last quarter million instructions and writes them out a
pass after an interrupt arrives, which is the only way to see what the processor
does with a vector. Two things came out of it.

The processor is not wedged. It sits in a one instruction loop,

	80008B1F 00000000| BRB 80008B1F

at IPL 0 with everything enabled, and it takes the interval clock normally -

	8000A0C8 04180000| MTPR #800000C1,#18
	8000A0D2 04180004| ADDL2 @#800022D4,80002B40

which is IPL 24 and VMS adding a tick to the system time. So the machine is
healthy and interruptible; it is waiting for the disk and nothing else.

Sorting the window by the level each instruction ran at:

| IPL | instructions |
|---|---|
| 0 | 247950 |
| 8 | 632 |
| 21 | 17 |
| 24 | 1362 |
| 31 | 28 |

The seventeen at 21 are not our interrupt being serviced. They open with VMS at
IPL 31 writing its own priority down - `MTPR 5E(R5),#12`, PR 18 hex being the
processor's IPL - and then calling a driver routine that fills in a control
block. That is VMS synchronising at device level to initialise the driver, not
answering a device.

And the adapter's own state, sampled while this goes on, has nothing held:
`uba_cr` 7C, so the interrupt field switch and the BR interrupt enable are both
on; `uba_dr` zero, so interrupts are not disabled; `nexus_req` zero and
`intr_pending` zero, so neither the adapter nor the core is holding a request -
while the count of interrupts granted on the bus keeps rising.

Which is the shape of an interrupt that is granted, consumed, and never turned
into a dispatch.

### The tables are rebuilt every batch

Following that to the end took reading what the driver does with the vector.
VMS is entered correctly, and this is what it finds:

	802EFA78 04150000| MOVQ R4,-(SP)
	802EFA7B 04150004| BICL3 #7FFFFE03,@#80029834,R4
	                   7FFFFE03 00000000 00000000 -> 00000000
	802EFA94 04150004| JMP @(R5)+
	8021906C 04150004| INCL @#80002C44
	                   0000007B -> 0000007C
	80219076 04150000| REI

The second instruction is the read of BRRVR, masked to the vector and the
adapter's own flag, and it reads **zero**. So the driver jumps to the routine
that counts an interrupt from a device that is not asking - the count going
7B to 7C - and returns. Every interrupt the controller raised had been landing
there, which is why the MSCP driver never saw the controller answer.

The vector was gone by the time it was read. `build_dib_tab()`, at the top of
`sim_instr()`, calls `init_ubus_tab()`, which clears the I/O page dispatch and
every interrupt vector before filling them in again from the devices the core
carries. simh enters `sim_instr()` once for a RUN command, so that is setup. A
processor driven in batches enters it thousands of times a second.

A device on the bus owns no entry to be rebuilt from, so both halves of what it
put there are lost. The dispatch half was already being re-asserted - that is
what `simh_shim_bus_reassert()` was for, and why it existed at all. The vector
half was not, and losing it is silent: the request stays raised, the processor
takes it, and the driver is handed a zero.

So a batch now schedules a restore one instruction in, where the reassert puts
back both.

## Stage 3 is done

	   VAX/VMS Version V4.7 28-Oct-1987 13:00

	PLEASE ENTER DATE AND TIME (DD-MMM-YYYY  HH:MM)  29-JUL-2026 15:00
	%%%%%%%%%%%  OPCOM  29-JUL-2026 15:01:15.49  %%%%%%%%%%%
	Logfile has been initialized by operator _OPA0:
	Logfile is SYS$SYSROOT:[SYSMGR]OPERATOR.LOG;19
	%SET-I-INTSET, login interactive limit = 64
	  SYSTEM       job terminated at 29-JUL-2026 15:01:16.62

Against what the plan asked for:

**VMS boots from the emulated UDA50 with its registers accessed over the real
bus.** Four consecutive boots, through startup to the login prompt. A run moves
1.19 million words through the map registers and takes a thousand interrupts
from the controller, with 2488 register accesses on the bus.

**File-level read and write, checked by reading the image on the host.** VMS
opens a new version of the operator log on each boot - `;16` through `;19` - and
the image on the host carries the timestamps typed at the console during those
boots, which a 1987 distribution image cannot have had.

**A map register that is not allocated fails the transfer the way a real UBA
fails it, rather than corrupting memory.** Seen in the handover from the
bootstrap to VMS's own driver: three transfers refused, at 756734 twice and at
the odd address 741677, each reported and none written anywhere. The machine
carried on and booted.

**Byte offset.** The path is simh's own, unmodified, and it now has a count of
its own, `dma_byte_offset`. It reads zero: VMS's MSCP driver hands the
controller longword aligned buffers, so nothing in this workload asks the map
registers to shift a transfer by a byte. The path is in place and instrumented;
it is not exercised by booting, and a diagnostic that asks for an unaligned
buffer is what would exercise it.

A caution for anyone reading the counters: `dma_words` and the rest accumulate
from when the device was installed, not from the last START, so progress is the
difference across one run and not the value after it.

Two operational traps cost time here. A parameter that changes the machine -
`bus_iopage`, `bootimage` - is refused while the processor is enabled, and the
refusal is a line in the log rather than anything the caller sees. And disabling
the processor while a batch is running cancels its worker, which leaves the
device wedged; restart the service rather than trying to recover it.

## What stage 2 still needs

The last hop. An interrupt now reaches the UNIBUS adapter's request registers,
and the adapter passes it to the processor only when its own control register
says to - the interrupt field switch and the BR interrupt enable, which an
operating system sets when it configures the adapter for devices it has drivers
for. VMS sets neither for a DL11 and a line clock it was never told about, so
the requests sit in the adapter unclaimed, which is why the count stops at one
per level rather than running at the clock's rate.

That is not a gap in the path but the end of what can be shown without a driver,
and it is what stage 5 exists for.

A backplane-less board shortens what follows, because `internal_bus` makes the
PRU answer its own cycles: the same path carries the DMA of stages 3 and 4 with
no external timing deadline to meet.

## Stage 4 — the transfers happen on the wire

	Logfile is SYS$SYSROOT:[SYSMGR]OPERATOR.LOG;24
	  SYSTEM       job terminated at 30-JUL-2026 18:00:33.02

	ints 1026   transfers answered here 0

VMS boots from the emulated UDA50 exactly as it did in stage 3, and the count of
transfers this program answered is zero. Every one of them went out as bus
cycles and was answered by the PRU.

### Where the memory went

A device's transfer can only happen on the wire if the memory it reaches is
somewhere the bus hardware can see, which the heap the core allocates from is
not. The board already shares a range with the PRU for emulating memory, so the
processor's memory is put there - the same range and the same memory, because a
device's transfer and the processor's own fetches are two ways of reaching one
array.

The core allocates it and the shim moves it, and that has to be reversible: the
core frees the array whenever it resizes memory, so it must be holding an
allocation of its own by then. Hence the pair, `relocate` and `restore`, with
the sizing step calling `restore` first. The shared range is four megabytes, so
the machine is that size rather than eight; VMS boots in it.

### What the PRU does with an address now

A device drives eighteen bits and knows nothing about what is behind them. The
slave path takes the map register for the page - one read of DDR, so it is taken
once and kept - and applies it, and a page whose register is not allocated
answers nothing, which is what a real adapter does. The map sits in the shared
range beside the memory it translates onto, with a word next to it saying
whether to apply it at all. Both are cleared when the range is mapped, because
it is reserved physical memory that keeps whatever the last program left in it.

The map the PRU reads is a copy, so it has to be current whenever a device could
act on a command. A device is set going by a touch of its registers - and not
always a write: **a UDA50 is asked to look at its command ring by a *read* of
IP**. Publishing only on writes left the copy holding a map from before the host
had programmed the page its buffer was in, and every data transfer was refused
as nonexistent memory - MSCP status 69, host buffer access error, subcode NXM -
while the comm area, whose pages were mapped earlier, worked. So the map goes
out before either kind of access.

### What is not verified

**The added slave latency is not measured.** The lookup costs two more reads of
DDR per slave memory cycle, the map register and the word saying whether to
apply it, and this project's own note on that range puts a PRU read of ARM DDR
at up to 400 ns. That is far inside the time a master waits before calling a bus
timeout, but it is arithmetic rather than a measurement: this board has no
backplane, so there is no bus to put an analyser on, and the internal-bus PRU
answers its own cycles with no real timing to meet. The second of those two
reads can be removed by keeping the switch in the PRU's own data memory rather
than in DDR, which would halve the addition.

**A real third-party UNIBUS DMA board is not tried**, and cannot be on this
board for the same reason. It is the sharpest test stage 4 has - a real
controller running the same driver as an emulated one - and it needs a UniBone
in a backplane.

## Stage 5 — the software that has never driven these models

### `uda.cpp` under VMS `DUDRIVER`

VMS's own disk driver, against the emulated UDA50 with the transfers going out
on the bus. Every item the plan asks for, and what the driver made of it.

**It is an RA81, and the geometry is the drive's own.**

	Disk DUA0:, device type RA81, is online, mounted, file-oriented device,
	    shareable, available to cluster, error logging is enabled.
	    Total blocks   891072    Sectors per track    51
	    Total cylinders  1248    Tracks per cylinder  14
	    Volume label "VAXVMSRL4"

**Mount.** The system volume mounts and stays mounted across the boot, and a
second unit takes a file system of its own: `INITIALIZE DUA1: SCRATCH` writes
the index file, the bitmap and the home block, and `MOUNT` accepts the result -
`%MOUNT-I-MOUNTED, SCRATCH mounted on _DUA1:`, cluster size 3, 111376 files
allowed.

**File I/O, verified.** `BACKUP/VERIFY` is the test worth running because it
reads back everything it wrote and compares it: a saveset of `SYS$SYSTEM:*.EXE`
onto the second unit, about seven megabytes, written and compared with no
`%BACKUP-E` of any kind. Afterwards:

	Error count    0    Operations completed    3383

**Multi-unit.** Two drives on one controller. VMS walks them at boot with GET
UNIT STATUS - opcode 3, modifier 1, "next unit" - and the controller answers

| unit | status | |
|---|---|---|
| 1 | 04 | unit available |
| 2..8 | 03 | unit offline |

which is what the enumeration is for: the one drive that is there is offered,
and the walk stops at the first that is not. Both units come up as RA81s and
both carry file systems.

**A unit that is not there.** `MOUNT DUA7: SCRATCH` answers
`%MOUNT-F-NOSUCHDEV, no such device available` and the controller's error count
does not move. The failure is reported and the system carries on, which is the
behaviour asked for.

**Error count zero throughout**, on both units, across a boot, an INITIALIZE, a
verified seven-megabyte backup and several thousand operations.

One thing that looked like a fault and was not: adding the second drive appeared
to stop the machine booting. It was the boot procedure - VMS asks for the date
and time and re-asks every thirty seconds, so attaching to the console after a
fixed sleep finds a screenful of prompts and a system that never started.
`tools/vax-boot.sh` attaches first and answers the prompt when it comes, and the
two-drive machine then boots exactly as the one-drive machine does.

### `deuna.cpp` under VMS `XEDRIVER` — one fix, one open fault

**The controller had no station address of its own.** The DELQA derives one -
DEC's OUI with the low three bytes of the board's own Ethernet address, kept in
a file so it is steady across boots and distinct per board - and puts it in its
`mac` parameter, which is what the widget shows. The DEUNA had a fixed constant
`08:00:2b:cc:dd:ee`, the same on every board, copied only into its own state and
never into the parameter, so nothing displayed it and no saved configuration
recorded it. Two controllers built to the same purpose should not differ in
this, so the derivation is now one function in `ether_bridge`, named for the
controller that asks, and the DEUNA gets `08:00:2b:0b:32:2f` on this board and
keeps it in `deuna.mac`.

**Enabling it makes VMS bug-check.** With the DEUNA on the bus, VMS dies during
startup:

	**** FATAL BUG CHECK, VERSION = V4.7 SSRVEXCEPT, Unexpected system service exception
	    IMAGE NAME = SYSINIT.EXE

not every time - sometimes it crashes, reboots and comes up on the second
attempt - and the image named has also been `LOGINOUT.EXE`, which is the shape
of something writing where it should not rather than a clean fault.

What it is not: it happens with the transfers going out on the wire and equally
with them answered by this program, so it is not stage 4's doing. And VMS never
touches the controller - not one bus cycle to 774510 in a whole boot, and the
controller's own trace stays empty. This VMS does not scan the I/O page; it
works from its saved configuration, and that configuration does not mention a
DEUNA. So the driver is not driving it, and the fault is in what the model does
of its own accord while enabled.

That also says what the next step is: `XEDRIVER` will not touch the device until
the system is told it exists, with `SYSGEN CONNECT`, so the driver has not in
fact seen this model yet. The crash has to be cleared first, because it happens
without the driver's help.
