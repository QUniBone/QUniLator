# Changes to the KD11-EA core (PDP-11/34)

Change log of this directory only, newest first. **Every change to these files is recorded here,
and defect fixes are recorded here only** — the repo-wide `CHANGES.md` in the root carries an entry
only when the core gains or loses functionality at feature level (a new CPU model, the MMU, an
FPU), never one per diagnostic made to pass.

Files: `kd11ea.c`/`kd11ea.h` (the CPU core), `kt11d.c`/`kt11d.h` (the memory management unit),
`11.h` (basic types, private to this directory). The ARM-side wrapper is `../cpu34.cpp`/`.hpp`, the
contract to the QUNIBUS adapter is `../cpu_bus_adapter.h`.


## Unreleased

### KT11-D: what MMR0 and MMR1 record, and what a write to the MMU's own registers does, found by FKTHB0

`FKTHB0`, the full memory management diagnostic and the broadest of the KT11-D tapes, reported three
errors on every pass and so never announced a clean one. Three defects, all in the bookkeeping the
MMU does *beside* the translation; the tape now runs clean and the suite is green again.

- **MMR0<6:1> only recorded aborts.** Bits 6-1 do not describe the last abort but the last relocated
  reference of any kind: the hardware keeps loading the mode and the page it was made in until an
  abort freezes them. Ours were written by `kt11d_abort()` alone, so between aborts they said
  nothing. The tape provokes an odd address trap — a CPU trap, not an MMU abort, so nothing is
  frozen — and then reads MMR0 through kernel page 7, expecting 000017: page 7, because reading MMR0
  *is* the most recent reference. It got 000001 and reported "SR0 OR SR2 CHANGED BY ODD ADDR.
  ERROR". `kt11d_relocate()` now loads the two fields on every reference it relocates, unless frozen;
  `kt11d_abort()` still writes them together with the abort bits, which are what does the freezing.
  Nothing changes while relocation is off, because the hot path returns before this.
- **A write to a KT11-D register set the W bit of the page it lies in.** The MMU registers are
  reached through kernel page 7 like anything else in the I/O page, and `kt11d_relocate()` set
  PDR<6> for every write it translated — so `MOV R0,@#177572`, a write to MMR0, marked KIPDR7 as
  written into ("WRITING SR0 SET W-BIT IN KIPDR7", 077506 for the expected 077406). But the KT11-D
  sits inside the KD11-EA: a reference to one of its registers is answered internally and never
  becomes a DATO on the page, so nothing was written into it. The new `kt11d_is_own_register()` says
  which physical addresses those are, and the W bit is now set only for a write which is not one of
  them. The same predicate is the gate in front of `lookup()` in `kt11d.c`, so the two decodes cannot
  drift apart — an address this rejects has no register behind it either.
- **MMR1 logged autoincrements of the PC.** MMR1 exists so that an abort handler can undo the
  register changes of the instruction it has to restart, and the PC is not its business: the aborted
  instruction is re-entered from MMR2 and from the PC on the stack, and backing it out here as well
  would move it twice. `kt11d_log_register()` logged R7 all the same, so an absolute-mode operand
  logged its own PC autoincrement — which the tape reads straight back with a `MOV @#177574,R0`,
  getting 000027 where it wants MMR1 empty ("SR1 DID NOT READ ALL ZEROS"). It now ignores R7. Index
  mode never reached it to begin with: `addrop()` advances the PC over the index word without
  logging.

`FKTHB0` neither rings the bell at the end of a pass nor halts on an error — it prints its errors and
then one `END PASS # n TOTAL ERRORS SINCE LAST REPORT n` line — so it needed a `pass-text` sidecar
like `FKAAC0` and `FKABD1`. The text reaches into the error count, because `END PASS` alone would
have passed the tape while it was still printing failures.

Verified: the full cputest run — 35 of 36 runs pass, 1 (`FKTGC0`) is skipped, none fail. Not run on
real hardware.

### KT11-D: maintenance mode, RESET and the modes which do not exist, found by FKTAA0

