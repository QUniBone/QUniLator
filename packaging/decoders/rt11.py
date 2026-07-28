"""RT-11 volume reader.

The home block sits at logical block 1 and the directory begins at block 6 as a
chain of two-block segments, each holding fixed-size entries that describe both
the files and the gaps between them.
"""

import struct

HOME_BLOCK = 1
FIRST_SEGMENT_BLOCK = 6
SEGMENT_BLOCKS = 2

RAD50 = " ABCDEFGHIJKLMNOPQRSTUVWXYZ$.?0123456789"

E_TENT = 0o000400   # tentative file, still open
E_MPTY = 0o001000   # unused area
E_PERM = 0o002000   # permanent file
E_EOS = 0o004000    # end of segment
E_PROT = 0o100000   # protected against deletion


class NotRT11(Exception):
    pass


def unrad50(word):
    return "".join(RAD50[(word // d) % 40] for d in (1600, 40, 1))


def rad50_name(words):
    return "".join(unrad50(w) for w in words).rstrip()


def decode_date(word):
    if word == 0:
        return None
    year = (word & 0o37) + 1972 + 32 * (word >> 14)
    day = (word >> 5) & 0o37
    month = (word >> 10) & 0o17
    if not (1 <= month <= 12 and 1 <= day <= 31):
        return None
    return f"{year:04d}-{month:02d}-{day:02d}"


def text(raw):
    """Printable form of a home-block string, or None when it holds no text.

    Diagnostic and console packs are often written without a home block, so the
    label fields come back as whatever the medium happened to hold.
    """
    if not all(32 <= b < 127 for b in raw):
        return None
    return raw.decode("ascii").rstrip() or None


class Volume:
    def __init__(self, rx):
        self.rx = rx
        self.home = self._read_home()
        self.files, self.free_blocks, self.segments = self._read_directory()

    def _read_home(self):
        raw = self.rx.block(HOME_BLOCK)
        checksum = sum(struct.unpack("<255H", raw[:510])) & 0xFFFF
        return {
            "pack_cluster_size": struct.unpack("<H", raw[0o722:0o724])[0],
            "first_directory_block": struct.unpack("<H", raw[0o724:0o726])[0],
            "system_version": rad50_name(struct.unpack("<H", raw[0o726:0o730])),
            "volume_name": text(raw[0o730:0o730 + 12]),
            "owner_name": text(raw[0o744:0o744 + 12]),
            "system_id": text(raw[0o760:0o760 + 12]),
            "checksum_ok": checksum == struct.unpack("<H", raw[510:512])[0],
        }

    def _read_directory(self):
        files = []
        free = 0
        segment = 1
        visited = set()
        total_segments = None

        while segment and segment not in visited:
            visited.add(segment)
            block = FIRST_SEGMENT_BLOCK + (segment - 1) * SEGMENT_BLOCKS
            try:
                raw = self.rx.block(block, SEGMENT_BLOCKS)
            except ValueError as exc:
                raise NotRT11(str(exc))
            total, next_segment, highest, extra, start = struct.unpack("<5H", raw[:10])

            if total_segments is None:
                if not 1 <= total <= 31 or extra % 2 or extra > 64 or highest > total:
                    raise NotRT11(f"segment header {total},{next_segment},"
                                  f"{highest},{extra}")
                total_segments = total

            entry_size = 14 + extra
            pos = 10
            data_block = start
            while pos + entry_size <= SEGMENT_BLOCKS * 512:
                status, n1, n2, ext, length, _job, date = struct.unpack(
                    "<7H", raw[pos:pos + 14])
                if status & E_EOS:
                    break
                if status & (E_PERM | E_TENT):
                    name = rad50_name((n1, n2))
                    suffix = rad50_name((ext,))
                    files.append({
                        "name": f"{name}.{suffix}" if suffix else name,
                        "blocks": length,
                        "bytes": length * 512,
                        "date": decode_date(date),
                        "start_block": data_block,
                        "protected": bool(status & E_PROT),
                        "tentative": bool(status & E_TENT),
                    })
                elif status & E_MPTY:
                    free += length
                data_block += length
                pos += entry_size

            segment = next_segment

        if total_segments is None:
            raise NotRT11("no directory segments")
        return files, free, total_segments

    def read_file(self, entry):
        return self.rx.block(entry["start_block"], entry["blocks"])
