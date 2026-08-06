# UniBone bring-up issues

The appliance image and the Debian package were developed on QBUS hardware.
This log records what the first UniBone board turned up, and how each was
fixed. The board is `unibone.huebner.org`, a BeagleBone Black carrying a
UniBone cape in a PDP-11 Unibus machine.

Entries are grouped by what is left to do, and numbered in the order they were
found. The numbers are cited from commit messages and from each other, so they
stay put when an entry moves between the sections.

## Open

### 12. A halted emulated CPU spun a core and took the board with it

Twice the board went unreachable: answering ping, with sshd unable to finish a
handshake. The first time it looked like a newly installed binary refusing to
start — it was the previous instance refusing to die, systemd still delivering
SIGKILL to its threads six minutes after the stop, `TimeoutStopSec` being no
help when the process starving the machine outranks the one trying to kill it.

The cause is in `cpu_c::worker()`, which has no wait anywhere:

    while (!workers_terminate) {
        ...
        ka11_condstep(&ka11);   // steps nothing while halted
        ...
    }

A running CPU is meant to take the core it is given — every pass executes an
instruction. A halted one steps nothing and spins at full speed polling the
switches, and on this single-core board that starves userspace. Both incidents
had the CPU enabled and halted: once during a stop, once after the double bus
error of a boot attempt.

The worker now waits a millisecond per pass while halted, which no operator
pressing START can notice. With the fix the CPU sits enabled and halted at a
load of 1.1 with ssh answering instantly, and XXDP boots with the board staying
responsive throughout.

Fixed in `10.02_devices/2_src/cpu.cpp`, for a halted CPU: it now waits a
millisecond per pass, and the board sits at a load of 1.1 with ssh answering
instantly while the CPU is enabled and stopped.

What remains open is the teardown. This entry used to claim that stopping the
service with the CPU *running* wedges the board regardless; that claim predates
issue 18, whose leaked mutex explains the stops that hung far better, and a stop
with XXDP running completed in two seconds during the diagnostic run of issue
16. It has not been re-tested since, so the honest state is: unknown, and
probably fixed by 18.

Two things behind it are unresolved either way. `workers_stop()` joins a worker
that may be inside a bus cycle with nothing bounding the wait. And
`qunibusadapter` runs SCHED_FIFO at priority 60 on a single core, so any thread
that fails to yield takes the whole board out of reach rather than merely
slowing it — which is what turned each of these into a power cycle instead of a
misbehaving service.

### Open questions

**Can an emulated CPU on a physical bus reach the board's own emulated
devices?** Issue 11 says no — the CPU took a bus timeout at the RL11 CSR — but
that was measured on a bench board with no backplane and no terminators, so the
cycle may have failed electrically rather than logically. It decides what a
UniBone standing in for a dead CPU can use: the real cards only, or those plus
its own emulated disks. One reading of 774400 from a board in a terminated
backplane, running the physical firmware with the emulated RL11 enabled,
settles it.

**Does the SLU poll floor actually stop the spin?** The fix in issue 16 is
supported rather than demonstrated: the kernel named that thread repeatedly, it
holds the only unbounded-poll path, and its divisor is zero both before an open
and after a close. But the spin was never reproduced with the floor in place,
because enabling a DL11 whose port cannot open is refused before the worker
starts. The remaining way in is a port closed under a running worker.

### Design note: one CPU device, not one per model

From Jörg, on the CPU20 device: the shape should be a single generic bus CPU
whose model is a parameter, `type = KA11` for the 11/20, `KD11-E` for the
11/34 Frits has, rather than a device class per machine. The module codes:

| model | CPU | model | CPU |
|---|---|---|---|
| 11/20 | KA11 | 11/45 | KB11-A |
| 11/05 | KD11-B | 11/34 | KD11-E |
| 11/04 | KD11-D | 11/60 | KD11-K |
| 11/40 | KD11-A | 11/70 | KB11-B |
| 11/44 | KD11-Z | | |

