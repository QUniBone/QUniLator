#!/usr/bin/env python3
"""Merge a QUniLator disk image and its copy-on-write overlay into one image.

A drive with the overlay switched on leaves its base image untouched and keeps
every block the guest writes in two files beside it:

    <image>.ovl       the written blocks, each at its own offset
    <image>.ovl.map   which blocks those are

This writes base-plus-overlay to a new file, the same result as "Consolidate ->
new file" in the web interface, and leaves all three inputs as they are. It
runs anywhere Python 3 does - on the board, or on any machine the three files
have been copied to over SFTP, SMB or FTP.

    flatten-overlay.py <image> <output>

The map is written when the drive closes the image: the drive switched off, or
the service stopped. Switch the drive off before copying the files, and copy
all three.
"""

import os
import struct
import sys

BLOCK = 512
MAP_MAGIC = b"QCOWMAP1"


def blocks_from_map(map_path, base_size):
    """(block_count, image_size, written block numbers) from a .ovl.map file."""
    with open(map_path, "rb") as f:
        raw = f.read()
    if raw[:8] != MAP_MAGIC or len(raw) < 24:
        raise ValueError(f"{map_path} is not an overlay map")
    block_count, image_size = struct.unpack("<QQ", raw[8:24])
    if image_size < base_size:
        raise ValueError(f"{map_path} describes a smaller disk than the image "
                         "beside it - the image was changed under the overlay")
    bitmap = raw[24:24 + (block_count + 7) // 8]
    written = [b for b in range(block_count) if bitmap[b >> 3] >> (b & 7) & 1]
    return image_size, written


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    image, output = sys.argv[1], sys.argv[2]
    ovl, ovl_map = image + ".ovl", image + ".ovl.map"
    if os.path.abspath(output) in (os.path.abspath(p) for p in (image, ovl, ovl_map)):
        sys.exit("the output must be a new file")
    if not os.path.exists(ovl):
        sys.exit(f"{ovl} not found - this image has no overlay")

    if not os.path.exists(ovl_map):
        sys.exit(f"{ovl_map} not found - switch the drive off so it writes "
                 "the map, then copy it along with the image and the overlay")

    try:
        image_size, written = blocks_from_map(ovl_map, os.path.getsize(image))
    except ValueError as e:
        sys.exit(str(e))

    with open(image, "rb") as src, open(ovl, "rb") as over, open(output, "wb") as out:
        while True:
            chunk = src.read(1 << 20)
            if not chunk:
                break
            out.write(chunk)
        for b in written:
            over.seek(b * BLOCK)
            data = over.read(BLOCK)
            out.seek(b * BLOCK)
            out.write(data + bytes(BLOCK - len(data)))
        out.truncate(image_size)

    print(f"{output}: {image_size} bytes, {len(written)} blocks from the overlay")


if __name__ == "__main__":
    main()
