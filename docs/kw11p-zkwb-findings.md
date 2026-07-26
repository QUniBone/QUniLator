# KW11-P emulation and the ZKWB diagnostic — findings

Status as of commit `8c6db60` (`feat: emulate the KW11-P programmable real-time
clock`).

## TL;DR

- The KW11-P register/interrupt model (`10.02_devices/2_src/kw11p.{cpp,hpp}`) is
  faithful to **DEC-11-HPWB-D** and works for its purpose: it provides the clock
  the DRS supervisor and other diagnostics expect, and RL diagnostics run clean
  with it enabled.
- **DEC's own KW11-P test ZKWB (`ZKWBJ1.BIC`, banner `CZKWBJ KW11-P RT CLK TST`)
  does NOT pass on the Q-bus board — it HANGS** in the counter register
  self-tests.
- The hang is a **rare, timing-dependent defect that is most likely fixable** —
  not the fundamental cadence limit an earlier draft claimed (that conclusion is
  retracted, see below). The counter readback is correct for *tens of thousands*
  of iterations and then misses **once**, at a **different value each run**; a
  systematic ARM-callback-latency wall would miss on the first iteration, every
  time. Correct-almost-always-then-rarely-wrong is the signature of a cross-thread
  race on the CTR shadow, or the active-write callback occasionally exceeding the
  PRU's bus-stall budget — not a cadence wall.
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

## What the evidence indicates (the "bus-timing limit" conclusion was wrong)

An earlier draft closed this as an un-fixable QBus timing limit. That does not
survive its own evidence and is retracted:

- The readback is **correct for tens of thousands of iterations, then misses
  once**, at a **different counter value each run**. A systematic limit — the ARM
  callback for the BUF write not completing within the CPU's ~1 µs to the next
  read — would fail on iteration 1, deterministically, not one time in tens of
  thousands. **Correct-almost-always with a rare, non-deterministic miss is a
  race or a budget overrun, not a cadence wall.**
- That the write's effect IS normally visible to the immediately-following read
  means QBone already makes an active DATO's side-effect land before the next
  cycle (the bus is effectively held for the callback). The open question is only
  why it *occasionally* does not.
- "Making CTR `active_on_dati` did not help" is consistent with this: if the miss
  is a rare race on the shared CTR shadow (or a callback overrun), changing the
  read side alone would not fix it. It is **not** evidence of a fundamental limit.

Two concrete, testable candidates for the rare miss:

1. **A cross-thread race on the CTR shadow / counter state.** `kw11p_c` has a
   `state_mutex` "to serialize state between the bus-access callback and the
   worker," plus `counter_frozen`/`run_start_ns`/`running` and a passive `reg_ctr`
   shadow written via `set_register_dati_value`. If any path that writes the CTR
   shadow or the counter state runs without the mutex, or the worker touches the
   CTR shadow while stopped, or the shadow store is not ordered before the PRU
   serves it, a rare interleaving yields a stale served value.
2. **The active-write callback occasionally exceeds the PRU's bus-stall budget.**
   If the PRU holds a DATO only up to a maximum wait before releasing it (so a
   slow ARM cannot stall the bus forever), a BUF-write callback that occasionally
   overruns that budget (scheduling jitter, contention, a heavier path at some
   counter values) lets the DATO complete before the CTR shadow is updated → the
   next DATI serves stale. Mitigable by making the shadow update the earliest,
   cheapest action on the write path.

Once a single mismatch slips through, the diagnostic issues RESET (which clears
the counter), so CTR then reads 0 permanently — turning one rare miss into the
hard hang that is observed.

**Caveat still standing:** the single failing cycle was not caught red-handed.
Host-side per-access logging lags minutes and destabilises timing, and an
`fflush`-per-line trace from inside the RT bus callback crashed the service. So
the *mechanism* above is inferred from the failure signature, not yet directly
observed — catching it is step 1 of the plan below.

## Regression (clean)

- XXDP boots from DL0 with the KW11-P enabled.
- The RL controller diagnostic **CZRLG (`ZRLGE0`) reaches `EOP` with `0 TOTAL
  ERRS`** with the clock present. RLV12 params: bus 174400, vector 160, BR4,
  RL02. Answer the destructive-load prompt `DOIT ANYWAY? Y` (it writes a scratch
  area; the XXDP pack still boots afterward). CZRLG dialog order differs from
  CZRLH: `RLV12=3`, BUS ADDRESS, VECTOR, DRIVE, DRIVE TYPE (RL01 Y/N), BR LEVEL,
  CHANGE SW.

## Root-cause plan (pick up here)

Goal: turn the rare miss from "inferred" into "caught", identify which candidate
it is, fix it, and get CZKWBJ to a real reported pass — the banner reprinting
every pass (see "how a pass is signalled"), not printing once and wedging.

1. **Catch the miss without perturbing timing.** Do NOT log per access (that
   crashed the service and adds latency). Arm a **one-shot, in-memory** capture
   that fires only on the FIRST mismatch: in the CTR read/compare path, when the
   value about to be served differs from the last value written to the counter,
   snapshot into a few static variables or a small ring buffer — value written,
   value served, `running`, `counter_frozen`, the acting thread id, and two
   `clock_gettime(CLOCK_MONOTONIC)` stamps (write-callback entry/exit and read
   time) — then stop capturing. Dump it after the run (a debug endpoint, or the
   devices-menu `m e` memory peek). This distinguishes a **stale served value**
   (race, candidate 1) from a **late write callback** (budget overrun,
   candidate 2).
2. **Audit `state_mutex` coverage (candidate 1).** In `kw11p.cpp`, list every site
   that reads or writes `counter_frozen`, `running`, `run_start_ns`, and the
   CTR/CSR/BUF shadows; confirm each holds `state_mutex`. Specifically: does the
   worker ever call `set_register_dati_value(reg_ctr, …)` while `running == 0`
   (racing the BUF-write callback)? Is the shadow store ordered before the PRU can
   publish it? Fix any unguarded path or missing barrier.
3. **Measure the write-callback budget (candidate 2).** From the step-1 stamps,
   compare the BUF-write callback duration against the PRU's DATO stall/timeout
   bound (find it in the qunibusadapter / PRU layer). If overruns correlate with
   the miss, minimise the write path: publish the CTR shadow as the first, cheapest
   action on a BUF/FIX write (one store under the lock), deferring heavier work
   (overflow (re)scheduling, event fires) until after the shadow is published.
4. **Re-validate.** Rebuild + deploy (`crossbuild` → scp `qbone-web` → `sudo
   install -m755 … /usr/bin/qbone`; `crossbuild -d` does NOT install), boot XXDP,
   run `R ZKWBJ1`, and confirm the **banner reprints every pass**. Re-check the RL
   regression (CZRLG clean, still boots).
5. **Only if 1–3 disprove both candidates** — the miss is genuinely the PRU
   releasing the DATO before any achievable callback can publish the shadow — is
   this a real bus-layer limit, and the fix moves to the qunibusadapter/PRU
   (guaranteeing an active DATO's shadow side-effect is visible to the next DATI).
   Prove that with the step-1 data before concluding it; do not assume it (an
   earlier draft did, and was wrong).

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
