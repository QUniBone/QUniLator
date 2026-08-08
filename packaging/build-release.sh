#!/bin/bash
# build-release.sh - build a complete, card-ready release image on an x86_64
# Linux machine, from a clean checkout, in one command.
#
# It is the local equivalent of .github/workflows/release-image.yml: it stages
# everything build-image.sh needs under $DIST (default ./dist), then runs it.
#
#   1. the rcn-ee Debian base image, dist/base.img.xz - downloaded and checked
#      against its published .sha256sum when it is not already there
#   2. the emulator package - ./crossbuild.sh and packaging/build-deb.sh, the
#      result copied into dist/
#   3. the disk images and boot configurations, dist/images and dist/configs -
#      taken from $ASSETS_URL when either directory is missing
#   4. packaging/build-image.sh, which produces <name>-dist.img
#
# Everything after the first run is incremental: a base image already in dist/
# is not fetched again, and neither are the assets. Delete the file or the
# directory to force a refresh.
#
# usage:
#   ./packaging/build-release.sh          # qbone (QBUS) image
#   ./packaging/build-release.sh -u       # unibone (UNIBUS) image
#   ./packaging/build-release.sh -x       # ... and compress it as a release does
#
#   -u  build the UNIBUS board's image instead of the QBUS board's
#   -p  skip the package build and use the <name>_*_armhf.deb already in dist/
#   -x  compress the finished image to <name>-dist.img.zst and .img.xz
#   -h  show this summary
#
# environment:
#   DIST               staging directory (default ./dist)
#   OUT                output image (default ./<name>-dist.img)
#   BASE_IMAGE_URL     the rcn-ee base image; defaults to the pinned Debian
#                      13.6 / 6.12 base this appliance is built on
#   ASSETS_URL         tar of the images/ and configs/ to ship, expanding to
#                      those two directories at its top level. Board-specific
#                      ASSETS_URL_QBONE / ASSETS_URL_UNIBONE win over it.
#   APT_REPO_URL       apt repository the flashed board updates itself from,
#   APT_REPO_KEY_URL   and its signing key. Without them the image ships a
#                      board that cannot update itself - a warning, not an
#                      error, since a local test image rarely needs one.
#   GROW, MARGIN_MB    passed through to build-image.sh

set -euo pipefail
cd "$(dirname "$0")/.."
HERE=$PWD

# The base image the appliance is built on: Debian 13.6 with the 6.12 kernel,
# the newest kernel an RT build exists for and the last that carries uio_pruss.
# Pinned by date, so a rebuild of an old release gets the base it was made
# with; see docs/debian-installation.md for why this one.
BASE_IMAGE_DEFAULT=https://rcn-ee.com/rootfs/debian-armhf-13-base-v6.12/2026-07-15/am335x-debian-13.6-base-v6.12-armhf-2026-07-15-4gb.img.xz

# the header comment above is the usage summary, so the options are documented
# in one place; print it up to the last commented line
usage() { sed -n '2,/^[^#]/p' "$0" | sed -n 's/^#\( \|$\)//p'; }

BOARD=qbone
FLAG=()
SKIP_PACKAGE=0
COMPRESS=0
while getopts "upxh?" opt; do
    case $opt in
        u) BOARD=unibone; FLAG=(-u);;
        p) SKIP_PACKAGE=1;;
        x) COMPRESS=1;;
        h|\?) usage; exit 0;;
        *) usage >&2; exit 1;;
    esac
done

DIST=${DIST:-$HERE/dist}
OUT=${OUT:-$HERE/$BOARD-dist.img}
BASE_IMAGE_URL=${BASE_IMAGE_URL:-$BASE_IMAGE_DEFAULT}
case $BOARD in
    unibone) ASSETS_URL=${ASSETS_URL_UNIBONE:-${ASSETS_URL:-}};;
    *)       ASSETS_URL=${ASSETS_URL_QBONE:-${ASSETS_URL:-}};;
esac

# The image build loop-mounts ext4 and runs an armhf chroot in a privileged
# container, which only a Linux host with Docker can do.
[ "$(uname -s)" = Linux ] || { echo "this script builds on Linux; on macOS use the CI workflow" >&2; exit 1; }
command -v docker >/dev/null || { echo "docker is required" >&2; exit 1; }
command -v curl   >/dev/null || { echo "curl is required" >&2; exit 1; }
docker info >/dev/null 2>&1 || { echo "cannot talk to the Docker daemon" >&2; exit 1; }

mkdir -p "$DIST"

# The base image and its 4G of build headroom, a decompressed working copy and
# the finished image want well over ten gigabytes between them.
AVAIL=$(df -Pm "$DIST" | awk 'NR==2 {print $4}')
[ "$AVAIL" -ge 15000 ] || echo "== WARNING: only $((AVAIL/1024))G free under $DIST; the build wants ~15G =="

# ---------------------------------------------------------------- base image
if [ -s "$DIST/base.img.xz" ]; then
    echo "== base image already staged: $DIST/base.img.xz ($(du -h "$DIST/base.img.xz" | cut -f1)) =="
