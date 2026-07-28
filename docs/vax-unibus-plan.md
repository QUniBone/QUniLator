# VAX Unibus host for QUniBone

Plan for running a VAX CPU on UniBone so the Unibus variants of the device
emulations can be driven by VMS, Ultrix and the VAX diagnostics.

## Goal

The Unibus device models in `10.02_devices/2_src` need a Unibus host to be
exercised. The KA11 emulation (`cpu.cpp`, `cpu20/`) supplies one, and it reaches
the PDP-11 operating systems. A VAX host adds three things the KA11 cannot:

- **VMS and Ultrix drivers.** `DUDRIVER` against `uda.cpp`/`mscp_server.cpp` is
  an MSCP initiator written independently of everything that has tested that
  code so far. `XEDRIVER` against `deuna.cpp` is the only driver that model has
  ever had — the DEUNA is Unibus-only, so the Q-bus board cannot reach it.
- **Unibus adapter DMA patterns.** VMS drivers allocate map registers, set the
  byte-offset bit and purge buffered data paths. Device models see address and
  transfer patterns that no PDP-11 driver produces.
- **The VAX diagnostics**, run standalone under the VAX Diagnostic Supervisor.

Throughput is not a goal. On the AM335x expect a fraction of a real 11/780; the
work is structured so nothing depends on speed.

## Decisions

- **Model: VAX-11/780** (simh `vax780`). Its console subsystem is fully modelled
  in simh and its boot path is well documented. The DW780 and DW750 Unibus
  adapters present the same map-register interface to devices, so the device-
  facing behaviour under test is the same either way.
- **VAX memory lives in a plain ARM array**, allocated by the CPU model, sized
  8-14 MB. On a real 780 memory sits on the SBI; keeping it off the Unibus is
  faithful and keeps the PRU out of the instruction path. `cpu.cpp:120` already
  establishes this pattern with its `direct_memory` parameter.
- **The I/O page goes over the real bus.** CPU register access reaches devices
  through `unibone_dati()`/`unibone_dato()` and real DATI/DATO cycles, so the
  register callbacks and PRU timing are what the test exercises.
- **The Unibus map ends up in the PRU slave path**, so device DMA runs on the
  bus wires with an 18-bit address and is translated where a real UBA would
  translate it. Stage 3 uses an ARM-side translation to get VMS booting; stage 4
  moves it to where it belongs.
- **simh is vendored under `91_3rd_party/simh_vax/`.** simh is MIT-style and the
  project is BSD-2-Clause, so the licences compose. Per the project rule on
  vendored code, warnings from it are handled with a scoped, documented
  suppression in the makefile rather than by editing upstream sources.

## Prerequisites

Hardware: a UniBone cape in a Unibus backplane. QBone supplies CPU, memory and
the Unibus adapter; no PDP-11 CPU board is present. The build side already
exists (`makefile_u`, `pru1_u`, `buslatches_u.cpp`, `qunibussignals_u.cpp`).

Artifacts to collect and confirm in stage 0:

- simh VAX sources, `vax780` set.
- The VMB primary bootstrap image that the 780 console loads (`vmb.exe`),
  supplied with simh — confirm the exact boot handshake against simh's own
  documentation rather than assuming it.
- VMS distribution media, an Ultrix image if that host is wanted, and the DEC
  diagnostic pack carrying the Diagnostic Supervisor and the device diagnostics.
- A listing of which Unibus device diagnostics the pack actually carries. The
  device coverage decides how much of stage 5 is diagnostics and how much is
  driver-level testing under VMS.

---

## Stage 0 — Harvest and host build

Bring the simh VAX780 core into the tree and build it on the workstation, with
no QBone involvement.

Work:

- Vendor the VAX780 file set under `91_3rd_party/simh_vax/`: the CPU core, the
  instruction extensions (compatibility mode, CIS, floating point, octaword),
  the MMU, and the 780-specific SBI, memory, UBA and standard-device files.
