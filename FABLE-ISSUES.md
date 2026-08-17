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

### 1.7 `intr_request_c::get_vector()` truncates vectors to 8 bits — FIXED in dc7f343
`priorityrequest.hpp:165`: returns `uint8_t` for a 16-bit vector. Only caller
is the resource-info line (`qunibusdevice.cpp:389`), so the damage is a wrong
octal vector shown for anything ≥ 0400 (floating vectors, second controllers).
One-character fix.

It was, and the character brought a second finding with it. The `intr_vector`
parameter is declared nine bits wide, so the truncation drops exactly the bit
the floating vectors need. Widening the getter then exposed what the missing
bit had been covering: `intr_request_c`'s constructor sets `vector = 0xffff`
for "not set yet", and an MSCP port has no vector until the guest's init
handshake gives it one — `uda` and `tqk50` had been listing that sentinel as a
plausible-looking `377`. The line prints `5/-` for a request with no vector
now, rather than a number nobody assigned.

Verified in the device menu on ubx, service stopped, since that menu's `ld` is
the only consumer of the string. With `rl`'s `intr_vector` set to 0500 the
installed binary printed

    - rl    Type RL11, ... INTRs:5/100
    - uda   Type UDA50, ... INTRs:5/377

and the new one prints `INTRs:5/500` and `INTRs:5/-`. Vectors below 0400 are
untouched (`ts` 5/224, `dzv11` 4/300,4/304, `DL11` 4/060,4/064). The board was
put back on the `xxx` configuration afterwards — XXDP boots from DL0 and lists
its directory, and `rl`'s vector is back at the 0160 default, the menu having
persisted nothing.

### 1.8 MSCP `DMARead()` leaks its buffer on a failed transfer — FIXED in c9b6400
`mscp_port.cpp:1176-1196`: `new uint16_t[...]`, then on `!dma_request.success`
returns `nullptr` **without `delete[]`**. Every NXM read a guest provokes leaks
the full transfer size. A guest OS with a buggy driver (or a diagnostic doing
deliberate NXM probing through the controller) bleeds the service dry over
hours. Add the `delete[]` in the failure branch.

---

## 2. Showstopper bugs

### 2.1 Mailbox DMA overwrite race → assert abort (known, issue #92) — FIXED in c3c87a9 and 6ce0736
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

Done, in two parts. The overwrite itself took the latch (`c3c87a9`), and that
half remains unproven against the bug — the window is one chunk's execution and
no test reached it. What the repro shows every time is the *other* half named in
issue #92, and that is what the second part fixes: **a request the PRU never
completes was never retired, so its slot stayed dead for the life of the
process.** On a dark or deviceless machine that is every later transfer on that
slot, and recovery took `systemctl restart`.

The obstacle was that **there was no way to tell the PRU to abandon a DMA.**
`ARM2PRU_INTR_CANCEL` existed for interrupts; there was no equivalent, so a
transfer parked in an arbitration nothing would grant could only be waited for.
`ARM2PRU_DMA_CANCEL` is that equivalent, in both firmwares. The PRU takes back a
transfer that has not started and refuses one that is on the bus, which ends
within a bus timeout per word anyway; the ARM reads which happened from
`mailbox.dma.cur_status` and there is no completion event either way, as with
the INTR cancel. Telling the two apart needed `DMA_STATE_ARBITRATING` to become
real — it was defined and never written, and a transfer that had merely been
requested was indistinguishable from one that had run to its end, both reading
`DMA_STATE_READY`. `DMA()` retires a request that outlasts its deadline on the
strength of that answer, and `requests_cancel_scheduled()` tries the cancel
before it resorts to marking an orphan.

Verified on ubx by the issue's own repro, before and after on the same rig. The
old binary: one 4 s wait, then `DMA slot 16 still holds a request of none: DATI
@ 001302 refused` and `/api/memory` silently answering `null` for good. The new
one: eight rounds, each pushed to the PRU (`cur_status=1`, the state that now
gets written), each retired, no refusals, and the whole wedge-and-recover cycle
three times over in one process — dark board, refused transfers, config apply,
`dc_on`, XXDP booting from DL0 — with the same PID and no restarts.

