"""Files-11 ODS-1 volume reader, the on-disk structure of RSX-11 and IAS.

The home block at LBN 1 locates the index file bitmap; the index file's own
header follows the bitmap, and its retrieval pointers give the block of every
other file header. File 4 is the master file directory, whose entries are the
user file directories ([g,m]) that hold the files.

Layout per the Files-11 On-Disk Structure Specification, structure level 1
(H.VLEV/H.FLEV 0x0101 and 0x0102).
"""

import struct

from rt11 import rad50_name, unrad50

HOME_LBN = 1
FORMAT_LABEL = "DECFILE11A"
STRUCLEVS = (0x0101, 0x0102)

INDEXF_FID = 1
MFD_FID = 4

# Index file VBNs before the bitmap: the boot block and the home block.
INDEX_LEAD_VBNS = 2

RECORD_TYPES = {0: "undefined", 1: "fixed", 2: "variable", 3: "sequenced"}

MONTHS = {m: i + 1 for i, m in enumerate(
    ("JAN", "FEB", "MAR", "APR", "MAY", "JUN",
     "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"))}


class NotODS1(Exception):
    pass


def swapped_long(raw):
    """A PDP-11 double word as FCS stores it: high-order word first."""
    hi, lo = struct.unpack("<HH", raw)
    return (hi << 16) | lo


def text(raw):
    return raw.decode("ascii", "replace").rstrip(" \0")


def rsx_time(date, time=b""):
    """'DDMMMYY' plus 'HHMMSS' as ISO text, or None when the field is empty.

    The year is two digits; RSX writes years from 2000 on as values above 99
    by carrying into the tens digit, so a tens character past '9' is decoded
    by its offset from '0'.
    """
    if len(date) < 7 or not any(date):
        return None
    try:
        day = int(date[0:2].decode("ascii"))
        month = MONTHS[date[2:5].decode("ascii").upper()]
        year = 1900 + (date[5] - 0x30) * 10 + (date[6] - 0x30)
    except (ValueError, KeyError, UnicodeDecodeError):
        return None
    if not (1 <= day <= 31 and 1900 <= year <= 2099):
        return None
    out = f"{year:04d}-{month:02d}-{day:02d}"
    if len(time) == 6 and time.isdigit():
        t = time.decode("ascii")
        out += f" {t[0:2]}:{t[2:4]}:{t[4:6]}"
    return out


def uic_directory(words):
    """The [g,m] a UFD's RAD50 name encodes, e.g. '001054' -> '[1,54]'."""
    name = rad50_name(words)
    if len(name) == 6 and all(c in "01234567" for c in name):
        return f"[{int(name[:3], 8):o},{int(name[3:], 8):o}]"
    return f"[{name}]"