QBUS: LSI-11 is KD11, F-11 is KDF11, J-11 is KDJ11. VAX: the 11/730, 750 and
780 by name; for the MicroVAXen, CVAX covers the family, with KD32 (MicroVAX I),
KA630 (II), KA640, KA650, KA655, KA660, KA670 and KA680.

## Closed

### 1. The service passed a QBUS address width

    unibone[730]: QUniLator web service starting, UNIBUS emulation.
    unibone[730]: FATAL QUNIBUS: Address width of 18 bits invalid!
    unibone.service: Main process exited, code=exited, status=1/FAILURE

`packaging/debian/qbone.service` is the template for both packages, and its
`ExecStart` carried `--addresswidth 22`. The Unibus is 18 bit, so the UNIBUS
build's `set_addr_width()` — whose 16 and 22 cases are compiled only for QBUS —
rejected it and the emulator exited before opening its web port.

The width is a property of the bus the binary was built for, so the binary
supplies it: 22 bit on QBUS, 18 on UNIBUS, with `--addresswidth` remaining for
a QBUS board fronting a 16 or 18 bit CPU. The unit passes no width at all.

Fixed in `main_qbone_web.cpp` (per-bus default and validation) and
`packaging/debian/qbone.service`.

### 2. The rejected width was reported as the current one

The FATAL above named 18 bits, which is the *valid* width — the argument was
22. `set_addr_width()` printed its member `addr_width`, already set to 18 by
the UNIBUS constructor, instead of the `_addr_width` it was refusing.

Fixed in `10.01_base/2_src/arm/qunibus.cpp`.

### 3. The UniBone package described itself as a Qbus emulator

`build-deb.sh` rebrands `qbone`/`QBone` to `unibone`/`UniBone` when it packages
the UNIBUS build, but the bus name was not among the tokens it rewrote, so the
unit's `Description=` and the package description both read "PDP-11 Qbus device
emulator" on a Unibus board.

`rebrand` now maps `Qbus` to the board's bus as well.

Fixed in `packaging/build-deb.sh`.

### 4. The priority-slot warning named the wrong slot

`set_priority_slot()` reported its member `priority_slot` — the slot the
request held before the call, 255 while still uninitialized — instead of the
`_priority_slot` it was refusing, so the startup log read "Slot 255 requested
by device dzv11" whatever slot had actually collided.

Fixed in `10.01_base/2_src/arm/priorityrequest.cpp`.

### 5. Priority slots collide across the larger UNIBUS device set

With the slot named correctly, a UNIBUS startup logs 12 collisions. The
emulator holds one backplane, and the compile-time slot defaults were chosen
against the QBUS device set; the UNIBUS build adds RK11, RF11, RL11 and KE11
on top of the same numbers:

| slot | holder | collides with |
|---|---|---|
| 2 | DL11 transmit | DL11b receive |
| 3 | DL11b transmit | LTC |
| 10 | RK11, KE11 | dzv11d |
| 12 | RF11 | dhv11 |
| 15 | RL11 | dhv11b |

The DL11/DL11b/LTC overlaps at 2 and 3 are not UNIBUS-specific: `DL11b` takes
`DL11`'s slot + 1, which is DL11's own transmit slot, and `LTC` sits at
`SLU_SLOT + 2`. Both buses carry them.

The device set occupies 1..24 and 30..31 on UNIBUS, leaving 19, 23 and 25..29
free — seven slots, where the two mux pools alone want twelve, so renumbering
the pools does not fit.

Slots matter for a device that is on the bus. Every device is constructed at
startup whether or not it is enabled, and the warning fired there, so a board
that enables neither dzv11d nor RK11 was told about their shared slot at every
boot. The slot is now checked against the devices actually on the bus, when a
device is installed or its slot changes, and a UNIBUS startup logs nothing.

Placement is settled where an operator makes it: a configuration is checked
before it is written, and one that puts two devices on the bus in one slot is
refused with 422 and the pair named,

    dhv11b and rl both use backplane slot 15