`FKTAA0`, the memory management logic tape, halted after 4522485 instructions. Three defects in the
KT11-D, again each uncovered by fixing the one before it; the tape now runs clean (15938177
instructions per pass). All three are KT11-D behaviour that was simply not implemented, not wrong
arithmetic.

- **Maintenance mode (MMR0<8>) did not exist.** MMR0<8> was not even writable, so with only that bit
  set relocation stayed off altogether and the tape's `CMP (R1),(R1)` — whose two reads must come
  from different physical addresses — compared a location with itself. Maintenance mode relocates
  the *destination* operand references of an instruction while relocation itself is off, so that a
  diagnostic can compare a relocated address against the unrelocated one the same instruction formed
  for its source; no software uses it. `kt11d_relocate()` now has this second way in, and `kd11ea.c`
  marks which references are the destination's: `set_dest_ref()`, set by `readop()`/`writedest()`
  and the new `addrdest()` for the single-operand instructions, cleared at the start of an
  instruction, on entry to the trap sequence and by `PUSH`.

  What counts is narrower than "every reference the destination makes": the words that *form* the
  address — an index word out of the instruction stream, the pointer word of a deferred mode — are
  not relocated, only the access to the operand itself. The tape settles it in both directions, with
  a `CMP #x, @#y` whose `y` must be read unrelocated for the destination reference to reach `y` at
  all, and a `CMPB #x, @#y` which then expects the relocated `y`.
- **RESET left the MMU registers alone.** The comment here said bus INIT does not reach the memory
  management; the tape says otherwise, setting MMR0<8>, executing `RESET` and expecting the bit to
  read back as zero (`FKTHB0` tests the same as "SR0 OR SR2 WERE NOT RESET BY A RESET"). INIT clears
  MMR0..MMR2 — which switches relocation off and releases an abort freeze — but not the address map,
  so an OS executing `RESET` still keeps its PAR/PDR pairs. That is the new `kt11d_init()`, called
  from `kd11ea_reset()`; `kd11ea_power_reset()` continues to clear everything through
  `kt11d_reset()`.
- **PSW<15:14> = 01 or 10 behaved like user mode.** The KD11-EA has kernel and user and nothing else
  — 01 is the supervisor mode of the 11/45 — and a memory reference made in one of them aborts
  whatever it addresses. The MMU only ever tracked the derived address space, so the two modes it
  does not have were indistinguishable from user. It now keeps `mode`/`prev_mode`/`access_mode`
  beside the spaces, aborts through the new `kt11d_abort_mode()`, and MMR0<6:5> reports the mode the
  reference was actually made in instead of a kernel/user bit. The tape sets PSW to 040000 and
  expects MMR0 to read 100040; `FKTHB0` tests it as "ILLEGAL MODE 01 NOT ABORTED". The three
  `SPACE()` sites in `kd11ea.c` became `SPACE_KERNEL`/`SPACE_PREV`/`SPACE_RESTORE`, which set the
  space and the mode together.

Outside this directory, one dependency was missing from `10.05_cputest/2_src/makefile`:
`testcore_cpu34.o` holds a `KD11EA` by value, which holds a `KT11D`, but did not depend on
`kt11d.h`. Growing the MMU struct then linked a binary whose objects disagreed about its size and
corrupted the heap at run time rather than failing to build.

Verified: cross-compile plus the full cputest run — 34 of 36 runs pass, only `FKTHB0` is left, 1 is
skipped. Not run on real hardware.

### MMU aborts: three defects found by FKTFA0

`FKTFA0`, the MMU abort tape, halted after 332 instructions. Three defects, each uncovered by fixing
the one before it; the tape now runs clean (2931203 instructions per pass). All three are about what
an aborted reference must *not* leave behind — the machine has to look as if the aborted instruction
never started, so that the handler can restart it.

- **A pop was not backed out when its read aborted.** `POP` in `kd11ea.c` was a bare `SP += 2`, so an
  `RTI` whose first pop aborted left `SP` two higher, and the tape — which `RTI`s into a user stack
  on a non-resident page and then reads the user `SP` back with `MFPI SP` — saw 040102 instead of
  040100. A pop is an autoincrement of `SP` like any other, so `POP` now registers it with
  `autoinc_pending()`, the machinery added for `TSTB (R0)+` in the FKABD1 round below, which the
  `dati()`/`dato()` abort paths already undo. Every `POP` is immediately followed by the read that
  commits or backs it out, so `RTS`, `MARK` and `MTPI` are covered by the same change.
