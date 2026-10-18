#!/bin/bash
#
# hil-os-upgrade.sh: the operating-system upgrade on the UniBone, as an
# operator runs it from the Updates page
#
# Runs on the CI runner on netmbx after hil-unibone.sh has installed the build
# under test (.github/workflows/hil.yml). It asks the board to check for
# updates, and when the BeagleBone's other packages have some, starts the
# upgrade through the web API and follows it the way the page does: the status
# must parse at every read, report progress while apt works, and keep moving;
# the service must answer throughout, since the upgrade leaves the emulator
# running; and afterwards nothing the button could upgrade may be left.
#
#   tools/ci/hil-os-upgrade.sh <output dir>
#
# Reads, as the runner provides them:
#   QUNILATOR_CI_HOST            the board (default unibone.huebner.org)
#   ~/.qbone-user, ~/.qbone-pw   the board's web login
#
# The output directory receives the status as read at every poll and the
# upgrade's log. Failures are GitHub error annotations and job summary lines.

set -u -o pipefail

out=${1:?usage: $0 <output dir>}
mkdir -p "$out"

host=${QUNILATOR_CI_HOST:-unibone.huebner.org}
user=$(cat ~/.qbone-user 2>/dev/null || true)
pw=$(cat ~/.qbone-pw 2>/dev/null || true)
api=http://$host/api
summary=${GITHUB_STEP_SUMMARY:-/dev/null}

# A check refreshes every apt source on a BeagleBone: minutes, not seconds.
CHECK_LIMIT=900
# The whole upgrade, and how long its status may stand still. apt reports a
# step or a log line every few seconds; the longest quiet stretch is the
# initramfs rebuild, about a minute and a half.
UPGRADE_LIMIT=3600
STALL_LIMIT=900

note() { printf '%s\n' "$*"; }

fail() {
    local title=$1; shift
    printf '::error title=%s::%s\n' "$title" "$*"
    { printf '### %s\n\n%s\n' "$title" "$*"; } >> "$summary"
    exit 1
}

pass() {
    note "ok: $*"
    printf -- '- %s\n' "$*" >> "$summary"
}

# GET /api/update into $u, or fail: the status must parse at every read - a
# file the service cannot parse leaves the page showing a state long gone.
# Returns 1 when the board did not answer at all.
read_status() {
    local answer code
    answer=$(curl -s -m 15 -u "$user:$pw" -w '\n%{http_code}' "$api/update")
    code=${answer##*$'\n'}
    u=${answer%$'\n'*}
    [ "$code" = 200 ] || return 1
    printf '%s\n' "$u" >> "$out/update-status.jsonl"
    printf '%s' "$u" | jq -e . > /dev/null 2>&1 \
        || fail "Update status does not parse" "GET /api/update answered something that is not JSON: $(printf '%s' "$u" | head -c 300)"
    return 0
}

field() { printf '%s' "$u" | jq -r "$1"; }

post() {
    curl -s -m 30 -u "$user:$pw" -X POST -o /dev/null -w '%{http_code}' "$api$1"
}

[ -n "$user" ] && [ -n "$pw" ] || fail "No board login" \
    "~/.qbone-user and ~/.qbone-pw are missing on $(hostname)."

printf '\n## Operating-system upgrade on %s\n\n' "$host" >> "$summary"

# --- the check ---------------------------------------------------------------

read_status || fail "unibone unreachable" "GET /api/update on $host did not answer."
before=$(field .checked_at)
code=$(post /update/check)
[ "$code" = 202 ] || fail "Check refused" "POST /api/update/check answered HTTP $code."
start=$SECONDS
while :; do
    sleep 5
    if read_status && [ "$(field .state)" != checking ] && [ "$(field .checked_at)" != "$before" ]; then
        break
    fi
    [ $((SECONDS - start)) -lt $CHECK_LIMIT ] || fail "Check did not finish" \
        "The update check on $host was still running after $((CHECK_LIMIT / 60)) minutes (state $(field .state))."
done
[ -z "$(field .error)" ] || fail "Check failed" "The update check reported: $(field .error)"
count=$(field '.os.count')
kept=$(field '.os.kept_back // [] | join(", ")')
pass "checked in $((SECONDS - start)) s: $count package(s) to upgrade${kept:+, kept back: $kept}"

if [ "$count" = 0 ]; then
    pass "nothing to upgrade tonight"
    exit 0
fi
field '.os.packages[] | "\(.name) \(.from) -> \(.to)"' > "$out/os-packages.txt"

# --- the upgrade ---------------------------------------------------------------

code=$(post /update/os)
[ "$code" = 202 ] || fail "Upgrade refused" "POST /api/update/os answered HTTP $code."

# The unit starts with --no-block; the state the last operation left stands
# until the upgrade writes its own.
start=$SECONDS
until read_status && [ "$(field .state)" = os-upgrading ]; do
    [ $((SECONDS - start)) -lt 120 ] || fail "Upgrade did not start" \
        "Two minutes after POST /api/update/os the state is still $(field .state)."
    sleep 2
done

start=$SECONDS
moved=$SECONDS
last=""
silent=0
saw_percent=false
while :; do
    sleep 5
    if ! read_status; then
        silent=$((silent + 1))
        continue
    fi
    state=$(field .state)
    [ "$state" = os-upgrading ] || break
    [ "$(field '.progress | type')" = object ] || fail "No upgrade progress" \
        "The status of a running upgrade carries no progress object."
    [ "$(field '.progress.percent | type')" = number ] && saw_percent=true
    now=$(field '[.progress, (.journal | last)] | tostring')
    if [ "$now" != "$last" ]; then
        last=$now
        moved=$SECONDS
    fi
    [ $((SECONDS - moved)) -lt $STALL_LIMIT ] || fail "Upgrade stalled" \
        "The upgrade's progress and log stood still for $((STALL_LIMIT / 60)) minutes at: $(field '.progress.step') $(field '.progress.percent')% $(field '.progress.message')"
    [ $((SECONDS - start)) -lt $UPGRADE_LIMIT ] || fail "Upgrade did not finish" \
        "The upgrade was still running after $((UPGRADE_LIMIT / 60)) minutes."
done
took=$((SECONDS - start))
field '.os_last.journal // [] | .[]' > "$out/os-upgrade.log"

[ "$state" = done ] && [ "$(field .os_last.state)" = done ] || fail "Upgrade failed" \
    "The upgrade ended in state $state: $(field .os_last.error // .error). The log is os-upgrade.log in the run's artifacts."
[ "$silent" = 0 ] || fail "Service went away during the upgrade" \
    "The web service failed to answer $silent poll(s) during the upgrade, which promises to leave the emulator running."
$saw_percent || fail "No upgrade percentage" \
    "The upgrade never reported apt's percentage, so the page showed no progress bar."
pass "upgraded $count package(s) in $((took / 60)) min $((took % 60)) s; the service answered throughout"

left=$(field '.os.count')
[ "$left" = 0 ] || fail "Packages left after the upgrade" \
    "After the upgrade $left package(s) still count as upgradable, so the page offers an upgrade that does nothing: $(field '[.os.packages[].name] | join(", ")')"
[ "$(field .os.reboot_required)" = true ] && pass "the upgrade asks for a reboot"
pass "nothing left that the upgrade would take"
exit 0
