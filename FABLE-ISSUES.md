# FABLE-ISSUES — code review findings

A deep review of the QUniLator code base, centred on the code that controls the
hardware: the ARM/PRU bus engine (`10.01_base/2_src/arm`, `.../pru1_q`,
`.../pru1_u`, `.../shared`), the emulated CPU cores (`10.02_devices/2_src/cpu20`,
`cpu34`, `cpu.cpp`), and the device layer, with spot checks of the storage/MSCP
path and the web service. Findings are ordered by the categories requested;
within each category the most important item comes first.

One fact frames several findings: **`assert()` is live in the deployed
binaries** (no `-DNDEBUG` anywhere in the build), so every reachable assert in
the engine is a process abort that takes the running machine's disks and
console down with it.

---

## 1. Technical correctness

### 1.1 `DMA()` returns a *stale* success flag when INIT is active — FIXED in c9b6400
`qunibusadapter.cpp:711-715`: a `DMA()` call during INIT sets
`dma_request.complete = true` and returns — but never touches
`dma_request.success`, `qunibus_end_addr`, or the buffer. Devices reuse their
`dma_request_c` objects, so `success` still holds the result of the *previous*
transfer. A device that transfers during a bus INIT can believe a DATI
succeeded and consume whatever the last transfer left in the buffer. The
matching cancel path (`requests_cancel_scheduled`, line 429) carefully sets
`success = false`; this early-out should do the same.

### 1.2 Mailbox command payloads are filled outside the lock that serializes commands — FIXED in e25ba6d
`mailbox_execute()` (`mailbox.cpp:119`) serializes the *execution* of ARM2PRU
commands under `arm2pru_mutex`, but every caller fills the payload **before**
calling it, unlocked. For DMA/INTR the payloads live in dedicated
`mailbox->dma` / `mailbox->intr` structs and all writers hold `requests_mutex`,
so those are safe in practice. But `mailbox->param` and
`mailbox->initializationsignal` **overlap in one union** (`mailbox.h:329-336`)
and are written by several threads with no common lock:

- CPU worker: `cpu.cpp:391/419` (`param` for `ARM2PRU_CPU_ENABLE`),
- web/control threads: `qunibus.cpp:265-277` (`initializationsignal` for INIT
  pulse, power cycle), `qunibus.cpp:468` (`param` for `ARM2PRU_CPU_BUS_ACCESS`),
- adapter worker: `qunibusadapter.cpp:291` (`param` on CPU unregister).

A power cycle from the web API racing a CPU start/stop can clobber the other
request's payload between fill and PRU pickup (e.g. `initializationsignal.id`
overwrites the low half of `param`). Low probability, impossible to debug when
it fires. Fix: pass the payload into `mailbox_execute()` (or take
`arm2pru_mutex` around fill + execute at every call site).

Both, as it turned out, because the payloads are not one shape. `mailbox.h`
now offers `mailbox_lock_c`, which holds `arm2pru_mutex`, and
`mailbox_execute_locked()` to be used inside it; `mailbox_execute()` keeps
taking the lock itself for a request with no payload, and gained an overload
that fills `param` under it, which is what most of the call sites wanted.
`qunibus.cpp` grew `set_initializationsignal(id, val)` around the same
mechanism, and its eleven call sites — INIT, the ACLO/DCLO and POK/DCOK power
sequences, HALT — read better for it.

The `buslatch` members are in that same union and had the same problem the
other way round: `buslatch_c::getval()` and `probe_grant_continuity()` fill an
address, run the command and then read the PRU's *answer* out of the union, so
the lock is held until the answer has been taken rather than released at the
end of the command. `address_overlay` has a field of its own but is still half
of a command, and is filled under the lock too.

Three places still write `mailbox->arm2pru_req` directly and are deliberately
left: `pru_c` probing with a NOP before any device exists,
`ddrmem_c::unibus_slave()` and `buslatches_c::test_timing()`, which start a PRU
loop that runs until the request byte is changed and so cannot be a
fill-and-ack at all. All three are single-threaded startup or test-firmware
paths, and none of them touches the payload union.

