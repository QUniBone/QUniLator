#!/usr/bin/env python3
"""Interleave a logical RX01/RX02 image into the physical one a drive holds.

The RX01 and RX02 are addressed by track and sector and hold the medium exactly
as it was written: 77 tracks of 26 sectors, track 0 reserved. The handler
addresses a volume's blocks through a 2:1 sector interleave with 6 sectors of
skew per track, so a block is two (RX02) or four (RX01) sectors scattered across
a track rather than a run of consecutive ones.

Archived images come both ways. An image in logical block order - block 0 at
offset 0, which is how the RT-11 V5.1 distribution set is kept - has to be
interleaved before a drive can serve it: read as physical sectors it presents
the bootstrap a boot block assembled out of two unrelated halves, and the
MXV11-B2 bootstrap reads it, rejects it and moves on to the next device.

An image is logical when block 1 (offset 0o1000 for an RX02, 0o1000 for an RX01
alike) holds the RT-11 home block: the volume name sits at offset 0o730 of it.

    rx-logical-to-physical.py logical.img ... outdir/

The sector size follows from the file size - 128 bytes for an RX01, 256 for an
RX02 - and the result is a full 77-track image with track 0 blank.
"""
import os
import sys

SECTORS_PER_TRACK = 26
TRACKS = 77
RX01_IMAGE_BYTES = 256256


def physical_sector(logical):
    """Track and sector, both numbered from 1, holding a logical sector."""
    track = logical // SECTORS_PER_TRACK + 1
    index = logical % SECTORS_PER_TRACK
    sector = (2 * index) % SECTORS_PER_TRACK + (1 if index >= 13 else 0)
    sector = (sector + 6 * (track - 1)) % SECTORS_PER_TRACK
    return track, sector + 1


def convert(src, dst):
    data = open(src, 'rb').read()
    sector_size = 128 if len(data) <= RX01_IMAGE_BYTES else 256
    out = bytearray(TRACKS * SECTORS_PER_TRACK * sector_size)
    placed = 0
    for logical in range((TRACKS - 1) * SECTORS_PER_TRACK):
        off = logical * sector_size
        if off + sector_size > len(data):
            break
        track, sector = physical_sector(logical)
        at = (track * SECTORS_PER_TRACK + sector - 1) * sector_size
        out[at:at + sector_size] = data[off:off + sector_size]
        placed += 1
    open(dst, 'wb').write(out)
    return sector_size, placed, len(out)


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__.strip().splitlines()[-3].strip())
    outdir = argv[-1]
    os.makedirs(outdir, exist_ok=True)
    for src in argv[1:-1]:
        dst = os.path.join(outdir, os.path.basename(src))
        sector_size, placed, size = convert(src, dst)
        print("%-20s %d byte sectors, %d placed -> %s (%d bytes)"
              % (os.path.basename(src), sector_size, placed, dst, size))


if __name__ == '__main__':
    main(sys.argv)
