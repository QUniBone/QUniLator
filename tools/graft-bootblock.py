#!/usr/bin/env python3
"""Graft the KDJ11-bootable chain loader onto an RL02 pack.

The pack's own block 0 (a bootstrap without the DEC bootable-volume header,
which the KDJ11 ROM refuses) moves to the stash block v6boot.mac reads it
back from, and block 0 becomes the header plus the chain loader. The stash
must lie outside the pack's filesystem; for a V6 pack the superblock's fsize
says where that ends, and the default block 20000 clears the 19000-block
filesystem of the distributed V6 pack with room to spare.

    tools/graft-bootblock.py v6boot.lst unix_v6.rl02.dsk unix_v6_boot.rl02.dsk
"""
import re
import struct
import sys

STASH_BLOCK = 20000  # 512-byte blocks; must match `target` in v6boot.mac


def parse(lst):
    """Memory bytes from a macro11 listing (the mkbootimg.py parser)."""
    mem = {}
    for line in open(lst):
        m = re.match(r'\s*(?:\d{1,5}\s+)?([0-7]{6})\s+(.*)', line)
        if not m:
            continue
        a = int(m.group(1), 8)
        rest = m.group(2)
        for tok in re.finditer(r'([0-7]{6}|[0-7]{3})', rest):
            t = tok.group(1)
            lead = rest[:tok.start()].strip()
            if lead and not re.match(r'^[0-7\s]+$', lead):
                break
            if len(t) == 6:
                v = int(t, 8)
                if rest[tok.end():tok.end() + 1] == "'":
                    v = (v - (a + 2)) & 0xFFFF
                mem[a] = v & 0xFF
                mem[a + 1] = (v >> 8) & 0xFF
                a += 2
            else:
                mem[a] = int(t, 8) & 0xFF
                a += 1
    return mem


def main():
    lst, src, dst = sys.argv[1:4]
    mem = parse(lst)
    block0 = bytes(mem.get(a, 0) for a in range(512))
    img = bytearray(open(src, 'rb').read())

    isize, fsize = struct.unpack('<HH', img[512:516])
    if STASH_BLOCK * 512 + 512 > len(img):
        sys.exit('the stash block lies beyond the image')
    if fsize > STASH_BLOCK:
        sys.exit(f'the filesystem ({fsize} blocks) covers the stash block')

    img[STASH_BLOCK * 512:STASH_BLOCK * 512 + 512] = img[0:512]
    img[0:512] = block0
    open(dst, 'wb').write(img)
    print(f'{dst}: original boot block stashed at block {STASH_BLOCK}, '
          f'chain loader installed (fs {fsize} of {len(img)//512} blocks)')


if __name__ == '__main__':
    main()