Verified on ubx: both buses build warning-free and the host suites pass; XXDP
boots from RL0 and lists the directory. The race itself is the reviewer's
example — a web power cycle against a CPU start/stop — so it was driven
directly for 30 s, `POST /control powercycle` every second against `halt` and
`continue` twice a second, which is `initializationsignal` and `param` fills
interleaving at speed. The service came through it, the journal holds nothing
but the memory probe's own timeout, and XXDP boots and lists afterwards.
Nothing here proves the old code would have failed that test — the window is a
few instructions wide — so this rests on the structure, not on a reproduction.

### 1.3 Cross-thread flags are plain/volatile bools, not atomics — FIXED in e8ce517
`line_INIT`, `line_ACLO`, `line_DCLO` and `deviceregister_servicing` are plain
`bool`s written by the adapter worker and read by device threads in `DMA()` /
`INTR()` (`qunibusadapter.cpp:711, 940, 783`); `priority_request_c::complete`,
`::executing_on_PRU` and `dma_request_c::success` are `volatile bool`
(`priorityrequest.hpp:80-81, 121`); `workers_terminate` likewise. These are
data races in the C++ memory model (UB), and `volatile` is not a
synchronisation primitive. On this single-cluster ARM32 with the mutex/condvar
handshakes around most of them it works today, but any compiler upgrade or
reordering can break it silently. Converting them to `std::atomic<bool>` with
relaxed/acquire-release ordering is cheap and mechanical.

All of them are `std::atomic<bool>` now, and `device_c::init_asserted` with
them: it is the same flag as `line_INIT` copied onto each device by the same
worker thread, and the reviewer's list simply did not reach it.
`deviceregister_servicing` had already been converted when it was added.

The ordering is the default, sequentially consistent, rather than the relaxed
one the finding offers: `complete` is not just a flag but the publication point
of a transfer — the words in the device's buffer and `success` are only
readable because the reader saw it set — so it needs the release/acquire
relationship a relaxed pair would not give. Nothing here is read at a rate that
makes the barrier worth counting; the emulated processor's spin loop reads the
mailbox and takes a mutex, not these.

What the change cost was not the flags but the classes holding them: an atomic
member deletes the implicit copy constructor, and every device declared its
requests as `intr_request_c intr_request = intr_request_c(this)`, a
copy-initialisation that C++11 (which is what the makefiles ask for) still
requires a copy constructor for. Those 29 declarations across 15 device headers
are now `intr_request_c intr_request{this}` — direct initialisation, which is
what `deuna.hpp` already used. No behaviour moved with them.

Verified on ubx: both buses build warning-free, the host suites pass (297 + 46 +
21 + 40 + 63 + 41 checks, no failures) and the 36 CPU-core diagnostics are
unchanged; on the board XXDP boots from RL0 through the M9312, lists all 727
files of the directory and reboots on a power cycle, with no error in the
journal.

### 1.4 Shell command built from a web-settable path — FIXED in b4e2479
`storageimage.cpp:116-118` runs `system("zcat <image>.gz ><image>")` with the
image path interpolated unquoted; `sharedfilesystem/filesystem_host.cpp:461` has
the same pattern. `image_filepath` is settable through the web API, so a
filename containing spaces or shell metacharacters breaks the command — or
executes it. The service runs as the operator with sudo rights on the board.
Use `fork`/`execvp` with an argument vector, or at minimum quote-escape.

