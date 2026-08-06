#!/bin/bash
# personalize-image.sh - prepare an SD card image that is already set up.
#
# build-image.sh produces the distributable image, which carries no personal
# data and asks for an operator the first time its web interface is opened.
# This creates that operator in a COPY of the image instead - your name, your
# password, your ssh public key - and writes a new image, so the card boots
# ready to use and no first-run dialog appears. The distributable is never
# modified, and nothing personal enters the repository: the name comes from the
# command line and the password and key from files you keep outside the tree.
#
# One account is the whole identity: the web interface, the SMB, FTP and SFTP
# shares of the image library, and an ssh login with sudo all answer to it. The
# emulator's own --setup-operator does the work inside the image, so a card
# prepared here and one set up through the dialog end up the same.
#
# usage:
#   PUSER=hans PUSER_KEY=~/.ssh/id_ed25519.pub \
#       ./packaging/personalize-image.sh qbone-dist.img qbone-hans.img
#
# The password is read from PUSER_PW_FILE, or asked for on the terminal.
#
# optional environment:
#   PUSER_PW_FILE the password, on the first line of a file
#   PUSER_HOST    the name this QUniLator has on the network (its host name)
#   PUSER_SHELL   login shell, must already be in the image (default /bin/bash)
#   PUSER_HOOK    a script run inside the image's chroot after the account is
#                 made, for anything else - dotfiles, extra packages. Its
#                 directory is mounted read-only at /personal in the chroot, so
#                 the hook can copy files from there.

set -euo pipefail

IN=${1:-}; OUT=${2:-}
[ -n "$IN" ] && [ -n "$OUT" ] || { echo "usage: personalize-image.sh IN.img OUT.img" >&2; exit 1; }
: "${PUSER:?set PUSER to the account name the operator will use}"
: "${PUSER_KEY:?set PUSER_KEY to your ssh public key file}"
export PUSER_SHELL=${PUSER_SHELL:-/bin/bash}
export PUSER_HOST=${PUSER_HOST:-}
[ -r "$IN" ] || { echo "no input image: $IN" >&2; exit 1; }
[ -r "$PUSER_KEY" ] || { echo "cannot read ssh key: $PUSER_KEY" >&2; exit 1; }
export PUSER

# The password never stands in a command line or in the container's
# environment: it reaches the image in a file mounted read-only, which is either
# one you keep or a private temporary this script writes and removes.
PWDIR=""
cleanup_pw() { [ -n "$PWDIR" ] && rm -rf "$PWDIR"; }
trap cleanup_pw EXIT
if [ -n "${PUSER_PW_FILE:-}" ]; then
    [ -r "$PUSER_PW_FILE" ] || { echo "cannot read password file: $PUSER_PW_FILE" >&2; exit 1; }
    PWPATH=$(cd "$(dirname "$PUSER_PW_FILE")" && pwd)/$(basename "$PUSER_PW_FILE")
else
    read -r -s -p "password for $PUSER: " pw1; echo
    read -r -s -p "again: " pw2; echo
    [ "$pw1" = "$pw2" ] || { echo "the two entries do not match" >&2; exit 1; }
    [ ${#pw1} -ge 8 ] || { echo "the password is at least 8 characters" >&2; exit 1; }
    PWDIR=$(mktemp -d); PWPATH=$PWDIR/password
    (umask 077; printf '%s\n' "$pw1" > "$PWPATH")
    unset pw1 pw2
fi

KEYDIR=$(cd "$(dirname "$PUSER_KEY")" && pwd); KEYFILE=$(basename "$PUSER_KEY")
HOOKMOUNT=(); export HOOKNAME=""
if [ -n "${PUSER_HOOK:-}" ]; then
    [ -r "$PUSER_HOOK" ] || { echo "cannot read hook: $PUSER_HOOK" >&2; exit 1; }
    HOOKDIR=$(cd "$(dirname "$PUSER_HOOK")" && pwd); HOOKNAME=$(basename "$PUSER_HOOK")
    HOOKMOUNT=(-v "$HOOKDIR":/personal:ro)
fi

echo "== copying $IN -> $OUT =="
cp "$IN" "$OUT"
OUTDIR=$(cd "$(dirname "$OUT")" && pwd); export OUTNAME=$(basename "$OUT")

echo "== creating the operator '$PUSER' in a privileged container =="
docker run --rm -i --privileged \
    -e PUSER -e PUSER_SHELL -e PUSER_HOST -e HOOKNAME -e OUTNAME \
    -v "$OUTDIR":/out \
    -v "$KEYDIR/$KEYFILE":/in/authorized_key:ro \
    -v "$PWPATH":/in/password:ro \
    ${HOOKMOUNT[@]+"${HOOKMOUNT[@]}"} \
    debian:trixie bash -euo pipefail <<'C'
export DEBIAN_FRONTEND=noninteractive
apt-get -qq update >/dev/null
apt-get -qq install -y util-linux qemu-user-static >/dev/null

LO=$(losetup -Pf --show /out/"$OUTNAME"); LB=$(basename "$LO")
trap 'umount -R /m 2>/dev/null || true; losetup -d "$LO" 2>/dev/null || true' EXIT
# no udev in the container: create the partition device nodes from /sys
for sp in /sys/block/"$LB"/"$LB"p*; do
    n=$(basename "$sp"); IFS=: read -r a b < "$sp/dev"; [ -e /dev/$n ] || mknod /dev/$n b "$a" "$b"
done
mkdir -p /m; mount "${LO}p3" /m
mount -t proc proc /m/proc; mount --bind /dev /m/dev

if ! chroot /m test -x "$PUSER_SHELL"; then
    echo "shell $PUSER_SHELL is not in the image - add it to build-image.sh first" >&2
    exit 1
fi
# whichever bus this image carries: the two build the same account
EMULATOR=""
for candidate in /usr/bin/qbone /usr/bin/unibone; do
    [ -x /m"$candidate" ] && EMULATOR=$candidate
done
[ -n "$EMULATOR" ] || { echo "no emulator in the image - is this a QUniLator image?" >&2; exit 1; }
export EMULATOR

install -m 600 /in/password /m/tmp/operator-password
install -m 644 /in/authorized_key /m/tmp/authorized_key
# expose the hook's directory to the chroot read-only by bind mount, so the
# hook can read assets from /personal without copying anything into the image
if [ -n "$HOOKNAME" ]; then mkdir -p /m/personal; mount --bind /personal /m/personal; fi

# the chroot inherits PUSER, PUSER_SHELL, PUSER_HOST, EMULATOR and HOOKNAME
chroot /m /bin/bash -euo pipefail <<'CHR'
export DEBIAN_FRONTEND=noninteractive
export QUNILATOR_DIR=/var/lib/qunilator
ARGS=(--setup-operator "$PUSER" --ssh-key /tmp/authorized_key)
[ -n "$PUSER_HOST" ] && ARGS+=(--hostname "$PUSER_HOST")
"$EMULATOR" "${ARGS[@]}" < /tmp/operator-password
shred -u /tmp/operator-password 2>/dev/null || rm -f /tmp/operator-password
rm -f /tmp/authorized_key
usermod -s "$PUSER_SHELL" "$PUSER"
if [ -n "$HOOKNAME" ] && [ -x /personal/"$HOOKNAME" ]; then /personal/"$HOOKNAME"; fi
CHR

if [ -n "$HOOKNAME" ]; then umount /m/personal; rmdir /m/personal; fi
sync
umount -R /m; losetup -d "$LO"; trap - EXIT
echo "-- the operator is in the image"
C

echo "== done: $OUT =="
