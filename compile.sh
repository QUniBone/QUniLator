#!/bin/bash
# Build the emulator on the BeagleBone itself. crossbuild.sh is the fast path on
# a desktop; this is the reference build, and the one to reach for when
# debugging on the board.
#
# options:
#   -a  recompile all from scratch, else rely on makefile rules
#   -n  do not restart the service after installing the new binaries
#   -N  do not install what was built at all, only build it
#
# On a board - one where <name>.service is installed - the two binaries that
# come out are put where the board runs them: the web service becomes
# /usr/bin/<name> and the menu-driven program /usr/bin/<name>-cli, and the
# service is restarted onto the new binary. That restart takes the emulated
# machine down with it, which is what -n is for.
#
# needs qunibone-platform.env, which qunibone-platform.sh writes from
# qunibone-platform.env.example; see README.Debian for what to install first

cd "$(dirname "$0")"

MAKE_CLEAN=0
INSTALL=1
RESTART=1
while [ $# -gt 0 ] ; do
    case "$1" in
        -a) MAKE_CLEAN=1 ;;
        -n) RESTART=0 ;;
        -N) INSTALL=0 ;;
        -h|--help)
            sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *)  echo "$(basename "$0"): unknown option \"$1\"" >&2
            echo "try \"$0 -h\"" >&2
            exit 1 ;;
    esac
    shift
done

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

if [ $MAKE_CLEAN = 1 ] ; then
  make clean
fi

if ! make CCDEFS="$CCDEFS" EXTRA_LIBS="$EXTRA_LIBS" ; then
  echo "The build failed; nothing was installed." >&2
  exit 1
fi
cd - >/dev/null

DEPLOY=10.03_app_demo/4_deploy$QUNILATOR_PLATFORM_SUFFIX
echo "Built $DEPLOY/qbone-web, the service that serves the web interface,"
echo "and $DEPLOY/demo, the same emulation driven from a terminal menu."

# The board's own name: the emulator is installed as /usr/bin/<name> and run by
# <name>.service. Same mapping as an appliance package, so a build on the board
# replaces exactly what the package put there.
SUFFIX=$QUNILATOR_PLATFORM_SUFFIX
. packaging/board.sh

if [ $INSTALL = 0 ] ; then
    exit 0
fi
if [ ! -f "/lib/systemd/system/$NAME.service" ] ; then
    # not a board, or one the package was never installed on: building is all
    # this can do, and saying so beats installing over somebody's desktop
    echo "No $NAME.service here, so this is not a board: nothing installed."
    exit 0
fi

# Everything below writes under /usr and restarts a unit.
SUDO=
if [ "$(id -u)" != 0 ] ; then
    SUDO=sudo
    if ! command -v sudo >/dev/null ; then
        echo "Not root and no sudo: run \"$0\" as root to install what it built." >&2
        exit 1
    fi
fi

# Install beside the target and rename: replaces a binary which is running.
# <name>-cli drives the PRU and the bus, which is root's work, so it is
# set-user-id root and executable only by qunilator-admin - the same ownership
# the package's postinst applies.
echo "Installing $DEPLOY/qbone-web as /usr/bin/$NAME ..."
$SUDO install -m 755 "$DEPLOY/qbone-web" "/usr/bin/$NAME.new" \
    && $SUDO mv "/usr/bin/$NAME.new" "/usr/bin/$NAME" || exit 1
echo "Installing $DEPLOY/demo as /usr/bin/$NAME-cli ..."
$SUDO install -m 755 "$DEPLOY/demo" "/usr/bin/$NAME-cli.new" \
    && $SUDO mv "/usr/bin/$NAME-cli.new" "/usr/bin/$NAME-cli" || exit 1
if getent group qunilator-admin >/dev/null ; then
    $SUDO chown root:qunilator-admin "/usr/bin/$NAME-cli"
    $SUDO chmod 4750 "/usr/bin/$NAME-cli"
fi

if [ $RESTART = 1 ] ; then
    echo "Restarting $NAME.service - the machine it is running goes down with it."
    $SUDO systemctl restart "$NAME.service"
else
    echo "$NAME.service still runs the old binary; \"sudo systemctl restart $NAME\" when ready."
fi