Neither runs a shell now. `storageimage.cpp` grew `uncompress_gz()`, which
forks, makes the output image the child's stdout with a file descriptor, and
`execlp("zcat", "zcat", "--", gz_path, …)` — the path is an argument, so there
is no command line for a metacharacter to be quoted into. A failed expansion
unlinks the half-written file, because a truncated image is one the retry loop
would then open as a valid empty disk. `filesystem_host.cpp::clear_disk_dir()`
walked its `rm -rf` glob through `/bin/sh`; it uses `nftw()` with `FTW_DEPTH`
now, removing each entry below the root, `FTW_PHYS` so a symlink is unlinked
rather than followed and `FTW_MOUNT` so the walk cannot leave its filesystem —
and the three shell globs it used to reach hidden files without matching `..`
are answered by the walk for free.

The `webstorage_image_*` helpers put no restriction on metacharacters in the
path — a value with a semicolon reaches the drive's `image_fname` unaltered —
so the input the finding describes is real and still arrives; it is the sink
that changed.

Verified against the deployed fixed binary on ubx, not just by reading. A
gzip image was placed on the board under the name `evil;touch qbone_pwned.rl02.gz`,
a drive pointed at `images/dl/evil;touch qbone_pwned.rl02` and powered on so the
spin-up opened it: the plain image appeared, decompressed correctly from the
hostile name (the feature still works), and no `qbone_pwned` marker was created
anywhere writable — under the old `system()` the `;touch` would have run as the
operator. The board was then restored to the `xxx` configuration and XXDP boots.
`clear_disk_dir()` was not exercised end to end — reaching it needs a shared
host directory set up on a drive — so that half rests on reading; both buses
build warning-free and the host suites pass.

Two other shell-outs were checked and left as they are: `webupdate.cpp` builds
`timeout 90 <updater> --changelog` for `popen()`, but every word of it is fixed
and nothing from the request reaches the line; and `webupdate`'s unit start
already uses `fork`/`execvp`. `ddrmem`/`buslatches` menu paths run no shell.

### 1.5 UNIBUS: an INIT pulse can be swallowed as a "stray event" — FIXED in 8c5f770
`qunibusadapter.cpp:1425-1446`: the worker derives INIT edges by comparing
`init_signal_cur` against its own `line_INIT`. If INIT asserts *and* deasserts
before the worker services the event (the PRU updates `init_signal_cur` in
place), no edge is seen, the event is acked as stray, and **no device is
reset**. Real UNIBUS INIT is ≥10 ms so a processor-driven INIT is safe, but a
short INIT from an odd console/diagnostic device would be lost. The QBUS side
solved this properly (synthesises assert+negate on every event,
`qunibusadapter.cpp:1450-1457`); the UNIBUS path could do the same when the
event arrives with no visible edge but a set signal counter.

That is what it does now. A pending init event with no edge means the line
went away from the level the worker holds and came back, so both halves are
played out — assert then negate, or negate then assert when INIT was already
asserted. The event count is read *before* the level is sampled, and every
count it covers is acknowledged together: they all describe the one pulse that
has since settled, and acknowledging a single one would play the same pulse
out again on the next pass.

Reproduced on ubx rather than reasoned about. The rig cannot make a short INIT
through any of its own paths — `qunibus_c::init()` holds the line for the
10 ms a PDP-11/70 does, and the emulated CPU's RESET goes through it — so the
wait was temporarily removed, which leaves the two mailbox commands separated
only by a PRU loop and lands both inside the worker's 100-300 µs wake window.
On the old code that stimulus logged

    EVENT_INIT: init_signal_cur=0x0, init_raise=0, init_fall=0
    EVENT_INIT: init_signal_cur=0x0, init_raise=0, init_fall=0

and nothing else: two events dropped as stray, no `worker_init_event()`, no
device reset — the finding exactly. With the fix the same stimulus logs one
`INIT asserted`, one `INIT negated` and `init_pulse=1`, once. A real 10 ms
INIT still takes the edge path untouched (`init_raise=1` then `init_fall=1`,
16 ms apart). The stimulus was then removed, the clean binary deployed, and
the machine boots XXDP from DL0 and reads its directory off the RL.

