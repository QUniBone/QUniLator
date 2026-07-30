# The VAX booted about half the time

The investigation, kept for the record: the machine described in `vax-host.md`
— a VAX-11/780 core with a UDA50 answering on the bus — booted VMS 4.7 to a
DCL prompt on roughly half of its attempts. The cause was an interrupt granted
with a zero vector at batch entry, found below and fixed; the machine now
boots **20 of 20** trials, ten of them with the DEUNA enabled. The failures
came in two shapes that looked nothing alike, and both were this one bug.

Branch `worktree-vax-cpu`, rebased onto `main` at `93a92ab`, board
`unibone.huebner.org`.

## What the failures look like

**Corrupt data from the disk.** VMS gets its banner out, answers the date
prompt, and then a file it reads off the system disk is not what it should be:

    %SYSINIT-E- error opening or mapping F11BXQP, status = 0001848C
    %SYSINIT-E- error opening or mapping F11BXQP, status = 000186D4
    -IMGACT-F-IMG_SIZ, image header descriptor length is invalid
    -SYSTEM-W-FILESTRUCT, unsupported file structure level
    %CLI-F-SYNTAX, error parsing '     '
    %BOOT-F-Unable to locate BOOT file

The VMS status code differs from run to run, which is what a corrupt read looks
like: the code is being taken out of whatever came back. Earlier runs also
produced `SSRVEXCEPT` bug checks in `SYSINIT.EXE` and `LOGINOUT.EXE`, which is
the same thing seen from further away.

**A stop before the date prompt.** VMS prints its banner and nothing follows.
The copy-on-write overlay records **zero written blocks** for these runs, so the
machine did no disk I/O at all after the banner. This one arrives in runs of
three and four consecutive failures.

## Measurements

Each trial discards the overlay first, so every one starts from the same
pristine system disk. `./tools/vax-boot-trials.sh <n>` runs them and scores them.

| configuration | result |
|---|---|
| no DEUNA, before the map fix | 5 of 10 passed |
| no DEUNA, before the map fix | 5 of 6 passed |
| no DEUNA, with the map fix | 6 of 10 passed — the first six, then four stops |
| no DEUNA, with the map fix, straight after a service restart | 2 of 4 passed |
| no DEUNA, with the zero-vector fix | **10 of 10 passed** |
| with the DEUNA, with the zero-vector fix | **10 of 10 passed** |

## What has been ruled out

**The DEUNA is not the cause.** The investigation began there, because the
machine failed whenever the ethernet controller was enabled. It fails at the
same rate with the DEUNA disabled and absent from the configuration. The
DEUNA's two genuine defects were found and fixed on the way (below), but
neither was this.

**The system disk is not corrupt.** `vms47.dsk` on the board is byte-identical
to the master at `~/vax/rd54-vms47-clean.dsk` (md5 `a0da6e7b02ef…`), verified
after the failures, and a boot from it succeeds about half the time.

**The copy-on-write overlay is not the cause.** It was suspected, and one
apparent case against it was self-inflicted: deleting the overlay files while
the service still had the image open let the service write its in-memory bitmap
back on shutdown, leaving dirty bits pointing at an overlay that no longer
existed. Cleared properly — service stopped first, or through
`POST /api/images/<image>/overlay/discard` — the overlay boots and reverts
correctly. The failure rate is the same with `use_overlay` off.

**Not the disk geometry.** The RA81 the drive announces is larger than the
RD54 volume the image carries, which is the right way round. The image file
growing past its original size is the base being sparse, not writes going
astray.

## The one fix made so far

The adapter's map was a **snapshot**: `publish_unibus_map()` copied all 496
registers into the DDR the PRU translates through, and it did so on every CPU
bus cycle. A real DW780 translates from the live registers. So a map register
the processor wrote took effect only when the processor next touched a device —
and a device need not wait for that. Our MSCP controller finds a command by
polling the ring, so it can begin a transfer with no CPU bus cycle in between
and translate it through the entries the map held before.

