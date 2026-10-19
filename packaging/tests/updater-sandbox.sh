# Sourced by updater-test.sh: a fake board for qunilator-update to run against.
#
# The updater drives apt, dpkg and systemd on a live machine, so there is nothing
# to test it against short of a board and a repository with two versions in it -
# and the failure paths (a package that will not start, a dpkg that dies, a
# repository that has gone away) are the ones hardest to arrange there and most
# important to get right. This builds a board out of stubs instead: an apt that
# serves a fake repository, a dpkg that answers about a fake installed version, a
# systemd that reports what the fake install did, and a /run file the health check
# reads.
#
# The script under test is copied and its absolute paths rewritten into the
# sandbox, so what runs is the shipped file, byte for byte apart from four paths,
# and the shipped file carries no test hooks of its own.
#
# The lock is stubbed rather than exercised: flock is not on every host this runs
# on, and a refused second request is one line of the script.

SANDBOX_ROOT=${SANDBOX_ROOT:?set SANDBOX_ROOT}
REPO_ROOT=${REPO_ROOT:?set REPO_ROOT}

SB="$SANDBOX_ROOT"
UPD="$SB/qunilator-update"

sandbox_build() {
    rm -rf "$SB"
    mkdir -p "$SB/state" "$SB/bin" "$SB/run" "$SB/apt/sources.list.d" "$SB/repo"

    # Four paths, each asserted so a rename in the updater turns into a failure
    # here rather than a test that quietly stops testing anything.
    sed -e "s#^STATE_DIR=/var/lib/qunilator/updates#STATE_DIR=$SB/state#" \
        -e "s#/etc/apt/sources.list.d/#$SB/apt/sources.list.d/#g" \
        -e "s#/run/qunilator/version#$SB/run/version#g" \
        -e "s#/var/run/reboot-required#$SB/run/reboot-required#g" \
        "$REPO_ROOT/packaging/debian/qunilator-update" > "$UPD"
    chmod +x "$UPD"
    for pat in "STATE_DIR=$SB/state" "$SB/apt/sources.list.d/" \
               "$SB/run/version" "$SB/run/reboot-required"; do
        grep -q -- "$pat" "$UPD" || {
            echo "sandbox: qunilator-update no longer contains a path this test" >&2
            echo "sandbox: rewrites ($pat) - update updater-sandbox.sh" >&2
            return 1
        }
    done

    : > "$SB/knobs"
    : > "$SB/upgradable"
    : > "$SB/holds"
    sandbox_stubs
    export SB_DIR="$SB"
    export PATH="$SB/bin:$PATH"
}

mkstub() { cat > "$SB/bin/$1"; chmod +x "$SB/bin/$1"; }

sandbox_stubs() {
    mkstub dpkg <<'EOF'
#!/bin/bash
case "$1" in
  -S) echo "$(sed -n 's/^owner=//p' "$SB_DIR/knobs" | tail -1): /usr/sbin/qunilator-update" ;;
  --audit) sed -n 's/^audit=//p' "$SB_DIR/knobs" | tail -1 ;;
  --configure)
     # a repair clears the audit finding, unless the knob says it cannot
     [ "$(sed -n 's/^repair_fails=//p' "$SB_DIR/knobs" | tail -1)" = yes ] && exit 0
     sed -i.bak "/^audit=/d" "$SB_DIR/knobs"; echo "audit=" >> "$SB_DIR/knobs"; exit 0 ;;
  --compare-versions)
     # enough of dpkg's ordering for dotted-numeric versions with a -revision
     norm() { echo "$1" | tr '.-' '  ' | awk '{printf "%05d%05d%05d%05d", $1,$2,$3,$4}'; }
     a=$(norm "$2"); b=$(norm "$4")
     case "$3" in gt) [[ "$a" > "$b" ]];; lt) [[ "$a" < "$b" ]];; *) false;; esac ;;
  *) exit 0 ;;
esac
EOF

    mkstub dpkg-query <<'EOF'
#!/bin/sh
# -W -f '${Version}' <pkg>  |  -W -f '${Status}' <pkg>
fmt=$3; pkg=$4
case "$fmt" in
  '${Version}') sed -n 's/^installed=//p' "$SB_DIR/knobs" | tail -1 ;;
  '${Status}')  [ "$pkg" = "$(sed -n 's/^owner=//p' "$SB_DIR/knobs" | tail -1)" ] \
                  && echo "install ok installed" || exit 1 ;;