Testing turned up a **second road to the same wedge**, pre-existing and not
described in the issue: the orphan latch could be stranded, and a stranded latch
holds off every DMA there is. `requests_cancel_scheduled()` sets it when the PRU
will not give a transfer back, and it clears on that transfer's completion —
but a `cpu_access` completion raises no PRU interrupt and the adapter worker
passes over it, because the emulated processor's own thread collects it inside
its spin. When a power event cancels the request that thread is waiting on, the
thread leaves, and the completion has no owner. Seen exactly so: `dma
signaled=112 acked=111` and `DMA held off: the PRU still carries a cancelled
transfer` forever after. The processor thread now collects that last completion
before it goes, bounded, since the PRU refused the cancel precisely because the
transfer is on the bus. Counters stay in step afterwards (`137/137`), and fifteen
INITs interleaved with fifteen slot-16 transfers into a running XXDP left the
slot live.

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

### 2.5 A priority-slot collision wedges the PRU's interrupt arbitration — FIXED in 4d1622d and 5f39247
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
with DL11's vector; it goes through `set_default_bus_params()` now.

**The wedge is not in the PRU.** Counters in the mailbox (`GET /api/debug/pru`)
partition every pass of the PRU's main loop, and with them the wedge was
provoked on ubx and watched: `rl` and `DL11` both on slot 15 at BR4, the
`install()` refusal turned back into a warning for the run, XXDP booted and left
to work. What comes out is not a stopped PRU:

```
loop=35580  arb=33423  blocked=0      pending=False dr=222/222
loop=55690  arb=19477  blocked=34356  pending=True  dr=224/223
loop=83790  arb=0      blocked=83790  pending=True  dr=224/223
loop=82787  arb=0      blocked=82787  pending=True  dr=224/223
loop=69805  arb=8624   blocked=59924  pending=False dr=225/225
loop=35971  arb=30946  blocked=0      pending=False dr=225/225
```

The PRU loops throughout — faster while wedged, the blocked path being the short
one — and reaches its arbitration worker **not once** in the middle two samples.
It is held at `EVENT_IS_ACKED(mailbox, deviceregister)`, and `dr=224/223` says by
what: **one device register event the ARM has not acknowledged**. So
`sm_arb_worker_cpu()` is never called, `ifs_intr_arbitration_pending` is never
cleared, and the emulated processor spins out its 100 ms and reports the PRU
stopped or hung. The PRU is doing what it should; the arbitration statemachines
have nothing wrong with them.

The stall is ARM-side, in `qunibusadapter_c::worker()`. Its event block is
guarded by `res > 0` — the result of `pru_backend->wait_event()`, which returns 0
on a timeout — so **the worker examines the mailbox only when a new PRU interrupt
arrives, never on the poll timeout**. An interrupt that is coalesced away (the
remoteproc path reads a *count*, so several PRU signals collapse into one wake)
leaves its event standing in the mailbox with nothing to look at it, and the
worker sleeps in `poll()` up to 100 ms at a time while the PRU holds the bus
cycle open. It recovers when the next unrelated interrupt arrives and the loop
drains the backlog, which is why the wedge comes and goes rather than sticking.
The slot collision does not cause this; it churns interrupt bookkeeping hard
enough to hit the window often. Thread states agree: the adapter worker sits in
`S` while the event stands unacknowledged, and the CPU thread spins in `R`.

A slot shared at **different** levels was tested the same way and does not wedge:
the listing ran to 236 entries with `blocked` at zero throughout, so the refusal
added above does cover the reachable case.

**Fixed, and it was a device, not the engine.** The guess above — that the
worker's `res > 0` guard was losing a wake — was wrong, and testing it said so:
the change was built, deployed and made no difference at all. A backtrace of the
adapter's thread taken while the machine was wedged named the line instead:

    #3  __nanosleep64
    #4  timeout_c::wait_ms(unsigned int)
    #5  RL11_c::do_operation_incomplete(char const*)

`do_operation_incomplete()` waits out the 200 ms a drive takes to not answer, and
two of its callers were in `on_after_register_access()` — which runs on the
adapter's event thread, inside the register access, with the PRU holding the bus
cycle open until that thread returns. So the wait stopped the machine dead for
200 ms at a time: no bus cycles, and the arbitration worker that the PRU reaches
only between them not run either, which is what the processor was reporting. The
waiting is worker()'s now, on the device's own thread, the way every other long
command already does it — which is also the more faithful emulation, since a
real RL11 releases the bus and sets the error 200 ms later rather than freezing
the machine.

