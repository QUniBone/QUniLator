#!/bin/sh
# Build the VAX machine on the board: a VAX-11/780 processor, a UDA50 with the
# VMS system disk on the bus, and a DEUNA.
#
# The service applies a saved config at startup, and none of the saved ones is
# a VAX, so a restart leaves every device off. This puts the machine back.
#
#   ./tools/vax-setup.sh            processor, UDA50 and system disk
#   ./tools/vax-setup.sh --deuna    the same with the ethernet controller
#   ./tools/vax-setup.sh --scratch  also the second RA81, for volume tests
#   ./tools/vax-setup.sh --fresh    throw away what earlier runs wrote first
#
# --fresh discards the copy-on-write overlays, so the machine boots the pristine
# system disk. A run that leaves the guest's filesystem damaged then costs the
# next run nothing, and a test that passes says so about the image it was given
# rather than about what the run before it happened to leave behind.
#
# It leaves the processor enabled and stopped; ./tools/vax-boot.sh starts it.
set -e
HOST=${QBONE_HOST:-unibone.huebner.org}
PW=$(cat ~/.qbone-pw 2>/dev/null || true)
API="http://$HOST/api"

DEUNA=no
SCRATCH=no
FRESH=no
for arg in "$@"; do
    case "$arg" in
        --deuna) DEUNA=yes ;;
        --scratch) SCRATCH=yes ;;
        --fresh) FRESH=yes ;;
        *) echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done

set_param() {  # device param value
    curl -sf -m 20 -u ":$PW" -X PUT -H 'Content-Type: application/json' \
        -d "{\"value\":\"$3\"}" "$API/devices/$1/params/$2" >/dev/null \
        || { echo "failed: $1.$2 = $3" >&2; exit 1; }
}

# The processor is built last, because it refuses parameter changes while it is
# enabled and because the devices have to answer on the bus before it looks.
# The UDA50 answers at its default 772150 and takes its interrupt vector from
# the host, which writes it into SA during the MSCP init handshake.
set_param uda enabled 1

set_param uda0 image vms47.dsk
set_param uda0 enabled 1

if [ "$SCRATCH" = yes ]; then
    set_param uda1 image scratch_ra81.dsk
    set_param uda1 enabled 1
fi

if [ "$DEUNA" = yes ]; then
    # The controller sits on the veth end the board provides for it; the peer
    # end is enslaved into br0 with eth0, so the LAN and the board itself both
    # see the emulated machine. A tap directly on eth0 never hears the board's
    # own traffic. The interface is settable only while the device is off, and
    # it may still be on from the machine this one replaces.
    set_param deuna enabled 0
    set_param deuna interface veth-pdp
    set_param deuna enabled 1
fi

# Discarding an overlay wants the drive attached and the processor still
# stopped, which is where the build has got to.
if [ "$FRESH" = yes ]; then
    for img in vms47.dsk scratch_ra81.dsk; do
        curl -sf -m 60 -u ":$PW" -X POST "$API/images/$img/overlay/discard" \
            >/dev/null 2>&1 && echo "discarded the overlay on $img"
    done
fi

# The board shares 4 MB of its DDR with the bus, and that is the whole of the
# VAX's memory, so asking for more is refused.
set_param cpuvax memory ${VAX_MEMORY:-4}
set_param cpuvax bootdevice RQ0
set_param cpuvax enabled 1

echo "machine built (deuna=$DEUNA scratch=$SCRATCH fresh=$FRESH); run ./tools/vax-boot.sh"
