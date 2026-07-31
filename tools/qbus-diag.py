#!/usr/bin/env python3
"""Read the QBone bus diagnostics out of PRU memory. Runs on the board, as root.

  qbus-diag.py alive    exit 0 if the bus shows activity within a second, 1 if not
  qbus-diag.py watch    print ALIVE/FROZEN on every change, to drive a monitor
  qbus-diag.py dump     counters, the DMA timeout snapshot, and the bus trace ring
  qbus-diag.py thaw     re-arm a trace ring that a DMA timeout froze

The firmware exposes everything through one symbol, `qbus_diag`, whose head
carries a magic and a layout version, so a stale address or a firmware built
without diagnostics is reported rather than printed as plausible numbers.
Take the address from the linker map of the running firmware:

    grep -E '^1 +[0-9a-f]+ +qbus_diag ' 10.01_base/4_deploy_q/pru1_code_qbus.out.map

Pass it with --addr, or record it in /etc/qbone-diag-addr at deploy time and
call the tool with no argument. The trace ring is only in a firmware built
with -DQBUS_BUS_TRACE; the counters and the timeout snapshot are always there.
"""

import argparse
import mmap
import os
import struct
import sys
import time

PRU1_DRAM_PHYS = 0x4A302000
ADDR_FILE = "/etc/qbone-diag-addr"
DIAG_MAGIC = 0x51444941  # "QDIA"
SUPPORTED_LAYOUT = 1

# qbus_diag_t's leading uint32 fields, in declaration order (see pru1_diag.h)
HEAD = ("magic", "layout_version", "trace_entries", "bus_activity",
        "intr_answered", "iak_abandoned", "orphan_rescued",
        "dma_grant_refused", "dma_abandoned",
        "timeout_dal_lo", "timeout_dal_hi", "timeout_dal_ext",
        "timeout_l4", "timeout_l6", "timeout_addr",
        "trace_frozen", "trace_count")
HEAD_FMT = "<%dI" % len(HEAD)
HEAD_SIZE = struct.calcsize(HEAD_FMT)
TRACE_FMT = "<IBBBB"
TRACE_SIZE = struct.calcsize(TRACE_FMT)

# A fetching PDP-11 drives SYNC constantly, and one sitting at a prompt still
# takes the 50 Hz line clock, whose handler fetches. A wedged one shows nothing.
RATE_ALIVE = 10  # transitions per second

ARB_STATES = ("grant_check", "dma_rply_wait", "intr_vector", "intr_complete",
              "orphan_vector", "orphan_complete", "noop")


def diag_addr(explicit):
    if explicit is not None:
        return explicit
    try:
        with open(ADDR_FILE) as f:
            return int(f.read().strip(), 0)
    except OSError:
        sys.exit("no --addr given and %s not readable; take the address of "
                 "qbus_diag from pru1_code_qbus.out.map" % ADDR_FILE)


def open_pru(write=False):
    flags = (os.O_RDWR if write else os.O_RDONLY) | os.O_SYNC
    prot = mmap.PROT_READ | (mmap.PROT_WRITE if write else 0)
    fd = os.open("/dev/mem", flags)
    return mmap.mmap(fd, 8192, mmap.MAP_SHARED, prot, offset=PRU1_DRAM_PHYS)


def read_head(m, base):
    d = dict(zip(HEAD, struct.unpack_from(HEAD_FMT, m, base)))
    if d["magic"] != DIAG_MAGIC:
        sys.exit("no diagnostics at 0x%x (magic %08x, expected %08x): wrong "
                 "address, or a firmware built without them"
                 % (base, d["magic"], DIAG_MAGIC))
    if d["layout_version"] != SUPPORTED_LAYOUT:
        sys.exit("firmware diagnostics layout %d, this tool speaks %d"
                 % (d["layout_version"], SUPPORTED_LAYOUT))
    return d


def field_off(base, name):
    return base + HEAD.index(name) * 4


def activity(m, base):
    return struct.unpack_from("<I", m, field_off(base, "bus_activity"))[0]