The slot collision only made it frequent. The reachable case needs no collision
at all: **Get Status on an empty drive slot**, which is how a program finds out
which drives a machine carries. Boot ROM polling a drive that is not there, no
collision anywhere, 30 seconds:

    before:  62 "arbitration pending for >100ms" errors
    after:    0

XXDP boots from DL0 and lists 191 files with none, and the counters stay at
`blocked=0`, `pending=false` throughout.

The `res > 0` guard is still there and still looks wrong — on a timeout the
worker deliberately ignores an event standing in the mailbox. Nothing observed
needs it, so it is left alone rather than changed on a hunch; worth its own look.

---

## 3. Performance — CPU cores and engine

The cores themselves (KA11, KD11-EA/KT11-D) are tight: table-free decode via
one big switch, the derived-page-descriptor MMU with the hot path inlined,
per-instruction trace gating cached (`cpu->tracing`). The measurement
infrastructure already in place (`cpu_access_profile_note`,
`event_latency_c`) is the right tool — the costs below are all *outside* the
cores, in the per-instruction engine round trips.


### 3.1 One PRU round trip per instruction for interrupt granting — FIXED in 5f6a1f0
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

Done as proposed. `sm_arb_worker_cpu()` publishes the BR4-7 request-line
state into a new mailbox byte, `ifs_intr_request_mask`, from the same latch
read it grants from; `unibone_grant_interrupts()` reads that one uncached
byte and takes the round trip only when a standing request could actually
be granted. One refinement beyond the proposal: the ARM also mirrors the
PRU's grant predicate against `ifs_priority_level`, so a standing request
the CPU's priority masks — an interrupt handler running at or above the
request's level, or the FETCHING window between vector and PSW fetch —
skips the round trip too, instead of paying it every instruction for a
grant the PRU would refuse. A skipped pass is therefore always one the PRU
would have refused: the mask is stale by at most one arbitration pass, and
only toward "fewer grants" — a request raised after the ARM's read is
granted at the next instruction boundary, exactly as one raised just after
the arbitration pass always was. `sm_arb_reset()` parks the mask at "all
four requesting", which keeps the ARM on the slow path until the first real
pass writes the truth. Both PRU trees carry the store, so the shared struct
means the same thing on either bus.

Measured on ubx (UniBone, emulated CPU20, non-PMI), A/B against the
unpatched binary on the same board and configuration:

| workload | before | after |
|---|---|---|
| branch-to-self loop | 117k instr/s | 871k instr/s (7.4×) |
| XXDP monitor idle | 92k instr/s | 290k instr/s (3.1×) |

PMI made no measurable difference on the tight loop either side, which
confirms the grant round trip — not memory access — was the per-instruction
ceiling. Interrupt delivery verified live: XXDP boots from RL and serves a
directory listing, and a DL11 transmit-interrupt storm (ISR writes XBUF and
counts into memory) sustains ~600 vectors/s through the new path,
exercising request → grant → vector → FETCHING → RTI continuously,
priority filter included. One measurement caveat for whoever repeats this:
the xxdp configuration autoboots and keeps the RL DMAing well past the
first prompt, so an "idle" `cycle_count` sample taken too early catches the
boot tail and reads several times low — wait for `/api/debug/pru` to show
the dma events near zero first.

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

## Review findings

A review of this branch against `main` (2026-08-08) confirmed the fixes above
do what they claim, and found 17 issues at the edges of the new mechanisms —
nine correctness (1-9), one efficiency (10), and seven cleanups (11-17), in
descending severity. **All seventeen are fixed**; each carries a note saying
how. Both buses build warning-free, the 36 MAINDEC diagnostic runs and the 508
host checks pass. Verified sound along the way: the `/api/debug/pru`
NULL-`mailbox` deref is unreachable in both entry paths, the init-pulse replay
cannot fabricate a reset without a real INIT edge, a refused CPU enable cannot
leave the PRU arbitrating for a missing CPU (CPUs carry no registers), and
concurrent `probe_range()` is fenced off by `operations_mutex`.

