#!/usr/bin/env python3
"""Scan every floppy image and write catalog.json.

Each image is identified as Files-11 ODS-2, RT-11, or foreign, and its
directory is read out.  Hashes let identical volumes be collapsed later.
"""

import glob
import hashlib
import json
import os
import re
import struct

import ods2
import rt11
import rx01

DISK_NAME = re.compile(r"^disk-(\d+)(?:-(.*))?\.img$")


def digest(data):
    return hashlib.sha256(data).hexdigest()


def logical_image(rx):
    return rx.block(0, rx01.BLOCKS)


def describe(hdr, directory, name, version):
    return {
        "directory": directory,
        "name": name,
        "version": version,
        "fid": hdr.fid[0],
        "segment": hdr.seg_num,
        "size_bytes": hdr.size_bytes,
        "blocks_on_volume": hdr.blocks_here,
        "blocks_total": hdr.blocks_used,
        "continued": hdr.continued,
        "record_type": hdr.record_type,
        "record_size": hdr.record_size,
        "created": hdr.created,
        "revised": hdr.revised,
        "revision": hdr.revision,
        "owner_uic": f"[{hdr.owner[1]:o},{hdr.owner[0]:o}]",
        "protection": hdr.protection,
    }


def scan_ods2(rx):
    vol = ods2.Volume(rx)
    files = []
    listed_fids = set()
    listed_names = set()
    seen = set()

    def collect(walker):
        for directory, name, version, hdr in walker:
            files.append(describe(hdr, directory, name, version))
            listed_fids.add(hdr.fid[0])
            listed_names.add(hdr.name)

    collect(vol.walk(seen=seen))

    # When the master file directory is gone, subdirectories whose own headers
    # survive still describe part of the tree.
    for fid in sorted(vol.header_lbns):
        if fid in listed_fids:
            continue
        hdr = vol.header(fid)
        if hdr is None or not hdr.name.endswith(".DIR;1"):
            continue
        collect(vol.walk(fid, (hdr.name.split(".")[0],), seen))

    # A damaged directory block hides files whose headers are still intact, so
    # take a second pass straight down the index file.  Headers that merely
    # continue an already-listed file are extension segments, not new files.
    recovered = 0
    for fid in sorted(vol.header_lbns):
        if fid in listed_fids:
            continue
        hdr = vol.header(fid)
        if hdr is None or hdr.name in listed_names:
            continue
        _, _, version = hdr.name.partition(";")
        entry = describe(hdr, None, hdr.name,
                         int(version) if version.isdigit() else 0)
        entry["recovered_from_index"] = True
        files.append(entry)
        listed_names.add(hdr.name)
        recovered += 1

    files.sort(key=lambda f: (f["directory"] or "", f["name"]))
    return {
        "filesystem": "ODS-2",
        "home": vol.home,
        "files": files,
        "listed_in_directory": len(files) - recovered,
        "recovered_from_index": recovered,
    }


def scan_rt11(rx):
    vol = rt11.Volume(rx)
    files = sorted(vol.files, key=lambda f: f["name"])
    return {
        "filesystem": "RT-11",
        "home": vol.home,
        "files": files,
        "free_blocks": vol.free_blocks,
        "directory_segments": vol.segments,
    }


def scan_foreign(rx):
    boot = rx.image[:512]
    info = {"filesystem": "foreign"}
    if boot[3:11] == b"MSDOS5.0" or boot[54:57] == b"FAT":
        info["filesystem"] = "FAT12"
        info["oem"] = boot[3:11].decode("ascii", "replace")
        info["bytes_per_sector"] = struct.unpack("<H", boot[11:13])[0]
        info["total_sectors"] = struct.unpack("<H", boot[19:21])[0]
        info["volume_label"] = boot[43:54].decode("ascii", "replace").strip()
    return info


def scan(path):
    rx = rx01.Volume(path)
    logical = logical_image(rx)
    match = DISK_NAME.match(os.path.basename(path))
    record = {
        "file": os.path.basename(path),
        "disk_number": int(match.group(1)) if match else None,
        "filename_label": match.group(2) if match and match.group(2) else None,
        "image_size": len(rx.image),
        "image_sha256": digest(rx.image),
        "logical_sha256": digest(logical),
        "blank_sectors": sum(
            1 for i in range(0, len(rx.image), rx01.SECTOR_SIZE)
            if not any(rx.image[i:i + rx01.SECTOR_SIZE])),
    }
    for reader in (scan_ods2, scan_rt11):
        try:
            record.update(reader(rx))
            return record
        except (ods2.NotODS2, rt11.NotRT11, ValueError, struct.error, IndexError):
            continue
    record.update(scan_foreign(rx))
    return record


def main():
    paths = sorted(glob.glob("*.img"))
    disks = []
    for path in paths:
        record = scan(path)
        disks.append(record)
        label = record.get("home", {}).get("volume_name") or record["filesystem"]
        print(f"{record['file']:26} {record['filesystem']:8} "
              f"{label:14} {len(record.get('files', [])):4} files")
    disks.sort(key=lambda d: (d["disk_number"] is None, d["disk_number"] or 0,
                              d["file"]))
    with open("catalog.json", "w") as f:
        json.dump({"disks": disks}, f, indent=1, sort_keys=False)
    print(f"\n{len(disks)} images -> catalog.json")


if __name__ == "__main__":
    main()
