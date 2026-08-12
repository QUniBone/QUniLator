#!/bin/bash
# Host test of packaging/debian/qunilator-devkit, against a fake board.
#
# The script it tests turns an appliance into a development board: it fetches
# the repository into /root and personalizes it. Two things there are worth a
# test rather than a board. The first is that it populates a directory which is
# *not* empty - /root always holds the shell's dotfiles, which is why the script
# fetches into a repository created in place instead of running "git clone"
# there - and must leave what it did not put there alone. The second is the
# personalization: the bus-specific examples merged into one tree, a shortcut in
# /root for each example, and no shortcut written over a file which is not one.
#
# The board is stubbed: a dpkg-query which answers about a fake installed
# package, an "id" which says root, a PRU compiler which is two empty files, and
# an origin repository built here out of the parts of this tree the script
# touches. Nothing installs, nothing reaches the network: the run is --no-apt.
#
# The shipped script runs as it ships, apart from the one absolute path that
# would otherwise reach outside the sandbox (the PRU tools directory), which is
# rewritten and asserted - so a rename there fails this test rather than
# silently stopping it from testing anything.
#
#   ./packaging/tests/devkit-test.sh        run every case
#   ./packaging/tests/devkit-test.sh -v     print each run's output

set -u

cd "$(dirname "$0")/../.."
REPO_ROOT=$(pwd)
SB=${TMPDIR:-/tmp}/qunilator-devkit-test.$$

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

# expect <what> <want> <got>
expect() {
    [ "$2" = "$3" ] && return 0
    fail "$1: expected \"$2\", got \"$3\""
    return 1
}

# ------------------------------------------------------------- the sandbox ----
# The origin repository: the files qunilator-devkit and qunibone-platform.sh
# work on, taken from this tree, so the test runs against the real
# personalization script and the real example trees rather than a mock of them.
build_origin() {
    local src=$SB/origin
    mkdir -p "$src"
    ( cd "$REPO_ROOT" && tar -c \
        qunibone-platform.sh qunibone-platform.env.example \
        compile.sh compile-bbb.env packaging/version.sh packaging/board.sh \
        packaging/debian/changelog \
        10.03_app_demo/5_applications 10.03_app_demo/5_applications_q \
        10.03_app_demo/5_applications_u ) | ( cd "$src" && tar -x )
    git -C "$src" init -q
    git -C "$src" -c user.email=t@t -c user.name=t add -A
    git -C "$src" -c user.email=t@t -c user.name=t commit -qm "test tree"
    git -C "$src" tag v99.0.0
}

# The board: stubs on PATH, and the script with its one outside path moved in.
build_stubs() {
    mkdir -p "$SB/bin" "$SB/cgt/bin"
    : > "$SB/cgt/bin/clpru"
    : > "$SB/cgt/bin/hexpru"
    chmod +x "$SB/cgt/bin/clpru" "$SB/cgt/bin/hexpru"

    # the emulator package this board is supposed to carry, set per case
    echo qbone > "$SB/pkg"
    echo 99.0.0-1 > "$SB/version"

    cat > "$SB/bin/dpkg-query" <<'EOF'
#!/bin/sh
# stub: answers about the one package the sandbox pretends is installed
pkg=$(cat "$SB_DIR/pkg"); ver=$(cat "$SB_DIR/version")
fmt=""; want=""
while [ $# -gt 0 ]; do
    case "$1" in
        -W) ;;
        -f) fmt=$2; shift ;;
        *) want=$1 ;;
    esac
    shift
done
[ "$want" = "$pkg" ] || exit 1
case "$fmt" in
    *Status*) echo "install ok installed" ;;
    *Version*) echo "$ver" ;;
esac
EOF
    # "root", without being root
    cat > "$SB/bin/id" <<'EOF'