### 1. A device DMA scheduled into the orphan window wedges the CPU spin loop
`qunibusadapter.cpp:990`: the 2.1 fix latches `dma_orphan_on_pru` when the PRU
refuses to cancel a cpu_access transfer, and the CPU spin loop in `DMA()`
collects the orphan — but only in its `activereq == NULL` branch. If a device
thread schedules a DMA in the window (`request_activate_lowest_slot()` makes
its request `prl->active` at 516 before `request_execute_active_on_PRU()`
early-returns on the latch at 558-560), the spin sees an `activereq` that is
neither `NULL` nor itself, matches neither of its two branches, and spins
forever — even the 100 ms bail sits inside the `NULL` branch. Nobody else can
collect the orphan: the worker's dispatch guard (1788) skips cpu_access
events, and the PRU raises no interrupt for them
(`pru1_q/pru1_statemachine_dma.c:119-121`). Reaching it takes the
INIT-during-cpu-access race plus a device DMA in a microseconds-wide window,
but every link is constructible. Fix: a third spin branch (or a worker-side
collector) for the foreign-active case.

**Fixed.** The spin no longer reads "my access was cancelled" off `prl->active`
being NULL. It asks the access's own slot: `requests_cancel_scheduled()` empties
the slot of everything it force-completes, so a request that is neither active
nor in its slot was cancelled, and one that is in its slot while another device's
DMA is active is simply waiting its turn - a state to go on spinning in. Every
state the spin can be in now falls into a branch.

### 2. The 100 ms orphan bail leaves the latch set for the life of the process
`qunibusadapter.cpp:1013`: when the owed completion never arrives, the bail
exits with `completed = true` but `dma_orphan_on_pru` still set — and nothing
else ever clears it. The only clear site is
`worker_device_dma_chunk_complete_event()` (1455), reachable solely through a
dma-event dispatch that on this path can never fire; INIT and power events only
*set* the latch in `requests_cancel_scheduled()`, never reset it. So the
holdoff outlives the fault: even after the PRU recovers (say, a restart), every
`request_execute_active_on_PRU()` returns early, the next emulated-CPU access
becomes `prl->active`, is held off, and its spin satisfies neither branch — an
unbounded spin with no bail applicable. Only a service restart recovers. Fix:
reset the latch on INIT/power (the events that prove a new bus epoch), so the
wedge cannot outlast what caused it.

**Fixed**, both halves. The bail now applies wherever the latch stands, not only
in the cancelled branch, so an access held off behind an orphan is bounded too;
where the access is still in the tables the bail takes it out of them, the way
`dma_request_abandon()` does, so its slot does not stay occupied. And
`requests_cancel_scheduled()` clears a latch older than the same 100 ms bound:
INIT and power are where the bus starts over, and a holdoff must not outlive the
epoch that caused it. `dma_orphan_since_ns` is what distinguishes an orphan still
in flight - microseconds of bus cycle left - from one the PRU will never
complete, so the negate edge of an INIT pulse does not clear the latch its own
assert edge just set.

### 3. A refused `install()` never unwinds `on_before_install()`
`qunibusdevice.cpp:87`: the 2.5 refusal path runs `on_before_install()` at 80,
then on `install()` returning false restores only the four bus-placement
params (93-97) and returns — `on_before_uninstall()`/`uninstall()`/
`on_after_uninstall()` never run, so every override's acquisitions leak. An
`slu_c` whose slot now hard-collides has already opened its tty and made
`serialport`/`baudrate` readonly (`dl11w.cpp:167-176`): the disabled device
keeps the port open (blocking the external console bridge) with its params
stuck readonly. `dhv11_c` leaks its bound TCP listeners; on QBUS `vcb01_c`
leaves the X window open and `DDRMEM_RANGE_DEVICE` claimed (`vcb01.cpp:231`) —
a PRU-answered bus window with no controller behind it. (The CPU variant is
unreachable: CPUs carry no iopage registers and no valid slot, so no refusal
fires for them.) Made newly reachable by finding 4. Fix: run the uninstall
callbacks on the refusal path.

**Fixed.** The refusal path runs `on_before_uninstall()` and
`on_after_uninstall()` before it gives the bus parameters back. `uninstall()` is
not among them: `install()` refuses before it registers anything, so there is no
handle to give back and `unregister_device()` asserts on that.