- **MMR2 was updated by an instruction fetch that aborted.** `kt11d_instruction_start()` stored the
  PC before the fetch, so running off the end of a page left MMR2 addressing the instruction that
  could *not* be fetched. MMR2 "is loaded with the 16-bit virtual address at the beginning of each
  instruction fetch, but is not updated if the instruction fetch is aborted" (PDP-11 Architecture
  Handbook): the tape plants a `SOB` in the last word of a page and expects MMR2 to hold *its*
  address, 016676, not the 016700 it could not reach. Setting MMR2 is now the separate
  `kt11d_instruction_fetched()`, called once the fetch has succeeded; `kt11d_instruction_start()`
  keeps clearing MMR1, which is right either way — an instruction that never started changed no
  registers.
- **An aborted instruction loaded its condition codes.** The codes are loaded when an instruction
  completes, so the PSW pushed by the abort trap must hold the ones from before it. The tape does
  `SEC` and then an `ADC` whose destination is on a read-only page: our `ADC` computed 0 + C, cleared
  C as it went and pushed 000000 where the tape expects 000001. The `be:` path now restores PSW<3:0>
  from the value saved at instruction start. The trap sequence is excluded by the new `in_trap`
  flag — it is not an instruction, and an abort inside one must leave the handler's freshly loaded
  PSW alone.

`FKTBA0` and `FKAAC0` still pass but take a different number of instructions per pass than before
(4573156 and 10888), since they too run code down these paths.

Verified: cross-compile plus the full cputest run — 33 of 36 runs pass, `FKTAA0` and `FKTHB0`
unchanged, 1 skipped. Not run on real hardware.

### RESET no longer re-initializes the interrupt mutex

`kd11ea_reset()` ran on every RESET opcode (i.e. at every OS boot) and did
`cpu->mutex = PTHREAD_MUTEX_INITIALIZER` — overwriting a mutex the qunibusadapter worker thread
may hold, or be blocked on, inside `kd11ea_setintr()`. That is undefined behaviour; the plausible
symptom is a machine that silently stops taking interrupts after a reboot. The mutex is now
initialized exactly once by the new `kd11ea_init()`, called when the owning `cpu34_c` (or the
cputest harness core) is created, and `kd11ea_reset()` clears `external_intr` under the lock
instead. Same fix applied in tandem to `../cpu20/ka11.c`, where the bug was inherited from.

Verified: cross-compile plus the full cputest run (33 passing runs unchanged, the three open KT11-D
tapes unchanged). Not run on real hardware — the race needs a device interrupt racing a RESET, which
the single-threaded cputest harness cannot produce.

### Four defects found by code review, none decided by a tape

All in `kd11ea.c`, from a full-tree review; the XXDP suite passes before and after (`FKACA0`, the
EIS tape, exercises all four instructions but happens not to pin any of these cases down).

- **ASHC with shift count -32 was undefined behaviour.** The right-shift path computes a positive
  count 1..32, and 32 reached `val >>= sh` on a `uint32_t` — undefined in C, and actually different
  on x86 (value unchanged) and ARM, so the cputest harness and the BeagleBone could disagree. The
  count-32 case is now explicit: all sign bits, C from the sign. The `1 << (sh - 1)` carry test on
  the remaining path was also UB at 32 (`1 << 31` on a signed int) and is `1u` now.
- **ASH left by 17 or more set V only for an initially negative operand.** The hardware shifts
  iteratively and sets V on a sign change at *any* step, and shifting ≥ 17 drives every bit of a
  nonzero operand through the sign position — so any nonzero operand overflows, e.g.
  `ASH #21, R0` with `R0 = 1` passes through 0100000 on its way to 0. V is now set for any nonzero
  operand.