else
    echo "== fetching the Debian base image =="
    echo "-- $BASE_IMAGE_URL"
    # download beside the target and rename only once it is whole, so an
    # interrupted fetch never looks like a staged base image
    rm -f "$DIST/base.img.xz.part"
    curl --fail-with-body -L --progress-bar -o "$DIST/base.img.xz.part" "$BASE_IMAGE_URL"

    # rcn-ee publishes a .sha256sum next to each image; check against it when
    # it is there, and say so plainly when it is not
    if curl --fail -sSL -o "$DIST/base.sha256sum.part" "$BASE_IMAGE_URL.sha256sum"; then
        want=$(awk '{print $1; exit}' "$DIST/base.sha256sum.part")
        got=$(sha256sum "$DIST/base.img.xz.part" | awk '{print $1}')
        rm -f "$DIST/base.sha256sum.part"
        if [ "$want" != "$got" ]; then
            rm -f "$DIST/base.img.xz.part"
            echo "checksum mismatch on the base image: expected $want, got $got" >&2
            exit 1
        fi
        echo "-- sha256 verified"
    else
        rm -f "$DIST/base.sha256sum.part"
        echo "-- WARNING: no published .sha256sum; the download is unverified"
    fi
    mv "$DIST/base.img.xz.part" "$DIST/base.img.xz"
fi

# ------------------------------------------------------------------ package
if [ "$SKIP_PACKAGE" = 1 ]; then
    ls "$DIST"/${BOARD}_*_armhf.deb >/dev/null 2>&1 \
        || { echo "-p given but no $BOARD deb in $DIST" >&2; exit 1; }
    echo "== using the staged package: $(ls "$DIST"/${BOARD}_*_armhf.deb) =="
else
    echo "== building the $BOARD package =="
    ./crossbuild.sh "${FLAG[@]}"
    ./packaging/build-deb.sh "${FLAG[@]}"
    # one deb per board in the staging directory: build-image.sh refuses to
    # guess between two versions. The build leaves its deb in the working
    # directory and never sweeps the older ones, so name the version this run
    # built rather than globbing - a glob would copy every version back in.
    DEB=${BOARD}_$(packaging/version.sh)_armhf.deb
    [ -f "$DEB" ] || { echo "build-deb.sh left no $DEB" >&2; exit 1; }
    rm -f "$DIST"/${BOARD}_*_armhf.deb
    cp "$DEB" "$DIST/"
fi

# ------------------------------------------------- disk images and configs
if [ -d "$DIST/images" ] && [ -d "$DIST/configs" ]; then
    echo "== images and configs already staged =="
elif [ -n "$ASSETS_URL" ]; then
    echo "== fetching the disk images and boot configurations =="
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT
    curl --fail-with-body -L --progress-bar -o "$tmp/assets.tar" "$ASSETS_URL"
    tar -xf "$tmp/assets.tar" -C "$DIST"
    [ -d "$DIST/images" ] && [ -d "$DIST/configs" ] \
        || { echo "the assets must hold images/ and configs/ at their top level" >&2; exit 1; }
    rm -rf "$tmp"; trap - EXIT
else
    cat >&2 <<EOF
missing $DIST/images and/or $DIST/configs, and no ASSETS_URL to fetch them from.

The image ships the sample operating systems and the boot configurations that
name them. Either set ASSETS_URL (or ASSETS_URL_QBONE / ASSETS_URL_UNIBONE) to
a tar holding images/ and configs/ at its top level, or place those two
directories under $DIST by hand - for instance from a running board:

    rsync -a <board>:/var/lib/qunilator/images/  $DIST/images/
    rsync -a <board>:/var/lib/qunilator/configs/ $DIST/configs/
EOF
    exit 1
fi
# build-image.sh copies these with a plain glob, which an empty directory fails
for d in images configs; do
    [ -n "$(ls -A "$DIST/$d")" ] || { echo "$DIST/$d is empty" >&2; exit 1; }
done

# --------------------------------------------------------------- the image
if [ -z "${APT_REPO_URL:-}" ] || [ -z "${APT_REPO_KEY_URL:-}" ]; then
    echo "== WARNING: no APT_REPO_URL/APT_REPO_KEY_URL set =="
    echo "==          the board flashed from this image will not be able to update itself =="
fi

echo "== building the $BOARD appliance image =="
NAME=$BOARD DIST=$DIST OUT=$OUT ./packaging/build-image.sh

# The privileged container writes the image as root; take it back so the caller
# can compress, copy or write it without sudo. The chown is done from a
# container, which is already root - sudo would want a terminal to authenticate
# on, which a scripted or CI run has not got.
if [ ! -O "$OUT" ]; then
    docker run --rm -v "$(cd "$(dirname "$OUT")" && pwd)":/out debian:trixie \
        chown "$(id -u):$(id -g)" "/out/$(basename "$OUT")"
fi

if [ "$COMPRESS" = 1 ]; then
    # Both formats a release publishes, from the same bytes: the installer takes
    # the .zst, which it decompresses faster than a card can be written, and an
    # imaging tool takes the .xz. zstd is given the file rather than a pipe, so
    # the frame header carries the size of the image inside it.
    echo "== compressing =="
    zstd -19 -T0 --long=27 --no-progress -f -o "$OUT.zst" "$OUT"
    xz -T0 -f "$OUT"
    echo "   $OUT.zst ($(du -h "$OUT.zst" | cut -f1))"
    OUT=$OUT.xz
    WRITE="xz -dc $OUT | sudo dd of=/dev/sdX bs=4M status=progress"
else
    WRITE="sudo dd if=$OUT of=/dev/sdX bs=4M status=progress"
fi

echo
echo "== release image ready: $OUT ($(du -h "$OUT" | cut -f1)) =="
echo "   write it to a card with:  $WRITE"
