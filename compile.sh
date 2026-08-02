#!/bin/bash
# Build the emulator on the BeagleBone itself. crossbuild.sh is the fast path on
# a desktop; this is the reference build, and the one to reach for when
# debugging on the board.
#
# option "-a": recompile all from scratch
# else rely on makefile rules
#
# needs qunibone-platform.env, which qunibone-platform.sh writes from
# qunibone-platform.env.example; see README.Debian for what to install first

cd "$(dirname "$0")"

# The installation root every makefile resolves its paths against: this tree,
# so a checkout is built wherever it was cloned to.
export QUNILATOR_DIR=${QUNILATOR_DIR:-$PWD}

. qunibone-platform.env
. compile-bbb.env

# guard against legacy qunibone-platform.env
if [ -z "$QUNILATOR_PLATFORM_SUFFIX" ] ; then
        QUNILATOR_PLATFORM_SUFFIX=$PLATFORM_SUFFIX
fi
if [ -z "$QUNILATOR_PLATFORM" ] ; then
        QUNILATOR_PLATFORM=$MAKE_QUNIBUS
fi

# makefile_u or makefile_q
MAKEFILE=makefile$QUNILATOR_PLATFORM_SUFFIX

# Debugging: remote from Eclipse. Compile on BBB is release.
export MAKE_CONFIGURATION=RELEASE
export QUNILATOR_PLATFORM

# Sun RPC, which the blinkenlight API client speaks. glibc dropped it, so the
# board links libtirpc instead - package libtirpc-dev, whose headers sit in a
# subdirectory of their own.
export CCDEFS="-I/usr/include/tirpc"
export EXTRA_LIBS="-ltirpc"

cd 10.03_app_demo/2_src

if [ "$1" == "-a" ] ; then
  make clean
fi

make CCDEFS="$CCDEFS" EXTRA_LIBS="$EXTRA_LIBS"
cd - >/dev/null

DEPLOY=10.03_app_demo/4_deploy$QUNILATOR_PLATFORM_SUFFIX
echo "Built $DEPLOY/qbone-web, the service that serves the web interface,"
echo "and $DEPLOY/demo, the same emulation driven from a terminal menu."

