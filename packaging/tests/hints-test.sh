#!/bin/bash
# Host test of packaging/debian/qunilator-hints, the login hints.
#
# The script says what a freshly flashed board still lacks - the source tree
# ("sudo qunilator-devkit") and the disk images ("sudo qunilator-fetch-images")
# - and must go quiet, line by line, as each of them appears. Nothing is
# remembered between logins, so what is tested is exactly that: a board built
# out of directories here, and what the script says about it.
#
# The three absolute paths it reads (the media tree, the devkit record, the
# default tree location) are rewritten to point into the sandbox, and the
# rewrite is asserted - so a rename there fails this test rather than silently
# testing nothing, or reading the machine this runs on.
#
#   ./packaging/tests/hints-test.sh        run every case
#   ./packaging/tests/hints-test.sh -v     print each run's output

set -u

cd "$(dirname "$0")/../.."
REPO_ROOT=$(pwd)
SB=${TMPDIR:-/tmp}/qunilator-hints-test.$$

VERBOSE=0
[ "${1:-}" = "-v" ] && VERBOSE=1

trap 'rm -rf "$SB"' EXIT

PASS=0
FAIL=0
CASE=""

case_begin() { CASE=$1; }
fail() {
    FAIL=$((FAIL + 1))
    echo "FAIL  $CASE"
    echo "      $1"
    [ -n "${OUT:-}" ] && echo "      output: $OUT"
    return 0
}
ok() { PASS=$((PASS + 1)); echo "PASS  $CASE"; }

# ------------------------------------------------------------- the sandbox ----
IMAGES=$SB/var/lib/qunilator/images
ETC=$SB/etc/qunilator
TREE=$SB/root

mkdir -p "$SB" "$IMAGES" "$ETC" "$TREE"

sed -e "s#^IMAGES_DIR=/var/lib/qunilator/images#IMAGES_DIR=$IMAGES#" \
    -e "s#^DEVKIT_ENV=/etc/qunilator/devkit.env#DEVKIT_ENV=$ETC/devkit.env#" \
    -e "s#^DEFAULT_TREE=/root#DEFAULT_TREE=$TREE#" \
    "$REPO_ROOT/packaging/debian/qunilator-hints" > "$SB/qunilator-hints"
chmod +x "$SB/qunilator-hints"
for want in "IMAGES_DIR=$IMAGES" "DEVKIT_ENV=$ETC/devkit.env" "DEFAULT_TREE=$TREE"; do
    grep -q "^$want\$" "$SB/qunilator-hints" || {
        echo "sandbox: qunilator-hints no longer sets ${want%%=*} the way this" >&2
        echo "sandbox: test rewrites it - update hints-test.sh" >&2
        exit 2
    }
done

# The pack the package seeds a board with. It is not a fetch, so it must not
# count as one - the name is the script's own SAMPLE_IMAGE.
sample=$(sed -n 's/^SAMPLE_IMAGE=//p' "$REPO_ROOT/packaging/debian/qunilator-hints")
[ -n "$sample" ] || { echo "sandbox: qunilator-hints names no sample image" >&2; exit 2; }
mkdir -p "$IMAGES/dl" "$IMAGES/roms"
: > "$IMAGES/dl/$sample"
: > "$IMAGES/roms/23-248F1.lst"

# a development tree, made and unmade per case
make_tree() {
    mkdir -p "$1/10.03_app_demo/5_applications"
    : > "$1/qunibone-platform.env"
}
unmake_tree() { rm -rf "$1"; mkdir -p "$1"; }

run() {
    OUT=$("$SB/qunilator-hints" 2>&1)
    [ $VERBOSE = 1 ] && { echo "--- output:"; echo "$OUT"; }
    return 0
}

says_devkit()  { case "$OUT" in *"sudo qunilator-devkit"*) return 0 ;; esac; return 1; }
says_images()  { case "$OUT" in *"sudo qunilator-fetch-images"*) return 0 ;; esac; return 1; }

# ----------------------------------------------------------------- the runs ---
case_begin "a freshly flashed board is told about both"
run
rc=0
says_devkit || { fail "no devkit hint"; rc=1; }
says_images || { fail "no fetch-images hint"; rc=1; }
case "$OUT" in
    *"restore the old .sh files and source code tree"*) ;;
    *) fail "the devkit hint does not read as specified"; rc=1 ;;
esac
case "$OUT" in
    *"To fetch disk images for booting"*) ;;
    *) fail "the images hint does not read as specified"; rc=1 ;;
esac
[ $rc = 0 ] && ok

case_begin "the shipped sample pack and the ROM listings are not a fetch"
run
says_images && ok || fail "the seeded tree counted as fetched images"

case_begin "a tree in the default place silences the devkit hint"
make_tree "$TREE"
run
rc=0
says_devkit && { fail "still offering the devkit"; rc=1; }
says_images || { fail "the images hint went with it"; rc=1; }
[ $rc = 0 ] && ok

case_begin "a checkout made by hand is not a devkit run"
rm -f "$TREE/qunibone-platform.env"
run
says_devkit && ok || fail "a tree with no qunibone-platform.env counted as one"
make_tree "$TREE"

case_begin "a fetched image silences the images hint"
: > "$IMAGES/dl/rt11v5.5.rl02.dsk.gz"
run
rc=0
says_images && { fail "still offering the fetch"; rc=1; }
[ -n "$OUT" ] && { fail "a board that has both should say nothing: $OUT"; rc=1; }
[ $rc = 0 ] && ok

case_begin "a tree the operator put elsewhere is found through the record"
unmake_tree "$TREE"
ELSEWHERE=$SB/opt/qunilator
make_tree "$ELSEWHERE"
printf 'DEVKIT_DIR=%s\nDEVKIT_REF=v1.0.0\n' "$ELSEWHERE" > "$ETC/devkit.env"
run
says_devkit && fail "the recorded tree was not looked at" || ok

case_begin "a recorded tree this login may not enter is taken on the record"
# what /root is to an ordinary login: the directory is there, its contents are
# not readable. The record is then the answer, or every login but root's would
# be told to run the devkit on a board which has already had it.
chmod 700 "$ELSEWHERE"
if [ "$(id -u)" = 0 ]; then
    echo "SKIP  $CASE (root enters any directory)"
else
    rm -f "$ELSEWHERE/qunibone-platform.env"    # unreachable, not absent
    chmod 000 "$ELSEWHERE"
    run
    says_devkit && fail "the record was not taken on trust" || ok
    chmod 755 "$ELSEWHERE"
fi
make_tree "$ELSEWHERE"

case_begin "a recorded tree that has been deleted brings the hint back"
rm -rf "$ELSEWHERE"
run
says_devkit && ok || fail "silent about a tree which is no longer there"

case_begin "an unpacked image counts, a compressed one counts"
rm -f "$IMAGES/dl/rt11v5.5.rl02.dsk.gz"
run
says_images || fail "the media tree is bare again but nothing was said"
: > "$IMAGES/dl/rt11v5.5.rl02.dsk"
run
says_images && fail "an unpacked image did not count" || ok

echo
echo "hints test: $((PASS + FAIL)) cases, $PASS passed, $FAIL failed"
[ $FAIL = 0 ]
