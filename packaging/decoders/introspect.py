#!/usr/bin/env python3
"""Introspect a single DEC disk/tape image and print its directory as JSON.

Reuses the dec-disketten decoders (rx01/rt11/ods2/scan) and adds ODS-1. The
block source is chosen by image size: a 256,256-byte RX01 floppy image gets the
physical-sector de-interleave; every other image is read as linear 512-byte
logical blocks (RL/RK/MSCP disks). RT-11, Files-11 ODS-1 (RSX) and ODS-2 are
decoded; XXDP reports as an unrecognized filesystem.

A drive running a copy-on-write overlay keeps its writes in "<image>.ovl" beside
the image, so the listing reads every block the overlay holds from there: what
is shown is the volume as the guest sees it. Which blocks those are comes from
"<image>.ovl.map", which the drive writes when it closes the image; while a
drive holds the image open there is no map, and the overlay's allocated regions
say instead, which the drive keeps exact by writing whole filesystem pages.

    introspect.py <image-file>  ->  JSON on stdout
"""
import bisect
import json
import os
import struct
import sys

import rx01
import rt11
import ods1
import ods2
import scan

BLOCK = 512


MAP_MAGIC = b"QCOWMAP1"


def map_extents(map_path, base_size):
    """Written block ranges from an overlay map, or None when there is none.

    A map describing a smaller disk than the base was written for an image
    since replaced, so it is ignored, as the drive ignores it.
    """
    try:
        with open(map_path, "rb") as f:
            raw = f.read()
    except OSError:
        return None
    if raw[:8] != MAP_MAGIC or len(raw) < 24:
        return None
    block_count, image_size = struct.unpack("<QQ", raw[8:24])
    if image_size < base_size:
        return None
    bitmap = raw[24:24 + (block_count + 7) // 8]
    out = []
    for b in range(min(block_count, len(bitmap) * 8)):
        if bitmap[b >> 3] >> (b & 7) & 1:
            if out and out[-1][1] == b:
                out[-1] = (out[-1][0], b + 1)
            else:
                out.append((b, b + 1))
    return out


def overlay_extents(fd):
    """Written block ranges [(first, end)] of a sparse overlay file.

    The drive fills a whole filesystem page of the overlay on the first write
    into it and resets the overlay by truncating it to holes, so its allocated
    data regions hold exactly the volume's current content.
    """
    out = []
    end = os.fstat(fd).st_size
    pos = 0
    while pos < end:
        try:
            data = os.lseek(fd, pos, os.SEEK_DATA)
        except OSError:
            break  # no data past pos
        hole = min(os.lseek(fd, data, os.SEEK_HOLE), end)
        out.append((data // BLOCK, (hole + BLOCK - 1) // BLOCK))
        pos = hole
    return out


class Image:
    """Byte access to an image file, with its overlay's blocks read over it."""

    def __init__(self, path):
        self.fd = os.open(path, os.O_RDONLY)
        self.size = os.fstat(self.fd).st_size
        self.ovl = None
        self.extents = []
        try:
            self.ovl = os.open(path + ".ovl", os.O_RDONLY)
        except OSError:
            return
        extents = map_extents(path + ".ovl.map", self.size)
        self.extents = extents if extents is not None else overlay_extents(self.ovl)
        self.starts = [first for first, _end in self.extents]
        if self.extents:
            self.size = max(self.size, os.fstat(self.ovl).st_size)

    @property
    def dirty_blocks(self):
        return sum(end - first for first, end in self.extents)

    def _in_overlay(self, block):
        i = bisect.bisect_right(self.starts, block) - 1 if self.extents else -1
        return i >= 0 and block < self.extents[i][1]

    def read(self, offset, length):
        out = bytearray()
        while length > 0:
            block, within = divmod(offset, BLOCK)
            n = min(length, BLOCK - within)
            fd = self.ovl if self._in_overlay(block) else self.fd
            chunk = os.pread(fd, n, offset)
            out += chunk + bytes(n - len(chunk))  # past EOF reads as zeros
            offset += n
            length -= n
        return bytes(out)


class LinearVolume:
    """Logical 512-byte block access to a plain disk image (no interleave)."""

    def __init__(self, image):
        self.image = image

    def block(self, n, count=1):
        off = n * BLOCK
        if n < 0 or off + count * BLOCK > self.image.size:
            raise ValueError(f"block {n}+{count} outside image")
        return self.image.read(off, count * BLOCK)


class FloppyVolume(rx01.Volume):
    """An RX01 image through the de-interleave, overlay included."""

    def __init__(self, image):
        self.image = image.read(0, rx01.IMAGE_SIZE)


class BootSector:
    """What scan_foreign reads: the first physical sector of the image."""

    def __init__(self, image):
        self.image = image.read(0, BLOCK)


def scan_ods1(rx):
    vol = ods1.Volume(rx)
    files = []
    for directory, hdr in vol.walk():
        files.append({
            "directory": directory,
            "name": hdr.name,
            "version": hdr.version,
            "fid": hdr.fid[0],
            "size_bytes": hdr.size_bytes,
            "blocks_on_volume": vol.blocks_mapped(hdr),
            "blocks_total": hdr.blocks_used,
            "record_type": hdr.record_type,
            "record_size": hdr.record_size,
            "created": hdr.created,
            "revised": hdr.revised,
            "owner_uic": f"[{hdr.owner[1]:o},{hdr.owner[0]:o}]",
            "protection": hdr.protection,
        })
    files.sort(key=lambda f: (f["directory"], f["name"]))
    return {"filesystem": "ODS-1", "home": vol.home, "files": files}


def introspect(path):
    image = Image(path)
    if image.size == rx01.IMAGE_SIZE:
        rx = FloppyVolume(image)      # RX01 floppy: undo the sector interleave
    else:
        rx = LinearVolume(image)      # RL/RK/MSCP disk: linear logical blocks
    record = {"file": os.path.basename(path), "image_size": image.size}
    if image.dirty_blocks:
        record["overlay_dirty_blocks"] = image.dirty_blocks
    for reader in (scan_ods1, scan.scan_ods2, scan.scan_rt11):
        try:
            record.update(reader(rx))
            return record
        except (ods1.NotODS1, ods2.NotODS2, rt11.NotRT11, ValueError,
                struct.error, IndexError, KeyError):
            continue
    record.update(scan.scan_foreign(BootSector(image)))
    return record


def main():
    if len(sys.argv) != 2:
        print(json.dumps({"error": "usage: introspect.py <image>"}))
        sys.exit(2)
    try:
        print(json.dumps(introspect(sys.argv[1]), indent=1))
    except Exception as e:  # a bad image should report, not crash the endpoint
        print(json.dumps({"filesystem": "unknown", "error": str(e)}))
        sys.exit(1)


if __name__ == "__main__":
    main()