- **MTPS in user mode could raise the priority.** PSW<7:5> was loaded from the operand regardless of
  mode, letting a user program lock out interrupts. It now keeps its value outside kernel mode, the
  same restriction RTI/RTT already had. This closes the "left alone deliberately" note under the
  KT11-D entry below.
- **DIV with an odd register trapped as a reserved instruction.** The hardware does not trap; the
  register pair select simply wraps so both halves are the same register, as `ASHC` already modelled
  (`reg | 1`). DIV now does the same. The result for an odd register is "unpredictable" on hardware,
  so nothing depends on which half wins.

Verified: cross-compile plus the full cputest run (33 passing runs unchanged, the three open KT11-D
tapes unchanged). Not run on real hardware.

### Traps: three defects found by FKABD1

`FKABD1`, the 11/34 trap test, lost control after 3480 instructions and ended in a double bus error.
Three defects in `kd11ea.c`, each uncovered by fixing the one before it; the tape now runs clean
(319027 instructions per pass).

- **The trap sequence did not end at an instruction boundary.** After pushing PS and PC and loading
  the vector, `step()` returned instead of arbitrating again, so a trap or interrupt which became
  pending *during* the trap sequence was only taken after the handler's first instruction. That is
  wrong for the kernel stack limit above all, since it is the trap sequence's own pushes which
  violate it: the tape traps into a handler with an illegal `JSR`, expects `SP` two words lower on
  entry than the trap left it — the stack trap taken in between — and its handler's first
  instruction repoints vector 4 at a catcher, so one instruction of delay sent the stack trap to the
  wrong place and walked the stack down through 0 into the I/O page. `trap:` now falls through to
  `service:`, which is what the `// TODO: is this correct?` there asked about.
- **An autoincrement stood after the reference it addressed had aborted.** `(R0)+` computes the
  address from the old register and writes the new one back, but on the KD11-EA that write-back does
  not survive a bus timeout or an MMU abort of the cycle it addresses. The tape finds the first
  nonexistent address with `TSTB (R0)+`, records `R0` in its trap 4 handler, and then aborts on the
  same address with `TSTB -(R0)` from one above it and re-executes that — which only lines up if the
  increment was backed out and the decrement, which is part of forming the address, was not.
  `addrop()` now hands the pending increment to `dati()`/`dato()`, which commit it when the cycle
  completes and undo it when it does not. `MMR1` is left recording the change, frozen as the
  hardware freezes it. Not carried over to `cpu20/ka11.c`: no tape decides it there.
- **`MOV` dropped a failed destination write.** Alone among the instructions which write memory it
  ignored the result of `writedest()`, so a bus timeout or MMU abort on the destination of a `MOV`
  silently did nothing instead of trapping. The tape catches it with a `MOV` to a read-only page.

Also in this tape's way: the `printf()` for an unimplemented 0700xx instruction, which `FKABD1`
sweeps to check that the whole group traps as reserved — 200 lines of debug output per pass. It is a
`trace()` now.

### The basic instruction set: five defects found by FKAAC0

`FKAAC0`, the 11/34 basic instruction test, halted after 2206 instructions. Fixing what it reported
uncovered the next defect each time, five in a row, all in `kd11ea.c`. The tape now runs clean.

- **Operand evaluation order.** The `RD_B`/`RD_U` macros evaluated *memory* operands first and
  *register* operands afterwards, so `MOV R0,(R0)+` fetched the source after the destination had
  already autoincremented that same register: it stored 000002 where the tape expects 000000. The
  order is now source strictly before destination. What the old order was working around is that
  `addrop()` overwrites `cpu->ba`, which after the destination has been evaluated must still address
  it for `writedest()`; a register operand therefore goes through `fetchop()` alone, never
  `readop()`, and never touches `ba`. Also the order in which `MMR1` records the two register
  changes, which is what an abort handler undoes.
  This one is **not** to be copied into `cpu20/ka11.c`, where the same macros read the same way:
  whether a register source sees the destination's autoincrement/autodecrement is a documented
  family difference. *PDP-11 Architecture Handbook* (1983), appendix B "PDP-11 Family Differences":
  the 23/24, 15/20, 25/40, 60, J-11 and T-11 modify the register before using it as the source, the
  04, 05/10, 34, 44, 45, 70, LSI-11 and VAX do not. The KD11-EA belongs to the second group - which
  is what `FKAAC0` tests - and the KA11 to the first, so the 11/20 core keeps its order.
