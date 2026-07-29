#!/bin/bash
#
# boot-vms.sh: boot VMS on the vendored VAX-11/780, on the host
#
# This is the harvest test of docs/vax-unibus-plan.md stage 0: a VMS that boots
# to DCL on the workstation says the file set taken from simh is complete, and
# says it before any bus work starts. It runs the stock simulator with simh's
# own device set, so nothing of QUniLator is under test here.
#
# The system disk is supplied by the caller, because no bootable VMS volume is
# distributed with the project:
#
#	./boot-vms.sh ~/vax/rd54-vms47-clean.dsk
#
# The image is copied first and the copy is what boots, so a run leaves the
# caller's volume untouched. Written against the VMS 4.7 startup dialog; another
# version answers different prompts and boot-vms.ini needs the new wording.
#
# Exits 0 when VMS reached its login prompt.

set -u

here=$(cd "$(dirname "$0")" && pwd)
vax780=$here/../4_deploy/vax780
rundir=$here/../4_deploy/boot-vms
timeout_s=${BOOT_VMS_TIMEOUT:-600}

if [ $# -ne 1 ]; then
    echo "usage: $0 <vms-system-disk-image>" >&2
    exit 2
fi
image=$1

if [ ! -x "$vax780" ]; then
    echo "$0: $vax780 is not built - run make in ../2_src" >&2
    exit 2
fi
if [ ! -r "$image" ]; then
    echo "$0: cannot read $image" >&2
    exit 2
fi

rm -rf "$rundir"
mkdir -p "$rundir"
cp "$image" "$rundir/system.dsk"

# The date answers the startup prompt. VMS 4.7 predates 2000 and takes a
# 21st-century date without complaint, so today's date keeps the log readable.
answer_date=$(date '+%d-%b-%Y %H:%M' | tr '[:lower:]' '[:upper:]')

echo "booting $image on $(basename "$vax780"), limit ${timeout_s}s"

# simh reads its own console from stdin. A pipe that outlives the run keeps it
# from seeing end of file while VMS is still starting.
( sleep "$timeout_s" ) |
    ( cd "$rundir" && timeout "$timeout_s" "$vax780" "$here/boot-vms.ini" \
        system.dsk "$answer_date" ) >"$rundir/sim.out" 2>&1
status=$?

log=$rundir/console.log
if [ ! -s "$log" ]; then
    echo "FAIL: no console output in $log (simulator exit $status)"
    exit 1
fi

fail=0
check () {                                      # check <what> <string>
    if grep -q "$2" "$log"; then
        echo "  ok       $1"
    else
        echo "  MISSING  $1: no \"$2\" in the console log"
        fail=1
    fi
}

check "the VMS banner"        "VAX/VMS Version"
check "the startup dialog"    "PLEASE ENTER DATE AND TIME"
check "OPCOM running"         "OPCOM"
check "the login prompt"      "Username:"

if [ $fail -ne 0 ]; then
    echo "FAIL: see $log"
    exit 1
fi

echo "PASS: VMS booted to its login prompt, log in $log"