### 1.6 KD11-EA/KA11 `DIV`: 0x80000000 / −1 is signed overflow — FIXED in a9ccbe2
`kd11ea.c:679` (`ldiv(prod, dv)`): dividing INT32_MIN by −1 overflows `long`
(32-bit on ARM), which is UB inside `ldiv`. On ARM it happens to produce a
value that then takes the V-bit path, matching hardware by accident. Guard the
one case explicitly (set V, leave registers) and the core is UB-free. Same
pattern in `cpu20/ka11.c` if EIS is ever enabled there.

The pair is intercepted before the divide now. Nothing observable moved: the
host, where `long` is 64 bits, computed a true 2147483648 and that is out of
16-bit range as well, so both widths already produced V=1, N=0, Z=1 and no
register write — which is what the guard sets. The KA11 needed nothing: the
11/20 has no EIS and traps the whole 0070000 group as reserved.

### 1.7 `intr_request_c::get_vector()` truncates vectors to 8 bits
`priorityrequest.hpp:165`: returns `uint8_t` for a 16-bit vector. Only caller
is the resource-info line (`qunibusdevice.cpp:389`), so the damage is a wrong
octal vector shown for anything ≥ 0400 (floating vectors, second controllers).
One-character fix.

### 1.8 MSCP `DMARead()` leaks its buffer on a failed transfer — FIXED in c9b6400
`mscp_port.cpp:1176-1196`: `new uint16_t[...]`, then on `!dma_request.success`
returns `nullptr` **without `delete[]`**. Every NXM read a guest provokes leaks
the full transfer size. A guest OS with a buggy driver (or a diagnostic doing
deliberate NXM probing through the controller) bleeds the service dry over
hours. Add the `delete[]` in the failure branch.

---

## 2. Showstopper bugs

### 2.1 Mailbox DMA overwrite race → assert abort (known, issue #92) — PARTIALLY in c3c87a9
`qunibusadapter.cpp:1249` (`assert(wordcount_transferred <= dmareq->wordcount)`).
`requests_cancel_scheduled()` (INIT/power event) clears `prl->active` and
releases the waiting device **while the PRU is still executing the transfer**
("active on the PRU are left running", line 411-438). The next `DMA()` request
sees the level idle and pushes `ARM2PRU_DMA`, overwriting `mailbox->dma`
(startaddr/wordcount/words) under the PRU's feet; the completion that then
arrives mixes the two transfers and trips the assert — observed as the service
aborting on a config apply after a refused DMA (repro filed with issue #92).
Fix direction: a "PRU busy" latch that survives `requests_cancel_scheduled()`
— don't push a new `ARM2PRU_DMA` until the orphaned completion has been
acked — or a generation counter in `mailbox->dma` so a stale completion is
discarded instead of accounted.

### 2.2 A guest-supplied DMA range that runs off the end of memory aborts the service — FIXED in c9b6400
`qunibusadapter.cpp:702`: `assert((unibus_addr + 2*wordcount) <=
addr_space_byte_count)` — and asserts are live. The MSCP port validates only
the *start* address of guest-supplied buffer descriptors
(`mscp_port.cpp:1128-1133`, `1164-1171`; comments there even say the goal is
"must not take the whole controller down"). A guest driver bug — or one
crafted command in a guest program — with a buffer descriptor near the top of
the address space aborts the whole emulator: machine, disks, console, gone.
Two-part fix: clamp/reject in `DMAWrite`/`DMARead` (report NXM upward, which
MSCP already knows how to do), and demote the engine assert to an error return
so no device can ever reach it. Worth auditing the other DMA devices (RL, RK,
TS, DELQA) for the same missing end-of-range check — the engine-side fix
covers them all at once.

### 2.3 Enabling a device whose registers collide with another aborts the service — FIXED in 347348d
`qunibusadapter.cpp:190-193`: `register_device()` hits `FATAL` (process abort)
when a new device's register address is already claimed. This runs at *enable*
time from a user action (web API, config apply): two DL11s at 776500, or a
user-moved base address overlapping a neighbour, kills the running machine.
The same function already returns `false` for the other configuration errors
(bad register config at 159-167, handle exhaustion at 213-217) and
`install()` handles refusal cleanly (`qunibusdevice.cpp:140-143`). This one
check should refuse, not abort.

### 2.4 An INTR re-raise with a different vector aborts the service — FIXED in 92df982
`qunibusadapter.cpp:963-964`: `assert(scheduled_intr_req->vector ==
intr_request.vector)` — a device that re-raises a pending interrupt after its
vector parameter changed (vectors are editable while a device is unplugged,
but the pending request survives INIT paths) aborts the process. The comment
above it already says "if different vector, it may not be ignored"; do that —
update the scheduled request's vector (and the PRU's `mailbox->intr.vector[]`
if not yet granted) instead of asserting.

