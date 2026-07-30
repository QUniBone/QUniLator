# Emulated Q-bus memory — implementation plan

The board answers bus cycles for a range of memory out of its own DDR, so a
machine can be given the memory its backplane does not carry. The operator
configures that range like any other card: it appears in the device list, is
captured by a configuration snapshot, and shows what it covers next to the
memory the machine already has.

## 1. What exists today

The mechanism is complete and reaches from the device tree down into the PRU.
Nothing in it needs inventing; what is missing is a caller and a model.

**The memory itself.** `02_bbb_config/01_cape/QBone.dtso` reserves 8 MB at
`0x9f000000` as `qbone-ddr`, `no-map`, and the PRU backend maps it in
`pru.cpp:198` into `ddrmem->base_virtual` / `base_physical`. `qunibus_memory_t`
covers the whole 22-bit space (`QUNIBUS_MAX_WORDCOUNT` = 2 MWords = 4 MB), so
any address a Q-bus can carry has a cell behind it already.

**The range.** `ddrmem_c::set_range()` (`10.01_base/2_src/arm/ddrmem.cpp:203`)
writes `memory_start_addr` / `memory_limit_addr` into the shared
`pru_iopage_registers`. The PRU tests that range *first*, before the I/O page
decode, in `emulated_addr_read()`, `emulated_addr_write_w()` and
`emulated_addr_write_b()` (`10.01_base/2_src/pru1_q/pru1_iopageregisters.c:55`,
`:92`, `:122`), and returns 1 for "memory", which the slave state machine
answers with RPLY without waking the ARM
(`pru1_statemachine_data_slave.c:131`, `:165`, `:170`). The same test sits in
the DMA path (`pru1_statemachine_dma.c:209`, `:291`, `:294`), so an emulated
disk transferring into an emulated range never puts a cycle on the wire. Block
mode is served too: the slave asserts REF and stays in
`state_data_slave_din_block_complete` for further DIN.

**Sizing.** `qunibus_c::test_sizer()` (`qunibus.cpp:515`) DATIs the whole
address space in one chunked DMA and returns the first address that timed out —
the top of the memory the machine physically carries.

**The menu program drives all of it.** `application_c::emulate_memory()`
(`10.03_app_demo/2_src/menus.cpp:121`) sizes physical memory and claims
everything from there up to the I/O page; the devices menu adds `m i` (install),
`m f` (fill), `m d` (dump to disk) and `m ll` / `m lp` / `m lt` (load from a
MACRO-11 listing, an absolute paper-tape image, an address/value text file).

**The service drives almost none of it.** `main_qbone_web.cpp:177` calls
`set_range()` in exactly one place, inside `if (emulated_cpu)`, and an emulated
CPU exists only on UNIBUS. On a QBUS board `memory_start_addr` and
`memory_limit_addr` stay 0 for the life of the process, and the board never
answers a memory cycle.

**One other claimant.** `vcb01_c::claim_video_memory()`
(`10.02_devices/2_src/vcb01.cpp:225`) takes the range for its 256 KB
framebuffer bank, warns if it is taking it from someone else, and releases it
by disabling the range outright on `release_video_memory()`.

**`/api/memory` is something else.** It examines and deposits by DMA (up to
4096 words per call) and works against whatever answers on the bus, physical
card or emulated range. It configures nothing.

## 2. What is missing

1. **A caller in service mode.** No path from the web API reaches
   `set_range()` except by enabling a VCB01.
2. **A model object.** Emulated memory is not a `device_c`, so it has no name,
   no parameters, no place in `GET /api/devices`, no place in a configuration
   snapshot, no widget, and no MCP tool. Everything the rest of the emulator
   gets for free from the device registry has to be built by hand or the memory
   has to become a device.
3. **A second range.** The PRU holds one. The VCB01 already owns it, so a
   memory card and a video board cannot both be installed.
4. **A collision check.** Claiming a range that a physical card also answers
   puts two slaves on one cycle, and the first symptom is corruption elsewhere.
   `test_sizer()` finds the top of physical memory but nothing checks a specific
   range before claiming it. This is the same check TODO.md asks for on I/O page
   registers and `vcb01-plan.md` asks for on the framebuffer bank.
