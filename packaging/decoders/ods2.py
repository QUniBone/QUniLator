"""Files-11 ODS-2 volume reader (enough of it to list a floppy).

Structure follows the VAX/VMS on-disk format: a home block at a known LBN names
the index file, the index file's retrieval pointers give the location of every
file header, and file 4 is the master file directory from which the rest of the
tree hangs.
"""

import struct
from datetime import datetime, timedelta

HOME_LBN = 1
STRUCLEV_ODS2 = 0x0201
FORMAT_LABEL = "DECFILE11B"

INDEXF_FID = 1
MFD_FID = 4

# VMS absolute time is 100ns units since the Smithsonian base date
VMS_EPOCH = datetime(1858, 11, 17)

RECORD_TYPES = {0: "undefined", 1: "fixed", 2: "variable", 3: "VFC",
                4: "stream", 5: "stream-LF", 6: "stream-CR"}


class NotODS2(Exception):
    pass


def vms_time(raw):
    quad = struct.unpack("<Q", raw)[0]
    if quad == 0:
        return None
    try:
        return (VMS_EPOCH + timedelta(microseconds=quad // 10)).isoformat(" ")
    except OverflowError:
        return None


def swapped_long(raw):
    """A VAX 'swapped longword': high-order word stored first."""
    hi, lo = struct.unpack("<HH", raw)
    return (hi << 16) | lo


def text(raw):
    return raw.decode("ascii", "replace").rstrip(" \0")


class Header:
    """One file header block."""

    def __init__(self, raw):
        self.raw = raw
        ido, mpo, aco, rso = raw[0], raw[1], raw[2], raw[3]
        self.seg_num, self.struclev = struct.unpack("<HH", raw[4:8])
        self.fid = struct.unpack("<HHBB", raw[8:14])
        self.ext_fid = struct.unpack("<HHBB", raw[14:20])
        self.map_inuse = raw[58]

        attr = raw[20:52]
        self.record_type = RECORD_TYPES.get(attr[0] & 0x0F, "?")
        self.record_size = struct.unpack("<H", attr[2:4])[0]
        self.blocks_used = swapped_long(attr[4:8])       # FAT$L_HIBLK
        self.end_block = swapped_long(attr[8:12])        # FAT$L_EFBLK
        self.first_free_byte = struct.unpack("<H", attr[12:14])[0]

        self.owner = struct.unpack("<HH", raw[60:64])    # UIC member, group
        self.protection = struct.unpack("<H", raw[64:66])[0]

        ident = raw[ido * 2:ido * 2 + 120]
        self.name = text(ident[:20])
        self.revision = struct.unpack("<H", ident[20:22])[0]
        self.created = vms_time(ident[22:30])
        self.revised = vms_time(ident[30:38])
        self.expires = vms_time(ident[38:46])
        self.backed_up = vms_time(ident[46:54])

        self.map_offset = mpo
        self.access_offset = aco
        self.reserved_offset = rso

    @property
    def continued(self):
        """True when the file runs onto another volume of the set."""
        return self.seg_num > 0 or self.ext_fid[0] != 0

    @property
    def size_bytes(self):
        """End-of-file position in bytes, or None when the header leaves it open.

        A file still being written across a volume set carries EFBLK 0x7FFFFFFF
        until the last member is closed, so its true length is only known from
        the volume that ends it.
        """
        if self.end_block >= 0x7FFFFFFF:
            return None
        if self.end_block == 0:
            return 0
        return (self.end_block - 1) * 512 + self.first_free_byte

    @property
    def blocks_here(self):
        """Blocks this header maps on the volume being read."""
        return sum(count for _lbn, count in self.extents())

    def extents(self):
        """(lbn, count) pairs from this header's retrieval pointers."""
        words = self.raw[self.map_offset * 2:
                         self.map_offset * 2 + self.map_inuse * 2]
        out = []
        i = 0
        while i + 2 <= len(words):
            word0 = struct.unpack("<H", words[i:i + 2])[0]
            fmt = word0 >> 14
            if fmt == 0:
                i += 2
            elif fmt == 1:
                if i + 4 > len(words):
                    break
                count = (word0 & 0xFF) + 1
                lbn = ((word0 >> 8) & 0x3F) << 16
                lbn |= struct.unpack("<H", words[i + 2:i + 4])[0]
                out.append((lbn, count))
                i += 4
            elif fmt == 2:
                if i + 6 > len(words):
                    break
                count = (word0 & 0x3FFF) + 1
                lbn = struct.unpack("<I", words[i + 2:i + 6])[0]
                out.append((lbn, count))
                i += 6
            else:
                if i + 8 > len(words):
                    break
                count = ((word0 & 0x3FFF) << 16)
                count |= struct.unpack("<H", words[i + 2:i + 4])[0]
                lbn = struct.unpack("<I", words[i + 4:i + 8])[0]
                out.append((lbn, count + 1))
                i += 8
        return out


class Volume:
    def __init__(self, rx):
        self.rx = rx
        self.home = self._read_home()
        self.headers = {}
        self.header_lbns = self._map_index_file()

    def _read_home(self):
        raw = self.rx.block(HOME_LBN)
        struclev = struct.unpack("<H", raw[12:14])[0]
        fmt = text(raw[496:508])
        if struclev != STRUCLEV_ODS2 or not fmt.startswith(FORMAT_LABEL):
            raise NotODS2(f"struclev {struclev:#06x}, format {fmt!r}")
        home = {
            "home_lbn": struct.unpack("<I", raw[0:4])[0],
            "alt_home_lbn": struct.unpack("<I", raw[4:8])[0],
            "alt_index_lbn": struct.unpack("<I", raw[8:12])[0],
            "struclev": struclev,
            "cluster": struct.unpack("<H", raw[14:16])[0],
            "index_bitmap_vbn": struct.unpack("<H", raw[22:24])[0],
            "index_bitmap_lbn": struct.unpack("<I", raw[24:28])[0],
            "max_files": struct.unpack("<I", raw[28:32])[0],
            "index_bitmap_size": struct.unpack("<H", raw[32:34])[0],
            "reserved_files": struct.unpack("<H", raw[34:36])[0],
            "relative_volume": struct.unpack("<H", raw[38:40])[0],
            "volume_set_count": struct.unpack("<H", raw[40:42])[0],
            "volume_owner": struct.unpack("<HH", raw[44:48])[0:2],
            "protection": struct.unpack("<H", raw[52:54])[0],
            "created": vms_time(raw[60:68]),
            "volume_set_name": text(raw[460:472]),
            "volume_name": text(raw[472:484]),
            "owner_name": text(raw[484:496]),
            "format": fmt,
        }
        return home

    def _map_index_file(self):
        """LBN of each file header, indexed by file number."""
        first = self.home["index_bitmap_lbn"] + self.home["index_bitmap_size"]
        index = Header(self.rx.block(first))
        if index.fid[0] != INDEXF_FID:
            raise NotODS2(f"index file header holds FID {index.fid}")
        self.headers[INDEXF_FID] = index

        # Header blocks are the index file's VBNs past the bitmap.
        blocks = []
        for lbn, count in index.extents():
            blocks.extend(range(lbn, lbn + count))
        skip = self.home["index_bitmap_vbn"] - 1 + self.home["index_bitmap_size"]
        return {n + 1: lbn for n, lbn in enumerate(blocks[skip:])}

    def header(self, fid_num):
        if fid_num in self.headers:
            return self.headers[fid_num]
        lbn = self.header_lbns.get(fid_num)
        if lbn is None:
            return None
        try:
            hdr = Header(self.rx.block(lbn))
        except (ValueError, struct.error):
            return None
        if hdr.fid[0] != fid_num or hdr.struclev != STRUCLEV_ODS2:
            return None
        self.headers[fid_num] = hdr
        return hdr

    def read_file(self, hdr):
        data = bytearray()
        for lbn, count in hdr.extents():
            for b in range(lbn, lbn + count):
                try:
                    data += self.rx.block(b)
                except ValueError:
                    return bytes(data)
        return bytes(data)

    def directory_entries(self, hdr):
        """Yield (name, version, fid_num) for a directory file."""
        data = self.read_file(hdr)
        for base in range(0, len(data), 512):
            block = data[base:base + 512]
            pos = 0
            while pos + 6 <= len(block):
                size = struct.unpack("<H", block[pos:pos + 2])[0]
                if size == 0xFFFF:
                    break
                record = block[pos:pos + size + 2]
                pos += size + 2
                if len(record) < 6:
                    break
                namecount = record[5]
                name = text(record[6:6 + namecount])
                vpos = 6 + namecount + (namecount & 1)
                while vpos + 8 <= len(record):
                    version, num, seq, rvn_nmx = struct.unpack(
                        "<HHHH", record[vpos:vpos + 8])
                    yield name, version, num
                    vpos += 8

    def walk(self, fid=MFD_FID, components=("000000",), seen=None):
        """Yield (directory, filename, version, header) over the whole tree.

        `directory` is VMS notation: [000000], [SYS0.SYS$LDR], and so on.
        """
        seen = seen if seen is not None else set()
        if fid in seen:
            return
        seen.add(fid)
        hdr = self.header(fid)
        if hdr is None:
            return
        directory = "[" + ".".join(components) + "]"
        for name, version, num in self.directory_entries(hdr):
            child = self.header(num)
            if child is None:
                continue
            yield directory, f"{name};{version}", version, child
            if name.endswith(".DIR"):
                below = (name[:-4],)
                nested = below if components == ("000000",) else components + below
                yield from self.walk(num, nested, seen)
