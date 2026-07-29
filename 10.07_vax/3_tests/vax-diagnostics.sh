#!/bin/bash
#
# vax-diagnostics.sh: run the DEC VAX diagnostics against the harvest
#
# The VAX equivalent of what 10.06_cputest does for the KA11 and KD11 with the
# MAINDEC tapes: DEC's own processor diagnostics, run on the simulator, judging
# the instruction set rather than an operating system's opinion of it.
#
#	EVKAA	the hardware core instruction test, run standalone
#	EVKAB	the basic instruction exerciser, under the Diagnostic Supervisor
#	EVKAC	the floating point instruction exerciser
#	EVKAD	the compatibility mode instruction exerciser
#
# The diagnostics and the script that drives them come from upstream simh and
# are vendored with the rest of it; 91_3rd_party/simh_vax/VAX/tests is the same
# directory upstream carries. The three Diagnostic Supervisor tests read their
# results with regular expressions, so they need a build with PCRE2 - the
# makefile takes it when pkg-config finds it, and reports the tests skipped when
# it does not.
#
# This tests the vendored core, not the shim: the script is written in scp's
# command language and needs its EXPECT and DO. shim-boot-vms.sh is what tests
# the shim.
#
#	./vax-diagnostics.sh
#
# The Diagnostic Supervisor attaches a UDA50 at 772150, which is where the
# emulated one answers - so once the bus stages are done, this same suite
# becomes a test of that device rather than of simh's.

set -u

here=$(cd "$(dirname "$0")" && pwd)
vax780=$here/../4_deploy/vax780
tests=$here/../../91_3rd_party/simh_vax/VAX/tests
rundir=$here/../4_deploy/vax-diagnostics
timeout_s=${VAX_DIAG_TIMEOUT:-1800}

if [ ! -x "$vax780" ]; then
    echo "$0: $vax780 is not built - run make in ../2_src" >&2
    exit 2
fi

rm -rf "$rundir"
mkdir -p "$rundir"

# The suite writes into the directory the diagnostics sit in, so it runs on a
# copy and leaves the vendored tree as it is.
cp -R "$tests"/. "$rundir/"

log=$rundir/run.log

# The simulator reads its own console from stdin; the script answers every
# prompt itself, so it is given a pipe that stays open and that nobody writes.
fifo=$rundir/console.in
mkfifo "$fifo"
sleep "$timeout_s" >"$fifo" &
holder=$!

started=$(date +%s)
( cd "$rundir" && timeout "$timeout_s" "$vax780" vax-diag_test.ini ) \
    <"$fifo" >"$log" 2>&1
status=$?
elapsed=$(( $(date +%s) - started ))

kill "$holder" 2>/dev/null
wait "$holder" 2>/dev/null
rm -f "$fifo"

grep -aE '^\*\*\*|MISSING|Missing' "$log" | sed 's/^/  /'
echo "the run took ${elapsed}s"

if [ $status -ne 0 ]; then
    echo "FAIL: the suite exited $status, see $log"
    exit 1
fi
if grep -aq "FAILED" "$log"; then
    echo "FAIL: a diagnostic reported an error, see $log"
    exit 1
fi
if ! grep -aq "PASSED.*EVKAA" "$log"; then
    echo "FAIL: EVKAA did not report a pass, see $log"
    exit 1
fi

if grep -aq "Missing Regular Expression support" "$log"; then
    echo "PASS: EVKAA passed; the Diagnostic Supervisor tests were skipped for"
    echo "      want of PCRE2 - install it and rebuild to run them"
else
    echo "PASS: every diagnostic passed, log in $log"
fi