5. **Bulk content operations.** Fill/clear, dump to a file, load a paper tape —
   the menu has them, the service does not. `POST /api/memory` covers loading
   4096 words at a time.
6. **A single-range assumption on the ARM side.** `deuna.cpp:940` and friends
   short-circuit their DMA by comparing against `ddrmem->qunibus_startaddr` /
   `qunibus_endaddr` directly.

## 3. The memory card as a device

New `memory_c : device_c` in `10.02_devices/2_src/memory.{hpp,cpp}`: no
registers, no worker threads, no I/O page presence — a `device_c` rather than a
`qunibusdevice_c`, the way `storagedrive_c` is one.

- `type_name` `MSV11` on QBUS, `MS11` on UNIBUS; instance name `MEM`;
  `category()` returns `"memory"`.
- Parameters: `startaddr` and `endaddr`, both octal
  (`parameter_unsigned_c`, radix 8), defaulting to the whole usable space —
  0 and `iopage_start_addr - 2` — and a `size` status parameter in KB derived
  from them, so the device list reads as a size without the operator doing
  octal arithmetic.
- `on_param_changed(enabled)` claims or releases the range; a change to
  `startaddr` / `endaddr` while enabled re-claims it.
- Claiming validates (even addresses, `start <= end`, `end < iopage_start_addr`,
  both inside the address width), probes for a collision (§5), calls
  `set_range()` and clears the range, as the VCB01 does for its bank — the DDR
  reservation is not zeroed at process start and a machine must not boot on the
  previous run's contents.
- No CSR and no parity: the MSV11 parity/CSR window at 17772100 is not modeled.
  An operating system sizes memory by probing for a timeout, which this answers
  correctly.

On the test rig — an 11/73 with a 2 MB card — the interesting range is
0o10000000 .. 0o17757776: everything above the card and below the I/O page,
2 MB less the 8 KB the I/O page takes, giving the machine 4 MB.

The device carries the range but does not decide it. Nothing sizes the machine
implicitly at startup: a bus sweep on every boot is both slow and wrong when the
CPU is running, so the range is what the configuration says it is, and the
operator sizes the machine explicitly (§5).

## 4. Two ranges in the PRU

A memory card and a VCB01 both want a window, and on a machine that carries a
video board the framebuffer sits above the memory. One range serves one of them.

- `pru_iopage_registers_t` (`10.01_base/2_src/shared/iopageregister.h:162`)
  grows `memory_start_addr[2]` / `memory_limit_addr[2]`.
- The three `emulated_addr_*` functions test both slots, unrolled — no loop.
  This is the RPLY-critical path of every bus cycle the board sees, including
  every cycle it does *not* answer, so the second test costs two compares and
  must not cost a branch table.
- `ddrmem_c::set_range(slot, start, end)`, with `enabled` / `qunibus_startaddr`
  / `qunibus_endaddr` becoming per-slot, plus a
  `ddrmem_c::contains(addr, byte_count)` helper for the ARM-side shortcuts.
- Slot 0 is the memory card, slot 1 the VCB01 bank. `vcb01.cpp` claims slot 1
  and drops its "taking the emulated memory range" warning; `deuna.cpp`,
  `menus.cpp` and `main_qbone_web.cpp` move to slot 0 and to `contains()`.
- `crossbuild.sh` rebuilds the PRU firmware with clpru in its own container, so
  a shared-struct change is an ordinary build, not a toolchain expedition.

If the two-slot change measures badly on the slave path, the fallback is a
single range with explicit arbitration: enabling the memory card while a VCB01
holds the range is refused, naming the holder, and vice versa. That is a
strictly smaller change and the device model above does not depend on which way
this goes.

## 5. Probing before claiming

Two separate operations, both explicit.

**Sizing the machine.** `test_sizer()` answers "where does physical memory
end", which is what the operator needs to place the card. It is a DATI sweep of
the whole address space — on the rig, a million words before it reaches the
timeout at 2 MB — so it runs as an operator action with the CPU halted, never
implicitly at startup or on a configuration apply.

**Checking a range.** Before claiming, DATI the first and last word of the range
and one word every 8 KB through it with `qunibus->dma()`, and refuse the claim
naming the address that answered. A card that answers anywhere in the range
makes the whole claim wrong. As TODO.md notes, this can only refuse on a
positive answer: silence does not prove the range is free.

