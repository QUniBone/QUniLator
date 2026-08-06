# DELQA emulation vs. DEC's CZQNA diagnostic — work plan

State as of 2026-07-19: **CZQNA passes with no errors**, with a loopback
connector installed and the external loopback tests enabled. Every test the
machine can run does: 1 through 14, with 15 skipped for want of a CPU
strapped to power-up mode 0.

Bridged to a live LAN four errors remain, and all four are wire frames
arriving mid-test: the failing descriptor holds `000370`, the reserved-bit
pattern of a normal received packet, in a list the diagnostic expected
untouched. Real hardware on a live segment behaves the same way, so run
CZQNA with the connector installed and treat the bridged count as a
measure of the network, not the emulation.

The count is measured with the DELQA bridged to a live LAN and the external
loopback question answered N. That matters: with the wire quiet (`ip link
set veth-br down`) the same build reports 8, test 7's transmit timeout
clears and tests 5 and 6 fail differently. State the wire condition
alongside any count.

## Running the loopback tests

Tests 13 and 14 want a loopback connector or an H4000 on the transceiver
port, and skip without one. The `loopback` parameter installs one:

    curl -X PUT http://qbone/api/devices/delqa/params/loopback -d '{"value":"1"}'

Then answer **Y** to "Test in External Loop Back Mode". Both tests pass. The
connector also disconnects the network, so a run with it installed is the
isolated environment the diagnostic expects, and is the one to debug in.

Two configurations worth keeping apart:

| configuration | errors | notes |
|---|---|---|
| no connector, external loopback N | 5 | tests 13-15 skip |
| connector installed, external loopback Y | 6 | 5, 13 and 14 pass, 15 skips |

Test 15 needs the CPU strapped for power-up mode 0 (J18-J19 and J18-J17
removed) and stays skipped.

Reference: DELQA User's Guide EK-DELQA-UG-002, OCR'd to text in
`diagnostics/reference/`. Grep that rather than paging through the scan.
Section 3 holds the programming model.

Two places the manual contradicts itself or the hardware, both settled by
measurement: section 3.6.5 says internal loopback is selected by *setting*
CSR08, while the CSR08/CSR09 table in 3.3.2.1 and the boot sequence in 3.6.1
both say *clearing* it, which is what the hardware does. And 3.6.5 says CSR00
must be clear for all loopback modes, while CZQNA runs its loopbacks with the
receiver enabled (`csr=010221`).

## Reproducing a run

The XXDP RL02 images live only in git history, removed by `a11c388`:

    git show a11c388^:10.03_app_demo/5_applications/xxdp.rl02/xxdp25.rl02.gz

Only `xxdp25.rl02` carries LQA support (`ZQNAJ0.BIC` = CZQNA
DEQNA-DELQA-DESQA functional test). The saved `XXDP` configuration boots it
from DL0.

Drive the console over websocat, not the browser — the web console wedges
under this load. With `pdpsession.sh` (see the memory note
`delqa-xxdp-czqna`):

    pdpsession.sh "sleep:32" "B DL0" "sleep:25" "R ZQNAJ0" "sleep:14" \
      "STA" "sleep:5" "Y" "sleep:4" "1" "sleep:4" "" "sleep:3" "" "sleep:3" \
      "N" "sleep:3" "N" "sleep:3" "Y" "sleep:6" "N" "sleep:85"

Answering, in order: change HW yes, 1 unit, CSR 174440 (default), vector 300
(default), no sanity timer (it resets an unstrapped CPU), no external
loopback, change SW **yes**, 4 Meg of memory **no**.

That last answer matters: the machine has 4088 KB, so claiming a full 4 MB
sends the diagnostic at memory that is not there and invents faults. It alone
moved the count from 129 to 97. A run that leaves it at the default is not
measuring the emulation.

## What the manual establishes

**Receive status word 1** (3.4.3.5). Bits 15 and 14 are a pair, and bit 15
set means the controller has *not* touched the descriptor:

    15 Lastnot / 14 Error-or-Used
       1 0  value initialized by the host
       1 1  used, not the last segment (chained)
       0 0  last segment, no errors      <- success is 0x0000
       0 1  last segment, with errors
    13 ESETUP  setup, external loopback, or internal-extended loopback packet
    12 Discard
    11 Runt    "the internal loopback operation was unsuccessful"
    10:08 RBL  all set for a setup packet
    02 Frame / 01 CRCERR / 00 OVF

