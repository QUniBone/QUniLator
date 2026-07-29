#!/usr/bin/env python3
"""List the files on an XXDP disk image.

XXDP keeps its directory in a chain of User File Directory blocks, found from
the Master File Directory in block 1: word 1 names the first UFD block, and
each UFD block links to the next in its word 0 and holds 28 entries of nine
words after it. An entry is the file name in three RADIX-50 words, a DOS/BATCH
date, and the extent - which is all a listing needs.

A diagnostic also says what it is in its own text, so --json carries the title
it prints when it starts: the file's blocks are read through the same links and
the first long printable run naming a MAINDEC part or a device is kept. That is
what tells CZUDH (a UDA50 subsystem test) from CZUDA (a UDC11 exerciser), which
their file names do not.

    ./tools/xxdp-dir.py xxdp25.rl02             names, one per line
    ./tools/xxdp-dir.py --json xxdp25.rl02      name, date, block count, title

The layout follows 10.02_devices/2_src/sharedfilesystem/filesystem_xxdp.cpp,
which reads and writes the same file system for the shared-directory feature.
"""
import argparse
import datetime
import json
import re
import struct
import sys

BLOCK_SIZE = 512
UFD_ENTRY_WORDS = 9
UFD_ENTRIES_PER_BLOCK = 28
MFD_BLOCK = 1

# A file's data blocks are linked the same way the directory's are: word 0 of
# each block names the next, and the remaining 510 bytes are the file.
BLOCK_LINK_BYTES = 2
MAX_FILE_BYTES = 1 << 20

RADIX50 = " ABCDEFGHIJKLMNOPQRSTUVWXYZ$.?0123456789"

PRINTABLE_RUN = re.compile(rb"[ -~]{16,}")

# A diagnostic names itself by the program identifier it prints, CZxxx or the
# MAINDEC part number, and the title follows it on the same line. That is the
# run worth keeping - error messages elsewhere in the file match everything
# else one might look for.
PROGRAM_NAME = re.compile(r"\b(?:CZ[A-Z]{2,5}[0-9]?|MAINDEC-[0-9A-Z-]+)")

# Failing that, a run naming the hardware is better than the first run of text.
TITLE_HINTS = ("DISK", "DRIVE", "TAPE", "CONTROLLER", "EXERCISER", "FORMATTER",
               "SUBSYS", "FUNCTIONAL", "RELIABILITY")


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


def read_directory(image, with_titles=False):
    def block(n):
        return image[n * BLOCK_SIZE:(n + 1) * BLOCK_SIZE]

    def word(b, i):
        return struct.unpack_from("<H", b, i * 2)[0]

    def file_bytes(start_block_nr):
        data, seen, n = bytearray(), set(), start_block_nr
        while n and n not in seen and len(data) < MAX_FILE_BYTES:
            seen.add(n)
            b = block(n)
            data += b[BLOCK_LINK_BYTES:]
            n = word(b, 0)
        return bytes(data)

    def title_of(start_block_nr):
        runs = [m.group().decode("ascii").strip()
                for m in PRINTABLE_RUN.finditer(file_bytes(start_block_nr))]
        for run in runs:
            named = PROGRAM_NAME.search(run)
            if named:
                # A CZxxx identifier opens the line, with stray bytes before it;
                # a MAINDEC part number closes one, with the title before it.
                if named.group().startswith("MAINDEC"):
                    return run
                return run[named.start():]
        for run in runs[:16]:
            if any(hint in run.upper() for hint in TITLE_HINTS):
                return run
        return runs[0] if runs else ""

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
            record = {
                "name": f"{name}.{extension}",
                "date": dos11_date(word(ufd, base + 3)),
                "blocks": word(ufd, base + 6),
            }
            if with_titles and extension in ("BIC", "BIN"):
                record["title"] = title_of(word(ufd, base + 5))
            files.append(record)
        block_nr = word(ufd, 0)
    return files


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("image", help="XXDP disk image")
    parser.add_argument("--json", action="store_true",
                        help="write name, date, block count and title as JSON")
    args = parser.parse_args()

    with open(args.image, "rb") as f:
        files = read_directory(f.read(), with_titles=args.json)

    if args.json:
        json.dump(files, sys.stdout, indent=1)
        sys.stdout.write("\n")
    else:
        for entry in files:
            print(entry["name"])


if __name__ == "__main__":
    main()