## 6. API

The device model carries the configuration, so enabling the card and setting its
range need nothing new — `GET /api/devices` and
`PUT /api/devices/MEM/params/<param>` already do it. What is added is the map
and the two actions:

- `GET /api/memory/map` — what is configured, no bus traffic: address width,
  I/O page start, the emulated ranges with their owners, and the last probe
  result with its age.
- `POST /api/memory/probe` — runs `test_sizer()`, returns the first
  non-existent address. Refused with 409 while the CPU runs.
- `POST /api/memory/fill` — `{address, count, value}` over an emulated range,
  written into DDR directly rather than by DMA.

`10.05_web/docs/api.md` gains these under the existing *Memory* section, whose
present text describes only examine/deposit.

Dump-to-file and paper-tape loading stay out of this plan: `POST /api/memory`
loads a program in 4096-word chunks today, and a file format for the rest wants
the media manager's file tree rather than a new endpoint.

## 7. Configuration snapshots

Free, once memory is a device: `webconfigs` captures every enabled device and
its non-default parameters, so a saved configuration carries the card and its
range, and applying one that does not name it switches it off. A configuration
that names both a memory card and a VCB01 whose ranges overlap is rejected on
apply like any other conflict, and the apply reports it.

The bundled configurations gain a memory card where they should have one —
`211bsd` in particular, so 2.11BSD comes up with 4 MB.

## 8. Frontend

`category: "memory"` needs a widget or it falls through
`widgetFor()` (`10.05_web/3_frontend/src/components/widgets/index.ts:41`) to
nothing. A `MemoryWidget` in a new `widgets/memory.ts`, registered under the
`memory` category:

- a bar of the whole address space showing the physical card (from the last
  probe), the emulated range, the VCB01 bank and the I/O page, each labelled
  with its octal bounds and size;
- start/end fields and the enable switch, like any other device's parameters;
- a *Size the machine* button calling `POST /api/memory/probe`, offering the
  result as the range to claim.

## 9. Documentation

`CLAUDE.md`'s hardware note says QBone does not fill the range above 2 MB under
`qbone.service` and that `emulate_memory()` runs only from the menus. That
becomes wrong when this lands and wants rewriting to describe the card.

## 10. The test machine: an 11/23 with no memory and no console

The rig for this work is a KDF11-A (PDP-11/23) carrying neither a memory card
nor a console SLU. Everything the machine needs to come alive is supplied by
the board: the memory by this feature, the console by the emulated DL11 at
777560. That makes the test unambiguous in both directions — nothing runs at
all until the memory card is claimed, and there is no physical memory anywhere
in the space for a claim to collide with.

**Console.** The DL11 is enabled with an empty `serialport` and reached over
`/ws/console/0`, and `external_console` is `off`. This is the opposite of the
11/73's arrangement, where the CPU carries its own SLU on `/dev/ttyS2` and the
DL11 must stay disabled; the note in `CLAUDE.md` is about that board, not this
one.

**Address width.** The KDF11-A is an 18-bit machine, so the service runs with
`--addresswidth 18` and the card's range is 0 .. 0o757776 — the whole 248 KB
below the I/O page, trap vectors included. The width is a command-line option
only, so the test edits the unit's `ExecStart`; a settings entry for it is
worth considering but is not part of this plan.

**Sizing with nothing there.** `test_sizer()` returns 0 on this machine: the
very first DATI times out. The probe, the map and the widget must read that as
"the machine carries no memory", the normal case here, and the card must be
claimable from address 0.

**Clock.** BEVNT on the 11/73 rig comes from an external line clock. If this
one has none, QBone's own `ltc_c` at 777546 supplies it, which some XXDP tests
want.

The steps, in order, each one resting on the one before:

1. **Console alone.** Power up with the memory card disabled. Micro-ODT lives
   in the CPU's microcode and talks to 777560, so the `@` prompt arrives on
   `/ws/console/0` with no memory in the machine at all. Examining any address
   answers a timeout. This separates a console fault from a memory fault before
   memory exists.
