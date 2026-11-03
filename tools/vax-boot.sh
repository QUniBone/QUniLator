#!/bin/sh
# Boot the emulated VAX and leave it at a DCL prompt, logged in as SYSTEM.
#
# The console has to be listened to from the moment the machine starts, not
# picked up once the boot is expected to be over: VMS asks for the date and
# time and re-asks every thirty seconds until it is answered. qcon attaches to
# the console first and only then throws the start switch, so everything it
# matches belongs to this boot. The dialog is console-harness's CI script, the
# one the release gate boots VMS with.
#
#   ./tools/vax-boot.sh                 restart and boot
#   ./tools/vax-boot.sh --no-restart    just drive a boot already under way
#
# The console output goes to stderr, qcon's verdict (PASS/FAIL and the step)
# to stdout, and the session is recorded to $VAX_BOOT_CAST, by default a cast
# in /tmp. The date given to VMS is this machine's, so the guest's clock agrees
# with the one on the wall.
set -e
cd "$(dirname "$0")/../console-harness"

HOST=${QBONE_HOST:-unibone.huebner.org}
# VMS wants DD-MMM-YYYY HH:MM, and its month names are upper case.
VMSDATE=$(date "+%d-%b-%Y %H:%M" | tr '[:lower:]' '[:upper:]')
CAST=${VAX_BOOT_CAST:-/tmp/vax-boot-$(date +%Y%m%dT%H%M%S).cast}

START=restart
[ "$1" = "--no-restart" ] && START=none

[ -f dist/src/cli.js ] || { npm ci --silent && npm run build --silent; } >&2

exec node dist/src/cli.js run scripts/unibone-ci-vms.yaml \
    --host "$HOST" --pw-file "$HOME/.qbone-pw" --start "$START" \
    --var VMS_DATE="$VMSDATE" --record "$CAST" --verbose
