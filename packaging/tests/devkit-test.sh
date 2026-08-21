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
    DEFAULT_BRANCH=$(git -C "$src" symbolic-ref --short HEAD)

    # A commit after that release, on a branch of its own: what a package built
    # between releases is built from, and what the tag-less resolution has to
    # find. It carries a file the released tree has not, so a test can tell
    # which of the two was checked out by looking at the tree.
    git -C "$src" checkout -q -b devbuild
    echo "built from here" > "$src/devbuild-marker"
    git -C "$src" -c user.email=t@t -c user.name=t add -A
    git -C "$src" -c user.email=t@t -c user.name=t commit -qm "a commit between releases"
    DEV_COMMIT=$(git -C "$src" rev-parse HEAD)
    git -C "$src" checkout -q "$DEFAULT_BRANCH"
}

# What the installed package says about the tree it came from - the file
# packaging/build-deb.sh writes as /usr/share/qunilator/build-ref.
#   build_ref <commit> <branch> <dirty>
build_ref() {
    cat > "$SB/build-ref" <<EOF
BUILD_VERSION=$(cat "$SB/version")
BUILD_COMMIT=$1
BUILD_BRANCH=$2
BUILD_DIRTY=$3
EOF
}
# a package built before this file existed, or by something that is not the
# packaging script
no_build_ref() { rm -f "$SB/build-ref"; }

# A second origin, carrying the personalization script as an older release wrote
# it: no "cd" to its own directory, and the example trees named through $HOME.
# The tree qunilator-devkit checks out is the one matching the emulator the
# board runs, so it is regularly older than the command doing the checking out -
# and this is the shape that told on a freshly flashed board, where the run died
# with "Platform settings in file qunibone-platform.env not found!" because the
# script was looking in whatever directory "sudo qunilator-devkit" was typed in.
build_legacy_origin() {
    local src=$SB/origin-legacy
    mkdir -p "$src"
    ( cd "$SB/origin" && tar -c --exclude=.git . ) | ( cd "$src" && tar -x )

    local before=$src/qunibone-platform.sh
    grep -q '^cd "$(dirname "$0")"' "$before" && grep -q '^TREE=\$PWD$' "$before" || {
        echo "sandbox: qunibone-platform.sh no longer roots itself the way this" >&2
        echo "sandbox: test undoes to build a legacy tree - update devkit-test.sh" >&2
        exit 2
    }
    sed -e '/^cd "$(dirname "$0")"/d' -e 's/^TREE=\$PWD$/TREE=$HOME/' \
        "$before" > "$before.legacy"
    mv "$before.legacy" "$before"
    chmod +x "$before"

    git -C "$src" init -q
    git -C "$src" -c user.email=t@t -c user.name=t add -A
    git -C "$src" -c user.email=t@t -c user.name=t commit -qm "legacy tree"
    git -C "$src" tag v99.0.0
}

# A third origin: the legacy tree with no example machines in it at all, which
# is what a ref older than "the example machines are back in the repository"
# looks like. A board whose emulator version was never tagged is checked out
# from the default branch, so this is a tree an operator really gets, and the
# run has to come through it saying what it did rather than only "cp: cannot
# stat .../5_applications_u/*".
build_bare_origin() {
    local src=$SB/origin-bare
    mkdir -p "$src"
    ( cd "$SB/origin-legacy" && tar -c --exclude=.git --exclude=./10.03_app_demo . ) \
        | ( cd "$src" && tar -x )
    git -C "$src" init -q
    git -C "$src" -c user.email=t@t -c user.name=t add -A
    git -C "$src" -c user.email=t@t -c user.name=t commit -qm "tree without examples"
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
        -e "s#^ETC_DIR=/etc/qunilator#ETC_DIR=$SB/etc#" \
        -e "s#^BUILD_REF_FILE=/usr/share/qunilator/build-ref#BUILD_REF_FILE=$SB/build-ref#" \
        "$REPO_ROOT/packaging/debian/qunilator-devkit" > "$SB/qunilator-devkit"
    chmod +x "$SB/qunilator-devkit"
    grep -q "^PRU_CGT_DIR=$SB/cgt\$" "$SB/qunilator-devkit" || {
        echo "sandbox: qunilator-devkit no longer sets PRU_CGT_DIR the way this" >&2
        echo "sandbox: test rewrites it - update devkit-test.sh" >&2
        exit 2
    }
    grep -q "^ETC_DIR=$SB/etc\$" "$SB/qunilator-devkit" || {
        echo "sandbox: qunilator-devkit no longer sets ETC_DIR the way this" >&2
        echo "sandbox: test rewrites it - update devkit-test.sh; without the" >&2
        echo "sandbox: rewrite the run would write into the real /etc" >&2
        exit 2
    }
    grep -q "^BUILD_REF_FILE=$SB/build-ref\$" "$SB/qunilator-devkit" || {
        echo "sandbox: qunilator-devkit no longer sets BUILD_REF_FILE the way" >&2
        echo "sandbox: this test rewrites it - update devkit-test.sh; without" >&2
        echo "sandbox: the rewrite it would read this machine's own /usr/share" >&2
        exit 2
    }
}

