#!/bin/bash
# build-image.sh - build the distribution image from the rcn-ee base.
#
# Produces a bootable microSD image: the rcn-ee Debian base, customised into an
# appliance - the emulator installed, eth0 moved to the legacy Ethernet driver,
# boot settings applied, the operator toolset added and nginx/cockpit removed.
# The machines a board can run come from its package, which carries the XXDP
# sample, and from configurations an operator imports. NAME picks the board:
# qbone (QBUS, the default) or unibone (UNIBUS); it sets the package installed,
# the hostname and the image name.
#
# The Linux-specific work (loop-mounting ext4, an armhf chroot, resizing) runs
# in a privileged Docker container, because macOS can do none of it. Apple
# Silicon runs the armhf chroot under qemu-user-static.
#
# Inputs, under $DIST (default ./dist):
#   base.img.xz          the rcn-ee base-v6.12 armhf image
#   <name>_*_armhf.deb   the emulator package for this board
# and from the repository:
#   02_bbb_config/01_cape/am335x-boneblack-bone.dts
#   packaging/debian/network/*
#
# Output: $OUT (default ./<name>-dist.img), ready to write to a card.
#
# usage:  DIST=./dist ./packaging/build-image.sh           # qbone image
#         NAME=unibone DIST=./dist ./packaging/build-image.sh   # unibone image

set -euo pipefail

# board: names the package installed, the hostname and the image
NAME=${NAME:-qbone}

# Optional apt repository the image points at, so a flashed board can
# apt update / apt upgrade. Empty by default - the public sources carry no
# repository URL; the caller (a workflow) supplies these from private config.
APT_REPO_URL=${APT_REPO_URL:-}
APT_REPO_KEY_URL=${APT_REPO_KEY_URL:-}

HERE=$(cd "$(dirname "$0")/.." && pwd)
DIST=${DIST:-$HERE/dist}
OUT=${OUT:-$HERE/${NAME}-dist.img}
# room to add beyond the base while building; the image is shrunk to fit after
GROW=${GROW:-4G}
# margin left in the root filesystem after the shrink
MARGIN_MB=${MARGIN_MB:-400}

ls "$DIST"/base.img.xz >/dev/null 2>&1 || { echo "missing $DIST/base.img.xz" >&2; exit 1; }
ls "$DIST"/${NAME}_*_armhf.deb >/dev/null 2>&1 || { echo "missing $DIST/${NAME}_*_armhf.deb" >&2; exit 1; }
[ "$(ls "$DIST"/${NAME}_*_armhf.deb | wc -l)" -eq 1 ] || { echo "expected exactly one $NAME deb in $DIST" >&2; exit 1; }
DTS=$HERE/02_bbb_config/01_cape/am335x-boneblack-bone.dts
NET=$HERE/packaging/debian/network
[ -r "$DTS" ] || { echo "missing $DTS" >&2; exit 1; }

echo "== registering the armhf binfmt handler in the Docker VM =="
docker run --privileged --rm tonistiigi/binfmt --install arm >/dev/null

echo "== building the $NAME image in a privileged container (this takes a while) =="
docker run --rm -i --privileged \
    -e GROW="$GROW" -e MARGIN_MB="$MARGIN_MB" -e NAME="$NAME" \
    -e APT_REPO_URL="$APT_REPO_URL" -e APT_REPO_KEY_URL="$APT_REPO_KEY_URL" \
    -v "$DIST":/dist \
    -v "$DTS":/in/board.dts:ro \
    -v "$NET":/in/network:ro \
    debian:trixie bash -euo pipefail -s <<'CONTAINER'
export DEBIAN_FRONTEND=noninteractive
apt-get -qq update >/dev/null
apt-get -qq install -y xz-utils fdisk util-linux e2fsprogs dosfstools \
    qemu-user-static systemd curl ca-certificates >/dev/null

echo "-- decompressing the base image"
xz -dc /dist/base.img.xz > /work.img

echo "-- growing it by $GROW to make room while building"
truncate -s +"$GROW" /work.img
# extend the third partition (root) to the new end of the disk
echo ", +" | sfdisk -q -N 3 /work.img >/dev/null