- **`JMP`/`JSR` in mode 2.** Both jumped to `cpu->b`, which for autoincrement is the register value
  *after* the increment: `MOV #15536,R0` / `JMP (R0)+` landed at 015540 instead of 015536. They now
  use the effective address `cpu->ba`. The two are the only readers of the `b` field, and `b`
  differs from `ba` only in this one mode.
- **`SXT` (0067DD) was not implemented** and trapped as a reserved instruction — the word form of
  the opcode whose byte form is `MFPS`, just as `MARK` is the word form of `MTPS`. Destination gets
  -1 if N is set and 0 if it is clear; `setnz()` then leaves N as it was and sets Z exactly when N is
  clear, which is the rule, and C is not affected.
- **`MARK` (0064NN) was not implemented** either, same trap. `SP <- PC + 2*NN; PC <- R5;
  R5 <- (SP)+`, condition codes untouched.
- **The T bit was writable.** `MTPS` and a bus write to 777776 both set PSW<4>, so the tape got a
  trace trap through vector 14 where it expects none, and read its PSW back as 000377 instead of
  000357. Only `RTI`/`RTT` can set T; an explicit write now leaves it alone.

None of the five is carried over to `cpu20/ka11.c`; all five are 11/34 behaviour that the 11/20
either differs in or does not have. The operand order is the family difference above. The 11/20 has
neither `SXT` nor `MARK`. Its `JMP`/`JSR` keep reading `cpu->b`, the incremented register — that is
what the field was introduced for ("B register before BUT JSRJMP" in `ka11.h`), i.e. deliberate KA11
behaviour. And its PSW is written straight through in `ka11.c` `dato()`, with no MTPS to protect
against. The 13 `ZKA*` tapes decide none of it: they pass on both cores, and none of them ever
executes a double operand instruction with a register source and the same register as an
autoincrement/autodecrement destination (checked by instrumenting `step()` for all 13 runs).

### MFPS with a register destination no longer aborts the emulator

`FKTHB0` killed the process outright — `Assertion '0' failed` in `addrop()`, no failure line at all,
and on a BeagleBone the same path would abort `demo`. The instruction is `MFPS R1` at 020566, the
read-back half of the diagnostic's MTPS/MFPS walk of the priority field. `addrop()` computes an
address, so it starts at mode 1 and asserts on mode 0; every other direct caller guards that
(`MFPI`/`MTPI` branch to a register-mode path, `JSR`/`JMP` do `goto ill`, the `RD_U` macro is itself
`if(dm != 0) …`) and the MFPS case was the one written without a guard. `MFPS Rn` is a legal
KD11-EA instruction, not something that should trap.

`addrop()` is now called only for `dm != 0`, and three further defects on the same lines went with
it:

- The destination was written as a word (`by = 0`). MFPS writes one byte to memory, and sign extends
  PS<7> through the whole word into a register, like MOVB.
- `addrop()` was passed byte flag 0, so `(R0)+`/`-(R0)` stepped by 2 instead of 1.
- No condition codes were set at all. MFPS sets N from PS<7> and Z from the byte, clears V and
  leaves C.

### HALT and RESET are kernel-only

Both privileged instructions executed in any processor mode: a user-mode HALT stopped the machine
and a user-mode RESET pulsed bus INIT. On real hardware neither can happen — a user program must not
be able to stop the processor or initialize the bus. `FKTDA1` tests exactly this and died on it
after 58 instructions.

HALT outside kernel mode is a reserved instruction and traps through vector 10 (`goto ri`) instead
of halting; RESET outside kernel mode is a no-op, so `kd11ea_reset()`/`unibone_bus_init()` only run
in kernel mode. Not carried over to `cpu20/ka11.c`: an 11/20 has no processor modes.

### The PSW is addressable at 777776 again

