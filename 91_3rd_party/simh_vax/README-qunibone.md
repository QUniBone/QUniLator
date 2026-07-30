# simh VAX-11/780, vendored

The VAX-11/780 simulator from Open SIMH, harvested so that a VAX host can drive
the UNIBUS device models of `10.02_devices/2_src`. See `docs/vax-unibus-plan.md`
for what the host is for.

## Provenance

| | |
|---|---|
| Upstream | https://github.com/open-simh/simh |
| Commit | `a1f57fa3738ed31148d31126ba1a7278ff845c6d` |
| Commit date | 2026-07-03 |
| Licence | MIT-style, `LICENSE.txt`, composes with the project's BSD-2-Clause |

The files below are byte-identical copies of that revision, in the directory
layout upstream uses. Upstream sources are not edited: the build in
`10.07_vax/2_src/makefile` carries the compiler flags and the documented warning
suppressions instead, so a re-harvest is a plain copy.

To refresh, clone upstream at the new revision and copy the same file list, then
update the commit and date above.

## What was taken

`sim/` — the simh core support layer, exactly the `SIM` list of the upstream
makefile plus the headers those files include: `scp.c`, `sim_console.c`,
`sim_fio.c`, `sim_timer.c`, `sim_sock.c`, `sim_tmxr.c`, `sim_ether.c`,
`sim_tape.c`, `sim_disk.c`, `sim_serial.c`, `sim_video.c`, `sim_imd.c`,
`sim_card.c`.

`VAX/` — the `VAX780` list of the upstream makefile: the CPU core
(`vax_cpu.c`, `vax_cpu1.c`), the instruction extensions (`vax_fpa.c` floating
point, `vax_cis.c` commercial instruction set, `vax_octa.c` octaword,
`vax_cmode.c` PDP-11 compatibility mode), the memory management unit
(`vax_mmu.c`), the symbolic and compatibility-mode disassemblers (`vax_sys.c`,
`vax_syscm.c`), and the 780-specific system files: `vax780_sbi.c`,
`vax780_mem.c`, `vax780_uba.c` (the DW780 UNIBUS adapter), `vax7x0_mba.c` (the
RH780 MASSBUS adapter), `vax780_stddev.c` (console, TU58, line clock),
`vax780_fload.c`, `vax_uw.c` and `vax780_syslist.c`. `vax_vmb_exe.h` carries the
VMB primary bootstrap the console loads.

Only the headers the 780 build reaches are here: `vax_defs.h`, `vax780_defs.h`,
`vaxmod_defs.h`, `vax_mmu.h` and `vax_watch.h`. `vax_defs.h` selects a
model-specific header per `-DVAX_*`; the branches for the other models refer to
files that were not taken, and `-DVAX_780` never compiles them.

`VAX/tests/` — DEC's own VAX processor diagnostics and the script that drives
them, as upstream carries them: `evkaa.exe`, the `VAX_MINIMUM_DIAGS.dsk` volume
holding EVKAB, EVKAC and EVKAD for the VAX Diagnostic Supervisor, the help files
under `diag780/`, and `vax-diag_test.ini`. `10.07_vax/3_tests/vax-diagnostics.sh`
runs them.

`PDP11/` — the UNIBUS peripherals the 780 shares with the PDP-11 simulator, the
`VAX780` list again: `pdp11_rq.c` (MSCP/UDA50), `pdp11_tq.c` (TMSCP/TU81),
`pdp11_rl.c`, `pdp11_rk.c`, `pdp11_rp.c`, `pdp11_tu.c`, `pdp11_hk.c`,
`pdp11_ry.c`, `pdp11_ts.c`, `pdp11_td.c`, `pdp11_tc.c`, `pdp11_dz.c`,
`pdp11_vh.c`, `pdp11_lp.c`, `pdp11_cr.c`, `pdp11_xu.c` (DEUNA/DELUA),
`pdp11_dmc.c`, `pdp11_dup.c`, `pdp11_ch.c` and `pdp11_io_lib.c`.

## What was left behind

The other VAX models (MicroVAX, VAXstation, 730, 750, 820, 860), their ROM
images and their workstation graphics devices; the SCSI layer; the slirp NAT
stack; the front-panel implementation, `sim_frontpanel.c`, whose header `scp.c`
still includes; and every non-VAX simulator in the upstream tree.
