#!/bin/bash
# Build the Debian package from an already cross-compiled binary. The QBUS build
# ships as "qbone", the UNIBUS build as "unibone" - same packaging, one brand
# rewritten to the other.
#
# Two programs are installed. "qbone-web" in the source tree becomes
# /usr/bin/<name>, the service the unit runs: it serves the web interface and
# has no menu. "demo" becomes /usr/bin/<name>-demo, the interactive tool for
# bus latches, master/slave tests and the device exercisers. The renames
# happen here so the tree stays mergeable with upstream.
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

# Board identity. The emulator is compiled for one bus, so the binary, the unit
# that runs it and the package carry the board's name: QBUS is qbone/QBone,
# UNIBUS is unibone/UniBone. The web interface shows the software's own product
# name, QUniLator, which is the same on both buses. The appliance's own files -
# the provisioning tools, their units, and the paths under /etc, /var/lib and
# /usr/share - are named "qunilator" whichever bus the cape bridges.
if [ "$SUFFIX" = _u ]; then
    NAME=unibone; DISPLAY=UniBone; OTHER=qbone; BUS=Unibus
else
    NAME=qbone;   DISPLAY=QBone;   OTHER=unibone; BUS=Qbus
fi

# Carry the board brand and its bus into the emulator's unit, which names its
# binary, and rewrite any board-brand token that lingers in a web asset.
# Everything else is installed as it is in the repository.
rebrand() {
    sed -e "s/qbone/$NAME/g" -e "s/QBone/$DISPLAY/g" -e "s/Qbus/$BUS/g"
}

BINARY=10.03_app_demo/4_deploy$SUFFIX/qbone-web
BINARY_DEMO=10.03_app_demo/4_deploy$SUFFIX/demo
if [ ! -x $BINARY ] || [ ! -x $BINARY_DEMO ]; then
    echo "no binary at $BINARY / $BINARY_DEMO - run ./crossbuild.sh first" >&2
    exit 1
fi

VERSION=$(sed -n 's/^[a-z]* (\([^)]*\)).*/\1/p' packaging/debian/changelog | head -1)
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
# mktemp makes it private; the package root has to be world readable
chmod 755 "$STAGE"

install -d -m 755 $STAGE/DEBIAN \
    $STAGE/etc/modprobe.d \
    $STAGE/etc/modules-load.d \
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

# The emulator, built for this board's bus
install -m 755 $BINARY $STAGE/usr/bin/$NAME
install -m 755 $BINARY_DEMO $STAGE/usr/bin/$NAME-demo
# its unit, the one that names the binary
rebrand < packaging/debian/qbone.service > $STAGE/lib/systemd/system/$NAME.service
# the web root: the Vite build output. index.html, the hashed JS/CSS bundles and
# the manifest carry the display brand, so rebrand those text assets; the
# favicons and other binaries copy as-is. The bundle hash is not recomputed, but
# index.html references the assets by their built names, so this stays coherent.
DIST=10.05_web/3_frontend/dist
for f in "$DIST"/*; do
    [ -f "$f" ] || continue
    b=$(basename "$f")
    case "$b" in
        index.html|site.webmanifest)
            rebrand < "$f" > $STAGE/usr/share/qunilator/frontend/"$b"
            chmod 644 $STAGE/usr/share/qunilator/frontend/"$b" ;;
        *)
            install -m 644 "$f" $STAGE/usr/share/qunilator/frontend/"$b" ;;
    esac
done
for f in "$DIST"/assets/*; do
    [ -f "$f" ] || continue
    b=$(basename "$f")
    case "$b" in
        *.js|*.css)
            rebrand < "$f" > $STAGE/usr/share/qunilator/frontend/assets/"$b"
            chmod 644 $STAGE/usr/share/qunilator/frontend/assets/"$b" ;;
        *)
            install -m 644 "$f" $STAGE/usr/share/qunilator/frontend/assets/"$b" ;;
    esac
done

# Everything below manages a BeagleBone carrying a cape and does the same job
# whichever bus it bridges, so it installs exactly as it is in the repository.
install -m 755 packaging/debian/qunilator-network packaging/debian/qunilator-setup \
    packaging/debian/qunilator-resize packaging/debian/qunilator-announce \
    packaging/debian/qunilator-rename $STAGE/usr/sbin/
# status LEDs: a tiny standalone daemon, cross-compiled here
arm-linux-gnueabihf-gcc -O2 -Wall -o $STAGE/usr/sbin/qunilator-leds packaging/debian/qunilator-leds.c
install -m 644 packaging/debian/qunilator-network.service \
    packaging/debian/qunilator-setup.service packaging/debian/qunilator-leds.service \
    packaging/debian/qunilator-resize.service packaging/debian/qunilator-announce.service \
    $STAGE/lib/systemd/system/
install -m 644 packaging/debian/network.conf $STAGE/etc/qunilator/network.conf
# The bundled empty configuration. The service adopts it as the default on a
# board that has never had one set, so a valid startup configuration always
# exists. Shipped as a template and copied into place by postinst only when
# absent, so an operator's own default.json is never overwritten.
install -m 644 packaging/debian/default-config.json $STAGE/usr/share/qunilator/default-config.json
# the image-introspection decoders: the web interface shells out to introspect.py
# to list the files inside an RT-11 / Files-11 image
install -d -m 755 $STAGE/usr/share/qunilator/decoders
install -m 644 packaging/decoders/*.py $STAGE/usr/share/qunilator/decoders/
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
    echo "Depends: libc6, libstdc++6, libgcc-s1, libx11-6, iproute2, device-tree-compiler, cpp, make, python3"
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
    install -d -m 755 /var/lib/qunilator/images
    chown -R qunilator:qunilator /var/lib/qunilator/images || true
    # Seed the bundled empty configuration as the startup fallback, without
    # ever clobbering an operator's own default.json.
    if [ ! -e /var/lib/qunilator/configs/default.json ]; then
        install -d -m 755 /var/lib/qunilator/configs
        install -m 644 /usr/share/qunilator/default-config.json \
            /var/lib/qunilator/configs/default.json || true
    fi
    if [ -d /run/systemd/system ]; then
        systemctl daemon-reload || true
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
    systemctl disable qbone.service qunilator-network.service || true
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