### 4. Three factory-default device pairs now hard-collide on slot and level
`device_configuration.cpp:110`: DL11b defaults to slots 3,4 at BR4
(`SLU_SLOT`+2, XMT at slot+1) and DZV11 to slots 4,5 at level 4
(`DZV11_SLOT=4`) — `check_slot_collisions()` (`qunibusdevice.cpp:375-390`)
refuses same-slot-same-level, so enabling both defaults refuses the second
device. On QBUS the level-4 disk variants collide too: DHV11b (slots 14,15) vs
RLV12 (slot 15), DZV11d (slots 10,11) vs RKV11 (slot 10); the UNIBUS RK11/RL11
escape only because they sit at level 5, which is warn-only. A defaults-only
configuration can genuinely fail to bring up its second device. Fix: re-space
the shipped default slots.

**Fixed** by re-spacing the three defaults that overlapped at level 4: the DZV11
pool base 4 -> 5 (instances at 5..12, clear of DL11b at 3,4), the DHV11 pool base
12 -> 17 (17..20, the first run of four that no level-4 device claims), and the
QBUS RKV11 slot 10 -> 13, which is what opens the run 5..12 for the DZV11s. No
two shipped defaults arbitrate on one slot at one level now. Slots still shared
at *different* levels are left alone: there are more devices than slots, and that
sharing costs no schedule-table entry and is only reported.

### 5. A config apply silently swallows a refused device enable
`webconfigs.cpp:1614`: `dev->enabled.set(true)` returns void, the
`on_param_changed` false result is swallowed, and apply never re-checks
`enabled.value` — so `POST /api/configs/<name>/apply` answers `{ok:true,
errors:[]}` (and the journal logs "0 rejections") while the device stayed off
the bus. Parameter rejections elsewhere in the same apply do reach the errors
array (1579-1584), so the vehicle exists, and `webpower.cpp:283-294` shows the
working pattern: set, re-check `enabled.value`, report the refusal.

**Fixed.** The apply re-reads `enabled.value` after the set and reports the
refusal into the errors array, with `last_error` behind it when the device gave
a reason - the pattern `webpower.cpp` already uses. Verified against a running
machine: a configuration placing a second SLU on the console's own address now
answers `{"ok": false, "errors": ["DL11b: did not go on the bus: device DL11b
not installed: registration refused"]}` where it used to answer `{"ok": true,
"errors": []}`. Only the branch that applies to a *powered* machine is this
one's; a dark machine goes through `webpower_set_in_machine()`, which reports
refusals at power-on and is untouched here. The web UI's Load button always
darkens the machine first, so it takes that other branch - this string reaches
an API client, not that button.

### 6. RL11 `reset()` clears `operation_incomplete_reason` without the lock
`rl11.cpp:345`: every other accessor of the new field holds
`on_after_register_access_mutex`; `reset()` (adapter thread, on INIT) NULLs it
with no lock — a data race, and the INIT-cancels-the-pending-OPI intent can
lose it. If the device worker consumes the reason (1017-1019) before `reset()`
clears it, a stale 200 ms OPI error and interrupt fire on a freshly reset
controller; and once the 200 ms wait is in flight nothing rechecks
`init_asserted`, so INIT cannot stop the completion either. New exposure: on
`main` the Get Status OPI ran synchronously on the adapter thread, serialized
against INIT dispatch. Fix: clear under the mutex, recheck `init_asserted`
after the wait.

**Fixed.** `operation_incomplete_reason` is a `std::atomic<const char *>`, stored
by `reset()` and taken by `worker()` with an `exchange()`, so exactly one of them
ever has it. Taking `on_after_register_access_mutex` in `reset()` was not an
option: `worker()` holds it across a command, so the INIT would have waited out
the very timeout it is cancelling, on the adapter's event thread. The second half
- INIT cannot stop a wait already in flight - is a `reset_epoch` counter sampled
across the 200 ms wait; `reset()` runs on the *falling* edge of INIT, so
`init_asserted` is false again by the time it could be asked.

### 7. The immediate-seek path can still stall the event thread 200 ms
`rl11.cpp:494`: `on_after_register_access` checks `drive_ready_line` at 490
and calls `state_seek()` directly on the adapter's event thread; `state_seek()`
rechecks at 756 and its not-ready branch calls `do_operation_incomplete()` —
the same 200 ms whole-machine stall this branch fixed for Get Status, whose
comment (517-527) spells out why it is forbidden. `drive_ready_line` is
written by other threads (drive power switch, image unload), so it can flip in
the window. Narrow, but the consequence is the full stall; deferring this
branch to the worker too (`execute_function_delayed = true`) closes it.

