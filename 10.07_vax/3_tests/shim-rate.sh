#!/bin/bash
#
# shim-rate.sh: measure how fast the shimmed core executes
#
# docs/plans/vax-unibus-plan.md wants the instruction rate recorded as the baseline a
# later stage is measured against: a stage that moves it by an order of
# magnitude has changed something structural. Run it on the build machine and on
# the board, and record both.
#
#	./shim-rate.sh [seconds]        default 10
#
# The program is a five-instruction loop - register arithmetic, one write and
# one read through the memory path - repeated until the time limit stops it.
# It is a proxy, not a benchmark: a real workload's mix is wider and its working
# set does not sit in one cache line. Read the number as an order of magnitude.

set -u

here=$(cd "$(dirname "$0")" && pwd)
shim=$here/../4_deploy/vax780-shim
rundir=$here/../4_deploy/shim-rate
seconds=${1:-10}

if [ ! -x "$shim" ]; then
    echo "$0: $shim is not built - run make in ../2_src" >&2
    exit 2
fi

rm -rf "$rundir"
mkdir -p "$rundir"

python3 - "$rundir/rate.bin" <<'PROGRAM'
import sys

program = bytearray()

# MOVL I^#0x10000, R4 - a scratch longword well clear of the program
program.extend (b'\xD0\x8F' + (0x10000).to_bytes (4, 'little') + b'\x54')

loop = len (program)
program.extend (b'\xC0\x01\x50')            # ADDL2 S^#1, R0
program.extend (b'\xD0\x50\x64')            # MOVL R0, (R4)
program.extend (b'\xD0\x64\x51')            # MOVL (R4), R1
program.extend (b'\xC1\x50\x51\x52')        # ADDL3 R0, R1, R2
program.extend (b'\x11\x00')                # BRB loop
program[-1] = (loop - len (program)) & 0xFF

open (sys.argv[1], 'wb').write (bytes (program))
PROGRAM

echo "running for ${seconds}s"
"$shim" -m 8 -l "$seconds" "$rundir/rate.bin" >"$rundir/console.out" 2>"$rundir/messages"
status=$?

cat "$rundir/messages"
if [ $status -ne 0 ]; then
    echo "FAIL: the core stopped with status $status"
    exit 1
fi

count=$(sed -n 's/.*after \([0-9]*\) instructions.*/\1/p' "$rundir/messages" | tail -1)
if [ -z "$count" ]; then
    echo "FAIL: no instruction count in the run's messages"
    exit 1
fi

awk -v n="$count" -v s="$seconds" 'BEGIN {
    printf "%.2f million instructions per second over %ds\n", n / s / 1e6, s
}'
