# KW11-P emulation and the ZKWB diagnostic — findings

Status as of commit `c13de23` (`feat: emulate the KW11-P programmable real-time
clock`).

## TL;DR

- The KW11-P register/interrupt model (`10.02_devices/2_src/kw11p.{cpp,hpp}`) is
  faithful to **DEC-11-HPWB-D** and works for its purpose: it provides the clock
  the DRS supervisor and other diagnostics expect, and RL diagnostics run clean
  with it enabled.
- **DEC's own KW11-P test ZKWB (`ZKWBJ1.BIC`, banner `CZKWBJ KW11-P RT CLK TST`)
  does NOT pass on the Q-bus board — it HANGS** in the counter register
  self-tests.
- The hang is a **QBus bus-cycle timing limit** (the ARM-in-the-loop cannot
  sustain the diagnostic's back-to-back counter write/read cadence over 65536
  tight iterations), not a register-model logic bug. This is the same class of
  limitation seen with the DHV11 self-test timing.
- **Do not read "banner then silence" as a pass.** A real pass reprints the
  banner every pass (see below).

## The device

| Register | Addr (18-bit) | Addr (16-bit) | Notes |
|---|---|---|---|
| CSR | 772540 | 172540 | RUN/RATE/MODE/UPCOUNT/FIX/INTR_ENB/DONE/ERROR |
| BUF | 772542 | 172542 | count-set buffer, **write-only** (reads 0) |
| CTR | 772544 | 172544 | counter output |

Vector 104, BR6, arbitration slot 22. The counter value is derived from elapsed
wall-clock time; the underflow/overflow interrupt is scheduled at the computed
instant rather than by ticking the counter in software.

### Manual-faithful behaviours that matter (DEC-11-HPWB-D)

- **BUF is write-only.** On a DATI, data is driven onto the bus only from the
  counter or the CSR (§3.2); the buffer has no read drivers. So reading 772542
  returns 0. Writing 772542 loads **both** the buffer and the counter. (The
  diagnostic's register test writes BUF=177777 then reads it back expecting 0 —
  this is how you know BUF is write-only.)
- **BUS INIT clears the counter and buffer**, via STOP L (the logic around
  §ref lines 2113–2149), and clears RUN/RATE/MODE/UP-DN/DONE/ERROR (CSR bit
  table, §ref 1701–1783). So `reset()` zeroing the counter/buffer is correct.
- **FIX (bit 5)** single-steps the counter in the selected direction; write-only,
  reads 0.

## ZKWB (CZKWBJ) — how a pass is signalled

`ZKWBJ1.BIC` is a **standalone MAINDEC** diagnostic (not DRS-based), on
`xxdp25.rl02`. Run it from the XXDP monitor with `R ZKWBJ1`.

- It prints the banner once at start.
- **There is no "END OF PASS" string.** The only textual outputs are the banner,
  "RESTARTING AFTER A POWER FAILURE", and the error table header
  `PC STATUS COUNTER TEMP`.
- On a completed pass the dispatcher (the TRAP-0 handler at `6136`) wraps back to
  test 1 at address `1046`, which **reprints the banner** — the banner-skip
  compare `CMP @#42,@#46` at `1052` is never satisfied because nothing ever sets
  loc 42 = loc 46. **Therefore a real pass shows the banner repeating every few
  seconds.**
- An error prints the `PC STATUS COUNTER TEMP` column table.

Consequence: **banner-once-then-silence is a hang, not a pass.** (This was an
earlier mis-call — silence was wrongly read as success.)

### Switch register

`177570` is **NXM on this board**. CZKWBJ's own probe (routine `11652`:
`TST @177570` under a bus-error catcher) times out and it falls back to a
**software switch register at loc 176 = 0**. Verified: after a run, loc 1044 (the
SR pointer cell) reads 176. Because the software SR is 0, there is no
halt-at-end-of-pass and no pass typeout to enable, and SR options cannot be set
via 177570.

## Proof of the hang

Method: run the diagnostic, then sample the CPU PC — halt via
`POST /api/control {"action":"halt"}`, read `R7/` in micro-ODT over
`/ws/console/ext`, continue; repeat.

- The PC is **pinned** (e.g. 8/8 samples at `001430`), `loc1020` (the walk index)
  is frozen, and CTR reads `000000` while `loc1020` holds a large value.
