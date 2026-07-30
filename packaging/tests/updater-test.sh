#!/bin/bash
# Host test of packaging/debian/qunilator-update, against a fake board.
#
# What this covers is the part of self-update that is hardest to arrange on real
# hardware and worst to get wrong: the failure paths. A package whose service
# will not start, a dpkg that dies mid-install, a repository that has gone away, a
# package system left half-configured by a power loss, a version the repository
# does not offer. Each of those decides whether an operator's board comes back or
# stays down, and none of them is something to discover on a board.
#
# It needs no board, no apt and no root: updater-sandbox.sh stubs the world. What
# it does not cover is the bus, the real apt, and the flock - so a green run here
# is a reason to try a board, not a substitute for it (see self-update-plan.md
# §14 for what the board still has to prove).
#
#   ./packaging/tests/updater-test.sh            run every case
#   ./packaging/tests/updater-test.sh -v         print each updater run's output

set -u

cd "$(dirname "$0")/../.."
REPO_ROOT=$(pwd)
SANDBOX_ROOT=${TMPDIR:-/tmp}/qunilator-updater-test.$$
export REPO_ROOT SANDBOX_ROOT

VERBOSE=0
[ "${1:-}" = "-v" ] && VERBOSE=1

# shellcheck source=packaging/tests/updater-sandbox.sh
. "$REPO_ROOT/packaging/tests/updater-sandbox.sh"

trap 'rm -rf "$SANDBOX_ROOT"' EXIT

PASS=0
FAIL=0
CASE=""

case_begin() {
    CASE=$1
    sandbox_reset
}

fail() {
    FAIL=$((FAIL + 1))
    echo "FAIL  $CASE"
    echo "      $1"
    [ -f "$SANDBOX_ROOT/state/status.json" ] \
        && echo "      status: $(cat "$SANDBOX_ROOT/state/status.json")"
    [ -n "${UPD_OUT:-}" ] && echo "      output: $UPD_OUT"
}

ok() {
    PASS=$((PASS + 1))
    echo "PASS  $CASE"
}

# expect <what> <want> <got>
expect() {
    [ "$2" = "$3" ] && return 0
    fail "$1: expected \"$2\", got \"$3\""
    return 1
}

expect_rc() {
    [ "$UPD_RC" = "$1" ] && return 0
    fail "exit status: expected $1, got $UPD_RC"
    return 1
}

expect_out() {
    case "$UPD_OUT" in
        *"$1"*) return 0 ;;
    esac
    fail "output does not mention \"$1\""
    return 1
}

run() {
    updater "$@"
    [ "$VERBOSE" = 1 ] && printf '      $ qunilator-update %s\n%s\n' "$*" "$UPD_OUT"
    return 0
}

echo "qunilator-update, against a fake board"
echo

sandbox_build || exit 1

# ---------------------------------------------------------------- the check ----

case_begin "check: reports installed and candidate"
run --check
expect_rc 0 &&
expect "state" idle "$(status_field state)" &&
expect "installed" 1.12.0-1 "$(status_field installed)" &&
expect "candidate" 1.13.0-1 "$(status_field candidate)" &&
expect "source_configured" true "$(status_bool source_configured)" &&
[ -n "$(status_field checked_at)" ] && ok || fail "checked_at was not stamped"

case_begin "check: a board with no apt source says so, and does not fail"
rm -f "$SANDBOX_ROOT/apt/sources.list.d/qbone.list"
run --check
expect_rc 0 &&
expect "source_configured" false "$(status_bool source_configured)" &&
expect "candidate" "" "$(status_field candidate)" &&
expect "error" "no update source configured" "$(status_field error)" && ok

case_begin "check: an unreachable repository is reported, not swallowed"
knob reachable no
run --check
expect_rc 1 &&
expect_out "could not be reached" &&
case "$(status_field error)" in
    *"could not be reached"*) ok ;;
    *) fail "the status file does not carry the reason" ;;
esac

case_begin "check: a candidate older than installed reports \"ahead\""
knob candidate 1.11.0-1
run --check
expect_rc 0 &&
expect "state" ahead "$(status_field state)" && ok

case_begin "check: an interrupted dpkg is reported"
knob audit "The following packages are only half configured: qbone"
run --check
expect_rc 0 &&
expect "needs_repair" true "$(status_bool needs_repair)" && ok

case_begin "check: the other packages are listed, the emulator's own excluded"
cat > "$SANDBOX_ROOT/upgradable" <<'EOF'
libc6/trixie 2.41-8 armhf [upgradable from: 2.41-7]
qbone/trixie 1.13.0-1 armhf [upgradable from: 1.12.0-1]
libstdc++6/trixie-security 14.3.0-1 armhf [upgradable from: 14.2.0-1]
EOF
printf 'linux-image-6.12.93-bone63\nqbone\n' > "$SANDBOX_ROOT/holds"
run --check
os=$(sed -n 's/.*"os":{\([^}]*\)}.*/\1/p' "$SANDBOX_ROOT/state/status.json")
count=$(sed -n 's/.*"count":\([0-9]*\).*/\1/p' "$SANDBOX_ROOT/state/status.json" | head -1)
expect_rc 0 &&
expect "os.count" 2 "$count" &&
case "$(cat "$SANDBOX_ROOT/state/status.json")" in
    *'"name":"qbone"'*) fail "the emulator package is in its own OS list" ;;
    *'"name":"libc6"'*)
        case "$(cat "$SANDBOX_ROOT/state/status.json")" in
            *'"held_back":["linux-image-6.12.93-bone63"]'*) ok ;;
            *) fail "the kernel hold is not reported (os: $os)" ;;
        esac ;;
    *) fail "libc6 is not in the OS list (os: $os)" ;;