# devkit <target dir> [extra args...]
#
# Run from a directory which is not the tree, and with a HOME which is not the
# tree either - which is what "sudo qunilator-devkit" looks like when it is
# typed from anywhere but /root. Nothing the run does may depend on either.
devkit() {
    local dir=$1
    shift
    mkdir -p "$SB/elsewhere"
    OUT=$(cd "$SB/elsewhere" && SB_DIR=$SB PATH="$SB/bin:$PATH" HOME=$SB/elsewhere \
        "$SB/qunilator-devkit" \
        --no-apt --url "$SB/origin" --dir "$dir" "$@" 2>&1)
    DEVKIT_RC=$?
    [ $VERBOSE = 1 ] && echo "$OUT"
    return $DEVKIT_RC
}

# ----------------------------------------------------------------- the runs ---
mkdir -p "$SB"
build_origin
build_legacy_origin
build_bare_origin
build_stubs

ROOT=$SB/root
mkdir -p "$ROOT"
# what a real /root holds before any of this: dotfiles, and one file which
# happens to carry the name of an example shortcut
echo "the operator's shell" > "$ROOT/.bashrc"
echo "not a shortcut" > "$ROOT/memory.sh"

# The package records a commit, as one built between releases does - and this
# board's version has a tag, which is the better answer and has to win.
build_ref "$DEV_COMMIT" devbuild 0

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
# and each one names the menu program this board installs on its "#!" line
expect "the common example's interpreter" "#!/usr/bin/qbone-cli --verbose" \
    "$(head -1 "$apps/memory/memory.sh")" || rc=1
expect "the QBUS example's interpreter" "#!/usr/bin/qbone-cli --verbose" \
    "$(head -1 "$apps/211bsd.mscp/211BSD_du0_73.sh")" || rc=1
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

case_begin "the run records the tree it made, which is what silences the login hint"
rc=0
[ -f "$SB/etc/devkit.env" ] || { fail "no devkit.env was written"; rc=1; }
grep -q "^DEVKIT_DIR=$ROOT\$" "$SB/etc/devkit.env" \
    || { fail "devkit.env does not name the tree: $(cat "$SB/etc/devkit.env" 2>&1)"; rc=1; }
[ $rc = 0 ] && ok

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
    # the common examples are rebranded to this board's menu program, the
    # UNIBUS ones name it already
    expect "the common example's interpreter" "#!/usr/bin/unibone-cli --verbose" \
        "$(head -1 "$appsu/memory/memory.sh")" || rc=1
    expect "the UNIBUS example's interpreter" "#!/usr/bin/unibone-cli --verbose" \
        "$(head -1 "$appsu/cpu20/cpu20_hello.sh")" || rc=1
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

# ------------------------------------------- which tree matches the binaries --
# A released board finds its sources by the tag of its version. A board running
# a build from between releases has no such tag, and the package's record of the
# commit it was built from is what keeps the checkout off a default branch that
# may be a long way from the binaries. These four cases are that ladder, top to
# bottom. The version is moved to one no tag answers for; the last one puts it
# back, since the cases after these are about other things.
echo 99.1.0-1 > "$SB/version"