LO=$(losetup -Pf --show /work.img)
trap 'umount -R /mnt 2>/dev/null || true; losetup -d "$LO" 2>/dev/null || true' EXIT
# the container has no udev, so the kernel's partition block devices have no
# /dev nodes; create them from what the kernel published under /sys
LB=$(basename "$LO")
partx -a "$LO" 2>/dev/null || true
for sp in /sys/block/"$LB"/"$LB"p*; do
    [ -e "$sp/dev" ] || continue
    name=$(basename "$sp"); IFS=: read -r maj min < "$sp/dev"
    [ -e "/dev/$name" ] || mknod "/dev/$name" b "$maj" "$min"
done
e2fsck -pf "${LO}p3" >/dev/null 2>&1 || true
resize2fs "${LO}p3" >/dev/null

mkdir -p /mnt
mount "${LO}p3" /mnt
# /proc and /dev are enough for apt and the device-tree build; /sys is left out
# so the target's setup logic never sees the container's eth0
mount -t proc proc /mnt/proc
mount --bind /dev /mnt/dev
# resolv.conf is a symlink into /run on the base image; give the chroot a real
# file so apt can resolve, and remember the link to restore afterwards
RESOLV_LINK=$(readlink /mnt/etc/resolv.conf 2>/dev/null || true)
cp --remove-destination /etc/resolv.conf /mnt/etc/resolv.conf

# stage the inputs inside the rootfs
mkdir -p /mnt/tmp/in
cp /dist/${NAME}_*_armhf.deb /mnt/tmp/in/
cp /in/board.dts /mnt/tmp/in/board.dts

echo "-- customising the rootfs (armhf chroot)"
chroot /mnt /bin/bash -euo pipefail <<'CHROOT'
export DEBIAN_FRONTEND=noninteractive

# free port 80: the emulator's web interface binds it and will not start behind
# nginx, which fronts cockpit on the base image
apt-get -qq purge -y nginx nginx-common libnginx-mod-http-fancyindex \
    cockpit-ws cockpit-system cockpit-packagekit 2>/dev/null || true
rm -f /var/www/html/Cockpit.html

apt-get -qq update >/dev/null
# the operator toolset the appliance is run and debugged with
apt-get -qq install -y gdb tcpdump zsh tmux ckermit >/dev/null
# mDNS, so <name>.local resolves and the web interface announces itself over
# DNS-SD. Installed here rather than assumed of the base image: finding the
# board's address is otherwise the hardest part of a first boot. libnss-mdns
# also lets the board resolve other .local names.
apt-get -qq install -y avahi-daemon libnss-mdns >/dev/null
# the emulator package pulls in iproute2 and the device-tree build tools
apt-get -qq install -y /tmp/in/${NAME}_*_armhf.deb >/dev/null

# nothing in the image uses the GPIO daemon
apt-get -qq purge -y gpiod 2>/dev/null || true

# keep the kernel the cape overlay and pinmux are verified against; an
# unattended upgrade off it changes what the port depends on
KIMG=$(dpkg-query -W -f='${Package}\n' 'linux-image-*' 2>/dev/null | grep bone | head -1)
[ -n "$KIMG" ] && apt-mark hold "$KIMG" >/dev/null 2>&1 || true

# Passwordless root at the physical console. It is reachable over ssh neither
# with that empty password nor at all: the drop-in denies empty-password
# logins and root logins. sshd_config includes this directory before its own
# body and takes the first value for each keyword, so these win. Ordinary
# password logins (the base image's debian account) still work, for onboarding.
passwd -d root
install -d -m 755 /etc/ssh/sshd_config.d
cat > /etc/ssh/sshd_config.d/10-${NAME}.conf <<'EOF'
PermitRootLogin no
PermitEmptyPasswords no
EOF
chmod 644 /etc/ssh/sshd_config.d/10-${NAME}.conf