class Header:
    """One file header block."""

    def __init__(self, raw):
        if len(raw) != 512:
            raise NotODS1("short header block")
        words = struct.unpack("<256H", raw)
        if sum(words[:255]) & 0xFFFF != words[255]:
            raise NotODS1("header checksum")
        self.raw = raw
        ido, mpo = raw[0], raw[1]
        self.fid = struct.unpack("<HH", raw[2:6])        # number, sequence
        self.struclev = words[3]
        prog, group = raw[8], raw[9]
        self.owner = (prog, group)
        self.protection = words[5]
        self.user_char, self.sys_char = raw[12], raw[13]

        attr = raw[14:46]
        self.record_type = RECORD_TYPES.get(attr[0] & 0x0F, "?")
        self.record_size = struct.unpack("<H", attr[2:4])[0]
        self.blocks_used = swapped_long(attr[4:8])       # F.HIBK
        self.end_block = swapped_long(attr[8:12])        # F.EFBK
        self.first_free_byte = struct.unpack("<H", attr[12:14])[0]

        ident = raw[ido * 2:ido * 2 + 46]
        fnam = struct.unpack("<3H", ident[0:6])
        ftyp = struct.unpack("<H", ident[6:8])[0]
        self.version = struct.unpack("<h", ident[8:10])[0]
        self.revision = struct.unpack("<H", ident[10:12])[0]
        self.revised = rsx_time(ident[12:19], ident[19:25])
        self.created = rsx_time(ident[25:32], ident[32:38])
        self.expires = rsx_time(ident[38:45])
        self.base = rad50_name(fnam)
        self.type = unrad50(ftyp).rstrip()
        self.name = f"{self.base}.{self.type};{self.version:o}"

        m = mpo * 2
        self.seg_num = raw[m]
        self.ext_fid = struct.unpack("<HH", raw[m + 2:m + 6])
        self.count_size, self.lbn_size = raw[m + 6], raw[m + 7]
        self.map_inuse = raw[m + 8]
        self.map_words = raw[m + 10:m + 10 + self.map_inuse * 2]

    @property
    def size_bytes(self):
        if self.end_block == 0:
            return 0
        return (self.end_block - 1) * 512 + self.first_free_byte

    def extents(self):
        """(lbn, count) pairs from this header's retrieval pointers.

        The standard pointer is 4 bytes: high LBN byte, count-1 byte, low LBN
        word. Other field sizes are laid out as count then LBN, low-order first.
        """
        out = []
        w = self.map_words
        if (self.count_size, self.lbn_size) == (1, 3):
            for i in range(0, len(w) - 3, 4):
                lbn = (w[i] << 16) | struct.unpack("<H", w[i + 2:i + 4])[0]
                out.append((lbn, w[i + 1] + 1))
            return out
        step = self.count_size + self.lbn_size
        if step == 0:
            return out
        for i in range(0, len(w) - step + 1, step):
            count = int.from_bytes(w[i:i + self.count_size], "little")
            lbn = int.from_bytes(w[i + self.count_size:i + step], "little")
            out.append((lbn, count + 1))
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
        if struclev not in STRUCLEVS or not fmt.startswith(FORMAT_LABEL):
            raise NotODS1(f"struclev {struclev:#06x}, format {fmt!r}")
        owner = struct.unpack("<BB", raw[30:32])
        return {
            "index_bitmap_size": struct.unpack("<H", raw[0:2])[0],
            "index_bitmap_lbn": swapped_long(raw[2:6]),
            "max_files": struct.unpack("<H", raw[6:8])[0],
            "cluster": struct.unpack("<H", raw[8:10])[0],
            "struclev": struclev,
            "volume_name": text(raw[14:26]),
            "volume_owner": f"[{owner[1]:o},{owner[0]:o}]",
            "protection": struct.unpack("<H", raw[32:34])[0],
            "created": rsx_time(raw[60:67], raw[67:73]),
            "owner_name": text(raw[472:484]),
            "format": fmt,
        }

    def _map_index_file(self):
        """LBN of each file header, indexed by file number."""
        first = self.home["index_bitmap_lbn"] + self.home["index_bitmap_size"]
        # The first sixteen headers are contiguous after the bitmap whatever the
        # index file maps, which is what lets a volume be mounted at all, and
        # what locates the index file's own extension headers.
        self.header_lbns = {n: first + n - 1 for n in range(1, 17)}
        index = Header(self.rx.block(first))
        if index.fid[0] != INDEXF_FID:
            raise NotODS1(f"index file header holds FID {index.fid}")
        self.headers[INDEXF_FID] = index

        blocks = []
        for hdr in self._segments(index):
            for lbn, count in hdr.extents():
                blocks.extend(range(lbn, lbn + count))
        skip = INDEX_LEAD_VBNS + self.home["index_bitmap_size"]
        lbns = dict(self.header_lbns)
        lbns.update({n + 1: lbn for n, lbn in enumerate(blocks[skip:])})
        return lbns

    def _segments(self, hdr):
        """The header and the extension headers its map area chains to."""
        seen = set()
        while hdr is not None and hdr.fid[0] not in seen:
            seen.add(hdr.fid[0])
            yield hdr
            ext = hdr.ext_fid[0]
            if ext == 0:
                return
            lbn = self.header_lbns.get(ext)
            if lbn is None:
                return
            try:
                hdr = Header(self.rx.block(lbn))
            except (ValueError, NotODS1):
                return

    def header(self, fid_num):
        if fid_num in self.headers:
            return self.headers[fid_num]
        lbn = self.header_lbns.get(fid_num)
        if lbn is None:
            return None
        try:
            hdr = Header(self.rx.block(lbn))
        except (ValueError, NotODS1, struct.error):
            return None
        if hdr.fid[0] != fid_num or hdr.struclev not in STRUCLEVS:
            return None
        self.headers[fid_num] = hdr
        return hdr

    def blocks_mapped(self, hdr):
        return sum(c for seg in self._segments(hdr) for _lbn, c in seg.extents())

    def read_file(self, hdr, limit_blocks=None):
        data = bytearray()
        for seg in self._segments(hdr):
            for lbn, count in seg.extents():
                for b in range(lbn, lbn + count):
                    if limit_blocks is not None and len(data) >= limit_blocks * 512:
                        return bytes(data)
                    try:
                        data += self.rx.block(b)
                    except ValueError:
                        return bytes(data)
        return bytes(data)

    def directory_entries(self, hdr):
        """Yield (fid_num, name words, type word, version) up to end of file."""
        used = hdr.end_block if hdr.end_block else hdr.blocks_used
        data = self.read_file(hdr, used or None)
        if hdr.end_block:
            data = data[:hdr.size_bytes]
        for pos in range(0, len(data) - 15, 16):
            num, _seq, _rvn, n1, n2, n3, typ, ver = struct.unpack(
                "<6HHh", data[pos:pos + 16])
            if num == 0:
                continue
            yield num, (n1, n2, n3), typ, ver

    def walk(self):
        """Yield (directory, header) for the MFD and every UFD it lists."""
        mfd = self.header(MFD_FID)
        if mfd is None:
            raise NotODS1("no master file directory")
        seen = {MFD_FID}
        ufds = []
        for num, words, typ, _ver in self.directory_entries(mfd):
            child = self.header(num)
            if child is None:
                continue
            yield "[0,0]", child
            if unrad50(typ).rstrip() == "DIR" and num not in seen:
                seen.add(num)
                ufds.append((uic_directory(words), child))
        for directory, ufd in ufds:
            for num, _words, _typ, _ver in self.directory_entries(ufd):
                child = self.header(num)
                if child is not None:
                    yield directory, child