as is one that runs a device past the last slot. A device's footprint comes
from its own requests — a mux whose receive and transmit interrupts arbitrate
separately holds two adjacent slots — so the check needs no table of its own.
It is bus-independent and runs on QBUS as well.

Fixed in `10.01_base/2_src/arm/priorityrequest.cpp`,
`10.01_base/2_src/arm/qunibusdevice.cpp` and `10.05_web/2_src/webconfigs.cpp`.

### 6. The UNIBUS controllers had no label

The label table is keyed by type name and carried the Q-bus variants alone, so
the device list showed the raw handle — `rl`, `rk`, `rx`, `ry`, `deuna`,
`M9312`, `ke` — where a QBone shows a role and a DEC code. RL11, RLV11, RK11,
RX11, RY211, DEUNA, KE11 and the M9312 are now in the table.

Fixed in `10.05_web/2_src/device_label.cpp`.

### 7. The emulated PDP-11/20 was unreachable from the web service

The KA11 emulation (`cpu_c` over `10.02_devices/2_src/cpu20`) is built into the
UNIBUS binary, but `main_qbone_web.cpp` called `devices_startup(false)`, so the
service never constructed it. That argument also decides who arbitrates the
bus, which is why it cannot be a device an operator switches on later: a board
is either a peripheral of a physical PDP-11, which arbitrates, or the machine
itself with the KA11 doing it.

So it is a machine setting, `emulated_cpu` in settings.json, read before the
device set is built and offered through `/api/settings` with a warning that it
takes effect at the next start; `--emulated-cpu` selects it for one run. A
board running the KA11 has no machine around it, so it supplies the memory as
well: everything below the I/O page that no physical card answers.

The CPU then appears as the device CPU20, labelled "Emulated CPU (KA11)", with
its console on the dashboard — RUN lamp, program counter, opcode count, switch
register, and the HALT, START and CONTINUE switches. On the board it starts,
fetches from emulated memory and halts on the zero word:

    Emulating memory 000000..757776.
    CPU HALT by opcode at 001000

Added in `main_qbone_web.cpp`, `10.05_web/2_src/websettings.cpp`,
`device_label.cpp` and the frontend's `widgets.ts`.

**Superseded.** The `emulated_cpu` setting is gone. It was a declaration about
the backplane, stored on the board and never re-asked, and it travelled with the
card exactly as a configuration does — so it was as stale as anything else after
a move, and failed silently. The processors are now always in the device set,
disabled, and enabling one is what tells the board it is the machine; the
arbitration and the memory claim follow from that. See `docs/plans/dark-boot-plan.md`.

### 8. The console switches were configuration

HALT, START and CONTINUE were `PARAM_CONFIG`, so a machine saved with HALT
asserted carried `halt_switch` into the configuration, and applying it left the
CPU unable to run with nothing on screen to say why:

    {"devices":[{"enabled":true,"name":"CPU20","params":{"halt_switch":"1"}}]}

START and CONTINUE are momentary — the worker acts on them and clears them in
the same pass — so what they hold is never a setting either. All three are
status now, which keeps them writable and out of every snapshot. The switch
register stays configuration, being operator data the running program reads.

Fixed in `10.02_devices/2_src/cpu.cpp` and the frontend's `widgets.ts`.

### 9. The M9312 PROM listings were not packaged

The emulated CPU has nothing to boot from without them. The listings sit in the
source at `10.02_devices/4_deploy` and the package now installs them to
`/usr/share/qunilator/roms`: the console/diagnostic PROM and the per-controller
boot PROMs, `23-751A9.lst` being the RL11 one.

Fixed in `packaging/build-deb.sh`.

### 10. The emulated CPU needs PMI to reach memory and the ROM

A board that runs the KA11 emulates the memory as well, and serves it from the
same PRU that drives the CPU's bus cycles — a board cannot answer its own
cycle. Every fetch therefore ended in a bus timeout, and the CPU died in a
double bus error at whatever address it started from, which reads exactly like
a HALT opcode in a zeroed machine.

