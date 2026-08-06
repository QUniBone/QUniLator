# RSX-11M+ over DELQA — implementation plan

Reconfigures the RSX-11M+ system to run its DECnet networking over the emulated
DELQA, reaching the house LAN. Implements the requirements in
[`rsx-delqa.md`](../planning/rsx-delqa.md).

## 1. Current state

QBone emulates the DELQA (validated under XXDP via CZQNA) and the host Ethernet
path works fully — the DELQA reaches external hosts on the LAN, and DEC multicast
is delivered, over the `veth-pdp`/`br0` host bridge. The `delqa` device defaults
to the standard DEQNA/DELQA CSR **774440** (octal), interrupt level 4, a MAC in
the DEC OUI (`08:00:2b:…`), bound to `veth-pdp`, and ships **disabled**.

The `rsx11mplus.dsk` system has **DECnet-11M+ generated and running**, but **no
QNA/DELQA line** is configured. Networking needs a NETGEN pass to add the QNA
line driver and a DECnet circuit on it — not a fresh DECnet build.

## 2. Emulator side

- **Enable the DELQA** for the RSX device set and save it as a configuration
  ([configuration-model-plan.md](configuration-model-plan.md)) — e.g. an `rsx`
  config with the DELQA on. The `external_console`/`ttys2` console bridge and the
  other board settings are untouched (they are separate from the configuration).
- **CSR and vector must match the RSX NETGEN answers** (§3). The CSR stays at the
  DEQNA standard **774440**; the interrupt vector is set to whatever NETGEN
  assigns to the QNA line (the emulator's `intr_vector` is configurable, so both
  sides are set to the same value). The DELQA MAC is the emulator's; RSX's DECnet
  node address is derived from it or set explicitly (§3).

## 3. RSX side — NETGEN in place

Done **on the running `rsx11mplus.dsk` system**, booted on QBone with the DELQA
enabled, then the modified pack is saved:

1. Inspect the current DECnet setup (installed tasks, `NCP SHOW`, the executor
   node) to confirm DECnet is up and no QNA line exists.
2. **NETGEN** to add the **QNA line driver** and a **DECnet circuit** on the
   DELQA, answering with the DELQA CSR **774440** and an interrupt vector; assign
   the DECnet node address/name and the line/circuit names.
3. Set the DECnet node address consistent with the DELQA MAC (DECnet-over-Ethernet
   derives the physical address `AA-00-04-00-<node>` from the area/node), or set
   the DELQA MAC to match the assigned DECnet address — the two must agree for
   DECnet to answer on the wire.
4. Bring the circuit up (`NCP SET CIRCUIT … STATE ON`) and make it persistent in
   the DECnet startup so it comes up on boot.
5. **Save the modified pack** as the deliverable image.

## 4. Connectivity goal — full LAN reach

The target is RSX reaching other machines on the house LAN, not only services on
the BBB. The host Ethernet path already carries DELQA traffic to external LAN
hosts and delivers DEC multicast, so this is a matter of the RSX-side generation,
not a host-networking blocker. Validation is DECnet reachability from RSX to
another node on the LAN (or a DECnet loopback/`NCP LOOP` across the circuit, and
a `NCP SHOW CIRCUIT` showing the line up with traffic).

## 5. Deliverable

- A **prepared RSX-11M+ image** with DELQA networking generated, kept under
  `images/` (e.g. `rsx11mplus-net.dsk`), left alongside the current
  `rsx11mplus.dsk`.
- The **documented procedure** — the NETGEN answers (CSR, vector, node address,
  circuit/line names), the DELQA config to enable it, and the alignment between
  the DELQA MAC and the DECnet address — so the image can be reproduced.

## 6. Decisions

Resolved:

- RSX has **DECnet installed but no QNA line**; the work is a **NETGEN** to add the
  QNA line driver and a circuit, **done in place** on the running system, saving
  the modified pack.
- The DELQA is **enabled and saved as a configuration**; board settings stay
  separate.
- **Full LAN reach** is the target; the host path already supports it.
- Deliverable is a **prepared image under `images/` plus the procedure**.

Open:

- The **interrupt vector** NETGEN assigns to the QNA line, to be set identically on
  the emulator's `delqa` (the CSR 774440 already matches the DEQNA standard).
- The **DECnet node address/area** to assign, and whether to align the DELQA MAC to
  it or let DECnet derive the physical address — settled during NETGEN against the
  existing LAN's DECnet addressing.
