#!/bin/sh
# Boot the emulated VAX and leave it at a DCL prompt, logged in.
#
# The console has to be listened to from the moment the machine starts, not
# picked up once the boot is expected to be over: VMS asks for the date and
# time and re-asks every thirty seconds until it is answered, so a procedure
# that sleeps and then connects finds a screen full of repeated prompts and a
# system that has not started. So this attaches first and waits for the prompt,
# however long the boot takes.
#
#   ./tools/vax-boot.sh                 restart and boot
#   ./tools/vax-boot.sh --no-restart    just drive a boot already under way
#
# The date given to VMS is this machine's, so the guest's clock agrees with the
# one on the wall.
set -e
cd "$(dirname "$0")/.."

HOST=${QBONE_HOST:-unibone.huebner.org}
PW=$(cat ~/.qbone-pw 2>/dev/null || true)
USERNAME=${VMS_USERNAME:-SYSTEM}
PASSWORD=${VMS_PASSWORD:-MANAGER}
# VMS wants DD-MMM-YYYY HH:MM, and its month names are upper case.
VMSDATE=$(date "+%d-%b-%Y %H:%M" | tr '[:lower:]' '[:upper:]')

if [ "$1" != "--no-restart" ]; then
    curl -s -m 25 -u ":$PW" -X PUT -H 'Content-Type: application/json' \
        -d '{"value":"1"}' \
        "http://$HOST/api/devices/cpuvax/params/start_switch" >/dev/null
    echo "machine restarted; watching the console"
fi

# One connection for the whole boot: the date prompt, the startup, and the
# login that follows it. The prompt can be twenty minutes away on a traced or
# heavily loaded board, so the wait is generous.
exec node tools/vax-console.mjs \
    --timeout 1200000 \
    --expect 'PLEASE ENTER DATE AND TIME' --send "$VMSDATE" \
    --expect 'SYSTEM       job terminated' \
    --timeout 120000 \
    --wait 3000 --send '' \
    --expect 'Username:' --send "$USERNAME" \
    --expect 'Password:' --send-hidden "$PASSWORD" \
    --expect '\$'