**Fixed.** `state_seek()` takes a `deferrable` flag: from the register access it
refuses instead of seeking when the drive turned not-ready between the caller's
test and its own, and the caller hands the command to `worker()` like every other
delayed one. From `worker()` it is called with `deferrable = false` and waits the
OPI out on the device's own thread, as before.

### 8. `probe_range()`'s dead-bus bail-out applies only to the first cycle
`qunibus.cpp:599`: a DATI that consumes the whole 2000 ms bound proves nothing
grants the bus, but the check runs only when `cycles == 1`. A machine that
stops arbitrating mid-probe (operator powers down or halts during a MEM/MRV11
claim) pays the full bound at every remaining step: 32-64 steps at the default
64 KiB step is 1-2 minutes parked — holding `operations_mutex`, so every other
device operation waits too — plus DMA-timeout ERROR spam. Fix: time each cycle
and apply the same `!hit && took >= bound` early return on every step.

**Fixed.** Every step is timed against the same bound, not only the first, so a
machine that stops arbitrating mid-probe costs one more cycle instead of the rest
of the walk. The message says which address it stopped at and after how many
cycles. Code change only: `probe_range()` was not exercised on the board — see
the note below on what trying to reach it found instead.

### 9. `line_INIT` is inverted during the UNIBUS pulse-replay window
`qunibusadapter.cpp:1693`: the replay sets `line_INIT` to the opposite of the
real bus level for the whole `worker_init_event()` device sweep, and device
threads read it lock-free as the `DMA()`/`INTR()` admission gate (852, 1129) —
so a concurrent transfer is refused on a quiet bus, or admitted mid-INIT and
reclaimed by the second sweep. Low severity: the replay only follows a real
INIT pulse, so the refusal approximates what INIT would have done, and the
QBUS path (1714-1716) has used the identical idiom all along — but the window
is real and new on UNIBUS.

**Fixed** by separating the level from the gate. `line_INIT` still carries what
each half of the replay stands for - a device that resets on an edge needs the
negate - and a new `init_replay` flag stands for the whole replay.
`bus_init_active()` is the two together, and that is what `DMA()` and `INTR()`
now ask, so nothing is admitted to a bus that is being reset.

### 10. The PRU cancel busy-spins up to 200 ms under `requests_mutex`
`qunibusadapter.cpp:717`: `dma_request_abandon()` and
`requests_cancel_scheduled()` call `dma_cancel_on_pru()` →
`mailbox_execute(ARM2PRU_DMA_CANCEL)` — a no-yield busy-spin bounded at ~200 ms
— while holding `requests_mutex`. With a hung PRU, every DMA timeout burns a
core with the adapter's central lock held: the CPU per-access path, the CPU
blocking spin, and worker dispatch all stall behind it. Normal case is
microseconds and a hung PRU means the machine is down anyway, so low severity;
still, the cancel could be issued after dropping the mutex and re-validated,
or the spin could yield.

**Fixed** the second way the finding offers: `mailbox_execute_locked()` spins
plain for the first millisecond - a request the PRU is running takes
microseconds, and a spin is the cheapest way to see it end - and yields past
that. The normal path is untouched; a hung PRU no longer burns a core for the
rest of the ARM2PRU timeout with the adapter's central lock held. The lock
ordering itself is left alone: dropping and retaking `requests_mutex` around the
cancel would have to re-validate every table it walks.

### 11. The init-pulse ack catch-up loop is a single store in disguise
`qunibusadapter.cpp:1701`: `while (mailbox->events.init.acked !=
init_signaled) EVENT_ACK(*mailbox, init);` — `EVENT_ACK` is a bare
post-increment (`mailbox.h:257`), the UNIBUS PRU never reads `init.acked`, and
the only readers are this thread. Up to 255 uncached read-modify-writes of PRU
shared RAM, and wrap-around reasoning for the reader, to reach a value one
volatile store away: `mailbox->events.init.acked = init_signaled;`.

**Fixed**: `mailbox->events.init.acked = init_signaled;`

