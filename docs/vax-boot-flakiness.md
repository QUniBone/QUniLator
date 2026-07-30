# The VAX boots about half the time

Where the investigation stands. The machine described in `vax-host.md` — a
VAX-11/780 core with a UDA50 answering on the bus — boots VMS 4.7 to a DCL
prompt on roughly half of its attempts. The other half fail in one of two
ways, and the two look nothing alike.

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

## Where to look next

The two failure shapes probably have two causes, and the second one — banner,
then nothing, no disk I/O, several in a row — is the one with the sharper edges.
Worth doing:

- Find out whether a failing machine's processor is still executing. `PC`,
  `PSL` and `cycle_count` are documented as `cpuvax` parameters but are not in
  what `GET /api/devices` returns, so there is currently no way to ask. Getting
  them back is the first step.
- Compare the board's log for a passing boot against a failing one at
  `verbosity 4`. A capture loop is in the session history; three failures were
  caught (6, 6 and 14 board lines) before a passing one was, so the comparison
  is not yet made. The failing ones end at `VAX running` with nothing after,
  and the UDA50 logs no controller activity at all.
- `boot device RQ0: not configured, would answer at 4004772150` appears on the
  failing boots. Whether it also appears on passing ones is unknown — it is
  probably just the core's own unused MSCP controller, but it has not been
  checked.
- The runs of consecutive failures suggest state that survives a machine
  teardown. A service restart did not clear it (2 passed, then 2 failed), so it
  is either in the PRU or it is not state at all.

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