esac

# ------------------------------------------------------------ the changelog ----

case_begin "changelog: the stanzas newer than installed, and no older ones"
run --changelog
expect_rc 0 &&
expect_out "qbone (1.13.0-1)" &&
case "$UPD_OUT" in
    *"1.12.0-1"*) fail "a stanza at or below the installed version was included" ;;
    *) ok ;;
esac

# --------------------------------------------------------------- installing ----

case_begin "install: a good update lands and is recorded"
run --install 1.13.0-1
expect_rc 0 &&
expect "state" done "$(status_field state)" &&
expect "installed" 1.13.0-1 "$(status_field installed)" &&
expect "last.from" 1.12.0-1 "$(status_last from)" &&
expect "last.to" 1.13.0-1 "$(status_last to)" &&
expect "rollback offered" true "$(status_bool rollback)" && ok

case_begin "install: a version the repository does not offer is refused"
run --install 9.9.9-1
expect_rc 1 &&
expect_out "not available" &&
expect "installed" 1.12.0-1 "$(status_field installed)" &&
expect "state" failed "$(status_field state)" && ok

case_begin "install: a new version that will not start is rolled back"
knob broken_version 1.13.0-1
run --install 1.13.0-1
expect_rc 1 &&
expect "state" rolled-back "$(status_field state)" &&
expect "installed" 1.12.0-1 "$(status_field installed)" &&
expect "last.from" 1.13.0-1 "$(status_last from)" &&
expect "last.to" 1.12.0-1 "$(status_last to)" &&
case "$(cat "$SANDBOX_ROOT/state/status.json")" in
    *'"journal":['*'qbone['*) ok ;;
    *) fail "the journal tail was not recorded for the interface to show" ;;
esac

case_begin "install: a dpkg that dies leaves the running version installed"
knob install_fails yes
run --install 1.13.0-1
expect_rc 1 &&
expect "state" failed "$(status_field state)" &&
expect "installed" 1.12.0-1 "$(status_field installed)" &&
expect_out "the install failed" && ok

case_begin "install: an unreachable repository is refused before dpkg"
knob reachable no
run --install 1.13.0-1
expect_rc 1 &&
expect "installed" 1.12.0-1 "$(status_field installed)" &&
expect "state" failed "$(status_field state)" && ok

case_begin "install: a package system needing repair is repaired first"
knob audit "The following packages are only half configured: qbone"
run --install 1.13.0-1
expect_rc 0 &&
expect "state" done "$(status_field state)" &&
expect "needs_repair" false "$(status_bool needs_repair)" &&
expect_out "repairing an interrupted install" && ok

case_begin "install: a repair that will not take refuses the install"
knob audit "The following packages are only half configured: qbone"
knob repair_fails yes
run --install 1.13.0-1
expect_rc 1 &&
expect "installed" 1.12.0-1 "$(status_field installed)" &&
expect_out "needs repair" && ok

case_begin "install-requested: the version comes from the request file"
printf '{"version":"1.13.0-1"}\n' > "$SANDBOX_ROOT/state/request.json"
run --install-requested
expect_rc 0 &&
expect "installed" 1.13.0-1 "$(status_field installed)" &&
[ ! -f "$SANDBOX_ROOT/state/request.json" ] && ok \
    || fail "the request file was left behind for the next run to repeat"

case_begin "install-requested: no request is not an install"
run --install-requested
expect_rc 1 && expect_out "no update requested" && ok

# ----------------------------------------------------------------- rollback ----

case_begin "rollback: steps back to the cached version"
run --install 1.13.0-1
run --rollback
expect_rc 0 &&
expect "state" rolled-back "$(status_field state)" &&
expect "installed" 1.12.0-1 "$(status_field installed)" &&
expect "last.from" 1.13.0-1 "$(status_last from)" &&
expect "last.to" 1.12.0-1 "$(status_last to)" && ok

case_begin "rollback: refused when the cache holds the installed version"
run --rollback
expect_rc 1 && expect_out "no cached package" && ok

case_begin "rollback: a check does not forget that one is possible"
run --install 1.13.0-1
before=$(status_bool rollback)
run --check
expect "rollback before the check" true "$before" &&
expect "rollback after the check" true "$(status_bool rollback)" && ok

# --------------------------------------------------------------- OS upgrade ----

case_begin "os-upgrade: upgrades the others and reports the reboot notice"
cat > "$SANDBOX_ROOT/upgradable" <<'EOF'
libc6/trixie 2.41-8 armhf [upgradable from: 2.41-7]
EOF
touch "$SANDBOX_ROOT/run/reboot-required"
run --os-upgrade
expect_rc 0 &&
expect "state" done "$(status_field state)" &&
expect "installed" 1.12.0-1 "$(status_field installed)" &&
case "$(cat "$SANDBOX_ROOT/state/status.json")" in
    *'"reboot_required":true'*) ok ;;
    *) fail "the reboot notice was not carried into the status" ;;
esac

case_begin "os-upgrade: a failure is reported and the emulator is untouched"
knob upgrade_fails yes
run --os-upgrade
expect_rc 1 &&
expect "state" failed "$(status_field state)" &&
expect "installed" 1.12.0-1 "$(status_field installed)" && ok

# ------------------------------------------------------------------- status ----

case_begin "status: prints the file, and makes one when there is none"
rm -f "$SANDBOX_ROOT/state/status.json"
run --status
expect_rc 0 &&
case "$UPD_OUT" in
    *'"package":"qbone"'*) ok ;;
    *) fail "the status output is not the status document" ;;
esac

case_begin "an unknown option is refused"
run --wat
expect_rc 1 && expect_out "unknown option" && ok

echo
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] || exit 1