`pmi` (Private Memory Interconnect) is the switch for it: the CPU then reaches
memory and emulated ROM cells in DDR directly, and only device registers go
over the bus. With it on, the KA11 executes the M9312 DL boot PROM.

So a self-contained machine needs `pmi` on. It ships off, which is right for a
CPU driving a real backplane, and there is nothing yet that says so when the
emulated CPU is the one running.

### 11. The emulated CPU cannot reach the board's own emulated devices

With PMI on and the M9312 holding the RL11 boot PROM, the CPU runs the ROM and
stops at the first device register. Its own cycle trace:

    773004 DATI 000261        sec
    773006 DATI 012700        mov #0,r0
    773012 DATI 012701        mov #rlcsr,r1
    773014 DATI 174400
    773034 DATI 010311        mov r3,(r1)
    774400 DATI 000000 nxm=1  the RL11 CSR: bus timeout
    777766 DATO 000004 nxm=1  the CPU error register, also nxm

The ROM is correct and the CPU is correct. What fails is the RL11 register
access: emulated device registers live in PRU RAM and are answered by the PRU's
slave logic during a physical bus cycle, and the same PRU is driving that cycle
as master for the CPU.

Memory and ROM avoid this because PMI reaches them in DDR without a bus cycle;
device registers have no such path. So the emulated CPU drives *physical*
peripherals on a real UNIBUS, and a wholly self-contained machine — emulated
CPU with emulated disks — stops here.

The same limit shows in `GET /api/memory`, which reads by DMA and so needs an
arbitrator: on a board whose only CPU is the emulated one, it answers "bus
timeout reading memory" whenever that CPU is halted, including for addresses
inside the emulated range.

The answer is a build option, `NO_PHYSICAL_BUS`, which Jörg Hoppe added upstream
in `a653b92`. It changes the PRU: the bus latches are kept internal and read
back what was written, instead of being driven onto a backplane. The board then
answers its own cycles, and a UniBone outside a machine is a whole PDP-11.

Our copies of `pru1_buslatches.h` were byte-identical to that commit's parent,
so both bus variants take the upstream file as it stands. The define reaches
the PRU compiler and the ARM code through `CC_CODE_FLAGS`, which both makefiles
now carry, and `crossbuild.sh -n` selects it. The makefiles' header
dependencies do not cover a changed define — Jörg's warning is to "compile all"
— so the chosen mode is recorded next to the firmware and a build that switches
modes rebuilds the firmware and every object, in either direction.

With that build the board reaches its own devices. The RL11 CSR reads over DMA,

    GET /api/memory?address=774400  →  {"words":[49280]}   = 0140200

and the emulated PDP-11/20 boots XXDP from an emulated RL02 through an emulated
RL11, with the M9312's DL boot PROM and the DL11 as console:

    CPU NOT SUPPORTED BY XXDP-XM
     BOOTING UP XXDP-SM SMALL MONITOR
     XXDP-SM SMALL MONITOR - XXDP V2.4  REVISION: D0
     BOOTED FROM DL0
     28KW OF MEMORY
     UNIBUS SYSTEM

The monitor is interactive, and a directory listing of the pack scrolls past.
XXDP-XM declining the CPU is right: the KA11 has no memory management, so the
extended-memory monitor steps aside for the small one.

The machine is saved as the configuration `pdp1120`: RL11 with the XXDP pack,
DL11, M9312 with the console and DL PROMs, and CPU20 with `pmi` on.

`GET /api/memory` works in this build too — its DMA no longer needs a physical
arbitrator — so a program can be loaded into memory and started from the
console without any of it existing in hardware.

### 13. The package does not record which bus it was built for

`build-deb.sh` packages whatever binary is in the deploy directory, and nothing
in the package says whether that binary drives a physical bus or keeps it
internal. Installing a package built from a physical-bus tree onto the
standalone board silently undid the internal bus, and the machine failed its
next boot with the double bus error of issue 11 — a full circle back to a
symptom that was supposedly closed.