esac
EOF

    # A package here is an empty .deb beside a .ver holding its version and a .tar
    # of its doc tree - the version and the changelog being all the updater reads
    # out of one. The cached copy is renamed to <pkg>.deb and its version is still
    # read from the package, as dpkg-deb would.
    mkstub dpkg-deb <<'EOF'
#!/bin/sh
case "$1" in
  -f) case "$3" in
        Version)
           # an update file's package carries its version as its content
           if grep -q '^Version: ' "$2" 2>/dev/null; then sed -n 's/^Version: //p' "$2"
           elif [ -f "${2%.deb}.ver" ]; then cat "${2%.deb}.ver"
           else v=$(ls "$(dirname "$2")"/*.ver 2>/dev/null | head -1)
                [ -n "$v" ] && cat "$v"; fi ;;
        Installed-Size) echo 6000 ;;
      esac ;;
  --fsys-tarfile) cat "${2%.deb}.tar" ;;
esac
EOF

    mkstub apt-get <<'EOF'
#!/bin/bash
knob() { sed -n "s/^$1=//p" "$SB_DIR/knobs" | tail -1; }
cmd=""
for a in "$@"; do case "$a" in update|download|install|upgrade) cmd=$a; break;; esac; done
# a simulated upgrade reports what it would keep back, and changes nothing
for a in "$@"; do case "$a" in -s|--simulate)
   if [ -s "$SB_DIR/kept" ]; then
      echo "The following packages have been kept back:"; sed 's/^/  /' "$SB_DIR/kept"
   fi
   echo "0 upgraded, 0 newly installed, 0 to remove and 0 not upgraded."; exit 0 ;;
esac; done
# The one-package repository an update file is laid out as: apt is pointed at
# its sources.list, which names the repository directory.
local_repo=""
for a in "$@"; do case "$a" in
  Dir::Etc::sourcelist=*/local/work/sources.list)
     local_repo=$(sed -n 's/.*file:\([^ ]*\) .*/\1/p' "${a#Dir::Etc::sourcelist=}") ;;
esac; done
if [ -n "$local_repo" ]; then
  case "$cmd" in
    update)
       [ "$(knob bundle_signed)" = no ] && {
          echo "Err:1 file:$local_repo trixie InRelease"
          echo "E: The repository 'file:$local_repo trixie InRelease' is not signed."; }
       exit 0 ;;
    download)
       spec=""; for a in "$@"; do case "$a" in *=*.*) spec=$a;; esac; done
       pkg=${spec%%=*}; ver=${spec#*=}
       [ "$(knob bundle_tampered)" = yes ] && {
          echo "E: Failed to fetch file:$local_repo/pool/main/${pkg}_${ver}_armhf.deb  Hash Sum mismatch" >&2
          exit 100; }
       deb=$(find "$local_repo/pool" -name "${pkg}_${ver}_armhf.deb" | head -1)
       [ -n "$deb" ] || { echo "E: Can't find a source to download version '$ver' of '$pkg'" >&2; exit 100; }
       cp "$deb" . ; exit 0 ;;
  esac
fi
case "$cmd" in
  update)
     # what the status said while the refresh ran, for the check's own test
     cp "$SB_DIR/state/status.json" "$SB_DIR/during-update.json" 2>/dev/null
     [ "$(knob reachable)" = no ] && {
        echo "E: Could not resolve host repo.invalid" >&2; exit 100; }
     exit 0 ;;
  download)
     spec=""; for a in "$@"; do case "$a" in *=*.*) spec=$a;; esac; done
     pkg=${spec%%=*}; ver=${spec#*=}
     [ -f "$SB_DIR/repo/${pkg}_${ver}_armhf.deb" ] \
        || { echo "E: Version '$ver' for '$pkg' was not found" >&2; exit 1; }
     for ext in deb tar ver; do
        cp "$SB_DIR/repo/${pkg}_${ver}_armhf.$ext" . 2>/dev/null || true
     done
     exit 0 ;;
  install)
     deb=""; for a in "$@"; do case "$a" in *.deb) deb=$a;; esac; done
     ver=$(dpkg-deb -f "$deb" Version)
     [ "$(knob install_fails)" = yes ] && {
        echo "dpkg: error processing archive $deb (--unpack):"
        echo " cannot copy extracted data: unexpected end of file"; exit 1; }
     sed -i.bak "s/^installed=.*/installed=$ver/" "$SB_DIR/knobs"
     # the package's postinst restarts the unit; the new instance publishes its
     # own version, unless this is the version the test declared broken
     if [ "$(knob broken_version)" = "$ver" ]; then
        rm -f "$SB_DIR/run/version"
        sed -i.bak "s/^active=.*/active=no/" "$SB_DIR/knobs"
     else
        echo "$ver" > "$SB_DIR/run/version"
        sed -i.bak "s/^active=.*/active=yes/" "$SB_DIR/knobs"
     fi
     echo "Setting up $(basename "$deb") ..."
     exit 0 ;;
  upgrade)
     [ "$(knob upgrade_fails)" = yes ] && {
        echo "E: Sub-process /usr/bin/dpkg returned an error code (1)" >&2; exit 100; }
     # apt's status stream on fd 3, and dpkg's output with the carriage returns
     # its progress redraws carry
     { echo "dlstatus:1:50:Retrieving file 1 of 1" >&3
       echo "pmstatus:libc6:40.5:Preparing libc6" >&3; } 2>/dev/null
     printf 'Unpacking libc6 ...\r\nSetting up libc6 ...\r\n\tdone\n'
     # held open until the updater has published progress (or five seconds),
     # and what it published kept for the test; sleep is stubbed, so the wait
     # takes the system's own
     if [ "$(knob slow_upgrade)" = yes ]; then
        for _ in $(seq 1 50); do
           grep -q '"step":"installing"' "$SB_DIR/state/status.json" 2>/dev/null && break
           command -p sleep 0.1
        done
        cp "$SB_DIR/state/status.json" "$SB_DIR/during-upgrade.json"
     fi
     exit 0 ;;
