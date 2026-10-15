#!/bin/bash
#
# hil-unibone.sh: the hardware-in-the-loop test on the UniBone
#
# Runs on the CI runner on netmbx (.github/workflows/hil.yml), against the
# UniBone on the same LAN. It installs the package under test on the board,
# then does what an operator does to get VMS running: uploads the system disk
# through the web API, imports a configuration that names it, applies it,
# switches the machine on and boots VMS to a DCL prompt. On the way it checks
# that the board refuses an upload cut off halfway and a drive given an image
# it does not have.
#
#   tools/ci/hil-unibone.sh <unibone_*.deb> <output dir>
#
# Reads, as the runner provides them:
#   QUNILATOR_CI_HOST       the board (default unibone.huebner.org)
#   QUNILATOR_CI_VMS_IMAGE  the VMS V4.7 system disk on this host
#   ~/.qbone-user, ~/.qbone-pw   the board's web login
#   ~/.ssh/id_ed25519       the key ci-deploy@<board> answers to
#
# The output directory receives the console recording and the board's journal
# excerpt. Every failure is reported as a GitHub error annotation and in the
# job summary, naming what went wrong in words an operator can act on.

set -u -o pipefail

deb=${1:?usage: $0 <unibone .deb> <output dir>}
out=${2:?usage: $0 <unibone .deb> <output dir>}
mkdir -p "$out"
out=$(cd "$out" && pwd)   # qcon runs from console-harness/

host=${QUNILATOR_CI_HOST:-unibone.huebner.org}
image=${QUNILATOR_CI_VMS_IMAGE:-}
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
user=$(cat ~/.qbone-user 2>/dev/null || true)
pw=$(cat ~/.qbone-pw 2>/dev/null || true)
api=http://$host/api
summary=${GITHUB_STEP_SUMMARY:-/dev/null}
# Each run imports under its own name: a configuration that is current cannot
# be deleted, and a run that failed may have left its own one current.
config=ci-vms47-$(date +%Y%m%d%H%M%S)
ci_image=ci/vms47.dsk

note() { printf '%s\n' "$*"; }

# One line for the annotation, the same words in the summary.
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

# curl against the board's API, in this shell: leaves the answer in $body and
# its HTTP status in $status (000 when the board did not answer).
call() {
    local method=$1 path=$2; shift 2
    local answer
    answer=$(curl -s -m 600 -u "$user:$pw" -X "$method" -w '\n%{http_code}' "$@" "$api$path")
    status=${answer##*$'\n'}
    body=${answer%$'\n'*}
    [ "$body" != "$answer" ] || body=
}

[ -f "$deb" ] || fail "No package" "$deb does not exist; the build job did not hand one over."
[ -n "$image" ] && [ -r "$image" ] || fail "No VMS system disk" \
    "QUNILATOR_CI_VMS_IMAGE (${image:-unset}) is not readable on $(hostname). roles/github-runner in netzhansa-infra puts it there."
[ -n "$user" ] && [ -n "$pw" ] || fail "No board login" \
    "~/.qbone-user and ~/.qbone-pw are missing on $(hostname). roles/github-runner in netzhansa-infra writes them."

printf '## Hardware test on %s\n\n' "$host" >> "$summary"

# --- is the board there? ----------------------------------------------------

if [[ $host =~ ^[0-9.]+$ ]]; then
    addr=$host
else
    addr=$(getent hosts "$host" | awk '{print $1; exit}')
fi
[ -n "$addr" ] || fail "unibone unreachable" \
    "$host does not resolve from $(hostname). The board is off or has no DHCP lease, so it has no DNS name. Power it up and check it joins the LAN."

http=$(curl -s -m 10 -o /dev/null -w '%{http_code}' "http://$host/api/version")
case "$http" in
    000) fail "unibone unreachable" \
        "$host ($addr) does not answer HTTP on port 80 from $(hostname). The board is off, still booting, or its service is down (systemctl status unibone on the board)." ;;
    401) ;; # answers, and wants the login - which is what comes next
    *)   fail "unibone answers oddly" "GET /api/version on $host ($addr) answered HTTP $http." ;;
esac
call GET /version
[ "$status" = 200 ] || fail "unibone login refused" \
    "$host answers but refuses the login of user '$user' (HTTP $status). The board's operator or password changed; update qunilator_ci_board_* in netzhansa-infra and re-run roles/github-runner."
pass "board $host ($addr) answers, running $(printf '%s' "$body" | jq -r '.version')"

# --- install the build under test -------------------------------------------

want=$(dpkg-deb -f "$deb" Version)
note "installing $(basename "$deb") on $host"
if ! ssh -T -i ~/.ssh/id_ed25519 -o BatchMode=yes -o ConnectTimeout=15 \
        -o StrictHostKeyChecking=accept-new "ci-deploy@$host" < "$deb" > "$out/deploy.log" 2>&1; then
    cat "$out/deploy.log"
    if grep -q "Permission denied\|Host key verification failed\|REMOTE HOST IDENTIFICATION" "$out/deploy.log"; then
        fail "unibone deploy refused" \
            "ci-deploy@$host refused the runner's key or its host key changed - the board was probably reflashed. Run playbooks/qunilator-ci.yml in netzhansa-infra (and clear the old host key from ~ghrunner/.ssh/known_hosts on $(hostname))."
    fi
    fail "unibone deploy failed" \
        "Installing $want on $host failed: $(tail -n 2 "$out/deploy.log" | tr '\n' ' ')"