Transmit status word 1 uses the same 15/14 pair with the same meanings.

**Three distinct loopback modes** (CSR08/CSR09):

    IL=0 EL=0  Internal
    IL=0 EL=1  Internal Extended
    IL=1 EL=1  External

Only the internal ones stay inside the chip; anything else drives the
transceiver port, which is what raises carrier when a connector is on it.

**CSR00 (RE) gates the runt check.** The diagnostic runs its address-filter
loopbacks with the receiver enabled, so a runt condition written against RE
does fire; measured `csr=010221` throughout tests 10 and 11.

**RBL depends on the mode** (3.5.6): normal datagrams carry `length - 60`;
other loopback modes carry the true length; looped setup packets set bits
10:08 and put the length in the lower byte.

**Setup filter modes ride in the packet length**: the table occupies 128
bytes and the host appends up to three more, bit 00 all-multicast and bit 01
promiscuous. The transmit descriptor's word count is a two's-complement byte
count (`0177677` for the 130-byte setup CZQNA sends to go promiscuous), so
reading modes out of its low bits reads the length's negation.

**A setup packet blocks reception** until its loopback completes (3.6.2.1),
so the reflected packet must always reach the receive list.

**Boot/diagnostic load** (3.6.1) is an exact sequence: software reset, two 2 KB
receive buffers, CSR = 1010 octal (BD=1, IL=0, EL=1 — internal extended),
wait 150 ms, clear CSR03, wait 150 ms.

## Running one test