Removing it with the KT11-D work below was wrong. `dati()`/`dato()` answered
`case 0777776: case 0777777:` with a bus timeout, which is inherited KA11 behaviour; the KD11-EA has
MFPS/MTPS *as well as* the PSW address, not instead of it, and 777776 is the only way to reach the
mode and priority bits. Six of the ten XXDP diagnostics died on their first access to it, after as
few as 3 instructions.

- `dati()` returns `cpu->psw` for 777776/777777. A byte read of the odd address gets PSW<15:8>, the
  caller shifting the half down as it does for any register.
- `dato()` routes the write through `kd11ea_set_psw()`, so a changed mode switches the stack pointer
  and the KT11-D address space and a changed PSW<7:5> reaches the arbitrator. A DATOB writes only
  the addressed half, using the same `mask` as the KT11-D registers.
- New `PSW_MASK` (0170377) is applied inside `kd11ea_set_psw()` rather than at the bus, so the bits
  an 11/34 has no flipflop for — PSW<11>, there being one register set only, and the unused <10:8> —
  can never be loaded from any source: 777776, MTPS, RTI/RTT or a trap vector.

The PSW is still not published as a QUNIBUS register of `cpu34_c`, so it remains visible to the
emulated CPU only, exactly like the KT11-D registers and like `cpu20_c`.

### KT11-D memory management

The core was a fork of the 11/20 KA11 and had no memory management, which limited the emulated 11/34
to 28K words and made RSX-11M, RT-11 XM and the KT11-D diagnostics impossible to run. It now has the
complete KT11-D, including the kernel/user processor modes which come with it.

**Design decisions worth knowing**

- The MMU registers are *not* published as QUNIBUS registers. On real hardware the KT11-D sits inside
  the KD11-EA and does not answer as a bus slave, so they are decoded by `kd11ea.c` `dati()`/`dato()`
  on the translated physical address, like the console switch register already was. `cpu34_c` keeps
  `register_count = 0`. This also keeps all MMU state in cached ARM memory, which matters: the
  translation runs on every CPU memory access, and a read of shared PRU RAM there would be uncached.
- Address translation is *not* offloaded to a PRU. The virtual address is produced in the ARM
  instruction loop, so a PRU would need a mailbox round trip (~1 µs) where the ARM needs ~20 ns; PRU1
  also has no timing slack, and the KT11-D has no UNIBUS map, so device DMA is never relocated. The
  PRU already receives the translated physical address for free through the existing mailbox field.
  For scale: one CPU memory access costs ~1 µs with PMI and ~10 µs over the real bus, so the
  translation is ~1-2 % of a PMI access and ~0.1-0.2 % of a bus cycle.

**New: `kt11d.c`, `kt11d.h`**

- MMR0..MMR2, kernel and user PAR/PDR blocks, 16 → 18 bit relocation with page length and access
  checks, aborts through vector 0250, MMR0<15:13> freeze of MMR0<6:1>/MMR1/MMR2, and the MMR1
  register-change log.
- `kt11d_relocate()` is inline in the header and works from a precomputed 16-entry `kt11d_page_t`
  array. One unsigned compare `(blk - blk_lo) > blk_span` covers both expansion directions; a `deny`
  bitmask covers the non-resident and read-only cases in one test. `kt11d_abort()` recomputes the
  exact cause from the raw PDR, so the bit semantics stay off the hot path.
- Coherency of the derived array: rebuilt only by `kt11d_rebuild()` from `kt11d_write_reg()` on a
  PAR/PDR write, and by `kt11d_rebuild_all()` on an MMR0 write and from `kt11d_reset()`. The W bit is
  written into the raw PDR only, which is why the raw `par[]`/`pdr[]` are kept alongside the cache -
  it never needs an invalidation.
- `kt11d_format()` renders the registers for the CPU state dump.

**Note on 11/34 versus 11/45.** Do not copy KT11-C behaviour into this file. The KT11-D has no MMR3,
no supervisor mode, no I/D separation, no memory management *traps* (only aborts) and no PDR A bit.
Its ACF is `PDR<2:1>`, two bits, not the 3-bit field of the KT11-C.