- Add a host makefile target that builds the core against a thin shim standing
  in for simh's `sim_defs.h`, event queue and device dispatch. Everything simh's
  own `scp` provides that the core touches gets a stub with a recorded
  behaviour, so the seams are explicit and small.
- Keep simh's `M[]` as the memory array and its own console device for now.

Verification:

- The core builds clean on the host under the shim.
- It boots VMS from a simh-emulated disk with simh's own device set, on the
  host. This proves the harvest is complete before any bus work starts.
- Record which simh files were taken and at what upstream revision, in a
  `README` beside the vendored sources.

## Stage 1 — CPU on the bone, memory only

Cross-build the core for the AM335x and run it under the QBone application, with
memory and console but no Unibus.

Work:

- Wrap the core as a `unibuscpu_c` subclass alongside `cpu_c`, following the
  structure of `cpu.cpp`: parameters, `worker()`, start/stop, power events.
- Replace simh's `sim_interval` accounting with
  `the_flexi_timeout_controller->emu_step_ns()`, the way `cpu.cpp:473` does for
  the KA11, so emulated devices get a coherent time source.
- Batch instructions per `worker()` iteration. The KA11 loop steps one
  instruction per pass through a long block of parameter and switch handling;
  a VAX core wants a run of instructions per pass with the housekeeping checked
  between runs.
- Console over the existing serial infrastructure so the VAX console terminal
  reaches the web console channel.

Verification:

- `./crossbuild.sh` completes with no warnings, including from the vendored
  tree, and `.clangd` carries any new include paths so the editor stays clean.
- The console prompt appears and responds on the bone.
- VMS boots from a file-backed disk handled entirely inside the core, over the
  web console. Slow is expected and fine.
- Record the instruction rate here as the baseline for later stages: a stage
  that changes it by an order of magnitude has changed something structural.

## Stage 2 — I/O page and interrupts over the real bus

Give the VAX a Unibus adapter whose register window reaches real bus cycles.

Work:

- Implement the UBA control registers, status, and the map register file as VAX
  I/O space, per the DW780 description in the manual.
- Route VAX access to the Unibus I/O space through
  `unibone_dati()`/`unibone_dato()` so it lands on the bus as DATI/DATO.
- Extend the interrupt path. `unibuscpu_c::on_interrupt(uint16_t vector)` passes
  the vector alone; the PRU arbitration already tracks five request levels, so
  widen the callback to carry the BR level and map BR4-7 to VAX IPL 14-17 with
  the UBA vector offset applied.
- Bus timeouts on the Unibus become UBA errors and a VAX machine check, rather
  than the PDP-11 trap 4 the KA11 wrapper produces.

Verification:

- With a `dl11w` or `dz11` configured, the VAX reads and writes its registers
  from the console and the values match what the device model reports through
  the web API.
- A device raising an interrupt reaches the VAX at the right IPL with the right
  vector. Check both a BR4 and a BR6 device.
- Reading an unpopulated I/O address produces a UBA error and a machine check
  rather than a hang.
- The device's own cycle trace and the CPU's `cycle_trace_buffer` agree on what
  happened on the bus.

## Stage 3 — Unibus map and DMA, ARM side

Get a DMA device working end to end, with the map translation done in C++.

Work:

- Apply the map registers to `unibus_addr` inside `qunibusadapter::DMA()` and
  service the transfer from the VAX memory array.
- Implement the byte-offset bit and the buffered data path purge semantics that
  VMS drivers rely on.
- Configure `uda.cpp` in its UDA50 mode as the boot device.

Verification:

- VMS boots from the emulated UDA50 with its registers accessed over the real
  bus. This is the first point where the device models are genuinely under test.
- A map register deliberately left unallocated causes the transfer to fail the
  way a real UBA fails it, and VMS reports it rather than corrupting memory.
- File-level read/write against the disk under VMS, verified by mounting the
  same image on the host afterwards.