esac
exit 0
EOF

    mkstub apt-cache <<'EOF'
#!/bin/sh
pkg=$2
case "$1" in
  policy) echo "$pkg:"
          echo "  Installed: $(sed -n 's/^installed=//p' "$SB_DIR/knobs" | tail -1)"
          echo "  Candidate: $(sed -n 's/^candidate=//p' "$SB_DIR/knobs" | tail -1)" ;;
  madison) for v in $(sed -n 's/^available=//p' "$SB_DIR/knobs" | tail -1); do
             echo "  $pkg |  $v | http://repo trixie/main armhf Packages"
           done ;;
esac
EOF

    mkstub apt <<'EOF'
#!/bin/sh
echo "Listing..."
cat "$SB_DIR/upgradable" 2>/dev/null
EOF

    mkstub apt-mark <<'EOF'
#!/bin/sh
case "$1" in showhold) cat "$SB_DIR/holds" 2>/dev/null ;; *) exit 0 ;; esac
EOF

    mkstub systemctl <<'EOF'
#!/bin/sh
case "$1" in
  is-active) [ "$(sed -n 's/^active=//p' "$SB_DIR/knobs" | tail -1)" = yes ] ;;
  *) exit 0 ;;
esac
EOF

    # the loopback probe: the service answers when the unit is up
    mkstub curl <<'EOF'
#!/bin/sh
[ "$(sed -n 's/^active=//p' "$SB_DIR/knobs" | tail -1)" = yes ] && echo 200 || echo 000
EOF

    mkstub journalctl <<'EOF'
#!/bin/sh
echo "qbone[1234]: web server failed to start on port 80"
echo "systemd[1]: qbone.service: Main process exited, code=exited, status=1"
EOF

    # the lock is not what this exercises, and flock is not on every host
    mkstub flock <<'EOF'
#!/bin/sh
exit 0
EOF

    # the health check waits a minute of wall clock it has no reason to spend here
    mkstub sleep <<'EOF'
#!/bin/sh
exit 0
EOF
}

