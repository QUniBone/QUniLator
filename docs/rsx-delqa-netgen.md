# RSX-11M+ DECnet over the emulated DELQA — NETGEN procedure

Brings RSX-11M+ DECnet up on the QBone DELQA so the PDP-11 reaches the house
LAN. This records what the running system actually has, the one change the
network generation needs, and the exact answers to make it.

## What the running RSX system has

Booted from `rsx11mplus.dsk` (MSCP `DU0:`), the system is **RSX-11M-PLUS V4.6
BL87**, node **30.30 name RSX11M** (console banner shows `PIDP11`), an
**area-routing** node. DECnet-11M-PLUS is generated and starts at boot: NETACP,
NICE, EVR, the CEX processes and NCP are all installed and the executor comes up
`On` (`NCP SET EXE STA ON`).

DECnet has exactly **one line and one circuit, `UNA-0`**, and both stay
`Cleared` because the line cannot load:

```
NCP SET LIN UNA-0 ALL
NTL -- Config File -- Device UNA-0 Offline
        CNT$DF  0,120,174510,5,,NX
NCP -- Set failed, operation failure
       Network Loader function failed
```

`UNA-0` is the **DEUNA/DELUA** Ethernet line — CSR **774510** (shown as the low
16 bits `174510`), vector **120**, priority 5. QBone emulates a **DELQA**, which
is a **DEQNA-class (QNA)** controller at CSR **774440**, register-incompatible
with the DEUNA. The DEUNA driver finds nothing at 774510, so `UNA-0` never
loads. `STARTUP.CMD` even carries the note `; Until UNA-0 is running correctly in
Simh ...` around the LAT block, so this mismatch predates this work.

`CETAB.MAC` (`DU0:[5,54]CETAB.MAC`) confirms the network was generated with the
UNA device only:

```
PDV$DF  <AUX,EVL,ECL,XPT,NCT,RTH,LAT,DLX>,<EPM>,<UNA>
DDM$DF  UNA,ZF.DDM!ZF.COU!ZF.DVP!ZF.MAN!ZF.LMC,5,0,1.
CNT$DF  0,120,174510,5,,NX     ; unit 0, vector 120, CSR 774510, prio 5
```

## The change the network needs

Replace the DEUNA line `UNA-0` with a **DEQNA/DELQA line `QNA-0`** at CSR
**774440**, vector **120**. Everything else (node address 30.30, area routing,
Ethernet Protocol Manager EPM, routing parameters) stays.

The QNA device-driver module is **already built on this pack**, so no driver
build is required — only a network-configuration change that selects QNA instead
of UNA and reloads:

| file | UIC | role |
|---|---|---|
| `QNA.TSK`, `QNA.STB`, `QNA.DAT` | `[5,54]` | built DEQNA/DELQA DDM (device-driver module) |
| `QNABLD.CMD` | `[5,24]` | its TKB build command (`DDM23/LB` modules `QNADSP…QNAGSD`) |
| `SECQNA.SYS`, `TERQNA.SYS` | `[5,54]`,`[136,54]` | QNA down-line-load images |
| `NETGEN.CMD`, `NETGEN.CLB`, `NETGEN.KIT` | `[137,10]` | the DECnet NETGEN kit |
| `NETGEN.GEN`, `NETCFG.TXT` | `[5,1]` | saved NETGEN answers + readable config summary |
| `VNP.TSK` | `[5,54]` | Virtual Network Processor (offline database editor) |
| `CFE*.TSK` | `[132,54]`,`[5,54]` | Configuration File Editor |

## Emulator side (already correct — no source change)

The `delqa` device already matches what a `QNA-0` line expects:

- **CSR 774440** — `base_addr` reads `4192544` decimal = `17774440` octal; the
  DEQNA/DELQA standard, and what `QNA-0` uses.
- **Vector** — the DEQNA/DELQA has **no vector jumper**; the guest driver writes
  the vector into the Vector Address Register (VAR). `delqa.cpp` makes
  `intr_vector` read-only ("the host sets the vector by writing VAR"), so it
  always reads 0 and the emulator adopts whatever the QNA driver programs —
  answer NETGEN with vector **120** and no emulator change is needed.
- **Station address** — leave the DELQA `mac` at its default. When DECnet brings
  the circuit up it programs the DECnet-derived physical address
  `AA-00-04-00-<node-lo>-<node-hi>` into the controller via an Ethernet **setup
  packet**; the emulator honours setup packets (`process_setup`), so the address
  aligns automatically.