DRS takes a test list on START and RESTART, colons between members and a
hyphen for a range (XXDP User's Manual AA-FK83A-TC, section 3.3.4):

    STA/TES:10-11        tests 10 and 11
    STA/TES:1:5-9:15     tests 1, 5..9 and 15
    STA/TES:11/FLA:PNT   print each test number as it runs
    STA/PASS:1           one pass, so the error count is per pass

**Pass `/PASS:1`.** Left alone the diagnostic keeps looping and the count at
`TOTAL ERRS` covers every pass since the start — a run that reported 108 was
six passes of 18, not a regression.

**`/FLA:HOE` is how to attribute an error.** It halts at the first one, so
the last `loopback rsw` line in the trace is the delivery that failed, next
to the console's own expected/actual and its "Matching address" /
"Non-matching address" wording. Every remaining loopback question was
settled this way; reading aggregate counts was not.

`/FLA:PNT` is also the way to enumerate a diagnostic's tests; DRS has no
command that lists them. Switches ride on START and RESTART only, never on
the monitor's `R`. A tests-10-11 run takes about 100 s against a full pass's
four minutes.

## What the loopback tests actually do

Tests 10 and 11 transmit **six-byte loopback frames that are nothing but a
destination address**, one per filter entry they want checked, and read
Runt out of receive status word 1: set when the address logic does not
answer to that address, clear when it does. The diagnostic reprograms the
setup filter between rounds, so the same address is expected recognised in
one round and rejected in the next. Reading `044000` as "a short frame is a
runt" misses this entirely — the length only selects the check.

Both tests pass now. What they pinned down about the filter:

- The filter answers to the station address and to all fourteen table
  entries, each matched exactly, whether or not the multicast bit is set.
  CZQNA programs unicast DECnet addresses (`aa:00:04:ff:ff:ff`) among them
  and wants them recognised, so the manual's "one active physical address"
  rule belongs to datagram reception, not to this question.
- **The setup RAM survives a software reset**; the mode bits do not. The
  diagnostic sets all-multicast, resets, and then wants a multicast address
  the table does not name reported as a failed loopback, while an address
  the table does name still has to be recognised. Getting only one of those
  two halves right leaves errors in both directions.
- Promiscuous and all-multicast do widen this check while they are on.

## Reading the diagnostic

The rev J binary CZQNA runs from is `ZQNAJ0.BIC` on the XXDP image; DEC never
fiched its listing (bitsavers carries revisions A to E, which are DEQNA-only
and renumber the tests). Read it by disassembling instead:

    python3 xxdpext.py xxdp25.rl02 ZQNAJ0 BIC ZQNAJ0.BIC   # walk the UFD
    pdp11                                                   # SIMH loads .BIC
      load ZQNAJ0.BIC
      ex -m 54600-54736      # disassemble around the reporting PC
      ex 55462-55572         # dump an expected-value table

The `PC:` in every error report is the reporting call site, so disassembling
back from it finds the comparison that failed. They all look like this:

    MOV @#55460,R0     ; word count
    MOV #3112,R1       ; the list being checked
    MOV #55462,R2      ; the expected values
    JSR PC,@#55316     ; compare, carry set on mismatch

and the comparison itself is `CMP (R1)+,(R2)+` with `SUB R0,R3` / `INC R3`,
so **Index is 1-based** and counts words from the start of the list. Word 0
of a descriptor is the flag word, 1 the descriptor bits, 2 the address, 3 the
length, 4 status word 1, 5 status word 2 — so index 5 is status word 1 of the
first descriptor, index 11 status word 1 of the second, and so on.

The expected tables are worth reading directly: they state what the hardware
leaves in every word of a list after an operation, which is information the
manual does not carry.

## Already falsified — do not retry

Runt conditions, each measured against a full run:

| condition | errors/pass |
|---|---|
| no Runt at all | 96, later 32 |
| length < 60 and address mismatch (original code) | 301 |
| length < 60 alone | 312 |
| address mismatch alone | 300, and 227 on the corrected baseline |
| `RST_LASTNOERR` (0x8000) as the loopback base | 314, regressed test 8 |
| short frame whose destination is not the physical address | 75 |
| the same, against the active physical address | 77 |
| destination unknown to the filter, multicast entries only | 24 with tests 10-11 at 23 |
| destination unknown to the filter, all entries | 24, then 18 once the modes came from the length |
| the same, filter kept across a software reset | 11 |
| the same, modes ignored by the loopback check | 21, and test 3 broke |
| the same, modes cleared by a software reset | 6 |
| loopback reflection dropped with no buffer armed, loss counted | **5** |
| the setup bit carried across a chained message | 5, no change - reverted |
| arming the receive list always walks it | 5 bridged, and test 5 passes isolated |
| runt measured on the frame rather than the segment | 5, and the runt bit leaves tests 6 and 7 |
| no status words on a partial transmit segment | 5, and test 6 stops reading `140000` |

The last three change no count bridged and each corrected a wrong value, so
read the reported actuals, not the total, when judging a change here.

The 0x8000 attempt came from guessing simh's macro value; the manual shows
0x8000 means *untouched by the controller*, which is why it regressed.

simh's `pdp11_xq.c` gates Runt on `RE && length < 60 && destination !=
setup.macs[0]`. That reads as the rule but is not: `macs[0]` is one entry of
a filter the diagnostic reprograms round by round, and CZQNA expects
broadcast, multicast and other table entries recognised too.

Also confirmed working, so not worth revisiting: the station address PROM
(the diagnostic reads it back exactly), device identity (`DELQA-DESQA in
DELQA mode`), and promiscuous decoding.

## Traps

`QUNILATOR_LOG` receives nothing while the demo runs; DEBUG output only
reaches the tmux pane (`tmux capture-pane -t qb -p -S -400`). Piping the
demo's output to a file yields an empty file, and `tmux pipe-pane` does not
attach either, so the pane's own scrollback is the log. It holds 2000 lines
by default, which one loopback test overruns — set the limit on the server
before the session exists:

    tmux set-option -g history-limit 200000
    tmux new-session -d -s qb ...

Set a device's verbosity **before** enabling it: changing it on a running
device does not reach the log. Through the web API that is a disable, a
verbosity write, and an enable, after the configuration is applied.

Deploy with the demo stopped, or `scp` fails silently on the busy binary and
the next run tests the old code.

The demo needs `QUNILATOR_DIR` or it finds no configurations and no images:

    cd ~/QUniBone && sudo QUNILATOR_DIR=$HOME/QUniBone \
        ./10.03_app_demo/4_deploy_q/demo --web --addresswidth 22

qbone answers on **192.168.2.223**; use `/usr/bin/ssh`, and prefix
`crossbuild.sh` with `PATH=/usr/bin:$PATH`.
