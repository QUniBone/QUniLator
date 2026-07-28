"""Geometry of an RX01 floppy image as written by FluxEngine.

FluxEngine stores sectors in physical order.  DEC operating systems address the
RX01 through a driver that spreads consecutive logical sectors across the track
with a 2:1 interleave and shifts the start of each track by six sectors, so a
512-byte logical block has to be gathered from four scattered physical sectors.
Track 0 carries the bootstrap loader and holds no logical data, which puts the
first logical block on track 1 and gives the volume 494 blocks.
"""

SECTOR_SIZE = 128
SECTORS_PER_TRACK = 26
TRACKS = 77
IMAGE_SIZE = SECTOR_SIZE * SECTORS_PER_TRACK * TRACKS
BLOCK_SIZE = 512
SECTORS_PER_BLOCK = BLOCK_SIZE // SECTOR_SIZE
BLOCKS = (TRACKS - 1) * SECTORS_PER_TRACK // SECTORS_PER_BLOCK


def sector_offset(i):
    """Byte offset of logical sector i within the physical image."""
    track, s = divmod(i, SECTORS_PER_TRACK)
    interleaved = 2 * s if s < 13 else 2 * s - 25
    skewed = (interleaved + 6 * track) % SECTORS_PER_TRACK
    return ((track + 1) * SECTORS_PER_TRACK + skewed) * SECTOR_SIZE


class Volume:
    """Logical block access to one image."""

    def __init__(self, path):
        with open(path, "rb") as f:
            self.image = f.read()
        self.path = path

    def block(self, n, count=1):
        if n < 0 or n + count > BLOCKS:
            raise ValueError(f"block {n}+{count} outside volume")
        out = bytearray()
        for sector in range(n * SECTORS_PER_BLOCK, (n + count) * SECTORS_PER_BLOCK):
            off = sector_offset(sector)
            out += self.image[off:off + SECTOR_SIZE]
        return bytes(out)
