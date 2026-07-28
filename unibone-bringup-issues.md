# UniBone bring-up issues

The appliance image and the Debian package were developed on QBUS hardware.
This log records what the first UniBone board turned up, and how each was
fixed. The board is `unibone.huebner.org`, a BeagleBone Black carrying a
UniBone cape in a PDP-11 Unibus machine.

## 1. The service passed a QBUS address width

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

## 2. The rejected width was reported as the current one

The FATAL above named 18 bits, which is the *valid* width — the argument was
22. `set_addr_width()` printed its member `addr_width`, already set to 18 by
the UNIBUS constructor, instead of the `_addr_width` it was refusing.

Fixed in `10.01_base/2_src/arm/qunibus.cpp`.

## 3. The UniBone package described itself as a Qbus emulator

`build-deb.sh` rebrands `qbone`/`QBone` to `unibone`/`UniBone` when it packages
the UNIBUS build, but the bus name was not among the tokens it rewrote, so the
unit's `Description=` and the package description both read "PDP-11 Qbus device
emulator" on a Unibus board.

`rebrand` now maps `Qbus` to the board's bus as well.

Fixed in `packaging/build-deb.sh`.

## 4. The priority-slot warning named the wrong slot

`set_priority_slot()` reported its member `priority_slot` — the slot the
request held before the call, 255 while still uninitialized — instead of the
`_priority_slot` it was refusing, so the startup log read "Slot 255 requested
by device dzv11" whatever slot had actually collided.

Fixed in `10.01_base/2_src/arm/priorityrequest.cpp`.

## 5. Priority slots collide across the larger UNIBUS device set

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

## 6. The UNIBUS controllers had no label

The label table is keyed by type name and carried the Q-bus variants alone, so
the device list showed the raw handle — `rl`, `rk`, `rx`, `ry`, `deuna`,
`M9312`, `ke` — where a QBone shows a role and a DEC code. RL11, RLV11, RK11,
RX11, RY211, DEUNA, KE11 and the M9312 are now in the table.

Fixed in `10.05_web/2_src/device_label.cpp`.

## 7. The emulated PDP-11/20 was unreachable from the web service

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

## 8. The console switches were configuration

HALT, START and CONTINUE were `PARAM_CONFIG`, so a machine saved with HALT
asserted carried `halt_switch` into the configuration, and applying it left the
CPU unable to run with nothing on screen to say why:

    {"devices":[{"enabled":true,"name":"CPU20","params":{"halt_switch":"1"}}]}

START and CONTINUE are momentary — the worker acts on them and clears them in
the same pass — so what they hold is never a setting either. All three are
status now, which keeps them writable and out of every snapshot. The switch
register stays configuration, being operator data the running program reads.

Fixed in `10.02_devices/2_src/cpu.cpp` and the frontend's `widgets.ts`.

## 9. The M9312 PROM listings were not packaged

The emulated CPU has nothing to boot from without them. The listings sit in the
source at `10.02_devices/4_deploy` and the package now installs them to
`/usr/share/qunilator/roms`: the console/diagnostic PROM and the per-controller
boot PROMs, `23-751A9.lst` being the RL11 one.

Fixed in `packaging/build-deb.sh`.

## 10. The emulated CPU needs PMI to reach memory and the ROM

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

## 11. The emulated CPU cannot reach the board's own emulated devices — open

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

Closing it means giving device registers the same direct path PMI gives memory:
the ARM can reach the PRU's iopage cells, but a register access is not a load
or a store — it has to run the device's DATI/DATO event, the one the PRU raises
today, or the read clears no flag and the write starts no seek.