# The client's locale stays on the client. The image generates en_US.UTF-8 and
# nothing else, so a session forwarding LANG and LC_* from a desktop in another
# language asks for a locale the board cannot set, and perl and apt-listchanges
# say so on every apt run. AcceptEnv names accumulate across directives, so
# withdrawing the locale ones takes an edit of the line that grants them; the
# terminal-capability variables stay.
sed -i 's|^AcceptEnv .*|AcceptEnv COLORTERM NO_COLOR|' /etc/ssh/sshd_config

# Network file access to the disk/tape image tree: SMB (Samba), FTP (vsftpd)
# and SFTP (over the SSH the board already runs). The image tree is owned by
# the qunilator system user (created by the emulator package's postinst), and
# all three authenticate against the qunilator group: the service account is in
# it, and so is the operator account the web interface creates when a user name
# is set. The web interface gives both accounts the web password. FTP is
# cleartext, offered for legacy clients on a trusted LAN; SFTP is the encrypted
# path.
#
# Recommends are off for these two: samba recommends samba-ad-dc, which brings
# winbind and libnss-winbind, and libnss-winbind's postinst writes winbind into
# the passwd: and group: lines of /etc/nsswitch.conf. The board joins no domain,
# so every first NSS lookup of a session then waits out the winbind timeout -
# about eight seconds before the first command of a login answers. The
# standalone file server this image runs needs nothing from the recommends.
DEBIAN_FRONTEND=noninteractive apt-get -qq install -y --no-install-recommends \
    samba vsftpd >/dev/null
cat > /etc/samba/smb.conf <<'EOF'
[global]
   workgroup = WORKGROUP
   server string = QUniLator media
   security = user
   map to guest = never
   server min protocol = SMB2
   disable netbios = yes
   dns proxy = no
   load printers = no
   printing = bsd
   printcap name = /dev/null
   disable spoolss = yes
   log level = 1

[images]
   comment = QUniLator disk and tape images
   path = /var/lib/qunilator/images
   valid users = @qunilator
   force user = qunilator
   force group = qunilator
   read only = no
   create mask = 0644
   directory mask = 0755
EOF
chmod 644 /etc/samba/smb.conf
cat > /etc/vsftpd.conf <<'EOF'
listen=YES
listen_ipv6=NO
anonymous_enable=NO
local_enable=YES
write_enable=YES
# the image tree is shared by the qunilator group, so an upload stays writable
# by every account that serves it
local_umask=002
use_localtime=YES
xferlog_enable=YES
pam_service_name=vsftpd
chroot_local_user=YES
allow_writeable_chroot=YES
local_root=/var/lib/qunilator/images
userlist_enable=YES
userlist_file=/etc/vsftpd.userlist
userlist_deny=NO
pasv_min_port=40000
pasv_max_port=40100
seccomp_sandbox=NO
EOF
chmod 644 /etc/vsftpd.conf
echo qunilator > /etc/vsftpd.userlist
chmod 644 /etc/vsftpd.userlist
# vsftpd's PAM stack checks the login shell against /etc/shells; the accounts
# that serve the shares have a nologin shell (no interactive login), so allow it
# there for FTP.
grep -qx /usr/sbin/nologin /etc/shells || echo /usr/sbin/nologin >> /etc/shells
# Confine the SSH login of an account in the qunilator group to an SFTP session
# in the image tree, unless it is also in qunilator-admin - the group that
# carries shell access and sudo, and that the web interface puts the operator's
# account in. sshd requires the chroot root to be root-owned and unwritable,
# which /var/lib/qunilator is; the writable images/ subdir inside it is where
# uploads land, so the session opens there.
cat >> /etc/ssh/sshd_config.d/10-${NAME}.conf <<'EOF'
Match Group qunilator,!qunilator-admin
    ChrootDirectory /var/lib/qunilator
    ForceCommand internal-sftp -d /images -u 0002
    AllowTcpForwarding no
    X11Forwarding no
    PermitTunnel no
EOF
systemctl enable smbd vsftpd >/dev/null 2>&1 || true

