#!/bin/bash
# Build C-Kermit for the QBone (BeagleBone, armhf) and optionally deploy it.
#
# The QBone runs Debian 8 "jessie" (glibc 2.19). To get a normal dynamically
# linked binary that runs there -- with working hostname resolution -- the
# build happens inside an armhf jessie container (qemu-emulated), so it links
# against the exact glibc the device has. The bookworm cross toolchain used by
# crossbuild.sh targets a far newer glibc and would only work when linked
# statically, which disables NSS/DNS lookups.
#
# usage (run from 91_3rd_party/ckermit/):
#   ./build.sh        build kermit (dynamic armhf binary)
#   ./build.sh -d     build and deploy to $QUNILATOR_HOST:~/bin/kermit
#
# environment:
#   QUNILATOR_HOST   ssh destination of the device (default hans@192.168.2.223)

set -e
cd "$(dirname "$0")"

QUNILATOR_HOST=${QUNILATOR_HOST:-hans@192.168.2.223}
IMAGE=qunibone-ckermit-jessie
WORKDIR=${CKERMIT_BUILD_DIR:-build}
CKVER=cku302
CKURL="https://kermitproject.org/ftp/kermit/archives/$CKVER.tar.gz"
PLATFORM=linux/arm/v7

DEPLOY=0
while getopts "d" opt; do
    case $opt in
        d) DEPLOY=1;;
        *) exit 1;;
    esac
done

# armhf jessie builder image: native gcc under qemu, matching the device's
# glibc 2.19. jessie is archived, so its apt sources point at archive.debian.org
# (main + the security pocket, whose deb8u2 toolchain packages the base image
# already partly has installed).
if ! docker image inspect $IMAGE >/dev/null 2>&1; then
    echo "Building $IMAGE builder image (qemu-emulated armhf) ..."
    docker build --platform $PLATFORM -t $IMAGE - <<'EOF'
FROM arm32v7/debian:jessie-slim
RUN set -e; \
    { echo 'deb http://archive.debian.org/debian jessie main'; \
      echo 'deb http://archive.debian.org/debian-security jessie/updates main'; \
    } > /etc/apt/sources.list \
 && echo 'Acquire::Check-Valid-Until "false";' > /etc/apt/apt.conf.d/10no-check-valid \
 && apt-get update \
 && apt-get install -y --no-install-recommends --allow-unauthenticated \
        gcc-4.9 make libc6-dev \
 && ln -sf gcc-4.9 /usr/bin/gcc \
 && rm -rf /var/lib/apt/lists/*
EOF
fi

mkdir -p "$WORKDIR"
SRC="$WORKDIR/src"
if [ ! -f "$SRC/ckcmai.c" ]; then
    echo "Fetching C-Kermit source ($CKVER) ..."
    [ -f "$WORKDIR/$CKVER.tar.gz" ] || curl -fsSL -o "$WORKDIR/$CKVER.tar.gz" "$CKURL"
    mkdir -p "$SRC"
    tar xzf "$WORKDIR/$CKVER.tar.gz" -C "$SRC"
fi

# Build the real compile+link target directly. The makefile's "linux" target
# probes for -lcrypt/-lresolv only under /usr/lib and /usr/lib64, missing the
# armhf multiarch path, so those libraries are named explicitly here. The
# feature flags match what "linux" would set for this glibc.
docker run --rm --platform $PLATFORM -v "$PWD/$SRC:/src" -w /src $IMAGE \
    make linuxa -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)" \
        "KFLAGS=-DHAVE_PTMX -DHAVE_OPENPTY -DHAVE_CRYPT_H \
-D_LARGEFILE_SOURCE -D_FILE_OFFSET_BITS=64" \
        "LIBS=-lcrypt -lresolv -lutil"

docker run --rm --platform $PLATFORM -v "$PWD/$SRC:/src" -w /src $IMAGE \
    sh -c 'cp wermit kermit && strip kermit'

BINARY="$SRC/kermit"
file "$BINARY"

if [ $DEPLOY = 1 ]; then
    echo "Deploying to $QUNILATOR_HOST:~/bin/kermit ..."
    ssh "$QUNILATOR_HOST" 'mkdir -p ~/bin'
    scp "$BINARY" "$QUNILATOR_HOST:bin/kermit.new"
    ssh "$QUNILATOR_HOST" 'mv ~/bin/kermit.new ~/bin/kermit && chmod +x ~/bin/kermit'
    echo "Deployed. Ensure ~/bin is on PATH, or run ~/bin/kermit."
fi