#!/bin/sh
[ "${1:-}" = "-u" ] && { echo 0; exit 0; }
exec /usr/bin/id "$@"
EOF
    chmod +x "$SB/bin/dpkg-query" "$SB/bin/id"

    sed -e "s#^PRU_CGT_DIR=/usr/share/ti/cgt-pru#PRU_CGT_DIR=$SB/cgt#" \
        "$REPO_ROOT/packaging/debian/qunilator-devkit" > "$SB/qunilator-devkit"
    chmod +x "$SB/qunilator-devkit"
    grep -q "^PRU_CGT_DIR=$SB/cgt\$" "$SB/qunilator-devkit" || {
        echo "sandbox: qunilator-devkit no longer sets PRU_CGT_DIR the way this" >&2
        echo "sandbox: test rewrites it - update devkit-test.sh" >&2
        exit 2
    }
}

# devkit <target dir> [extra args...]
devkit() {
    local dir=$1
    shift
    OUT=$(SB_DIR=$SB PATH="$SB/bin:$PATH" "$SB/qunilator-devkit" \
        --no-apt --url "$SB/origin" --dir "$dir" "$@" 2>&1)
    DEVKIT_RC=$?
    [ $VERBOSE = 1 ] && echo "$OUT"
    return $DEVKIT_RC
}

# ----------------------------------------------------------------- the runs ---
mkdir -p "$SB"
build_origin
build_stubs

ROOT=$SB/root
mkdir -p "$ROOT"
# what a real /root holds before any of this: dotfiles, and one file which
# happens to carry the name of an example shortcut
echo "the operator's shell" > "$ROOT/.bashrc"
echo "not a shortcut" > "$ROOT/memory.sh"

case_begin "a QBUS board is populated from an empty-but-for-dotfiles /root"
if ! devkit "$ROOT"; then
    fail "the run failed (exit $DEVKIT_RC)"
else
    rc=0
    [ -f "$ROOT/compile.sh" ] || { fail "no compile.sh in the tree"; rc=1; }
    [ -f "$ROOT/.bashrc" ] || { fail ".bashrc was removed"; rc=1; }
    expect "the dotfile is untouched" "the operator's shell" "$(cat "$ROOT/.bashrc")" || rc=1
    expect "checked out the tag matching the installed version" "v99.0.0" \
        "$(git -C "$ROOT" describe --tags --exact-match 2>/dev/null)" || rc=1
    [ $rc = 0 ] && ok
fi

case_begin "the platform file names this board's bus"
rc=0
grep -q '^QUNILATOR_PLATFORM_SUFFIX=_q$' "$ROOT/qunibone-platform.env" \
    || { fail "no QBUS suffix in qunibone-platform.env"; rc=1; }
grep -q '^QUNILATOR_PLATFORM=QBUS$' "$ROOT/qunibone-platform.env" \
    || { fail "no QBUS platform in qunibone-platform.env"; rc=1; }
[ $rc = 0 ] && ok

case_begin "the QBUS examples are merged into one tree"
rc=0
apps=$ROOT/10.03_app_demo/5_applications
[ -f "$apps/211bsd.mscp/211BSD_du0_73.sh" ] || { fail "no QBUS example in 5_applications"; rc=1; }
[ -f "$apps/memory/memory.sh" ] || { fail "no common example in 5_applications"; rc=1; }
[ -e "$ROOT/10.03_app_demo/5_applications_q" ] && { fail "5_applications_q still there"; rc=1; }
[ -e "$ROOT/10.03_app_demo/5_applications_u" ] && { fail "5_applications_u still there"; rc=1; }
[ -f "$apps/cpu20/cpu20_hello.sh" ] && { fail "a UNIBUS example landed on a QBUS board"; rc=1; }
[ $rc = 0 ] && ok

case_begin "every example has a shortcut in the tree root, and each one resolves"
rc=0
count=0
while read -r script; do
    link=$ROOT/$(basename "$script")
    count=$((count + 1))
    [ -L "$link" ] || { fail "$link is not a symbolic link"; rc=1; break; }
    [ -f "$link" ] || { fail "$link does not resolve"; rc=1; break; }
done < <(find "$apps" -name '*.sh' ! -name memory.sh)
[ "$count" -gt 10 ] || { fail "only $count examples found; the tree looks wrong"; rc=1; }
[ $rc = 0 ] && ok