# boot settings the cape needs, the same ones <name>-setup writes. QBone.dtbo
# is the bus-agnostic cape overlay, shared by both boards.
U=/boot/uEnv.txt
set_uenv() {
    if grep -qE "^#?$1=" "$U"; then sed -i "s|^#\?$1=.*|$1=$2|" "$U"
    else echo "$1=$2" >> "$U"; fi
}
sed -i "s|^uboot_overlay_pru=|#uboot_overlay_pru=|" "$U" 2>/dev/null || true
for k in emmc video audio; do set_uenv "disable_uboot_overlay_$k" 1; done
set_uenv enable_uboot_overlays 1
grep -q "^uboot_overlay_addr4=QBone.dtbo$" "$U" || set_uenv uboot_overlay_addr4 QBone.dtbo

# eth0 as a plain NIC: build the legacy-Ethernet device tree from the on-image
# sources and install it as the DTB U-Boot loads. The kernel version comes from
# the rootfs, not uname (which under qemu reports the build host's kernel).
KVER=$(basename "$(ls -d /boot/dtbs/*/ | head -1)")
KMM=$(echo "$KVER" | cut -d. -f1-2)
SRC=/opt/source/dtb-${KMM}.x
BASE_DTB=am335x-boneblack-uboot.dtb
cp /tmp/in/board.dts "$SRC/src/arm/ti/omap/am335x-boneblack-bone.dts"
( cd "$SRC" && make ARCH=arm CPP=cpp DTC=dtc \
    src/arm/ti/omap/am335x-boneblack-bone.dtb >/dev/null 2>&1 )
DTB="$SRC/src/arm/ti/omap/am335x-boneblack-bone.dtb"
[ -e "$DTB" ] || { echo "device tree build failed" >&2; exit 1; }
cp "/boot/dtbs/$KVER/$BASE_DTB" "/boot/dtbs/$KVER/$BASE_DTB.stock"
cp "$DTB" "/boot/dtbs/$KVER/$BASE_DTB"

# The address above the login prompt. agetty expands these when it writes
# /etc/issue to the console, so the serial console shows where the board can be
# reached without anyone logging in. \n is the hostname, \4{br0} the bridge's
# IPv4 address - blank until DHCP answers.
cat > /etc/issue <<'ISSUE'
Debian GNU/Linux \r on \m

\n - web interface: http://\4{br0}/
      over USB:     http://\4{usb0}/

ISSUE
chmod 644 /etc/issue

rm -rf /tmp/in
CHROOT

echo "-- enabling the services (offline, from the host)"
# qunilator-setup.service runs the setup --auto on first boot so the image
# configures its network bridge with no login; the image enables it, a package
# install leaves it disabled
# <name>-announce prints the board's address on the console once it has one
systemctl --root=/mnt enable qunilator-network.service ${NAME}.service qunilator-setup.service qunilator-leds.service qunilator-resize.service qunilator-announce.service >/dev/null 2>&1 || true
# The update check, so a flashed board knows where it stands. The package's
# postinst enables it too, but its systemd calls are guarded on a running systemd
# and the install above happens in a chroot, so this is what enables it here.
systemctl --root=/mnt enable qunilator-update-check.timer >/dev/null 2>&1 || true
# mDNS: <name>.local, and the DNS-SD advertisement the package drops in
# /etc/avahi/services. The postinst enables it, this makes sure of it.
systemctl --root=/mnt enable avahi-daemon.service >/dev/null 2>&1 || true
# The USB gadget serial getty spins on a tty that is not reliably present and
# wedges the console; the appliance is reached over the physical UART, the web
# interface and ssh. Only the getty is masked - the gadget's network interface
# stays, and usb0.network addresses it. Mask the GPIO daemon's unit too -
# nothing here uses it.
systemctl --root=/mnt mask serial-getty@ttyGS0.service gpio-manager.service >/dev/null 2>&1 || true
# a persistent journal survives reboots
mkdir -p /mnt/var/log/journal

# point the image at the apt repository so the board can upgrade from it (the
# key and source are fetched from the container, which has curl; the armhf
# chroot need not run curl under qemu)
if [ -n "$APT_REPO_URL" ] && [ -n "$APT_REPO_KEY_URL" ]; then
    echo "-- adding the $NAME apt repository"
    install -d -m 0755 /mnt/usr/share/keyrings
    curl -fsSL -o "/mnt/usr/share/keyrings/${NAME}-archive-keyring.gpg" "$APT_REPO_KEY_URL"
    printf 'deb [signed-by=/usr/share/keyrings/%s-archive-keyring.gpg] %s trixie main\n' \
        "$NAME" "$APT_REPO_URL" > "/mnt/etc/apt/sources.list.d/${NAME}.list"