### 12. `register_device()` claims its slot before checking, forcing rollbacks
`qunibusadapter.cpp:180`: the device slot is claimed
(`devices[device_handle] = &device; device.handle = device_handle;`) before
the two refusal checks run, so both refusal paths carry a copy-pasted two-line
rollback. Neither check reads `devices[device_handle]`; checking first removes
both rollbacks — the same check-before-claiming ordering `install()` itself
now advertises. A third refusal path that forgets the undo pair leaves a
device that "looks installed" with a stale handle, tripping `install()`'s
assert — the abort class this branch exists to remove.

**Fixed.** The free handle is found first and claimed last, after both refusals;
the two copy-pasted rollbacks are gone, and a refusal added later cannot forget
one.

### 13. The PRU diag-counter code is duplicated across both bus trees
`pru1_q/pru1_main_qbus.c:139` and `pru1_u/pru1_main_unibus.c:125-149`: the
reset block (magic written last — load-bearing, per its own comment) and three
increment sites are character-for-character copies. `shared/mailbox.h` is the
established home for cross-tree mailbox code as macros (`EVENT_SIGNAL` et
al.); a `MAILBOX_DIAG_RESET`/`MAILBOX_DIAG_COUNT` there carries the logic
once. An edit to one main that reorders the magic write or adds a counter
leaves the other bus silently reporting fiction to the shared endpoint. (The
`ARM2PRU_DMA_CANCEL` blocks are legitimately bus-specific; not flagged.)

**Fixed.** `MAILBOX_DIAG_RESET()` and `MAILBOX_DIAG_COUNT()` live in
`shared/mailbox.h` beside `EVENT_SIGNAL` and the rest; both mains call them.

### 14. `/api/debug/pru` sleeps 50 ms before checking availability
`webserver.cpp:209`: the handler reads `mailbox->diag.magic` only after
sampling twice around a 50 ms `usleep` — on firmware without the counters,
every request holds a civetweb worker 50 ms and does six uncached PRU-RAM
reads only to discard them. Reading magic first returns immediately and merges
the two adjacent availability blocks into one if/else. It also hand-writes the
JSON/HTTP boilerplate that `webapi.cpp`'s `send_json()` (:72) already wraps —
the third verbatim copy of the header block in webserver.cpp. (Auth is not
affected: `begin_request_handler` runs before any URI handler.)

**Fixed.** The magic is read first, and the sampling happens only in the branch
that is going to report it, so firmware without the counters answers at once.
The hand-written header block is gone: `send_json()` is now `web_send_json()`,
declared in `webserver.hpp` and shared.

### 15. `uncompress_gz()` is the fourth hand-rolled fork/exec block
`storageimage.cpp:92`: `webstorage.cpp:805`, `websystem.cpp:85` and
`webupdate.cpp:227` each already carry a private fork/dup2/execlp/waitpid
block, with subtly different EINTR handling (webstorage's `waitpid` does not
retry on EINTR; the others do). None is exported, so there was no helper to
call — but this was the moment to hoist one (in 10.01_base utils, where
10.02_devices can depend without reaching into 10.05_web) rather than add
copy four.

**Fixed.** `subprocess_run()` in `10.01_base/2_src/arm/utils.cpp` runs a program
with an argument vector, optionally handing it a descriptor as stdout or
collecting its output, and retries the wait on EINTR. All four callers use it:
`storageimage.cpp`, `webstorage.cpp`, `websystem.cpp` and `webupdate.cpp` - the
last of which drops its two hand-written absolute paths for a PATH lookup, which
is what they were doing anyway.

### 16. `orphan_wait_ns` / `ORPHAN_WAIT_NS` are twins meaning different things
`qunibusadapter.cpp:953`: a start timestamp (with 0 as "not waiting") beside a
duration, differing only in case, hand-rolling the one-shot elapsed check that
`timeout_c::start`/`reached()` packages. An edit that mixes the twins compiles
cleanly and silently breaks the 100 ms bound — in the one path that only runs
during a rare cancelled-transfer race. Storing a deadline once collapses the
ladder and names what the variable holds.

**Fixed.** One `orphan_deadline_ns`, set once to now + the bound and compared
against; the bound itself is a file-scope `dma_orphan_wait_ns` shared with
`requests_cancel_scheduled()`, which needs the same number for finding 2.

