#/!bin/bash
# option "-a": recompile all from scratch
# else rely on makefile rules
#
# to be called after qunibone-platform.sh

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

cd 10.03_app_demo/2_src

if [ "$1" == "-a" ] ; then
  make clean
fi

make
cd ~

echo "To run binary, call"
echo "10.03_app_demo/4_deploy/demo"

