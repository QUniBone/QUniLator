# RSX-11M+ DECnet over the emulated DELQA — QNA-0 generation

Brings RSX-11M+ DECnet up on the QBone DELQA so the PDP-11 reaches the house
LAN. This records what the running system had, the one change the network
configuration needed, and how it was made.

**Outcome (2026-07-25):** done on `images/rsx11mplus-net.dsk`. The DECnet
Ethernet line was changed from the never-loading `UNA-0` (DEUNA, CSR 774510) to
`QNA-0` (DEQNA/DELQA, CSR 774440, vector 120, priority 5) by rebuilding the
network configuration table `CETAB` from an edited `CETAB.MAC` using the kit's
own assemble/build command files, and renaming the boot-time NCP line/circuit
commands in `LB:[1,2]STARTUP.CMD`. After reboot `QNA-0` loads, the circuit comes
up `On` automatically, and the DELQA transmits DECnet router-hello and MOP
loopback frames onto the house LAN from the DECnet-derived station address
`AA-00-04-00-1E-78`. The board carries a saved config **`rsx-net`** that boots
this networked pack one apply away.

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

`rsx11mplus-net.dsk` was found in exactly this state — a plain copy of the
pristine pack: `NCP SHOW KNOWN LINES/CIRCUITS` reported the single `UNA-0`
line/circuit `Cleared`, and the boot log carried the same "Network Loader
function failed" for `NCP SET LIN UNA-0 ALL`. The work below was done on it.

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
ssh hans@qbone sudo cp /var/lib/qunilator/images/rsx11mplus.dsk \
                       /var/lib/qunilator/images/rsx11mplus-net.dsk