The vector assertion could not in fact fire: a device re-raises by passing the
same `intr_request_c` member, so it compared the object with itself. The abort
that *was* reachable is the `device` assertion on the line above — two devices
put on one priority slot at one level, which `warn_on_slot_collisions()` only
warns about. Both are gone; the vector is now corrected on a pending request,
which matters because MSCP and DELQA take their vector from the guest.

### 2.5 A priority-slot collision wedges the PRU's interrupt arbitration — ARM SIDE FIXED in 4d1622d
*Not from the review — found on ubx while fixing 2.4.*

`qunibusdevice.cpp`, `warn_on_slot_collisions()`: two devices given the same
priority slot are **warned** about and installed anyway.

    WRN     rl] Slot 1 used by device rl is also used by DL11

`slot` is a writable parameter, so this is an ordinary operator mistake, and a
saved configuration carries it. Once both devices interrupt on that slot the
PRU's interrupt arbitration stops moving:

    ERR  cpu34] unibone_grant_interrupts(): PRU arbitration pending for >100ms
                - PRU stopped or hung?

repeating for as long as the machine is left running, with the emulated
processor unable to make progress. `dc_off`/`dc_on` and a power cycle did not
clear it on ubx; only `systemctl restart` did.

2.4 removed the assertion that used to abort the service on the second device's
INTR, so the process now survives — but the arbitration is wedged all the same,
and the refusal path added there was never reached in the run that wedged it. So
the slot collision costs more than the assertion did, and the damage is in the
bookkeeping the two devices share, not in the ARM-side check.

Worth doing first: make `warn_on_slot_collisions()` refuse the install rather
than warn, the way an address conflict now does (2.3) — a collision has no
working outcome to allow. That is ARM-side and small. What actually wedges the
PRU is a separate question and needs the arbitration state machines read; the
refusal keeps a board out of it meanwhile.

Done: `install()` refuses a slot shared **at one arbitration level**, which is
one entry of `request_levels[level].slot_request[slot]` and the collision that
drops interrupts. A slot shared at different levels shares no entry — the PRU is
never told a slot, `mailbox_intr_t` carrying one vector per BR line — so that
stays a warning. It caught a shipped configuration on the way: `DL11b` was given
its slot and vector by assigning `parameter.value` directly, which never reaches
the RCV/XMT requests, so the second SLU had been arbitrating on DL11's slots
with DL11's vector; it goes through `set_default_bus_params()` now. The PRU-side
wedge is untouched and still open.

---

## 3. Performance — CPU cores and engine

The cores themselves (KA11, KD11-EA/KT11-D) are tight: table-free decode via
one big switch, the derived-page-descriptor MMU with the hot path inlined,
per-instruction trace gating cached (`cpu->tracing`). The measurement
infrastructure already in place (`cpu_access_profile_note`,
`event_latency_c`) is the right tool — the costs below are all *outside* the
cores, in the per-instruction engine round trips.

