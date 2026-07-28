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