- Byte-offset transfers move the data VMS expects, checked with an odd-address
  transfer.

## Stage 4 — DMA on the wire

Move the translation into the PRU so DMA cycles appear on the bus.

Work:

- Widen the emulated memory window past the current 22-bit word array
  (`qunibus_memory_t` in `10.01_base/2_src/shared/qunibus.h`) to hold VAX
  physical memory, byte-addressed.
- Place the map register file in PRU-visible DDR and add the lookup to the PRU
  slave path: index [addr 17:9] into the map, add offset [8:0].
- Remove the stage 3 ARM-side interception so the device drives its 18-bit
  address onto the bus as it should.

Verification:

- Everything stage 3 verified still passes, with the DMA now visible as bus
  cycles.
- Measure the added slave response latency and confirm it stays inside the
  Unibus slave timing budget with margin.
- A real third-party Unibus DMA board — a UDA50, an RL11 — works under VMS
  alongside the emulated ones. This is the payoff of doing the PRU version, and
  a real controller running the same driver as an emulated one is the sharpest
  differential test available.

## Stage 5 — Device validation

The point of the exercise: run the software that has never driven these models.

Work and verification, per device:

- **`uda.cpp` (UDA50)** — VMS `DUDRIVER`: boot, mount, heavy file I/O,
  multi-unit configurations, error paths with a deliberately missing unit.
- **`deuna.cpp` (DEUNA)** — VMS `XEDRIVER`: interface up, ARP and IP traffic,
  DECnet to another node, multicast and promiscuous modes. First driver this
  model has ever seen.
- **`rl11.cpp` / `rl0102.cpp`** — VMS `DLDRIVER`: mount and file I/O against the
  same images the XXDP diagnostics already cover, for cross-checking.
- **`dz11` / `dhv11` Unibus variants** — VMS terminal driver: logins on several
  lines, modem control, flow control under load.
- **VDS diagnostics** for whichever of the above the diagnostic pack covers, run
  standalone.

Each finding gets recorded the way the existing diagnostic notes are, with the
manual passage that decides the correct behaviour. The project rule holds here:
the manual is the specification and the diagnostic only validates.

## Stage 6 — Integration

Work:

- Expose the VAX CPU as a configurable device through the web API and the
  configuration model, alongside the existing CPU, with its memory size, boot
  device and console routing as parameters.
- Document the endpoints in `10.05_web/docs/api.md`.
- Dashboard treatment for CPU state, matching what the existing CPU offers.

Verification:

- A saved configuration containing the VAX plus its devices applies cleanly from
  a cold start and boots without manual intervention.
- The API reports CPU state consistently with the console.

---

## Risks

- **Console and boot handshake.** The 780 console subsystem and the VMB load are
  the least portable part of the harvest, because they reach outside the CPU
  core into simh's host facilities. Stage 0 exists to hit this on the
  workstation where it is cheap to debug.
- **Slave timing in stage 4.** The map lookup adds work to a path with a hard
  deadline. Measuring the margin is a stage 4 exit criterion, not an
  afterthought.
- **Memory window growth.** Widening `qunibus_memory_t` touches the DDR
  reservation shared with the PRU and the code that indexes it. Expect this to
  reach further than the diff suggests.
- **Diagnostic coverage is unknown** until the pack listing is read. If the
  Unibus device diagnostics are thin, stage 5 leans on the VMS drivers, which is
  still a strong test but a less pointed one.

## Open questions

- Whether Ultrix is worth carrying as a second host, or whether VMS plus the
  diagnostics gives enough independent driver coverage.
- Whether the KA11 and the VAX should share the bus-access wrapper in `cpu.cpp`
  or keep separate ones. Decide after stage 2, when the differences in the
  interrupt and bus-timeout paths are concrete.
- Whether a MASSBUS adapter is ever wanted. RP06 and RM03 are the disks VMS of
  that era expects, and they are not Unibus devices, so they would live entirely
  inside the CPU model with no bus involvement.