# mkdeb <pkg> <version> "<changelog versions, newest first>"
mkdeb() {
    _pkg=$1; _ver=$2; _vers=$3
    : > "$SB/repo/${_pkg}_${_ver}_armhf.deb"
    echo "$_ver" > "$SB/repo/${_pkg}_${_ver}_armhf.ver"
    _d=$(mktemp -d)
    mkdir -p "$_d/usr/share/doc/$_pkg"
    { for _v in $_vers; do
        echo "$_pkg ($_v) trixie; urgency=low"
        echo
        echo "  * what changed in $_v"
        echo
        echo " -- Hans Huebner <hans.huebner@gmail.com>  Tue, 28 Jul 2026 07:54:10 +0200"
        echo
      done
    } | gzip -9 -n -c > "$_d/usr/share/doc/$_pkg/changelog.Debian.gz"
    ( cd "$_d" && tar -cf "$SB/repo/${_pkg}_${_ver}_armhf.tar" ./usr )
    rm -rf "$_d"
}

# mkbundle <deb name> <version> [extra member] - an update file, as the service
# leaves an upload for the updater. The package's content is its version, which
# is what the dpkg-deb stub reads back out of it.
mkbundle() {
    _d=$(mktemp -d)
    echo "signed index" > "$_d/InRelease"
    _pkg=${1%%_*}
    printf 'Package: %s\nVersion: %s\nFilename: pool/main/%s\n\n' "$_pkg" "$2" "$1" \
        > "$_d/Packages"
    echo "Version: $2" > "$_d/$1"
    _members="InRelease Packages $1"
    if [ -n "${3:-}" ]; then
        mkdir -p "$_d/$(dirname "$3")"; : > "$_d/$3"; _members="$_members $3"
    fi
    # shellcheck disable=SC2086
    ( cd "$_d" && tar -cf "$SB/state/upload.tar" $_members )
    rm -rf "$_d"
}

# a source signed with a keyring that exists, which is what an update file is
# checked against
signed_source() {
    : > "$SB/apt/keyring.gpg"
    echo "deb [signed-by=$SB/apt/keyring.gpg] http://repo.invalid trixie main" \
        > "$SB/apt/sources.list.d/qbone.list"
}

# knob <name> <value> - what the stubs report about the world
knob() {
    sed -i.bak "/^$1=/d" "$SB/knobs" 2>/dev/null || true
    echo "$1=$2" >> "$SB/knobs"
}

# the board every case starts from: 1.12.0-1 installed and running, 1.13.0-1
# published, a source configured, nothing broken
sandbox_reset() {
    rm -rf "$SB/state" "$SB/repo"
    mkdir -p "$SB/state" "$SB/repo"
    : > "$SB/knobs"
    knob owner qbone
    knob installed 1.12.0-1
    knob candidate 1.13.0-1
    knob available "1.11.0-1 1.12.0-1 1.13.0-1"
    knob active yes
    knob audit ""
    knob reachable yes
    knob install_fails no
    knob upgrade_fails no
    knob repair_fails no
    knob broken_version ""
    knob bundle_signed yes
    knob bundle_tampered no
    echo "1.12.0-1" > "$SB/run/version"
    rm -f "$SB/run/reboot-required"
    echo "deb http://repo.invalid trixie main" > "$SB/apt/sources.list.d/qbone.list"
    : > "$SB/upgradable"
    : > "$SB/holds"
    : > "$SB/kept"
    rm -f "$SB/during-update.json" "$SB/during-upgrade.json"
    mkdeb qbone 1.11.0-1 "1.11.0-1 1.10.0-1"
    mkdeb qbone 1.12.0-1 "1.12.0-1 1.11.0-1 1.10.0-1"
    mkdeb qbone 1.13.0-1 "1.13.0-1 1.12.0-1 1.11.0-1 1.10.0-1"
}

# run the updater, keeping its output and exit status for the assertions
updater() {
    UPD_OUT=$("$UPD" "$@" 2>&1)
    UPD_RC=$?
    return 0
}

# a scalar field of the status file, by name
status_field() {
    sed -n "s/.*\"$1\":\"\([^\"]*\)\".*/\1/p" "$SB/state/status.json" | head -1
}

status_bool() {
    sed -n "s/.*\"$1\":\([a-z]*\).*/\1/p" "$SB/state/status.json" | head -1
}

# a field of the nested "last" object
status_last() {
    sed -n 's/.*"last":{\([^}]*\)}.*/\1/p' "$SB/state/status.json" |
        sed -n "s/.*\"$1\":\"\([^\"]*\)\".*/\1/p" | head -1
}
