#!/bin/bash
#
# shim-boot-vms.sh: boot VMS on the shimmed core, with the disk inside it
#
# The control for the bus stages of docs/vax-unibus-plan.md. It runs the core in
# the form it takes when embedded - shim/ in place of simh's command
# interpreter, console and timer - and carries an operating system on it, using
# simh's own MSCP controller as the disk rather than the emulated one on the
# bus. What passes here says the shim is sound: the event queue keeps a device's
# time, the console byte channel carries a terminal session, the loader places a
# bootstrap where the processor expects it, and the batching of simh_shim_run()
# does not lose an interrupt over some 10^9 instructions.
#
# When a later stage cannot boot VMS from the emulated UDA50 over the bus, this
# is what says whether the fault is above or below the shim.
#
#	./shim-boot-vms.sh ~/vax/rd54-vms47-clean.dsk
#
# The image is copied first, so a run leaves the caller's volume untouched.
# Written against the VMS 4.7 startup dialog.
#
# Exits 0 when VMS reached its login prompt.

set -u

here=$(cd "$(dirname "$0")" && pwd)
shim=$here/../4_deploy/vax780-shim-disk
rundir=$here/../4_deploy/shim-boot-vms
limit=${SHIM_BOOT_TIMEOUT:-900}

if [ $# -ne 1 ]; then
    echo "usage: $0 <vms-system-disk-image>" >&2
    exit 2
fi
image=$1

if [ ! -x "$shim" ]; then
    echo "$0: $shim is not built - run make in ../2_src" >&2
    exit 2
fi
if [ ! -r "$image" ]; then
    echo "$0: cannot read $image" >&2
    exit 2
fi

rm -rf "$rundir"
mkdir -p "$rundir"
cp "$image" "$rundir/system.dsk"

console=$rundir/console.log
input=$rundir/console.in
mkfifo "$input"

# The shimmed core has no EXPECT: its console is a byte channel, and the
# operator is whoever holds the other end. Here that is this script, which
# watches the log for each prompt and writes the answer.
await () {                                      # await <what> <string> <seconds>
    local waited=0

    while [ "$waited" -lt "$3" ]; do
        if grep -a -q "$2" "$console" 2>/dev/null; then
            echo "  ok       $1"
            return 0
        fi
        if ! kill -0 "$sim" 2>/dev/null; then
            echo "  MISSING  $1: the core stopped first"
            return 1
        fi
        sleep 2
        waited=$((waited + 2))
    done
    echo "  MISSING  $1: nothing in ${3}s"
    return 1
}

( cd "$rundir" && "$shim" -m 8 -l "$limit" \
    -s "RQ0 RD54" -a RQ0=system.dsk -B RQ0 ) <"$input" >"$console" 2>&1 &
sim=$!
exec 3>"$input"                                 # hold the write end open

started=$(date +%s)
status=0

if await "the VMS banner" "VAX/VMS Version" 300 &&
   await "the startup dialog" "PLEASE ENTER DATE AND TIME" 120; then
    printf '%s\r' "$(date '+%d-%b-%Y %H:%M' | tr '[:lower:]' '[:upper:]')" >&3
    if await "OPCOM running" "OPCOM" 300 &&
       await "startup complete" "login interactive limit" 300; then
        sleep 5
        printf '\r' >&3
        await "the login prompt" "Username:" 120 || status=1
    else
        status=1
    fi
else
    status=1
fi

elapsed=$(( $(date +%s) - started ))
exec 3>&-
kill "$sim" 2>/dev/null
wait "$sim" 2>/dev/null
rm -f "$input"

echo "the run took ${elapsed}s"
if [ $status -ne 0 ]; then
    echo "FAIL: see $console"
    exit 1
fi
echo "PASS: VMS booted to its login prompt on the shimmed core, log in $console"