case_begin "a file which is not a shortcut is never replaced by one"
rc=0
[ -L "$ROOT/memory.sh" ] && { fail "the operator's memory.sh was replaced by a link"; rc=1; }
expect "the file is as it was" "not a shortcut" "$(cat "$ROOT/memory.sh")" || rc=1
[ $rc = 0 ] && ok

case_begin "the examples and the tree's own scripts are executable"
rc=0
[ -x "$apps/memory/zkma.sh" ] || { fail "an example is not executable"; rc=1; }
[ -x "$ROOT/compile.sh" ] || { fail "compile.sh is not executable"; rc=1; }
[ $rc = 0 ] && ok

case_begin "4_deploy points at the directory of this board's bus"
expect "4_deploy" "$ROOT/10.03_app_demo/4_deploy_q" \
    "$(readlink "$ROOT/10.03_app_demo/4_deploy")" && ok

case_begin "a second run changes nothing and still succeeds"
if ! devkit "$ROOT"; then
    fail "the second run failed (exit $DEVKIT_RC)"
else
    rc=0
    [ -f "$apps/211bsd.mscp/211BSD_du0_73.sh" ] || { fail "the merge did not survive"; rc=1; }
    [ -L "$ROOT/211BSD_du0_73.sh" ] || { fail "the shortcut did not survive"; rc=1; }
    [ -e "$ROOT/10.03_app_demo/5_applications_q" ] && { fail "5_applications_q is back"; rc=1; }
    expect "the dotfile is still untouched" "the operator's shell" "$(cat "$ROOT/.bashrc")" || rc=1
    [ $rc = 0 ] && ok
fi

case_begin "a UNIBUS board gets the UNIBUS examples"
echo unibone > "$SB/pkg"
ROOT_U=$SB/root_u
if ! devkit "$ROOT_U"; then
    fail "the run failed (exit $DEVKIT_RC)"
else
    rc=0
    appsu=$ROOT_U/10.03_app_demo/5_applications
    grep -q '^QUNILATOR_PLATFORM=UNIBUS$' "$ROOT_U/qunibone-platform.env" \
        || { fail "no UNIBUS platform in qunibone-platform.env"; rc=1; }
    [ -f "$appsu/cpu20/cpu20_hello.sh" ] || { fail "no UNIBUS example in 5_applications"; rc=1; }
    [ -f "$appsu/211bsd.mscp/211BSD_du0_73.sh" ] && { fail "a QBUS example landed on a UNIBUS board"; rc=1; }
    expect "4_deploy" "$ROOT_U/10.03_app_demo/4_deploy_u" \
        "$(readlink "$ROOT_U/10.03_app_demo/4_deploy")" || rc=1
    [ $rc = 0 ] && ok
fi
echo qbone > "$SB/pkg"

case_begin "a branch named on the command line is what gets checked out"
git -C "$SB/origin" branch -q testing
ROOT_B=$SB/root_b
if ! devkit "$ROOT_B" --branch testing; then
    fail "the run failed (exit $DEVKIT_RC)"
else
    expect "branch" "testing" "$(git -C "$ROOT_B" rev-parse --abbrev-ref HEAD)" && ok
fi

case_begin "a board with no emulator package is refused"
echo nothing-installed > "$SB/pkg"
if devkit "$SB/root_none" 2>/dev/null; then
    fail "the run succeeded on a machine which is not a board"
else
    case "$OUT" in
        *"not a board"*) ok ;;
        *) fail "refused, but not with the reason expected: $OUT" ;;
    esac
fi
echo qbone > "$SB/pkg"

case_begin "a missing PRU compiler stops the run before it fetches anything"
rm -f "$SB/cgt/bin/clpru"
if devkit "$SB/root_nocgt" 2>/dev/null; then
    fail "the run succeeded without a PRU compiler"
else
    case "$OUT" in
        *"no PRU compiler"*) ok ;;
        *) fail "refused, but not with the reason expected: $OUT" ;;
    esac
fi
: > "$SB/cgt/bin/clpru"; chmod +x "$SB/cgt/bin/clpru"

echo
echo "devkit test: $((PASS + FAIL)) cases, $PASS passed, $FAIL failed"
[ $FAIL = 0 ]