- **interface `veth-pdp`** — the host bridge that already reaches the LAN.

Enable the DELQA in the RSX device set. The saved `rsx11mplus` config does *not*
include it, so either add it to that config or apply the config and then, with
the machine halted, `PUT /api/devices/delqa/params/enabled {"value":"true"}`
before booting.

## Procedure

Do this **on a copy of the pack**, never on `rsx11mplus.dsk`:

```
ssh hans@qbone sudo cp /var/lib/bone/images/rsx11mplus.dsk \
                       /var/lib/bone/images/rsx11mplus-net.dsk
```

Point `uda0` at the copy, enable `delqa`, boot RSX (console `/ws/console/ext`,
driven one char at a time — see the console-channel memory; ~0.12 s/char is
safe, long command lines otherwise overrun the SLU and RSX answers `^U`):

- Boot ROM Dialog: `BOOT DU0`
- Answer the time/date prompt, `N` to Load LAT, `N` to Load TCP/IP.

### Reconfigure the DECnet line to QNA

The change is a NETGEN-level one (it selects which DDM the network loads). Run
NETGEN reusing the saved answers and change only the Ethernet line:

1. `SET /UIC=[137,10]` then `@NETGEN` (the kit's driver command file).
2. Take the saved answers / DEC defaults where offered (`NETGEN.GEN` holds the
   last run: "Generate ALL components", area-routing node 30.30, max address
   1023, etc.).
3. At the Ethernet line definition, define the line as a **DEQNA/DELQA (QNA)**:
   - line/circuit name **`QNA-0`**
   - controller **CSR 774440**
   - **vector 120**
   - priority 5
   Remove or replace the `UNA-0` line.
4. Let NETGEN assemble `CETAB` and build the network. The QNA DDM (`QNA.TSK`) is
   already present, so the build is a reconfigure, not a driver build.

The resulting `CETAB.MAC` should read (compare against the UNA form above):

```
PDV$DF  <...>,<EPM>,<QNA>
DDM$DF  QNA,...
CNT$DF  0,120,174440,5,,NX     ; vector 120, CSR 774440
SLT$DF  QNA,EPM,XPT,...         ; line name QNA-0
```

### Bring the circuit up and make it persistent

At the MCR prompt after the network loads:

```
NCP SET LINE QNA-0 STATE ON
NCP SET CIRCUIT QNA-0 STATE ON
NCP SHOW KNOWN CIRCUITS        ; expect QNA-0 = On
```

Persist by adding the QNA line/circuit-on to the DECnet startup that
`LB:[1,2]STARTUP.CMD` invokes (the same place the `UNA-0` block sits today), so
it comes up on every boot.

### Verify LAN reach

```
NCP LOOP CIRCUIT QNA-0                 ; loopback across the wire
NCP SHOW CIRCUIT QNA-0 COUNTERS        ; frames sent/received incrementing
NCP LOOP NODE <lan-decnet-node>        ; if a DECnet node is on the LAN
```

or watch host-side traffic on `veth-pdp`/`br0` for the DECnet multicast
`AB-00-00-03-00-00` and the node's `AA-00-04-00-…` frames.

### Save the deliverable

With the circuit generated and verified, the copy `rsx11mplus-net.dsk` is the
networked pack. Keep it under `/var/lib/bone/images/` alongside the pristine
`rsx11mplus.dsk`, and add a saved config (e.g. `rsx-net`) that enables `uda`,
`uda0`→`rsx11mplus-net.dsk`, `delqa`, `KW11`, and the `rl`/`rl0` exchange drive.

## NETGEN answer summary

| item | value |
|---|---|
| line / circuit name | `QNA-0` (DEQNA/DELQA) |
| CSR | 774440 |
| vector | 120 |
| priority | 5 |
| node address | 30.30 (unchanged) |
| node name | RSX11M (unchanged) |
| routing | area-routing (unchanged) |
| DELQA physical address | DECnet-derived `AA-00-04-00-xx-xx` (setup packet) |

## Manual alternative to NETGEN

If a full NETGEN dialog is impractical over the console, the same result comes
from editing `CETAB.MAC` directly (UNA→QNA, CSR `174510`→`174440`), reassembling
it against the network macro library, rebuilding `CETAB.TSK`, and reloading the
network (reboot). `VNP` edits the same database in an offline system image. Both
are multi-step builds; NETGEN with the saved answers is the supported path.
