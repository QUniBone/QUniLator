#!/usr/bin/env python3
"""List the files on an XXDP disk image.

XXDP keeps its directory in a chain of User File Directory blocks, found from
the Master File Directory in block 1: word 1 names the first UFD block, and
each UFD block links to the next in its word 0 and holds 28 entries of nine
words after it. An entry is the file name in three RADIX-50 words, a DOS/BATCH
date, and the extent - which is all a listing needs.

    ./tools/xxdp-dir.py xxdp25.rl02             names, one per line
    ./tools/xxdp-dir.py --json xxdp25.rl02      name, date and block count

The layout follows 10.02_devices/2_src/sharedfilesystem/filesystem_xxdp.cpp,
which reads and writes the same file system for the shared-directory feature.
"""
import argparse
import datetime
import json
import struct
import sys

BLOCK_SIZE = 512
UFD_ENTRY_WORDS = 9
UFD_ENTRIES_PER_BLOCK = 28
MFD_BLOCK = 1

RADIX50 = " ABCDEFGHIJKLMNOPQRSTUVWXYZ$.?0123456789"


def rad50(value):
    return (RADIX50[value // 1600 % 40] + RADIX50[value // 40 % 40]
            + RADIX50[value % 40])


def dos11_date(value):
    """A DOS/BATCH date: years since 1970 in thousands, day of year in the rest."""
    if value == 0:
        return ""
    year, day_of_year = 1970 + value // 1000, value % 1000
    try:
        date = datetime.date(year, 1, 1) + datetime.timedelta(days=day_of_year - 1)
    except ValueError:
        return ""
    return date.isoformat()


def read_directory(image):
    def block(n):
        return image[n * BLOCK_SIZE:(n + 1) * BLOCK_SIZE]

    def word(b, i):
        return struct.unpack_from("<H", b, i * 2)[0]

    block_nr = word(block(MFD_BLOCK), 1)
    files, visited = [], set()
    while block_nr and block_nr not in visited:
        visited.add(block_nr)
        ufd = block(block_nr)
        for entry in range(UFD_ENTRIES_PER_BLOCK):
            base = 1 + entry * UFD_ENTRY_WORDS
            if word(ufd, base) == 0:
                continue                        # unused entry
            name = (rad50(word(ufd, base)) + rad50(word(ufd, base + 1))).rstrip()
            extension = rad50(word(ufd, base + 2)).rstrip()
            files.append({
                "name": f"{name}.{extension}",
                "date": dos11_date(word(ufd, base + 3)),
                "blocks": word(ufd, base + 6),
            })
        block_nr = word(ufd, 0)
    return files


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("image", help="XXDP disk image")
    parser.add_argument("--json", action="store_true",
                        help="write name, date and block count as JSON")
    args = parser.parse_args()

    with open(args.image, "rb") as f:
        files = read_directory(f.read())

    if args.json:
        json.dump(files, sys.stdout, indent=1)
        sys.stdout.write("\n")
    else:
        for entry in files:
            print(entry["name"])


if __name__ == "__main__":
    main()