Map writes now carry through as they are made: `simh_shim_map_changed()` is
called from the two places the core assigns `uba_map[]` (`vax780_uba.c` at the
register write and in the reset loop) and writes the single entry into the
shared array. The whole-map publish remains for building the machine. The
per-cycle copy of 2 KB is gone, which the processor gets back.

This is right on its own terms, and it did not close the gap.

## The stop before the date prompt: an interrupt granted with a zero vector

The processor of a stopped machine turned out to be running: 1.25 M
instructions a second with PC pinned in a tight kernel loop at IPL 0, zero bus
cycles, zero DMA. That is VMS's idle loop — the system is not hung, it is
waiting for an I/O completion that never arrived. (`PC`, `PSL` and
`cycle_count` were available all along, in the `statusparams` collection of
`GET /api/devices` rather than `params`, which is why earlier reads missed
them.)

The interrupt is lost at batch entry. `sim_instr()` opens with
`build_dib_tab()`, which zeroes the vector the shim planted in `int_vec` for
the bus, then `SET_IRQL` finds the request the bus thread raised between
batches, and the main loop dispatches it before a single event has run:
`get_vector()` reads the zeroed entry, `uba_get_ubvector()` clears the request
on the way, and `if (vec)` silently drops the zero. The request is consumed,
the vector never dispatched, the driver's ISR never runs, and VMS waits
forever for a completion that was granted into nothing.

The stage-3 restore scheduled one instruction into the batch —
`sim_activate (&shim_restore_unit, 1)` — which is one instruction too late:
the dispatch happens before the event queue is looked at. Every interrupt
raised while the processor was between batches ran this race, and the batch
gap is a few percent of wall time, which is why the machine booted at all and
why it failed by the third or fourth attempt rather than the first.

The fix is to reassert where the wipe happens: `build_dib_tab()` itself now
calls `simh_shim_bus_reassert()` after its `init_ubus_tab()`, so the vectors
and the dispatch are back before the entry sequence evaluates anything. The
per-batch restore unit is gone; the reassert at the top of
`sim_process_event()` stays for a device reconfiguring itself mid-batch.

The corruption shape was the same bug earlier in the boot: a completion lost
mid-SYSINIT leaves the driver to time out and recover, and what the recovery
reads is not what the interrupted sequence would have delivered. With the
reassert in `build_dib_tab()` both shapes are gone — twenty consecutive trials
pass, ten of them with the DEUNA enabled, where before the fix three or four
of every ten failed.

## Fixed along the way

**The DEUNA's transmit worker woke every 100 microseconds** at realtime
priority to look at a ring that only exists once a driver has started the
controller. On a board with one core that took 27% of it while the controller
sat stopped. Both workers now pick their wait from the controller's state: a
millisecond running, fifty stopped, which is what the DELQA has always waited.
Idle cost fell from 29% to 0.4%. Committed as `f538cee`.

**The DEUNA read the port control block back over the bus after every command**
to log it, using the shared `qunibus->dma_request` from its own worker thread —
a request that belongs to the CPU thread — while holding `state_mutex`. The
snapshots and both readers are gone. Committed as `7c33f16`.

**The DEUNA had no station address of its own.** Committed as `482be6c`.

## Tools

- `tools/vax-setup.sh` — builds the machine on the board: processor, UDA50,
  system disk, optionally `--deuna` and a `--scratch` volume. `--fresh`
  discards the overlays first. No saved config describes a VAX, so a service
  restart leaves every device off and the machine has to be put back.
- `tools/vax-boot.sh` — boots and logs in, driving the console.
- `tools/vax-boot-trials.sh <n>` — n independent trials, each from a pristine
  disk, scored pass or fail with the first VMS diagnostic.
- `tools/vax-console.mjs` — the console driver. `--restart` throws the start
  switch after the socket is open and the board has replayed the channel's
  history, and prints `=== machine restarted ===`. Everything above that line
  in a log belongs to an earlier boot; a reader that ignores this will diagnose
  the wrong run.