### 3.1 One PRU round trip per instruction for interrupt granting
`cpu.cpp:87-105` — `unibone_grant_interrupts()` runs before **every** opcode
fetch (`kd11ea_condstep`/`ka11_condstep`) and costs a full
`mailbox_execute(ARM2PRU_ARB_GRANT_INTR_REQUESTS)` plus a spin on
`ifs_intr_arbitration_pending` that the code itself documents as "often
60-80 µs of idle spinning". At 60-80 µs per instruction this alone caps the
emulated CPU near ~15k instructions/s regardless of everything else. The
grant *window* must exist per instruction, but the round trip only matters
when a BR is actually pending. Have the PRU maintain a "device BR pending"
byte in the mailbox (it already sees the request lines every loop); the ARM
then reads one uncached byte and skips the whole round trip in the common
no-interrupt case. That is the single biggest speedup available to the
emulated processors.

### 3.2 The CPU bus-access spin loop contends on the global request mutex
`qunibusadapter.cpp:822-851`: while the PRU performs a single-word CPU access,
the CPU thread loops `pthread_mutex_lock(&requests_mutex)` /
`dynamic_cast` / unlock. Every iteration takes the same mutex that every
device DMA/INTR and the worker thread need, and performs RTTI. On a machine
with disk + network I/O running, the CPU thread and the realtime worker fight
over one lock at instruction rate. Improvements, in increasing order of
effort:
- hoist the `dynamic_cast` out of the loop (the active request can only
  change under the mutex; a type tag — see 4.6 — removes RTTI entirely);
- spin on `EVENT_IS_ACKED(*mailbox, dma)` (one uncached byte compare) first
  and take the mutex only when the event has fired or the active request
  pointer changed;
- add a `__builtin_arm_yield()`/short pause in the loop so the spinning
  low-priority CPU thread stops stealing whole scheduler quanta from device
  workers on the single core.

### 3.3 `direct_memory` (PMI) is the big lever and defaults off
`cpu.cpp:128, 185-200`: with `direct_memory`, memory DATI/DATO bypass the PRU
entirely (`ddrmem->pmi_exam/deposit`) — the code's own speed estimate is 5×
(`emulation_speed` 0.5 vs 0.1). Two costs remain on that path worth removing:
- `qunibusadapter->is_rom(addr)` on **every DATI** (`cpu.cpp:190`) reads the
  PRU shared-RAM iopage table — an uncached bus access per instruction fetch.
  Cache the ROM range(s) ARM-side (they change only on M9312/MRV11 install);
- `the_flexi_timeout_controller->emu_step_ns()` is called per access and is a
  cheap early-out today (`timeout.cpp:344-347`), but only because
  `CPU_CONTROLLED_TIME` is off; if it is ever enabled, that path takes a
  mutex — worth a comment or a lock-free fast path now.

### 3.4 Per-instruction bookkeeping in the CPU worker loop
`cpu.cpp:458-551`: every pass writes `runmode.value`, `pc.value`, calls
`core_set_switches()`, `core_apply_options()`, `core_publish_status()` (seven
parameter stores in `cpu34.cpp:130-139`), increments `cycle_count.value`, and
evaluates the whole switch/power block. None of it needs instruction-rate
freshness — the readers are the web UI and the panel. Publishing every N
instructions (or on a 10-20 ms clock) trims a measurable constant from every
instruction; keep the switch handling immediate by checking a single dirty
flag.

### 3.5 Device DMA is chunk-serialised through one mailbox buffer
`PRU_MAX_DMA_WORDCOUNT` is 4096 words and each chunk costs an ARM↔PRU round
trip plus a `memcpy` into/out of the mailbox (`qunibusadapter.cpp:532-535`,
`1255-1257`). For a UDA doing large transfers to *emulated* memory, the data
crosses: DDR (image cache) → mailbox (PRU RAM) → PRU copies word-by-word into
the same DDR. When both the source device buffer and the target range live in
board DDR and no external card claims the range, the transfer could be a
straight ARM-side `memcpy` with the counters bumped — the same shortcut
`registered_cpu->on_dma()` already takes for the VAX (`qunibusadapter.cpp:722`).
Worth profiling before building: on a QBUS rig with a real CPU the guest's
memory is real and this doesn't apply, but on an all-emulated UNIBORN rig it
is every disk block twice over a slow path.

