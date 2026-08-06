# RSX-11M+ over DELQA

**Status:** Ready — implementation plan drafted in
[`rsx-delqa-plan.md`](../plans/rsx-delqa-plan.md).

Reconfigure the RSX-11M+ installation so its networking runs over the emulated
DELQA, changing both the RSX system and the emulator configuration.

## Current state

QBone emulates the DELQA (validated under XXDP via CZQNA), and the host Ethernet
path works fully, including reaching external hosts on the LAN. The RSX-11M+
install does not yet use the DELQA for its networking.

## Requirements

- The RSX-11M+ system runs its network over the **emulated DELQA** — the QNA/DEUNA
  line driver bound to the DELQA at its Q-bus address.
- The **emulator configuration enables the DELQA** for the RSX device set, saved
  as a configuration ([configuration-model.md](configuration-model.md)).
- The connectivity goal is **full LAN reach** — RSX talks to other machines on
  the house LAN, not only services on the BBB.

## Decisions

- **Full LAN reach** is the target — the host Ethernet path already carries
  traffic to external hosts, so this is a matter of the RSX-side network
  generation, not a host-networking blocker.

## Open questions

- What does RSX-11M+ use for networking today — is DECnet-11 already generated,
  or does the network need building (NETGEN) with the QNA driver added?
- The RSX side is a SYSGEN/NETGEN change to include the DELQA line driver and
  assign a DECnet node address/MAC. Is that done on the running RSX, or by
  preparing a fresh system image?
- What connectivity is the goal — RSX to services on the BBB itself, or the wider
  LAN? The CPSW raw-TX and multicast constraints bound what LAN reach is possible.
- Does the deliverable include a **prepared RSX-11M+ image** (with the DELQA
  network generated) kept under `images/`, or only the procedure to reconfigure
  an existing install?
- Which DELQA CSR address/vector does RSX expect, and does it match the emulator's
  default?