case_begin "with no tag for its version, the commit the package records is checked out"
build_ref "$DEV_COMMIT" devbuild 0
ROOT_C=$SB/root_commit
if ! devkit "$ROOT_C"; then
    fail "the run failed (exit $DEVKIT_RC)"
else
    rc=0
    expect "HEAD" "$DEV_COMMIT" "$(git -C "$ROOT_C" rev-parse HEAD)" || rc=1
    expect "detached, not on a branch" "HEAD" \
        "$(git -C "$ROOT_C" rev-parse --abbrev-ref HEAD)" || rc=1
    [ -f "$ROOT_C/devbuild-marker" ] || { fail "the tree is not that commit's"; rc=1; }
    grep -q "^DEVKIT_REF=$DEV_COMMIT\$" "$SB/etc/devkit.env" \
        || { fail "devkit.env does not record the commit"; rc=1; }
    [ $rc = 0 ] && ok
fi

case_begin "a package built from a modified tree says so"
build_ref "$DEV_COMMIT" devbuild 1
if ! devkit "$SB/root_dirty"; then
    fail "the run failed (exit $DEVKIT_RC)"
else
    case "$OUT" in
        *"built from a modified tree"*) ok ;;
        *) fail "the run never said the package's tree was not the commit" ;;
    esac
fi

case_begin "a commit the server does not have falls back to the branch it was built on"
build_ref 0000000000000000000000000000000000000001 devbuild 0
ROOT_F=$SB/root_nocommit
if ! devkit "$ROOT_F"; then
    fail "the run failed (exit $DEVKIT_RC)"
else
    rc=0
    expect "branch" "devbuild" "$(git -C "$ROOT_F" rev-parse --abbrev-ref HEAD)" || rc=1
    case "$OUT" in
        *"is not on"*) ;;
        *) fail "the run never said the commit could not be had"; rc=1 ;;
    esac
    [ $rc = 0 ] && ok
fi

case_begin "a package that records nothing falls back to the default branch, and says it is a guess"
no_build_ref
ROOT_G=$SB/root_noref
if ! devkit "$ROOT_G"; then
    fail "the run failed (exit $DEVKIT_RC)"
else
    rc=0
    expect "branch" "$DEFAULT_BRANCH" "$(git -C "$ROOT_G" rev-parse --abbrev-ref HEAD)" || rc=1
    case "$OUT" in
        *"is a guess"*) ;;
        *) fail "the run never said the branch is only a guess"; rc=1 ;;
    esac
    [ $rc = 0 ] && ok
fi
echo 99.0.0-1 > "$SB/version"

case_begin "a tree whose personalization script predates this command is still personalized"
ROOT_L=$SB/root_legacy
if ! devkit "$ROOT_L" --url "$SB/origin-legacy"; then
    fail "the run failed (exit $DEVKIT_RC)"
else
    rc=0
    appsl=$ROOT_L/10.03_app_demo/5_applications
    [ -f "$ROOT_L/qunibone-platform.env" ] || { fail "no qunibone-platform.env in the tree"; rc=1; }
    [ -f "$appsl/211bsd.mscp/211BSD_du0_73.sh" ] || { fail "the examples were not merged"; rc=1; }
    [ -e "$ROOT_L/10.03_app_demo/5_applications_q" ] && { fail "5_applications_q still there"; rc=1; }
    [ -e "$SB/elsewhere/qunibone-platform.env" ] \
        && { fail "the run personalized the directory it was started from"; rc=1; }
    [ $rc = 0 ] && ok
fi

case_begin "a tree with no example machines says so, and finishes"
ROOT_N=$SB/root_noex
if ! devkit "$ROOT_N" --url "$SB/origin-bare"; then
    fail "the run failed (exit $DEVKIT_RC)"
else
    rc=0
    [ -f "$ROOT_N/compile.sh" ] || { fail "the sources are not there"; rc=1; }
    case "$OUT" in
        *"carries no example machines"*) ;;
        *) fail "the run never said the tree has no examples"; rc=1 ;;
    esac
    case "$OUT" in
        *"The examples are in"*) fail "the closing text points at examples which are not there"; rc=1 ;;
    esac
    [ $rc = 0 ] && ok
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