### 3.6 QBUS_BUS_TRACE is four latch reads per main-loop pass
`pru1_main_qbus.c:227-253` — flagged as debug firmware in the comments, and
each `buslatches_getbyte` is ~store+load over the 100 MHz latch bus. Just make
sure release firmware builds define it off; a stray enable costs every DMA and
slave cycle its margin.

---

## 4. Structural improvements

### 4.1 One error-handling policy for the engine
The adapter currently mixes four failure behaviours: `FATAL` (2.3), live
`assert` (2.1, 2.2, 2.4), `ERROR` + refuse (`register_device` register-config
check, `DMA()` slot-held check), and silent stale state (1.1). The rule the
newer code already follows — *user- or guest-reachable conditions refuse with
an error; only internal invariants assert* — should be applied to the whole
file. Most of section 2 disappears when it is.

### 4.2 Register-handle allocation fragments and never recovers holes — FIXED in 0856452
`qunibusadapter.cpp:204-218` allocates register handles strictly above the
highest handle in use; holes from unregistered devices are only reclaimed when
everything above them is also gone. A long-running board whose operator
enables/disables devices in an unlucky order creeps toward the 254-handle
ceiling and then refuses installs until restart. A free-list (or first-fit
scan for a contiguous run, which the 4-bit-per-entry map makes easy) fixes it
permanently.

First-fit it is, over `register_by_handle[]` — the allocation record itself,
rather than the IO page map the old scan derived it from. The unlucky order is
two devices leapfrogging: disable one and re-enable it above the other, then the
same for the other. On ubx that refused `rl` on round 40 before the fix and ran
200 rounds after it. Cycling a *single* device never showed it, the top dropping
back when the device on top leaves, which is why it went unnoticed.

### 4.3 Dead and vestigial code in the hot files — PARTIALLY in 347348d
- `qunibusadapter_c::on_init_changed()` (`qunibusadapter.cpp:125-131`) is never
  called — the adapter is not in its own `devices[]` table — so the ARM-side
  "cancel all BR/NPR on INIT" never runs. The QBUS PRU clears
  `device_request_mask` itself (`pru1_statemachine_initialization.c`, INIT
  branch), so this is currently harmless, but verify the UNIBUS PRU does the
  equivalent, then delete or wire the dead method.
- The second writable/active-DATI `FATAL` block in `register_device()`
  (`qunibusadapter.cpp:242-250`) is unreachable — the same condition is
  refused at 159-167. — FIXED in 347348d; the other two bullets stand.
- `#ifdef TODO` blocks (emulated-CPU arbitration in `pru1_main_qbus.c`,
  `pru1_statemachine_arbitration.c:162-192`) and large commented-out
  experiments throughout the adapter deserve either a plan reference or
  removal; they obscure the code that actually runs.

### 4.4 Per-device register array is sized for the theoretical maximum
`iopageregister.h:119` sets `MAX_IOPAGE_REGISTERS_PER_DEVICE` to 255, and
every `qunibusdevice_c` embeds `registers[255]` — tens of KB per instance,
and the demo app instantiates every device it knows. The commented-out `32`
("RK611 has the most?") is the real bound. Shrinking it (or moving to a
sized-at-construction vector) cuts memory and, more importantly, makes the
struct-embedding intent honest.

