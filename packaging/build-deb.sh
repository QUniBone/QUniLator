#!/bin/bash
# Build the Debian package from an already cross-compiled binary. The QBUS build
# ships as "qbone", the UNIBUS build as "unibone" - same packaging, one brand
# rewritten to the other.
#
# Two programs are installed. "qbone-web" in the source tree becomes
# /usr/bin/<name>, the service the unit runs: it serves the web interface and
# has no menu. "demo" becomes /usr/bin/<name>-cli, the interactive tool for
# bus latches, master/slave tests and the device exercisers. The renames
# happen here so the tree stays mergeable with upstream.
#
# <name>-cli drives the PRU and the bus, which is root's work, and it is run by
# an operator at a terminal rather than through sudo: it is installed set-user-id
# root and executable only by qunilator-admin, the group that already carries the
# right to sudo and a login shell. Everyone outside that group is left with the
# web interface.
#
# The binary carries the PRU firmware inside it, and links against libraries
# the appliance image already has, which the control file names. dpkg-deb runs
# in the same container the cross build uses, since macOS has no dpkg.
#
# usage:
#   ./packaging/build-deb.sh            package the QBUS build as "qbone"
#   ./packaging/build-deb.sh -u         package the UNIBUS build as "unibone"

set -e
cd "$(dirname "$0")/.."

# The web frontend is a Vite + Preact + TypeScript project; its bundle is built
# on the dev machine and shipped as the civetweb docroot (source only in git).
# Node lives on the dev machine, not in the Debian packaging container, so build
# here before the container re-entry below; inside the container npm is absent
# and the already-built dist/ is used.
FRONTEND=10.05_web/3_frontend
if command -v npm >/dev/null 2>&1; then
    echo "Building web frontend ..."
    ( cd "$FRONTEND" && npm ci && npm run build )
elif [ ! -d "$FRONTEND/dist" ]; then
    echo "no $FRONTEND/dist and no npm to build it -" >&2
    echo "run 'npm ci && npm run build' in $FRONTEND first" >&2
    exit 1
fi

# dpkg-deb, GNU find and dtc are all Debian tools, and the host is usually
# macOS, so the packaging runs in a container. Re-enter one when the tools are
# not here; inside, the check passes and the script continues.
IMAGE=qunibone-package
if ! command -v dpkg-deb >/dev/null 2>&1 || ! command -v dtc >/dev/null 2>&1; then
    if ! command -v docker >/dev/null 2>&1; then
        echo "needs dpkg-deb and dtc, or docker to supply them" >&2
        exit 1
    fi
    if ! docker image inspect $IMAGE >/dev/null 2>&1; then
        echo "Building $IMAGE docker image ..."
        docker build -t $IMAGE - <<'EOF'
FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
        device-tree-compiler xz-utils \
        gcc-arm-linux-gnueabihf libc6-dev-armhf-cross \
    && rm -rf /var/lib/apt/lists/*
EOF
    fi
    exec docker run --rm -v "$PWD:/qunibone" -w /qunibone $IMAGE \
        ./packaging/build-deb.sh "$@"
fi

SUFFIX=_q
while getopts "u" opt; do
    case $opt in
        u) SUFFIX=_u;;
        *) exit 1;;
    esac
done

# Board identity, the rebrand filter that carries it into the emulator's unit
# and the web assets, and the staging of the web root. Everything else is
# installed as it is in the repository.
. packaging/board.sh

BINARY=10.03_app_demo/4_deploy$SUFFIX/qbone-web
BINARY_DEMO=10.03_app_demo/4_deploy$SUFFIX/demo
if [ ! -x $BINARY ] || [ ! -x $BINARY_DEMO ]; then
    echo "no binary at $BINARY / $BINARY_DEMO - run ./crossbuild.sh first" >&2
    exit 1
fi

VERSION=$(packaging/version.sh)
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
# mktemp makes it private; the package root has to be world readable
chmod 755 "$STAGE"

install -d -m 755 $STAGE/DEBIAN \
    $STAGE/etc/modprobe.d \
    $STAGE/etc/modules-load.d \
    $STAGE/etc/apt/apt.conf.d \
    $STAGE/usr/bin \
    $STAGE/usr/share/qunilator/frontend/assets \
    $STAGE/usr/share/doc/$NAME \
    $STAGE/etc/qunilator \
    $STAGE/lib/systemd/system \
    $STAGE/lib/firmware \
    $STAGE/usr/sbin \
    $STAGE/usr/share/qunilator/network \
    $STAGE/var/lib/qunilator/images \
    $STAGE/var/lib/qunilator/configs
# The media tree's seeded folders, one per medium by DEC device mnemonic, plus
# roms/ for the images a PROM card is programmed from. Shipped empty so an
# operator uploading a file over the web interface or one of the file shares
# finds the place it belongs, and so the layout the API documents is the layout
# a fresh board has. Nested folders below these are the operator's own.
install -d -m 755 $STAGE/var/lib/qunilator/images/dk \
    $STAGE/var/lib/qunilator/images/dl \
    $STAGE/var/lib/qunilator/images/du \
    $STAGE/var/lib/qunilator/images/mu \
    $STAGE/var/lib/qunilator/images/rx \
    $STAGE/var/lib/qunilator/images/roms
# The updater's state: staged packages, the cached previous package and the
# version the interface requested. It sits inside the state directory the file
# shares are chrooted to, so the private mode is what keeps a share login away
# from it.
install -d -m 700 $STAGE/var/lib/qunilator/updates

# The emulator, built for this board's bus
install -m 755 $BINARY $STAGE/usr/bin/$NAME
install -m 755 $BINARY_DEMO $STAGE/usr/bin/$NAME-cli
# its unit, the one that names the binary
rebrand < packaging/debian/qbone.service > $STAGE/lib/systemd/system/$NAME.service
# the seed unit runs that binary and orders itself before that unit, so it
# carries the board's name too
rebrand < packaging/debian/qunilator-seed.service \
    > $STAGE/lib/systemd/system/qunilator-seed.service
chmod 644 $STAGE/lib/systemd/system/qunilator-seed.service
# the web root, branded for this board
stage_frontend $STAGE/usr/share/qunilator/frontend

# Everything below manages a BeagleBone carrying a cape and does the same job
# whichever bus it bridges, so it installs exactly as it is in the repository.
install -m 755 packaging/debian/qunilator-network packaging/debian/qunilator-setup \
    packaging/debian/qunilator-resize packaging/debian/qunilator-announce \
    packaging/debian/qunilator-rename packaging/debian/qunilator-update \
    packaging/debian/qunilator-devkit \
    packaging/debian/qunilator-usb-gadget $STAGE/usr/sbin/
# The sample disk images are in neither the repository nor this package - too
# large, and not all of them ours to distribute - so the tool that fetches them
# from retrocmp ships instead, under the name the other board commands have. It
# needs no checkout to work: with no qunibone-platform.env or build.env to read,
# it takes the bus from which emulator is installed here.
install -m 755 tools/fetch-images.py $STAGE/usr/sbin/qunilator-fetch-images
# status LEDs: a tiny standalone daemon, cross-compiled here
arm-linux-gnueabihf-gcc -O2 -Wall -o $STAGE/usr/sbin/qunilator-leds packaging/debian/qunilator-leds.c
install -m 644 packaging/debian/qunilator-network.service \
    packaging/debian/qunilator-setup.service packaging/debian/qunilator-leds.service \
    packaging/debian/qunilator-resize.service packaging/debian/qunilator-announce.service \
    packaging/debian/qunilator-update.service packaging/debian/qunilator-update-os.service \
    packaging/debian/qunilator-update-check.service \
    packaging/debian/qunilator-update-check.timer \
    packaging/debian/qunilator-usb-gadget.service \
    $STAGE/lib/systemd/system/
# The login on the gadget's serial port needs agetty to open that port itself;
# the drop-in says why.
install -d -m 755 $STAGE/lib/systemd/system/serial-getty@ttyGS0.service.d
install -m 644 packaging/debian/serial-getty-ttyGS0.conf \
    $STAGE/lib/systemd/system/serial-getty@ttyGS0.service.d/10-gadget.conf
# The three UARTs, so a login the web interface moves onto any of them comes up
# at the speed the others run at. The drop-in only describes the login; which
# ports carry one is the interface's setting.
for uart in ttyS0 ttyS1 ttyS2; do
    install -d -m 755 $STAGE/lib/systemd/system/serial-getty@$uart.service.d
    install -m 644 packaging/debian/serial-getty-uart.conf \
        $STAGE/lib/systemd/system/serial-getty@$uart.service.d/10-uart.conf
done
# An unattended upgrade would stop the operator's running machine with no
# warning, which is what the interface's install dialog exists to prevent.
install -m 644 packaging/debian/apt-unattended-qunilator.conf \
    $STAGE/etc/apt/apt.conf.d/51qunilator-unattended
install -m 644 packaging/debian/network.conf $STAGE/etc/qunilator/network.conf
# Shell access to the board, granted to a group rather than to an account.
# sudo refuses a file it can write or that anyone but root owns, so 0440.
install -d -m 750 $STAGE/etc/sudoers.d
install -m 440 packaging/debian/sudoers-qunilator-admin \
    $STAGE/etc/sudoers.d/qunilator-admin
# The bundled empty configuration. The service adopts it as the default on a
# board that has never had one set, so a valid startup configuration always
# exists. Shipped as a template and copied into place by postinst only when
# absent, so an operator's own default.json is never overwritten.
install -m 644 packaging/debian/default-config.json $STAGE/usr/share/qunilator/default-config.json
# The sample machine. XXDP is the DEC diagnostic monitor, small enough to ship
# and the one pack that says whether a board works at all, so a freshly flashed
# board has something to run before its operator has found anything of their
# own. The pack is read-only and belongs to the package: a drive takes its
# writes into a copy-on-write overlay, so the shipped file stays as it was.
# Stored compressed in git, where it is the one binary that is not a build
# product. It is an RL02 pack, so it goes in dl/ like every other one.
xz -dc packaging/images/xxdp25.rl02.xz > $STAGE/var/lib/qunilator/images/dl/xxdp25.rl02
chmod 444 $STAGE/var/lib/qunilator/images/dl/xxdp25.rl02
# and the machine that boots it, as a template postinst copies in when the
# board has no configuration of that name. Only a UNIBUS build carries the
# processors, so that one is a whole PDP-11/20 with the ROMs to boot the pack;
# a QBUS board is a peripheral of a real machine, which brings its own.
install -m 644 packaging/debian/sample-config$SUFFIX.json \
    $STAGE/usr/share/qunilator/sample-config.json
# the image-introspection decoders: the web interface shells out to introspect.py
# to list the files inside an RT-11 / Files-11 image
install -d -m 755 $STAGE/usr/share/qunilator/decoders
install -m 644 packaging/decoders/*.py $STAGE/usr/share/qunilator/decoders/
# the M9312 PROM listings (ak6dn.com), the console/diagnostic ROM and the
# per-controller boot ROMs. The M9312 loads a socket from one of these files, so
# a board with an emulated CPU has something to boot from.
install -d -m 755 $STAGE/usr/share/qunilator/roms
install -m 644 10.02_devices/4_deploy/*.lst $STAGE/usr/share/qunilator/roms/
# DNS-SD advertisement for the web interface. A template: qunilator-setup substitutes
# the board's name and identifier and installs it under /etc/avahi/services, so
# the file under /etc is generated rather than a conffile every board would show
# as locally modified.
install -m 644 packaging/debian/avahi-qunilator.service $STAGE/usr/share/qunilator/avahi-qunilator.service
# referenced by the units' Documentation=, and by policy under the package's doc
# directory, which is named for the package
install -m 644 packaging/debian/README.Debian $STAGE/usr/share/qunilator/README.Debian
install -m 644 packaging/debian/README.Debian $STAGE/usr/share/doc/$NAME/README.Debian
chmod 644 $STAGE/lib/systemd/system/$NAME.service

# qunilator-setup builds this into the loaded DTB so eth0 is a plain, bridgeable
# NIC. The same eth0 fix serves both boards.
install -m 644 02_bbb_config/01_cape/am335x-boneblack-bone.dts \
    $STAGE/usr/share/qunilator/am335x-boneblack-bone.dts
# the bridge that carries the emulated machine, installed by qunilator-setup
for f in br0.netdev br0.network eth0.network veth-br.network veth-pdp.network usb0.network; do
    install -m 644 packaging/debian/network/$f $STAGE/usr/share/qunilator/network/$f
done

# Both cape overlays: capemgr loads UniBone-00B0.dtbo from the cape's EEPROM
# up to 4.19, and U-Boot applies QBone.dtbo by name after it. Both boards use
# the same bus-agnostic overlay, so these names do not track the brand.
install -m 644 02_bbb_config/01_cape/UniBone-00B0.dtbo $STAGE/lib/firmware/
dtc -@ -I dts -O dtb -o $STAGE/lib/firmware/QBone.dtbo \
    02_bbb_config/01_cape/QBone.dtso 2>&1 \
    | grep -v "ranges_format\|avoid_default_addr_size\|avoid_unnecessary_addr_size\|unique_unit_address" || true
[ -s $STAGE/lib/firmware/QBone.dtbo ] || { echo "dtc produced no overlay" >&2; exit 1; }
chmod 644 $STAGE/lib/firmware/QBone.dtbo

# uio_pdrv_genirq matches no compatible of its own; the one it looks for is a
# module parameter, so the overlay's node binds to nothing until it is set.
install -m 644 packaging/debian/modprobe-bone.conf $STAGE/etc/modprobe.d/bone.conf
install -m 644 packaging/debian/modules-load-bone.conf $STAGE/etc/modules-load.d/bone.conf
rebrand < packaging/debian/changelog | gzip -9 -n -c > $STAGE/usr/share/doc/$NAME/changelog.Debian.gz
chmod 644 $STAGE/usr/share/doc/$NAME/changelog.Debian.gz

# binary control file: the source stanza's fields, plus the installed size
INSTALLED_KB=$(du -sk $STAGE | cut -f1)
{
    echo "Package: $NAME"
    echo "Version: $VERSION"
    echo "Section: misc"
    echo "Priority: optional"
    echo "Architecture: armhf"
    # libc6, libstdc++6 and libgcc-s1 are what the emulator links against -
    # the NEEDED entries of the binary, no more. iproute2 is for the ip(8)
    # calls in <name>-network and <name>-setup; device-tree-compiler, cpp and
    # make let <name>-setup build the legacy-Ethernet device tree. The
    # operator toolset and the nginx removal belong to the image preparation,
    # not here. Spelled out rather than taken from packaging/debian/control,
    # whose ${misc:Depends} is a debhelper substitution this build does not do,
    # and which has no ${shlibs:Depends} to compute the libraries either.
    # curl is what qunilator-update's health check speaks HTTP to the loopback
    # with, after an install; the appliance image uses curl in the build
    # container, not in the chroot, so it has to be declared rather than assumed.
    echo "Depends: libc6, libstdc++6, libgcc-s1, libx11-6, iproute2, device-tree-compiler, cpp, make, python3, curl"
    # the two boards ship the same cape overlay and firmware files, and a BBB
    # carries one cape, so they are mutually exclusive on a machine
    echo "Conflicts: $OTHER"
    echo "Replaces: $OTHER"
    echo "Maintainer: Hans Huebner <hans.huebner@gmail.com>"
    echo "Installed-Size: $INSTALLED_KB"
    sed -n '/^Description:/,$p' packaging/debian/control | rebrand
} > $STAGE/DEBIAN/control

# files under /etc, which dpkg must not overwrite once they have been edited
{
    echo "/etc/qunilator/network.conf"
    echo "/etc/modprobe.d/bone.conf"
    echo "/etc/modules-load.d/bone.conf"
    echo "/etc/apt/apt.conf.d/51qunilator-unattended"
    echo "/etc/sudoers.d/qunilator-admin"
} > $STAGE/DEBIAN/conffiles

cat > $STAGE/DEBIAN/preinst <<'PREINST'
#!/bin/sh
set -e
# The emulator took its startup commands from this file while it was the menu
# program. The service has no menu, and the machine a board comes up as is a
# saved configuration, so the file is obsolete: dpkg leaves a conffile behind
# when a package stops shipping it, and this removes it.
if [ -e /etc/qbone/startup.cmd ]; then
    # The helper also clears dpkg's own record of the conffile, but it acts
    # only when the version being replaced is older than its guard. Reinstalls
    # and rebuilds of one version leave the file behind, so remove it either
    # way: nothing reads it any more.
    if [ -x /usr/bin/dpkg-maintscript-helper ] \
            && dpkg-maintscript-helper supports rm_conffile 2>/dev/null; then
        dpkg-maintscript-helper rm_conffile /etc/qbone/startup.cmd 1.6.0-1~ -- "$@" || true
    fi
    rm -f /etc/qbone/startup.cmd
fi
# The appliance's config directory moved from /etc/bone to /etc/qunilator when
# the software was named QUniLator. Carry an operator's edited network.conf
# across so dpkg does not orphan it as a stale conffile.
if [ -x /usr/bin/dpkg-maintscript-helper ] \
        && dpkg-maintscript-helper supports mv_conffile 2>/dev/null; then
    dpkg-maintscript-helper mv_conffile \
        /etc/bone/network.conf /etc/qunilator/network.conf 1.12.0-1~ -- "$@"
fi
PREINST
cat > $STAGE/DEBIAN/postinst <<'POSTINST'
#!/bin/sh
set -e
if [ -x /usr/bin/dpkg-maintscript-helper ] \
        && dpkg-maintscript-helper supports mv_conffile 2>/dev/null; then
    dpkg-maintscript-helper mv_conffile \
        /etc/bone/network.conf /etc/qunilator/network.conf 1.12.0-1~ -- "$@"
fi
if [ "$1" = configure ]; then
    # Carry the pre-QUniLator layout forward: persistent state moves from
    # /var/lib/bone to /var/lib/qunilator, and the provisioning units were
    # renamed bone-* to qunilator-*. dpkg removes the old package files on
    # upgrade, but the operator's state and the old enable-symlinks are ours.
    if [ -d /var/lib/bone ] && [ ! -d /var/lib/qunilator ]; then
        mv /var/lib/bone /var/lib/qunilator
    fi
    if [ -d /run/systemd/system ]; then
        for old in bone-network bone-setup bone-resize bone-announce bone-leds; do
            systemctl disable "$old.service" >/dev/null 2>&1 || true
        done
    fi
    # A dedicated system user owns the image tree so the SMB/FTP/SFTP shares can
    # serve it under one login; the emulator itself keeps running as root for the
    # PRU. The web interface sets this user's password when the operator sets the
    # web password. Creation is idempotent.
    if ! getent group qunilator >/dev/null; then
        addgroup --system qunilator || true
    fi
    if ! getent passwd qunilator >/dev/null; then
        adduser --system --ingroup qunilator --home /var/lib/qunilator \
            --no-create-home --shell /usr/sbin/nologin qunilator || true
    fi
    # Membership of this group is what carries the right to sudo, and sshd
    # leaves a member of it a shell rather than the SFTP session the qunilator
    # group otherwise gets. The web interface puts the operator's account in it.
    if ! getent group qunilator-admin >/dev/null; then
        addgroup --system qunilator-admin || true
    fi
    # The interactive program takes the PRU, the GPIOs and the bus, all of which
    # are root's. An operator at a terminal runs it directly rather than through
    # sudo, so it is set-user-id root, and only qunilator-admin may execute it -
    # the same group that already carries sudo and a login shell. Re-asserted on
    # every upgrade: the group does not exist while dpkg-deb builds the archive,
    # so the mode cannot be carried in it.
    cli="/usr/bin/${DPKG_MAINTSCRIPT_PACKAGE:-qbone}-cli"
    if [ -x "$cli" ]; then
        chown root:qunilator-admin "$cli" || true
        chmod 4750 "$cli" || true
    fi
    install -d -m 2775 /var/lib/qunilator/images
    # the seeded media folders, re-asserted on an upgrade so a board that
    # predates one of them gets it
    for d in dk dl du mu rx roms; do
        install -d -m 2775 /var/lib/qunilator/images/$d || true
    done
    # The sample pack sits in dl/ with the tree's other RL packs. A board that
    # carries it at the root of the tree follows it there: the overlay holding
    # what was written to the pack moves with it, and a configuration naming the
    # old path is pointed at the new one. Both spellings of the path are
    # rewritten - a saved configuration escapes the separator as \/ - and the
    # dot file holding the running configuration counts as one.
    #
    # The emulator is stopped for the move: one still running holds the pack and
    # writes the overlay's bitmap back to the old path as it stops. It is started
    # again with the rest of the units below.
    imgs=/var/lib/qunilator/images
    cfgs=/var/lib/qunilator/configs
    pack_re='images\\\{0,1\}/xxdp25\.rl02'
    emulator_was_active=
    if [ -e "$imgs/xxdp25.rl02.ovl" ] || [ -e "$imgs/xxdp25.rl02.ovl.map" ] \
            || grep -qs "$pack_re" $cfgs/*.json $cfgs/.*.json; then
        if [ -d /run/systemd/system ] && systemctl is-active --quiet qbone.service; then
            emulator_was_active=yes
            systemctl stop qbone.service || true
        fi
        for f in xxdp25.rl02.ovl xxdp25.rl02.ovl.map; do
            if [ -e "$imgs/$f" ]; then
                mv -f "$imgs/$f" "$imgs/dl/$f" || true
            fi
        done
        sed -i -e 's|images/xxdp25\.rl02|images/dl/xxdp25.rl02|g' \
               -e 's|images\\/xxdp25\.rl02|images\\/dl\\/xxdp25.rl02|g' \
            $cfgs/*.json $cfgs/.*.json 2>/dev/null || true
    fi
    # The tree belongs to the qunilator group and every member of it may write:
    # the service account owns the files, and the operator account the web
    # interface creates for the file shares reaches them through the group.
    # set-group-id on the directories keeps what either of them creates in the
    # group. Re-asserted on every upgrade, next to the ownership.
    chown -R qunilator:qunilator /var/lib/qunilator/images || true
    chmod -R g+w /var/lib/qunilator/images || true
    find /var/lib/qunilator/images -type d -exec chmod g+s {} + || true
    # The updater's state stays root-only: it sits inside the tree the SMB/FTP/
    # SFTP shares are chrooted to, and holds staged packages and the requested
    # version. Re-asserted on every upgrade in case a mode was widened.
    install -d -m 700 -o root -g root /var/lib/qunilator/updates
    # Seed the bundled empty configuration as the startup fallback, without
    # ever clobbering an operator's own default.json.
    if [ ! -e /var/lib/qunilator/configs/default.json ]; then
        install -d -m 755 /var/lib/qunilator/configs
        install -m 644 /usr/share/qunilator/default-config.json \
            /var/lib/qunilator/configs/default.json || true
    fi
    # The sample machine that boots the shipped XXDP pack, on the same terms:
    # placed when the board has no configuration by that name, so an operator
    # who has edited or deleted it keeps their decision.
    if [ ! -e /var/lib/qunilator/configs/xxdp.json ]; then
        install -d -m 755 /var/lib/qunilator/configs
        install -m 644 /usr/share/qunilator/sample-config.json \
            /var/lib/qunilator/configs/xxdp.json || true
    fi
    if [ -d /run/systemd/system ]; then
        systemctl daemon-reload || true
        # The update check, enabled on an upgrade as well as on a first install,
        # so a board that predates self-update starts checking once it has been
        # updated by hand. The timer only reads a version and writes a status
        # file; nothing is installed without an operator asking for it.
        systemctl enable --now qunilator-update-check.timer || true
        # The seed reader, enabled on an upgrade as well: it is what lets an
        # operator whose password is gone write a file onto the card's boot
        # partition from any workstation and have it applied at the next boot.
        # Its condition passes it over on every boot that finds no such file.
        systemctl enable qunilator-seed.service || true
        # The USB gadget, enabled on an upgrade as well, so a board that
        # predates it gains the fixed address its login banner offers. Building
        # a gadget takes nothing away from a board already reachable over the
        # LAN, and the unit's condition passes it over where there is no
        # peripheral controller to bind.
        systemctl enable --now qunilator-usb-gadget.service || true
        # A login on the gadget's serial port. An image built before the gadget
        # existed masked this getty, because the tty it names was never created
        # and the unit spun; the gadget creates it now, so lift the mask.
        systemctl unmask serial-getty@ttyGS0.service || true
        systemctl enable --now serial-getty@ttyGS0.service || true
        # $2 is the previously configured version, empty on a first install.
        # Enabling only then keeps a unit the operator disabled disabled.
        # The emulator needs boot settings qunilator-setup applies and a reboot to
        # pick them up, so a first install enables without starting.
        if [ -z "$2" ]; then
            systemctl enable qunilator-network.service qbone.service || true
        else
            for unit in qunilator-network.service qbone.service; do
                if systemctl is-active --quiet $unit; then
                    systemctl restart $unit || true
                fi
            done
            # the emulator stopped for the media-tree move, above
            if [ -n "$emulator_was_active" ]; then
                systemctl start qbone.service || true
            fi
        fi
    fi
    echo "qbone: run 'sudo qunilator-setup' to configure the boot settings, the"
    echo "qbone: network and the services. See /usr/share/doc/qbone/README.Debian"
fi
POSTINST
cat > $STAGE/DEBIAN/prerm <<'PRERM'
#!/bin/sh
set -e
if [ "$1" = remove ] && [ -d /run/systemd/system ]; then
    systemctl stop qunilator-update-check.timer || true
    systemctl stop qbone.service || true
    systemctl stop qunilator-network.service || true
fi
PRERM
cat > $STAGE/DEBIAN/postrm <<'POSTRM'
#!/bin/sh
set -e
if [ -x /usr/bin/dpkg-maintscript-helper ] \
        && dpkg-maintscript-helper supports mv_conffile 2>/dev/null; then
    dpkg-maintscript-helper mv_conffile \
        /etc/bone/network.conf /etc/qunilator/network.conf 1.12.0-1~ -- "$@"
fi
if [ "$1" = remove ] && [ -d /run/systemd/system ]; then
    systemctl disable qbone.service qunilator-network.service \
        qunilator-update-check.timer || true
    systemctl daemon-reload || true
fi
POSTRM
# the maintainer scripts are written with the qbone brand, then rewritten in
# place to this board's, so $NAME does not have to be threaded through their
# runtime-shell variables ($1, $2, $@)
for s in preinst postinst prerm postrm; do
    rebrand < "$STAGE/DEBIAN/$s" > "$STAGE/DEBIAN/$s.rebranded"
    mv "$STAGE/DEBIAN/$s.rebranded" "$STAGE/DEBIAN/$s"
done
chmod 755 $STAGE/DEBIAN/preinst $STAGE/DEBIAN/postinst $STAGE/DEBIAN/prerm $STAGE/DEBIAN/postrm

# md5sums over everything outside DEBIAN
( cd $STAGE && find . -type f ! -path "./DEBIAN/*" -printf "%P\0" \
    | xargs -0 md5sum > DEBIAN/md5sums )
chmod 644 $STAGE/DEBIAN/md5sums

# xz keeps the archive readable by the dpkg on Debian 8, which predates zstd
OUT=${NAME}_${VERSION}_armhf.deb
dpkg-deb -Zxz --build --root-owner-group "$STAGE" "$OUT"
echo
dpkg-deb --info "$OUT"
echo
dpkg-deb --contents "$OUT"