The two are different products for different boards, and the package should say
which one it is.

Closed by issue 20: the bus mode became a setting, both firmwares ship in one
binary, and there is no longer a wrong package to install.

### 14. The console card showed a terminal with nothing behind it

The dashboard's console card carried three terminals — the two emulated SLUs
and a Web Serial port — and picked one at mount:

    (store.activeTerm === 'slu0' && !en0) || ... ? en0 ? 'slu0' : ... : 'serial'

`devEnabled('DL11')` reads the device model, which on a fresh load has not
arrived yet, so DL11 looked disabled and the card fell back to the Web Serial
terminal. With no port granted that terminal is blank, while the DL11 socket
connected and wrote its 40 KB of replayed history into a hidden one. The
handshake was 101 and one 40 KB message arrived — into a terminal nobody could
see. The tab switcher that would have revealed this, `liveTab()`, is exported
and called from nowhere, so the other two terminals were unreachable anyway.

A machine has one console, and which port carries it is a machine setting: the
ttyS2 bridge, a Web Serial port, or — with neither — the emulated DL11 at
777560. The card now holds one terminal that follows that setting, and every
other serial line is reached over its own TCP port instead. The card names the
live source and, when the link is down, greys the screen and says
*disconnected*, so a terminal with nothing behind it can never again look like
an idle machine.

Multi-client consoles are unchanged, and the answerer protocol now covers the
emulated DL11 as well: it had applied only to the external console, so several
browsers watching `/ws/console/0` each answered the guest's identification
queries through xterm's built-in reply. Now one answers, whichever channel
carries the console.

Fixed in `10.05_web/3_frontend/src/lib/terminals.ts`, `components/Dashboard.ts`,
`store.ts`, `types.ts` and `styles.css`.

### 15. The control panel and the CPU's switches were two panels on one machine

`POST /api/control` was written for a physical CPU on the bus, and its run
controls are bus signals:

    #if defined(QBUS)
        if (dec.do_halt)   { qunibus->set_halt(1); ... }
        if (dec.do_resume) { qunibus->set_halt(0); ... }
    #endif

On UNIBUS there is no HALT line to pull, so `halt` and `continue` were compiled
out entirely: the buttons posted, answered 200, and did nothing. `restart`
survived as a bus power cycle, which the emulated CPU takes as its ACLO-inactive
path — `ka11_reset` then a power-up vector fetch from location 24. On a machine
where nothing set that vector, a running XXDP restarted into nonsense: one
instruction executed, `PC 000012`, stopped. Meanwhile the CPU widget drove the
KA11's console switches, which worked. Two panels, one machine, and the older
one silently ineffective.

An emulated CPU's switches are the machine's front panel, so the run controls
are those switches now: `halt` asserts the HALT switch, `continue` releases it
and pulses CONTINUE, and `restart` is the sequence a KA11 front panel actually
has — stop, LOAD ADDR from the address the M9312 resolved for its boot PROM,
START. `init` and `powercycle` stay bus operations whichever CPU is present,
and `dc_on`/`dc_off` keep their power bookkeeping.

On the board, against a running XXDP:

    halt      run=False halt=True  PC=150750
    continue  run=True  halt=False PC=150664
    restart   run=True  halt=False → the XXDP banner again

Fixed in `10.05_web/2_src/webapi.cpp`.

### 16. The SLU worker polls on a zero interval and spins at realtime priority

Four times the board went unreachable: answering ping, sshd unable to finish a
handshake. Sampling every thread across a service stop, at a realtime priority
above the emulator's own, showed the stop completing in two seconds — the hang
is not deterministic, and the rules built around it ("halt the CPU first",
"deploy only into an idle machine") were superstition on three samples.