def cmd_alive(base):
    m = open_pru()
    read_head(m, base)
    before = activity(m, base)
    time.sleep(1.0)
    delta = (activity(m, base) - before) & 0xFFFFFFFF
    alive = delta >= RATE_ALIVE
    print("bus_activity +%d/s (%s)" % (delta, "ALIVE" if alive else "FROZEN"))
    sys.exit(0 if alive else 1)


def cmd_watch(base):
    # One line per state change; a freeze is called after two dead windows.
    m = open_pru()
    read_head(m, base)
    state, dead = None, 0
    before = activity(m, base)
    while True:
        time.sleep(1.0)
        now = activity(m, base)
        delta = (now - before) & 0xFFFFFFFF
        before = now
        dead = 0 if delta >= RATE_ALIVE else dead + 1
        new = "FROZEN" if dead >= 2 else (
            "ALIVE" if delta >= RATE_ALIVE else state)
        if new != state and new is not None:
            print(new, flush=True)
            state = new


def decode(value, names):
    return "+".join(n for b, n in enumerate(names) if value & (1 << b)) or "-"


L4_NAMES = ("SYNC", "DIN", "DOUT", "RPLY", "WTBT", "BS7", "REF", "INIT")
L6_NAMES = ("IRQ4", "IRQ5", "IRQ6", "IRQ7", "DMR", "IAKI", "DMGI", "SACK")


def cmd_dump(base):
    m = open_pru()
    d = read_head(m, base)
    print("counters: bus_activity=%(bus_activity)d "
          "intr_answered=%(intr_answered)d iak_abandoned=%(iak_abandoned)d "
          "orphan_rescued=%(orphan_rescued)d "
          "dma_grant_refused=%(dma_grant_refused)d "
          "dma_abandoned=%(dma_abandoned)d" % d)
    print("DMA timeout snapshot: DAL7:0=%02x DAL15:8=%02x DAL21:16/BS7=%02x "
          "l4=%02x[%s] l6=%02x[%s] addr=%s"
          % (d["timeout_dal_lo"], d["timeout_dal_hi"], d["timeout_dal_ext"],
             d["timeout_l4"], decode(d["timeout_l4"], L4_NAMES),
             d["timeout_l6"], decode(d["timeout_l6"], L6_NAMES),
             oct(d["timeout_addr"])))

    entries = d["trace_entries"]
    if entries == 0:
        print("bus trace: not in this firmware (build with -DQBUS_BUS_TRACE)")
        return
    count = d["trace_count"]
    shown = min(count, entries)
    print("bus trace: %d transitions, %s, last %d follow (oldest first)"
          % (count,
             "FROZEN by a DMA timeout" if d["trace_frozen"] else "running",
             shown))
    prev_ts = None
    for k in range(count - shown, count):
        off = base + HEAD_SIZE + (k % entries) * TRACE_SIZE
        ts, l4, l6, l7, arb = struct.unpack_from(TRACE_FMT, m, off)
        gap = "" if prev_ts is None else "+%dns" % (
            ((ts - prev_ts) & 0xFFFFFFFF) * 5)
        prev_ts = ts
        state = ARB_STATES[arb] if arb < len(ARB_STATES) else "?%d" % arb
        print("  %10d %12s  l4=%02x[%s] l6=%02x[%s] l7=%02x arb=%s"
              % (ts, gap, l4, decode(l4, L4_NAMES), l6, decode(l6, L6_NAMES),
                 l7, state))


def cmd_thaw(base):
    m = open_pru(write=True)
    read_head(m, base)
    struct.pack_into("<I", m, field_off(base, "trace_frozen"), 0)
    print("bus trace re-armed")


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command",
                        choices=("alive", "watch", "dump", "thaw"))
    parser.add_argument("--addr", type=lambda s: int(s, 0),
                        help="address of qbus_diag in PRU1 data RAM "
                             "(default: read from %s)" % ADDR_FILE)
    args = parser.parse_args()
    handler = {"alive": cmd_alive, "watch": cmd_watch,
               "dump": cmd_dump, "thaw": cmd_thaw}[args.command]
    handler(diag_addr(args.addr))


if __name__ == "__main__":
    main()