### 4.5 Parameter writes from the web thread are unsynchronised with device workers
`parameter.cpp`/`parameter.hpp` have no locking; a `PUT
/api/devices/<dev>/params/<p>` runs `parse()`/`on_param_changed()` on the
civetweb thread while the device worker reads `.value`. For bools and ints
this is benign-in-practice (see 1.3); for `std::string` parameters
(`image_filepath`, serial port names) a concurrent read during reassignment
is a real crash risk. The `enabled` path is careful (install/uninstall
choreography), but ordinary parameters are not. Either marshal parameter
changes onto the owning device's worker (a small queue), or give
`parameter_c` a mutex and return copies.

### 4.6 Replace `dynamic_cast` type dispatch in the request scheduler
`request_schedule`, `request_execute_active_on_PRU`,
`worker_device_dma_chunk_complete_event` and the CPU spin loop all
`dynamic_cast` a `priority_request_c*` to decide DMA vs INTR. The level index
already encodes it (`PRIORITY_LEVEL_INDEX_NPR` ⇔ DMA); a `kind` enum on the
base class makes the dispatch free and lets the hot loops drop RTTI (see 3.2).

### 4.7 Static-assert the ARM/PRU shared-struct layout
`mailbox.h`/`iopageregister.h` rely on hand-counted "--- dword ---" comments
to keep the packed ARM view and the natural PRU view identical, and on
`pru_iopage_register_t` being a power-of-2 size for cheap indexing. One
`static_assert(sizeof(...))`/`offsetof` block per struct on the ARM side (and
`_Static_assert` on the PRU side) turns a silent protocol skew after the next
struct edit into a compile error. Cheap insurance for the highest-consequence
interface in the system.

### 4.8 Duplicated bus logic between pru1_q and pru1_u
The two PRU trees are sibling forks (`pru1_statemachine_dma.c`, `_arbitration`,
`_data_slave`, mailbox glue) that have already diverged in quality — e.g. the
QBUS DMA machine grew the SACK-release-on-abort fix (`pru1_q/...dma.c:108-125`)
and the holdoff/orphan-IAK machinery, while the UNIBUS twin did not need them
but also didn't inherit shared cleanups. Full unification is likely blocked by
PRU code-size limits and genuinely different protocols, but the *ARM-facing*
contract (mailbox handling, event signalling, DMA chunk accounting) could live
in one shared source included by both, so a fix like 2.1 lands on both buses
by construction.

### 4.9 `cpu_base_c::worker()` mixes five concerns
`cpu.cpp:445-552` interleaves console-switch emulation, power-event
serialisation, trigger/breakpoint checks, time-base bookkeeping and status
publication around the actual `core_condstep()`. Each is simple; together
they make the per-instruction path hard to reason about (and hard to make
fast, see 3.4). Extracting switch/power handling into functions called from
the loop would let the loop read as: handle-rare-events, step, publish.

---

## Notes on what was checked and found sound

- The QBUS arbitration state machine's DMR holdoff, orphan-IAK rescue and
  interrupt-register/BIRQ ordering (`pru1_statemachine_arbitration.c`) are
  carefully reasoned and documented against observed 11/73 behaviour.
- The signal/ack **counter** protocol for PRU→ARM events (`mailbox.h:190-206`)
  correctly survives re-raise-before-ack; no lost-event window there.
- The KT11-D derived-descriptor design (`kt11d.c`/`kt11d.h`) is coherent, and
  the MAINDEC-anchored comments in `kd11ea.c` (autoinc undo, MMR1/MMR2
  freezing, maintenance-mode destination references, condition-code rules on
  aborts) match the family documentation they cite; the CPU test suite pins
  them.
- The DATOB flipflop merge after INIT (`qunibusdevice.cpp:228-242`), the
  refused-slot DMA path (`qunibusadapter.cpp:746-759`) and the stale-completion
  tolerance (`qunibusadapter.cpp:1228-1237`) are recent, deliberate fixes and
  look right.
- `web_bus_mutex`/timeout discipline in the web layer (`webbus.hpp`) correctly
  bounds civetweb workers against a bus that never grants.
