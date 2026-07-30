#!/bin/sh
# Boot the VAX repeatedly from a pristine disk and report how often it succeeds.
#
# A boot that fails one time in three says something different from one that
# never works, and the difference is not visible in a single run. Each trial
# discards the copy-on-write overlay first, so every one starts from the same
# system disk and the trials are independent of each other.
#
#   ./tools/vax-boot-trials.sh 5            five trials, no ethernet
#   ./tools/vax-boot-trials.sh 5 --deuna    five trials with the DEUNA enabled
set -e
cd "$(dirname "$0")/.."

HOST=${QBONE_HOST:-unibone.huebner.org}
PW=$(cat ~/.qbone-pw 2>/dev/null || true)
TRIALS=${1:-5}
shift 2>/dev/null || true
SETUP_ARGS="$*"

LOGDIR=${TRIAL_LOGDIR:-/tmp/vax-trials}
mkdir -p "$LOGDIR"
pass=0
fail=0

i=1
while [ $i -le "$TRIALS" ]; do
    curl -s -m 25 -u ":$PW" -X PUT -H 'Content-Type: application/json' \
        -d '{"value":"0"}' \
        "http://$HOST/api/devices/cpuvax/params/enabled" >/dev/null || true
    sleep 2
    ./tools/vax-setup.sh --fresh $SETUP_ARGS >/dev/null

    log="$LOGDIR/trial-$i.log"
    if ./tools/vax-boot.sh > "$log" 2>&1; then
        pass=$((pass + 1))
        echo "trial $i: PASS"
    else
        fail=$((fail + 1))
        # The first VMS complaint is what distinguishes one failure from
        # another, and it has to come from this boot: the log opens with the
        # console channel's replayed history, which carries the diagnostics of
        # every boot before it.
        why=$(sed -n '/=== machine restarted ===/,$p' "$log" \
              | grep -a -m1 -E '%[A-Z]+-[EFW]-|-[A-Z]+-[EFW]-|Unable to locate' \
              | tr -d '\r' | cut -c1-90)
        echo "trial $i: FAIL  ${why:-no diagnostic on the console}"
    fi
    i=$((i + 1))
done

echo "----"
echo "$pass passed, $fail failed of $TRIALS (logs in $LOGDIR)"