The kernel had recorded what actually happened, in the boot that wedged:

    watchdog: BUG: soft lockup - CPU#0 stuck for  45s! [DL11.0:16379]
    watchdog: BUG: soft lockup - CPU#0 stuck for 105s! [DL11.0:16379]
    watchdog: BUG: soft lockup - CPU#0 stuck for 280s! [DL11.0:16379]
    rcu: INFO: rcu_preempt detected stalls on CPUs/tasks
    systemd-journald.service: Failed with result 'watchdog'

`DL11.0` is the SLU receive worker, SCHED_FIFO at priority 60 on a board with
one core. Its poll interval comes from the character time:

    unsigned poll_periods_us = (rs232.CharTransmissionTime_us * 9) / 10;
    timeout.wait_us(poll_periods_us);

`CharTransmissionTime_us` is zero until a port opens, and `CloseComport()` sets
it back to zero. A worker still looping across a closed or unopened port
therefore waits zero nanoseconds per pass: a tight loop at realtime priority,
which starves everything below it. Ping keeps answering because the network
stack runs in softirq; nothing in userspace gets a slice, which is why the
board looks alive and reachable-but-not.

The interval is floored at 10 ms when the character time is zero, so a line
with no port behind it idles instead of spinning.

This is a supported fix rather than a demonstrated one. Enabling a DL11 whose
port cannot open is refused before the device installs, so that route no longer
reaches the worker at all — the remaining way in is a port closed under a
running worker. What is established: the kernel named this thread repeatedly,
it is the only unbounded-poll path in it, and the value it divides by is zero
both before an open and after a close.

Fixed in `10.02_devices/2_src/dl11w.cpp`.

Two things the run turned up that are not fixed. `PANEL.0` sits in
uninterruptible sleep inside `omap_i2c_xfer_msg` during shutdown, where no
signal can reach it until the transfer completes — a second way for a stop to
hang, independent of this one. And the structure behind all of it stands: a
single core carrying several SCHED_FIFO-60 device threads has no headroom, so
any thread that fails to yield takes the machine out of reach rather than
merely slowing it down.

### 17. The panel's RUN lamp showed operator intent, not the machine

The control panel's RUN lamp is `powered && !halted`, where `halted` moves only
when an operator presses HALT or CONTINUE — it reflects the HALT line, which is
all QBone can see of a physical CPU. An emulated CPU halts on its own too: a
HALT opcode, a breakpoint, a double bus error, the power-up vector fetch of a
`powercycle`. The panel showed RUN lit beside a CPU widget reading HALTED and
`PC 000000`, and the widget was right.

The 10 Hz state poll now takes the run state from the emulated CPU when there
is one, so the lamp follows the machine. A physical CPU still shows its HALT
line.

Fixed in `10.05_web/2_src/webevents.cpp`.

### 18. The SLU transmit worker waited on a mutex it did not hold

A configuration apply while the machine was running left the web interface
dead — every request timing out — with the board itself healthy and the service
still "active". The log named it:

    [ERR slu] DL11.worker(1) did not return within 1000 ms: cancelling it,
              which may leave one of its mutexes locked.

`worker_xmt()` enters by locking `on_after_register_access_mutex`, waits on
`pthread_cond_wait(&on_after_xmt_register_access_cond,
&on_after_xmt_register_access_mutex)`, and leaves by unlocking the *xmt* mutex.
The entry lock was the wrong one, so the wait held a mutex the thread never
owned. `workers_stop()` signals the condition variable up to a hundred times
before giving up — the mechanism is there and the SLU implements `worker_wake()`
— but a wait on an unowned mutex cannot hand over reliably, so the worker never
saw `workers_terminate` and was cancelled inside the wait. The comment beside
that cancellation says exactly what follows: the thread re-acquires the mutex
while unwinding and dies owning it, and every later lock blocks for the life of
the process. The next apply took it and the civetweb threads piled up behind
`operations_mutex`.

Nothing goes wrong while the machine runs, which is why this survived: the
mutex wrongly held belongs to the register-access path of a different class,
so it is never contended. Only the shutdown handshake depends on ownership.