fi
cat "$out/deploy.log"

# The package restarts the service; wait for it to answer with the new build.
v=
for _ in $(seq 1 60); do
    call GET /version
    v=$(printf '%s' "$body" | jq -r '.version' 2>/dev/null || true)
    [ "$status" = 200 ] && [ "$v" = "$want" ] && break
    sleep 2
done
[ "$v" = "$want" ] || fail "unibone did not come back" \
    "After installing $want the service on $host answers '${v:-nothing}' (HTTP $status) after two minutes."
pass "installed $want"

call GET /configs
previous=$(printf '%s' "$body" | jq -r '.current // empty')
stale=$(printf '%s' "$body" | jq -r '.configs[].name | select(startswith("ci-vms47-"))')
case "$previous" in ci-vms47-*) previous= ;; esac

# Whatever happens from here, the board goes back to the configuration it had.
restore() {
    call POST /control -H 'Content-Type: application/json' -d '{"action":"dc_off"}'
    if [ -n "$previous" ]; then
        call POST "/configs/$previous/apply"
        note "restored configuration $previous"
    fi
}
trap restore EXIT

# Switched off, the machine holds no image open, so the CI image can be
# replaced; the configurations earlier runs left are removed with it.
call POST /control -H 'Content-Type: application/json' -d '{"action":"dc_off"}'
for c in $stale; do call DELETE "/configs/$c"; done
call DELETE "/images/$ci_image"

# --- the board refuses what it cannot use -----------------------------------

call PUT /devices/uda1/params/image -H 'Content-Type: application/json' \
    -d '{"value":"ci/does-not-exist.dsk"}'
[ "$status" = 404 ] || fail "Missing image accepted" \
    "PUT of an image that does not exist answered HTTP $status ($body) instead of 404."
pass "a drive is refused an image the board does not have"

msg=$(python3 "$here/cut-upload.py" "$host" "$user" "$pw" ci/cut.dsk) \
    || fail "Cut-off upload accepted" "$msg"
call DELETE /images/ci/cut.dsk
[ "$status" = 404 ] || fail "Cut-off upload left a file" \
    "An upload cut off halfway left images/ci/cut.dsk behind."
pass "an upload cut off halfway is refused and leaves no file"

# --- what an operator does: upload, import, apply, boot ---------------------

size=$(stat -c %s "$image")
note "uploading $(basename "$image") ($size bytes) as images/$ci_image"
call POST "/images?dir=ci" -F "file=@$image;filename=vms47.dsk"
[ "$status" = 200 ] || fail "Image upload failed" "Uploading the VMS system disk answered HTTP $status: $body"
call GET /images
got=$(printf '%s' "$body" | jq -r --arg p "$ci_image" '[.. | objects | select(.path? == $p) | .size][0] // empty')
[ "$got" = "$size" ] || fail "Image upload truncated" \
    "The board holds images/$ci_image with ${got:-no} bytes; $size were sent."
pass "uploaded the system disk, $size bytes"

call POST "/configs/$config/import" -H 'Content-Type: application/json' \
    --data @"$here/vax780-vms47.qcfg.json"
[ "$status" = 200 ] || fail "Import refused" "Importing $config answered HTTP $status: $body"
call POST "/configs/$config/apply"
errors=$(printf '%s' "$body" | jq -r '.errors | join("; ")' 2>/dev/null)
[ "$status" = 200 ] && [ -z "$errors" ] || fail "Apply refused" \
    "Applying $config answered HTTP $status: ${errors:-$body}"
call POST /control -H 'Content-Type: application/json' -d '{"action":"dc_on"}'
[ "$status" = 200 ] || fail "Power on refused" "dc_on answered HTTP $status."
pass "imported and applied $config"

vms_date=$(date '+%d-%b-%Y %H:%M' | tr '[:lower:]' '[:upper:]')
note "booting VMS"
(cd "$root/console-harness" && node dist/src/cli.js run scripts/unibone-ci-vms.yaml \
    --host "$host" --pw-file ~/.qbone-pw --var VMS_DATE="$vms_date" \
    --record "$out/vms-boot.cast") 2>&1 | tee "$out/qcon.log"
booted=${PIPESTATUS[0]}

# The board's own account of the run, whichever way it went.
call GET "/log?limit=400"
printf '%s' "$body" > "$out/board-log.json"

# qcon names the step that failed on a line starting FAIL.
[ "$booted" = 0 ] || fail "VMS did not boot" \
    "$(grep -m1 '^FAIL' "$out/qcon.log" || tail -n 1 "$out/qcon.log") - the console recording vms-boot.cast is in the run's artifacts."
pass "VMS V4.7 booted to a DCL prompt and logged in"

exit 0