### 17. New `sprintf` where CLAUDE.md asks for bounded `snprintf`
`qunibusdevice.cpp:474`: the edited `get_qunibus_resource_info` adds a new
unbounded `sprintf(tmpbuff, ...)` (the `"%s%d/-"` branch) — while the same
diff uses `snprintf(..., sizeof ...)` correctly in `check_slot_collisions`.
It follows the seven pre-existing `sprintf` calls in the function, which
tempers severity, but touched lines are the ones the rule applies to. The
bare `0xffff` no-vector sentinel compared here also deserves a named constant
shared with its writer (`priorityrequest.cpp:102`).

**Fixed**, and the rest of the function with it: the pieces are assembled into a
`std::string` and copied into the static buffer with one bounded `snprintf`, so
neither `tmpbuff` nor `buffer` can be run past - the testcontroller's 31*4
interrupt requests were the case that could. The sentinel is
`intr_request_c::vector_none`, named where the constructor writes it.
### Found while verifying: `POST /api/memory/probe` hangs on a bus nobody arbitrates — issue #95

Not one of the seventeen, and **not caused by any of them** — the A/B below is
what says so. Recorded because it wedges the service and takes an operator's
board with it.

`memory_probe()` (`webapi.cpp:1066`) calls `qunibus->test_sizer()`, which issues
one blocking `DMA()` over the whole address space with no `timeout_ms` — the
default 0, which `DMA()` documents as "the unbounded wait, which is what a caller
that cannot make progress without the data wants". On a backplane where nothing
grants the bus, the PRU parks in NPR/NPG/SACK arbitration and the transfer never
ends, so the request never completes and nothing bounds the wait. It holds
`device_configuration_c::operations_mutex` for all of it, so every later device
operation queues behind it: `dc_on` answers `held_by: "validating configuration
for power on"` from then on, and only a service restart recovers. The API doc
already says to run the probe with the CPU halted; a *dark* machine is the case
it does not cover, and that is the state a range is probed in before a card is
placed.

Reproduced on a UniBone with nothing carried, twice, with a 65 s client bound:
the fixed binary hangs, and the branch tip **without** the seventeen fixes hangs
identically. So it is the pre-existing unbounded wait, not the `sched_yield()`
finding 10 added to the mailbox spin.

It is the same shape as finding 8 at a different call site: `probe_range()` now
bounds every cycle and bails on a dead bus, and `dma_request_abandon()` retires a
transfer that outlasted its deadline — but both only apply to a DMA that was
given a deadline, and `test_sizer()` gives none. Giving it one (and reporting the
dead bus, as `probe_range()` does) is the fix; it was left alone here because it
is outside the seventeen, and is tracked as issue #95.

**Fixed.** `test_sizer()` no longer hands the adapter one transfer to split: it
sweeps a chunk of `PRU_MAX_DMA_WORDCOUNT` at a time and bounds every chunk at
2 s, the bound and the reasoning `probe_range()` already uses. A chunk that
consumes the whole bound is a bus nobody granted — no address above it can
answer either — so the sweep logs that and returns 0 with `*no_grant` set;
a chunk that fails in microseconds is the bus timeout the sizer went looking
for, and its address is the answer. Every chunk is timed, not only the first,
because a machine can stop arbitrating mid-sweep. `POST /api/memory/probe`
turns `no_grant` into `504` with the wording `/api/memory/read` and
`/api/memory/write` already use, and leaves the last probe that did reach the
bus standing rather than filing a dark machine as one with no memory.

Verified on the UniBone, machine dark: the deployed build gave no answer in
25 s and left `dc_on` queued behind `operations_mutex` with the journal silent;
the fixed build answers `504` in 2.009 s, `dc_on` follows in 0.83 s, and a probe
of the machine once powered answers in 0.48 s. `dc_off` and probe again is the
same 504. The journal now names it, at `info`: "memory sizing stopped at 000000
after 2003 ms: nothing is arbitrating the bus, so nothing can answer", over the
adapter's `dma.cur_status=1` — the PRU parked in arbitration.

Pre-existing on `main`, so not counted above, but adjacent: the worker acks
the dma event outside `requests_mutex` (a rare double-dispatch window the new
spin body neither widens nor closes — the issue #94 caveat), and the blocking
DMA deadline uses CLOCK_REALTIME on a default-clock condvar, which a clock
step mis-times — worth a `CLOCK_MONOTONIC` condattr now that
`dma_request_abandon()` acts on the timeout.

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