- Two wedge sites, both "write the counter, read it straight back":
  - **BUF-load walk:** `001414 MOV loc1020,@BUF` / `001422 CMP loc1020,@CTR`
  - **FIX single-step walk:** `002250 MOV #40,@CSR` / `002256 CMP loc1020,@CTR`
- It walks `loc1020` through all 65536 values and wedges at a **different value
  every run** (observed 040112, 105267, 142162, 156301) — timing-dependent, not
  a fixed value.
- The base-class register trace shows the readback **tracking correctly**
  (CTR = 114701, 114702 = BUF) for tens of thousands of iterations, then a single
  readback lags → mismatch.
- The diagnostic does **not** re-write BUF on a mismatch; its error path issues
  RESET (which per the manual clears the counter), so CTR then reads 0
  permanently → a hard hang.

## Why it is bus timing, not a model bug

- The model sets CTR correctly on every write (verified in the register trace).
- The worker thread is idle during the walks (`running = 0`, zero worker updates
  to CTR); no spurious `reset()` or `fire_event()`.
- The register semantics match the manual (BUF write-only, INIT clears
  counter/buffer, single-step, vector 104).
- **Attempted fix that did not work:** making CTR `active_on_dati` so a read
  computes `current_counter()` synchronously in the DATI callback (arguably more
  faithful — the counter drives the bus at read time). It hung **identically**.
  That rules out the passive-readback-staleness theory and points at the
  ARM-in-the-loop serving of back-to-back active-register cycles at diagnostic
  speed (~140 µs/iteration × 65536). This experiment was reverted (it adds a
  round-trip per read and fixed nothing); the committed model keeps CTR passive.

**Honest caveat:** the single failing bus cycle was not caught red-handed. On the
BBB, journald lags minutes behind under any per-access logging and
`journalctl` reads routinely time out; writing a trace file with `fflush` per
line from inside the bus callback **crashed the service** (I/O in the RT bus
path). So the evidence strongly indicates a bus-cycle timing race and rules out
the obvious model bugs, but there is no single-cycle SSYN-window capture.

## Regression (clean)

- XXDP boots from DL0 with the KW11-P enabled.
- The RL controller diagnostic **CZRLG (`ZRLGE0`) reaches `EOP` with `0 TOTAL
  ERRS`** with the clock present. RLV12 params: bus 174400, vector 160, BR4,
  RL02. Answer the destructive-load prompt `DOIT ANYWAY? Y` (it writes a scratch
  area; the XXDP pack still boots afterward). CZRLG dialog order differs from
  CZRLH: `RLV12=3`, BUS ADDRESS, VECTOR, DRIVE, DRIVE TYPE (RL01 Y/N), BR LEVEL,
  CHANGE SW.

## Picking this up later

Open questions / next steps if someone wants ZKWB to actually pass:

1. Confirm the failing cycle at the PRU level (PRU-side instrumentation, or a
   logic capture), to turn "strongly indicated" into a proven SSYN-window
   measurement. Host-side (ARM) logging cannot see it and destabilises timing.
2. If it is confirmed as the active-register serving cadence, the fix would be at
   the QBone bus/PRU layer (how fast a passive register updated as a side-effect
   of an adjacent active DATO becomes visible to the next DATI), not in
   `kw11p.cpp`.
3. `active_on_dati` for CTR is available as a starting point but did not help on
   its own.

### Reverse-engineering the .BIC (reusable technique)

- `xxdp25.rl02` is an XXDP filesystem: files are singly-linked 512-byte blocks
  (word 0 of each block = next block number); the RAD50 directory entry gives the
  start block and length.
- `ZKWBJ1.BIC` is formatted-binary / absolute-loader records
  (`001,000,count,addr,data,checksum`). Rebuild a memory image from the records,
  then disassemble.
- Entry at `200` is `JMP @#1046`, then a JMP dispatch table. Tests reach the
  registers via PC-relative-deferred addressing through pointer cells
  loc 1000/1002/1004 (= 172540/542/544).
- The hang PC comes from the vector-4 trap frame on the stack (SP, SP+2 = pushed
  PC/PS) or directly from `R7/` after halting.

## Operational notes

- Run diagnostics at **verbosity ≤ 3**. Debug-level (5) per-access logging in the
  bus callback adds real latency and causes its own SSYN-timeout artifacts; it is
  not what makes or breaks a ZKWB pass, but keep it low.
- Board hygiene: default config `xxdp`, KW11-P enabled, rl0 = `xxdp25.rl02`,
  verbosity 3, external console on `ttyS2`.