else
    # Said out loud: the source entry is what makes self-update work at all, and a
    # board flashed from an image without one reports "no update source" in the
    # web interface for the rest of its life.
    echo "-- WARNING: no APT_REPO_URL/APT_REPO_KEY_URL:"
    echo "--          this image's board will not be able to update itself"
fi

echo "-- resetting identity"
# restore the managed resolv.conf symlink the base image shipped
if [ -n "$RESOLV_LINK" ]; then
    rm -f /mnt/etc/resolv.conf
    ln -s "$RESOLV_LINK" /mnt/etc/resolv.conf
fi
# "uninitialized" (not an empty file) is what makes systemd mark the next boot
# as first boot, so sshd-keygen.service (ConditionFirstBoot=yes) generates the
# host keys before ssh starts. An empty file skips that, ssh then fails until
# something else makes keys.
echo uninitialized > /mnt/etc/machine-id
rm -f /mnt/etc/ssh/ssh_host_*
# The base image processes several first-boot actions through bbbio-set-sysconf
# and reboots for each: regenerating ssh host keys (ssh_regenerate) and growing
# the root filesystem (growpart/growpart_done). sshd-keygen.service and
# qunilator-resize.service now do both without those reboots - and the base's
# reboot lands while the emulator is mid-startup, wedged in a PRU syscall,
# hanging the reboot. Drop the flags so bbbio-set-sysconf has nothing to do.
rm -f /mnt/etc/bbb.io/ssh_regenerate \
      /mnt/etc/bbb.io/growpart /mnt/etc/bbb.io/growpart_done
echo "$NAME" > /mnt/etc/hostname
sed -i "s/\\bBeagleBone\\b/$NAME/g; s/127\\.0\\.1\\.1.*/127.0.1.1\\t$NAME/" /mnt/etc/hosts 2>/dev/null || true
rm -f /mnt/var/lib/qunilator/settings.json
find /mnt/var/log -type f -exec truncate -s0 {} + 2>/dev/null || true
rm -rf /mnt/var/lib/apt/lists/* /mnt/var/cache/apt/archives/*.deb
rm -f /mnt/root/.bash_history /mnt/home/*/.bash_history 2>/dev/null || true

sync
umount -R /mnt
trap - EXIT

echo "-- shrinking the root filesystem to fit"
e2fsck -pf "${LO}p3" >/dev/null 2>&1 || true
# minimum, then add the margin
MINBLK=$(resize2fs -P "${LO}p3" 2>/dev/null | awk '{print $NF}')
BLKSZ=$(dumpe2fs -h "${LO}p3" 2>/dev/null | awk -F: '/Block size/{print $2}' | tr -d ' ')
TARGET=$(( MINBLK + MARGIN_MB * 1024 * 1024 / BLKSZ ))
resize2fs "${LO}p3" "$TARGET" >/dev/null
e2fsck -pf "${LO}p3" >/dev/null 2>&1 || true

# resize the partition to the filesystem and truncate the image to the
# partition end, so the file is only as large as its content
FS_BYTES=$(( TARGET * BLKSZ ))
P3_START=$(partx -g -o START -n 3:3 "$LO" | tr -d ' ')
losetup -d "$LO"
NEW_SECTORS=$(( (FS_BYTES + 511) / 512 ))
echo ", ${NEW_SECTORS}" | sfdisk -q -N 3 /work.img >/dev/null
END_SECTOR=$(( P3_START + NEW_SECTORS ))
truncate -s $(( END_SECTOR * 512 )) /work.img

cp /work.img /dist/${NAME}-dist.img
echo "-- done: $(du -h /dist/${NAME}-dist.img | cut -f1)"
CONTAINER

mv "$DIST/${NAME}-dist.img" "$OUT"
echo "== image ready: $OUT ($(du -h "$OUT" | cut -f1)) =="