```

Point `uda0` at the copy, enable `delqa`, boot RSX (console `/ws/console/ext`,
driven one char at a time — see the console-channel memory; ~0.12 s/char is
safe, long command lines otherwise overrun the SLU and RSX answers `^U`):

- Boot ROM Dialog: `BOOT DU0`
- Answer the time/date prompt, `N` to Load LAT, `N` to Load TCP/IP.

### Reconfigure the DECnet line to QNA (CETAB rebuild)

The line hardware — device mnemonic, CSR, vector — lives in the network
configuration table `CETAB`, generated from `DU0:[5,54]CETAB.MAC` and task-built
into `CETAB.TSK`, which `NTINIT` loads at every boot (the running system loads
the highest `[5,54]CETAB.TSK`). Changing the line is therefore an edit of the
device macros in `CETAB.MAC` plus a rebuild of `CETAB.TSK` using the kit's own
command files. The QNA DDM (`QNA.TSK`) is already on the pack, so this is a
configuration rebuild, not a driver build.

The device macros differ from the DEUNA form only in the mnemonic and the CSR —
the DDM/unit/line-table flags are the same for the two Ethernet DDMs, so two
global substitutions suffice. Driven over the paced console (`EDT` line mode):

1. `SET /UIC=[5,54]` then `EDT DU0:[5,54]CETAB.MAC` (opens the latest version).
2. `S/UNA/QNA/ WHOLE` — 6 substitutions (PDV$DF, SLT$DF, DDM$DF and the three
   `EXP*` export lines).
3. `S/174510/174440/ WHOLE` — 1 substitution (the `CNT$DF` CSR).
4. `EXIT` — writes `CETAB.MAC;40`.
5. Rebuild with the kit's assemble/build command files (they reference
   `[130,10]NETLIB/ML`, `[5,10]RSXMC` and `[5,54]CETAB`, and are the same ones
   `NETBLDNET.CMD` runs). Assign the build logicals to `SY:` first:

   ```
   ASN SY:=IN:  &  ASN SY:=OU:  &  ASN SY:=LS:  &  ASN SY:=MP:
   MAC @DU0:[5,24]CETABASM.CMD      ; -> [5,24]CETAB.OBJ;9
   TKB @DU0:[5,24]CETABBLD.CMD      ; -> [5,54]CETAB.TSK;9 (PAR=CTBPAR)
   ```

6. Rename the boot-time NCP commands so they name the new line. In
   `EDT DU0:[1,2]STARTUP.CMD`: `S/UNA-0/QNA-0/ WHOLE` — 2 substitutions
   (`NCP SET LIN QNA-0 ALL`, `NCP SET CIR QNA-0 STA ON`).
7. Reboot (halt → boot ROM `173000G` → `B DU0`) so `NTINIT` loads the rebuilt
   `CETAB.TSK`.

The generated `DU0:[5,54]CETAB.MAC;40` device section reads (compare against the
UNA form above — only the mnemonic and CSR changed):

```
PDV$DF  <AUX,EVL,ECL,XPT,NCT,RTH,LAT,DLX>,<EPM>,<QNA>
SLT$DF  QNA,EPM,XPT,LF.TIM!LF.BRO,0,0,MASTER,0.,0.,15.,5.,64.
DDM$DF  QNA,ZF.DDM!ZF.COU!ZF.DVP!ZF.MAN!ZF.LMC,5,0,1.
CNT$DF  0,120,174440,5,,NX     ; unit 0, vector 120, CSR 774440, prio 5
UNT$DF  0,177470,5,,3.,,3.
```

### Circuit up and persistent

The circuit comes up `On` automatically at boot: `STARTUP.CMD` runs
`@LB:[5,1]NETINS` (which loads the rebuilt `CETAB` and brings the executor `On`)
and then the two edited lines `NCP SET LIN QNA-0 ALL` / `NCP SET CIR QNA-0 STA
ON`. On the first boot after the rebuild the DECnet log shows `QNA-0` loading
with no loader failure (contrast the old `UNA-0` "Network Loader function
failed") and a `Circuit up` event for `QNA-0`. At MCR:

```
NCP SHOW KNOWN LINES       ; QNA-0  On
NCP SHOW KNOWN CIRCUITS    ; Circuit = QNA-0  State = On
```

The `UNA-0` block in `STARTUP.CMD` was edited in place (§ CETAB rebuild step 6),
so no separate persistence step is needed — a cold boot brings `QNA-0` up.

### Verify LAN reach — achieved

`NCP SHOW CIRCUIT QNA-0 COUNTERS` shows *Bytes/Data blocks sent* incrementing
(router-hellos every ~15 s), `Circuit down` and `Initialization failure` both 0.
*Bytes received* stays 0 because the house LAN has no other DECnet node to peer
with.

Host-side, `tcpdump -e -i br0` confirms the DELQA is on the wire from the
DECnet-derived station address **`AA-00-04-00-1E-78`** (node 30.30 = area 30 /
node 30 → 30·1024+30 = 30750 = 0x781E, little-endian):

```
aa:00:04:00:1e:78 > ab:00:00:04:00:00  DN (0x6003)  router-hello l2rout src 30.30 ...
aa:00:04:00:1e:78 > ab:00:00:03:00:00  DN (0x6003)  router-hello l2rout src 30.30 ...
aa:00:04:00:1e:78 > cf:00:00:00:00:00  Loopback (0x9000)  Forward Data ...   (from NCP LOOP)
```

The setup packet programmed the station address with no emulator change, as
designed. `NCP LOOP CIRCUIT QNA-0` egresses MOP loopback frames (seen above) but
reports "Loop failed … Receiver, Unlooped count = 1": nothing on the LAN
reflects them, so the loop has no return — a property of the peerless LAN, not
of the configuration. `NCP LOOP NODE` and a full DECnet conversation await a
second DECnet node on the LAN.

### The deliverable

`rsx11mplus-net.dsk` (under `/var/lib/qunilator/images/`, alongside the pristine
`rsx11mplus.dsk`) is the networked pack. The board carries a saved config
**`rsx-net`** that enables `uda`, `uda0`→`rsx11mplus-net.dsk`, `delqa` (CSR
774440, vector 120), `KW11`, and the `rl`/`rl0` exchange drive — `POST
/api/configs/rsx-net/apply`, then boot `DU0`, and the networked machine is up.

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

## NETGEN as an alternative

The CETAB rebuild above is the method used, and is well suited to the paced
console: it is two `EDT` substitutions plus the kit's own build commands, each
step verifiable, and reversible by re-copying the pristine pack. DEC's supported
generator, NETGEN, produces the same `CETAB` from a dialog: `SET /UIC=[137,10]`,
`@NETGEN`, take the saved answers (`[5,1]NETGEN.GEN`, `NETDDM.GEN`, `DECGEN.GEN`
hold the last run — "Generate ALL components", area-routing node 30.30, max
address 1023) and change only the Ethernet line to `QNA-0` / CSR 774440 /
vector 120 / priority 5. The saved DDM answers (`[5,1]NETDDM.GEN`) are the
authoritative source for the device macros — they carry the exact `DDM$DF` /
`CNT$DF` / `UNT$DF` / `SLT$DF` lines NETGEN emits — which is what confirms the
CETAB edit above needs only the mnemonic and CSR changed.