*Unverified against the manual*, isolated so they are a one-line fix in `kt11d_rebuild()` and the
`KT11D_*_WRITABLE` constants if `EK-KD11A-TM` disagrees: the exact ACF encoding, and the masking of
the PAR to 12 bits.

**Changed: `kd11ea.c`, `kd11ea.h`**

- `psw` widened from `byte` to `word` for the mode fields, second stack pointer in `stackpointer[]`,
  `trap_vector` so the bus-error path can trap through 4 or 0250.
- `ubxt()` replaced by `kt11d_relocate()`.
- New `kd11ea_set_psw()` is the single place which assigns the PSW: it switches the stack pointer on
  a mode change, tells the MMU which address space is current, and calls
  `unibone_prioritylevelchange()`. Instructions which only touch the condition codes still assign
  `cpu->psw` directly - going through the setter on every opcode would call into the arbitrator each
  time.
- CPU internal registers are decoded on the *physical* address now. With relocation on the IO page is
  reached through a PAR, so the old virtual `(ba & 0177400) == 0177400` test was wrong.
- The trap sequence was reordered: the vector is fetched in kernel space first, the new PSW gets its
  previous-mode field, and only then are the old PSW and PC pushed - onto the new mode's stack. The
  old order pushed onto the old stack, which is correct only for a single-mode 11/20.
- MFPI/MTPI (and MFPD/MTPD, identical here since there is no I/D space) and RTT implemented; they
  used to take the reserved instruction trap. RTI/RTT may not change the mode or priority fields in
  user mode, and RTI takes a pending trace trap immediately where RTT defers it by one instruction.
  These are really implemented on the 11/34 and even the D variants are decoded; these only got their
  new meaning in the pdp 11/45.
- The stack limit red zone applies to the kernel stack only.
- The RESET opcode no longer resets the memory management: bus INIT does not reach the KT11-D on real
  hardware, and an OS executing RESET must keep its address map. `kd11ea_reset()` is now the RESET
  opcode only; console START and power-up go through the new `kd11ea_power_reset()`, which also
  clears the MMU, the PSW and both stack pointers. `cpu34_c::core_reset()` calls the latter.
- Fixed: MTPS changed PSW<7:5> without telling the arbitrator, leaving
  `mailbox->arbitrator.ifs_priority_level` stale.
- Removed the PSW at 777776. The 11/34 has no bus-addressable PSW - that is why it has MFPS/MTPS - so
  an access there is a bus timeout now. Programs which wrote the CPU PSW through 777776 must use MTPS.
- `kd11ea_printstate()`/`kd11ea_tracestate()` show the 16 bit PSW, the processor mode, both stack
  pointers and the full MMU register set, since the MMU registers cannot be read over the bus.

**Left alone deliberately.** MTPS can still write the priority field and the T bit. Protection
semantics suggest user mode should not be able to, but this predates the MMU work and was not
confident enough to change; check against the manual.

Verified: cross-compile only (`./crossco -a`, UNIBUS, clean under `-Wall -Wextra -Wshadow`). Not yet
run on real hardware; the KT11-D diagnostics are the acceptance test still to do.

### EIS and MFPS/MTPS are native instructions

The 11/20 feature switches were removed from the KA11 core, so the conditionals around them here are
gone too and the instructions are always executed: EIS (MUL, DIV, ASH, ASHC, XOR, SOB, opcode group
0070000) and MTPS/MFPS (0006400/0006700). The byte bit test in MTPS/MFPS stays - without it
006400/006700 are MARK/SXT, not MTPS/MFPS. `KD11EA_SWAB_VBIT` remains compiled in: SWAB clears the
V bit.

### Directory created

`kd11ea.c`, `kd11ea.h`, `11.h` forked from `cpu20/ka11.c`, `ka11.h`, `11.h` (Angelo Papenhoff's
KA11). All externally visible symbols renamed `kd11ea_*` / `KD11EA` and internal helpers made
`static`, so both cores link into one binary. `cpu34/11.h` must never be included together with
`cpu20/11.h` in one compilation unit.

At that point the fork still executed the 11/20 instruction set, without memory management.


## Known gaps

- Console ODT is not emulated.
