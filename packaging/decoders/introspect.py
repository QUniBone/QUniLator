#!/usr/bin/env python3
"""Introspect a single DEC disk/tape image and print its directory as JSON.

Reuses the dec-disketten decoders (rx01/rt11/ods2/scan). The block source is
chosen by image size: a 256,256-byte RX01 floppy image gets the physical-sector
de-interleave; every other image is read as linear 512-byte logical blocks
(RL/RK/MSCP disks). The reused readers cover RT-11 and Files-11 ODS-2; ODS-1
(RSX) and XXDP are not decoded yet and report as an unrecognized filesystem.

    introspect.py <image-file>  ->  JSON on stdout
"""
import json
import os
import struct
import sys

import rx01
import rt11
import ods2
import scan

BLOCK = 512


class LinearVolume:
    """Logical 512-byte block access to a plain disk image (no interleave)."""

    def __init__(self, path):
        with open(path, "rb") as f:
            self.image = f.read()
        self.path = path

    def block(self, n, count=1):
        off = n * BLOCK
        end = off + count * BLOCK
        if n < 0 or end > len(self.image):
            raise ValueError(f"block {n}+{count} outside image")
        return self.image[off:end]


def source_for(path):
    if os.path.getsize(path) == rx01.IMAGE_SIZE:
        return rx01.Volume(path)      # RX01 floppy: undo the sector interleave
    return LinearVolume(path)         # RL/RK/MSCP disk: linear logical blocks


def introspect(path):
    rx = source_for(path)
    record = {"file": os.path.basename(path), "image_size": len(rx.image)}
    for reader in (scan.scan_ods2, scan.scan_rt11):
        try:
            record.update(reader(rx))
            return record
        except (ods2.NotODS2, rt11.NotRT11, ValueError, struct.error,
                IndexError, KeyError):
            continue
    record.update(scan.scan_foreign(rx))
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