The entry now locks the mutex the wait releases and the exit unlocks. A cold
apply and RESTART of the saved machine boots XXDP with no worker cancelled.

Fixed in `10.02_devices/2_src/dl11w.cpp`.

### 19. The boot address was not part of the saved machine

`bootaddress_label` is the MACRO-11 label the M9312 resolves to a power-on PC,
and it is read-only while the device is on the bus — so it is set with the
M9312 unplugged, and a configuration that omits it leaves RESTART with no
address to load. The `pdp1120` configuration omitted it, and the restarts that
appeared to work did so only because the PC still held 173004 from a hand-set
boot.

With `bootaddress_label = dl0n` saved, an apply resolves it:

    m9312: Code label "dl0n" resolved, auto boot PC = 773004

and RESTART alone boots the machine.

### 20. The bus mode is a setting, not a build

`NO_PHYSICAL_BUS` was a compile-time switch, so the internal bus meant a
separate binary — and a package that records nothing about which one it holds
(issue 13), which is how installing the wrong one silently reverted this board
and broke its next boot.

Nothing in the ARM code reads that define: it reaches `pru1_buslatches.h` and
nothing else, so the only artefact that differs is the PRU1 firmware. The
emulator already carries several PRU images and picks one at startup —
`PRUCODE_TEST` and `PRUCODE_EMULATION` — so the internal bus becomes a third,
`PRUCODE_EMULATION_INTERNAL_BUS`, built from the same sources into their own
object directory and carried alongside the others. One binary serves both
boards, and `crossbuild.sh -n` is gone.

The setting is `internal_bus`, independent of `emulated_cpu`, because the two
are orthogonal:

| emulated CPU | bus | the machine |
|---|---|---|
| no | physical | a peripheral of a real PDP-11 |
| yes | physical | the CPU of a real backplane full of real cards |
| yes | internal | a machine entirely by itself |
| no | internal | a device set nothing drives; for poking device models |

Binding them would have forbidden the second row — a UniBone standing in for a
dead CPU among real peripherals, which is much of the point of the board.

Both take effect at the next start of the service, since the firmware is chosen
when the PRU is loaded. On the board:

    web] Internal bus.
    PRU] Loaded and started PRU code with id = 3
    web] Emulated CPU: KA11 (PDP-11/20).

and XXDP boots from the emulated RL02 as before.

**Open**: whether an emulated CPU on a *physical* bus can reach the board's own
emulated devices. Issue 11 says no, but that was measured on a bench board with
no backplane and no terminators, so the cycle may have failed electrically
rather than logically. It decides what the second row above promises — real
cards only, or real cards plus the board's emulated ones. The test is one
reading of 774400 from a UniBone in a terminated backplane with the physical
firmware and the emulated RL11 enabled.

### 21. Switching CPU models under a running machine crashed the service

With the 11/34 enabled and running XXDP, applying the `pdp1120` configuration
killed the service with SIGSEGV. The apply disables the CPU model that is not in
the snapshot, and `cpu_base_c::on_after_uninstall()` cleared the `unibone_cpu`
pointer there.

That pointer is how a C emulation core reaches the ARM side: every
`unibone_dati()`, `unibone_dato()` and `unibone_datob()` dereferences it for the
trigger probe and the DMA request. The worker thread is stopped *after* the
callback returns — `qunibusdevice_c::on_param_changed()` forwards to
`device_c::on_param_changed()`, which calls `workers_stop()` — so for the length
of that window a running CPU went on executing instructions, and its next bus
cycle read a null pointer. A halted CPU survived it; one executing XXDP did not.

The pointer now stays on the CPU that was disabled, and is taken over by the
next model in `on_before_install()`, which refuses only a CPU that is still
enabled. A disabled CPU with a stopped worker is a harmless target, and there is
no window in which the pointer is null under a running core.

Applying `pdp1120` against a running 11/34 and `pdp1134` back now swaps the
model both ways with the machine live, and the 11/34 reboots into XXDP-XM.