2. **The card appears live.** Claim the range with the CPU sitting in ODT. The
   same examine now answers, and a deposit reads back — the whole feature end
   to end, at the CPU's own timing, without restarting anything. This is also
   the RPLY-timing measurement (§11), and the thing to try by hand before any
   of this plan is built.
3. **Both sides see one memory.** `POST /api/memory` writes a pattern that ODT
   reads back at the same addresses, and a value deposited from ODT is read by
   `GET /api/memory`. The ARM's DDR mapping and the PRU's are the same cells.
4. **Read-modify-write.** The slave path handles a DOUT following a DIN inside
   one SYNC (`pru1_statemachine_data_slave.c`, `state_data_slave_din_block_-
   complete`), but only device registers have ever exercised it. Every `INC
   (R0)` against memory is a DATIO; run a loop that does one and check the
   count.
5. **A program runs from it.** The KDF11-A has no boot ROM and QBUS has no
   emulated M9312, so a bootstrap is loaded with `POST /api/memory` and started
   from ODT. Then a guest that fits 248 KB — RT-11, or the XXDP memory
   exercisers, which read and write patterns across the whole range and are the
   closest thing to a verdict on the card.
6. **DMA into the range.** An RL or MSCP disk transferring into memory the
   board itself serves, which the PRU short-circuits without a bus cycle
   (`pru1_statemachine_dma.c:209`). Booting RT-11 from `du0` covers it.

Two things this rig cannot answer, both needing the 11/73 and its 2 MB card:

- **Block mode across the boundary.** With the whole space emulated there is no
  boundary. A DATBI block that starts in the physical card and would run past
  its top has to be exercised where a card ends — reads and writes straddling
  0o10000000.
- **Coexistence with real memory**, and with it the collision probe of §5,
  which has nothing to find here.

## 11. Other risks

- **RPLY timing.** `ddrmem.h` warns that a PRU read of ARM DDR may take 400 ns.
  The Q-bus allows 10 µs, so the margin is wide, but it has never been measured
  with a real CPU as master. Step 2 above is that measurement.
- **Contents survive INIT, not the operator.** A power cycle keeps the range
  claimed and its contents, so a program loaded with `POST /api/memory` and
  started from the console still works — the workflow the console notes
  describe. Only claiming or re-ranging the card clears it.
- **2.11BSD is not a test for this rig.** It wants 22-bit addressing and far
  more than 248 KB, so it stays a test for the 11/73 with the range above its
  card.

## 12. State of the work

Built, and building clean for the board and the frontend:

- `memory_c` (`10.02_devices/2_src/memory.{hpp,cpp}`), the `MEM` device, with
  `startaddr`/`endaddr`/`probe` and a derived `size`. Registered in the device
  set, so `/api/devices`, the parameter endpoint and configuration snapshots
  carry it with no work of their own.
- Two ranges in the PRU, tested in line by `DDRMEM_ADDR_EMULATED`;
  `ddrmem_c::set_range(slot, …)` with `contains()`, `slot_of()`,
  `overlapping_slot()`, `clear_range()` and `fill_range()`. The VCB01 bank moved
  to the device slot; the DEUNA's DMA shortcut moved to `contains()`.
- `qunibus_c::probe_range()`, and the claim that refuses on what it finds.
- `GET /api/memory/map`, `POST /api/memory/probe`, `POST /api/memory/fill`,
  documented in `api.md`.
- The `MemoryWidget` address-space bar, registered for the `memory` category.

Left:

- Everything in §10 and §11: none of it has met a machine yet.
- The bundled configurations. The named machines (`211bsd` and the rest) live in
  `/var/lib/qunilator/configs` on the board rather than in this repository, so a
  memory card goes into them there — and the 11/73 carries its own 2 MB, so the
  card belongs in a configuration only where the machine needs it.

## 13. Order of work

1. Measure the slave path with a hand-claimed range against the 11/23
   (§10, steps 1–2). Everything else depends on the answer.
2. `memory_c`, single range, arbitration against the VCB01 by refusal. Device
   list, parameters, configuration snapshots.
3. The range probe and `test_sizer()` as an operator action; `/api/memory/map`,
   `/api/memory/probe`.
4. Two PRU slots, so a VCB01 and a memory card coexist.
5. The frontend widget and the address-space bar.
6. `/api/memory/fill`, the bundled configurations, api.md and CLAUDE.md.
