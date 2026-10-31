/* storageimage_vhd.cpp: a Virtual Hard Disk (VHD) file as storage medium

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see storageimage.hpp for the full text.

   The layout follows Microsoft's "Virtual Hard Disk Image Format Specification"
   (version 1.0). Every field is big-endian.

   Footer, 512 bytes, at the end of every VHD and, for a dynamic one, copied at
   its start:
       0  cookie "conectix"        40  original size (u64)
       8  features (u32)           48  current size (u64) - the disk's size
      12  format version (u32)     56  cylinders (u16), heads, sectors (u8)
      16  data offset (u64)        60  disk type: 2 fixed, 3 dynamic, 4 differencing
      24  time stamp (u32)         64  checksum (u32)
      28  creator application      68  unique id (16 bytes)
      32  creator version (u32)    84  saved state (u8)
      36  creator host OS (u32)    85  reserved - SIMH keeps its drive type here

   Dynamic header, 1024 bytes, at the footer's data offset:
       0  cookie "cxsparse"        28  max table entries (u32)
       8  data offset (u64)        32  block size (u32)
      16  table offset (u64)       36  checksum (u32)
      24  header version (u32)     40  parent fields (differencing disks only)

   The block allocation table (BAT) holds one u32 per block: the sector at which
   the block starts in the file, or 0xFFFFFFFF for a block never written, which
   reads as zeros. A block is a sector bitmap - one bit per sector, most
   significant first, padded to whole sectors - followed by the block's data. A
   sector whose bit is clear reads as zeros.

   The footer and the dynamic header are kept as found: a write changes the
   disk's bytes, a BAT entry and a bitmap, and moves the closing footer when the
   file grows, so no checksum is recomputed and what another emulator keeps in
   their reserved areas stays.
*/

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>

#include "logger.hpp"
#include "medium_write_protect.hpp"
#include "utils.hpp"
#include "storageimage_vhd.hpp"

const unsigned storageimage_vhd_c::SECTOR ;
const uint32_t storageimage_vhd_c::BAT_UNUSED ;

static const unsigned SECTOR = storageimage_vhd_c::SECTOR ;
static const uint32_t TYPE_FIXED = 2, TYPE_DYNAMIC = 3, TYPE_DIFFERENCING = 4 ;
// The block size the specification recommends, and what SIMH and Virtual PC write.
static const uint32_t NEW_BLOCK_SIZE = 2 * 1024 * 1024 ;
// Seconds from the Unix epoch to the VHD one, 2000-01-01 00:00:00 UTC.
static const uint32_t VHD_EPOCH = 946684800u ;

static uint64_t be_get(const uint8_t *p, unsigned n)
{
    uint64_t v = 0 ;
    for (unsigned i = 0 ; i < n ; i++)
        v = (v << 8) | p[i] ;
    return v ;
}

static void be_put(uint8_t *p, unsigned n, uint64_t v)
{
    for (unsigned i = n ; i-- > 0 ;) {
        p[i] = (uint8_t) v ;
        v >>= 8 ;
    }
}

// The one's complement of the byte sum, the checksum field counted as zero.
static uint32_t vhd_checksum(const uint8_t *p, size_t len, size_t checksum_at)
{
    uint32_t sum = 0 ;
    for (size_t i = 0 ; i < len ; i++)
        if (i < checksum_at || i >= checksum_at + 4)
            sum += p[i] ;
    return ~sum ;
}

static bool read_at(int fd, uint8_t *buf, size_t len, uint64_t pos)
{
    while (len > 0) {
        ssize_t n = pread(fd, buf, len, (off_t) pos) ;
        if (n < 0 && errno == EINTR)
            continue ;
        if (n <= 0)
            return false ;
        buf += n ;
        len -= (size_t) n ;
        pos += (uint64_t) n ;
    }
    return true ;
}

static bool write_at(int fd, const uint8_t *buf, size_t len, uint64_t pos)
{
    while (len > 0) {
        ssize_t n = pwrite(fd, buf, len, (off_t) pos) ;
        if (n < 0 && errno == EINTR)
            continue ;
        if (n <= 0)
            return false ;
        buf += n ;
        len -= (size_t) n ;
        pos += (uint64_t) n ;
    }
    return true ;
}

static bool valid_footer(const uint8_t *f)
{
    return memcmp(f, "conectix", 8) == 0
           && (uint32_t) be_get(f + 64, 4) == vhd_checksum(f, SECTOR, 64) ;
}

static uint64_t round_up(uint64_t v, uint64_t unit)
{
    return (v + unit - 1) / unit * unit ;
}

bool storageimage_vhd_c::parse(int fd, layout_t *l, std::string *err)
{
    struct stat st ;
    if (fstat(fd, &st) != 0) {
        *err = "cannot read the file" ;
        return false ;
    }
    uint64_t file_size = (uint64_t) st.st_size ;
    // The footer closes the file; a dynamic disk also carries a copy of it at
    // the start, which stands in for one a truncated copy lost.
    l->footer_at = file_size >= SECTOR ? file_size - SECTOR : 0 ;
    l->footer_at_end = file_size >= SECTOR
                       && read_at(fd, l->footer, SECTOR, l->footer_at) && valid_footer(l->footer) ;
    if (!l->footer_at_end
            && !(file_size >= SECTOR && read_at(fd, l->footer, SECTOR, 0)
                 && valid_footer(l->footer) && be_get(l->footer + 60, 4) == TYPE_DYNAMIC)) {
        *err = "not a VHD image: it carries no VHD footer" ;
        return false ;
    }
    l->disk_size = be_get(l->footer + 48, 8) ;
    l->type = (uint32_t) be_get(l->footer + 60, 4) ;
    l->bat.clear() ;
    if (l->type == TYPE_DIFFERENCING) {
        *err = "a differencing VHD holds only the changes to its parent disk; "
               "merge it into a fixed or dynamic VHD first" ;
        return false ;
    }
    if (l->disk_size % SECTOR != 0) {
        *err = "the VHD's disk size is not a whole number of sectors" ;
        return false ;
    }
    if (l->type == TYPE_FIXED) {
        if (!l->footer_at_end || l->disk_size > l->footer_at) {
            *err = "the VHD is shorter than the disk it describes" ;
            return false ;
        }
        return true ;
    }
    if (l->type != TYPE_DYNAMIC) {
        *err = "unknown VHD disk type " + std::to_string(l->type) ;
        return false ;
    }

    l->header_at = be_get(l->footer + 16, 8) ;
    uint8_t header[1024] ;
    if (l->header_at + sizeof header > file_size
            || !read_at(fd, header, sizeof header, l->header_at)
            || memcmp(header, "cxsparse", 8) != 0
            || (uint32_t) be_get(header + 36, 4) != vhd_checksum(header, sizeof header, 36)) {
        *err = "the VHD's dynamic disk header is missing or damaged" ;
        return false ;
    }
    l->table_at = be_get(header + 16, 8) ;
    uint32_t entries = (uint32_t) be_get(header + 28, 4) ;
    l->block_size = (uint32_t) be_get(header + 32, 4) ;
    if (l->block_size < SECTOR || l->block_size % SECTOR != 0
            || l->block_size > 256u * 1024 * 1024
            || (uint64_t) entries * l->block_size < l->disk_size
            || l->table_at + (uint64_t) entries * 4 > file_size) {
        *err = "the VHD's block table does not fit the disk it describes" ;
        return false ;
    }
    uint32_t sectors = l->block_size / SECTOR ;
    l->bitmap_bytes = (uint32_t) round_up((sectors + 7) / 8, SECTOR) ;
    std::vector<uint8_t> table((size_t) entries * 4) ;
    if (entries > 0 && !read_at(fd, table.data(), table.size(), l->table_at)) {
        *err = "cannot read the VHD's block table" ;
        return false ;
    }
    l->bat.resize(entries) ;
    // A block lying past the footer is one the file lost: what the table says
    // is there cannot be read, and a block allocated at the end would land on it.
    uint64_t data_end = l->footer_at_end ? l->footer_at : file_size ;
    for (uint32_t b = 0 ; b < entries ; b++) {
        l->bat[b] = (uint32_t) be_get(&table[(size_t) b * 4], 4) ;
        if (l->bat[b] != BAT_UNUSED
                && (uint64_t) l->bat[b] * SECTOR + l->bitmap_bytes + l->block_size > data_end) {
            *err = "the VHD is shorter than its block table says" ;
            return false ;
        }
    }
    return true ;
}

bool storageimage_vhd_c::is_vhd_file(const std::string &path, uint64_t *disk_size)
{
    int f = ::open(path.c_str(), O_RDONLY) ;
    if (f < 0)
        return false ;
    struct stat st ;
    uint8_t footer[SECTOR] ;
    bool vhd = fstat(f, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= (off_t) SECTOR
               && ((read_at(f, footer, SECTOR, (uint64_t) st.st_size - SECTOR) && valid_footer(footer))
                   || (read_at(f, footer, SECTOR, 0) && valid_footer(footer)
                       && be_get(footer + 60, 4) == TYPE_DYNAMIC)) ;
    ::close(f) ;
    if (vhd && disk_size != nullptr)
        *disk_size = be_get(footer + 48, 8) ;
    return vhd ;
}

bool storageimage_vhd_c::takes(const std::string &fname)
{
    if (fname.empty())
        return false ;
    std::string path = scriptpath_resolve(fname) ;
    struct stat st ;
    if (stat(path.c_str(), &st) == 0)
        return is_vhd_file(path) ;
    return fname.size() > 4 && strcasecmp(fname.c_str() + fname.size() - 4, ".vhd") == 0 ;
}

// The cylinders/heads/sectors the specification derives from a disk's size.
static void chs_geometry(uint64_t disk_size, uint8_t *geometry)
{
    uint64_t total = disk_size / SECTOR ;
    if (total > 65535ull * 16 * 255)
        total = 65535ull * 16 * 255 ;
    uint64_t spt, heads, cth ;
    if (total >= 65535ull * 16 * 63) {
        spt = 255 ;
        heads = 16 ;
        cth = total / spt ;
    } else {
        spt = 17 ;
        cth = total / spt ;
        heads = (cth + 1023) / 1024 ;
        if (heads < 4)
            heads = 4 ;
        if (cth >= heads * 1024 || heads > 16) {
            spt = 31 ;
            heads = 16 ;
            cth = total / spt ;
        }
        if (cth >= heads * 1024) {
            spt = 63 ;
            heads = 16 ;
            cth = total / spt ;
        }
    }
    be_put(geometry, 2, cth / heads) ;
    geometry[2] = (uint8_t) heads ;
    geometry[3] = (uint8_t) spt ;
}

static void random_bytes(uint8_t *p, size_t len)
{
    int f = ::open("/dev/urandom", O_RDONLY) ;
    if (f < 0 || !read_at(f, p, len, 0)) {
        uint64_t seed = (uint64_t) time(nullptr) ^ ((uint64_t) getpid() << 32) ;
        for (size_t i = 0 ; i < len ; i++) {
            seed = seed * 6364136223846793005ull + 1442695040888963407ull ;
            p[i] = (uint8_t) (seed >> 56) ;
        }
    }
    if (f >= 0)
        ::close(f) ;
}

bool storageimage_vhd_c::create_dynamic(const std::string &path, uint64_t disk_size, std::string *err)
{
    if (disk_size == 0 || disk_size % SECTOR != 0) {
        *err = "a VHD's disk is a whole number of 512-byte sectors" ;
        return false ;
    }
    uint64_t blocks64 = (disk_size + NEW_BLOCK_SIZE - 1) / NEW_BLOCK_SIZE ;
    if (blocks64 > 0xFFFFFFF0u / 4) {
        *err = "the disk is too large for a VHD" ;
        return false ;
    }
    uint32_t blocks = (uint32_t) blocks64 ;
    uint64_t table_at = 3 * SECTOR ; // after the footer copy and the dynamic header
    uint64_t table_bytes = round_up((uint64_t) blocks * 4, SECTOR) ;

    uint8_t f[SECTOR] ;
    memset(f, 0, sizeof f) ;
    memcpy(f, "conectix", 8) ;
    be_put(f + 8, 4, 2) ;                 // features: the reserved bit, always set
    be_put(f + 12, 4, 0x00010000) ;       // format version 1.0
    be_put(f + 16, 8, SECTOR) ;           // the dynamic header follows the footer copy
    time_t now = time(nullptr) ;
    be_put(f + 24, 4, (uint64_t) now > VHD_EPOCH ? (uint64_t) now - VHD_EPOCH : 0) ;
    memcpy(f + 28, "qunl", 4) ;           // creator application
    be_put(f + 32, 4, 0x00010000) ;
    be_put(f + 36, 4, 0x5769326B) ;       // creator host OS "Wi2k", as the specification lists
    be_put(f + 40, 8, disk_size) ;
    be_put(f + 48, 8, disk_size) ;
    chs_geometry(disk_size, f + 56) ;
    be_put(f + 60, 4, TYPE_DYNAMIC) ;
    random_bytes(f + 68, 16) ;
    be_put(f + 64, 4, vhd_checksum(f, SECTOR, 64)) ;

    std::vector<uint8_t> head((size_t) (table_at + table_bytes), 0) ;
    memcpy(head.data(), f, SECTOR) ;
    uint8_t *dh = head.data() + SECTOR ;
    memcpy(dh, "cxsparse", 8) ;
    be_put(dh + 8, 8, UINT64_MAX) ;       // data offset: unused
    be_put(dh + 16, 8, table_at) ;
    be_put(dh + 24, 4, 0x00010000) ;
    be_put(dh + 28, 4, blocks) ;
    be_put(dh + 32, 4, NEW_BLOCK_SIZE) ;
    be_put(dh + 36, 4, vhd_checksum(dh, 1024, 36)) ;
    memset(head.data() + table_at, 0xFF, (size_t) table_bytes) ; // no block allocated

    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0664) ;
    if (fd < 0) {
        *err = strerror(errno) ;
        return false ;
    }
    bool ok = write_at(fd, head.data(), head.size(), 0)
              && write_at(fd, f, SECTOR, head.size()) ;
    if (!ok)
        *err = strerror(errno) ;
    ::close(fd) ;
    if (!ok)
        unlink(path.c_str()) ;
    return ok ;
}

// ----------------------------------------------------------------------------

storageimage_vhd_c::storageimage_vhd_c(std::string _image_fname)
{
    // resolved once, so every open() works on the same file; see
    // storageimage_binfile_c
    image_fname = scriptpath_resolve(_image_fname) ;
    fd = -1 ;
    readonly = false ;
    write_protected = medium_write_protected(image_fname) ;
    beyond_end_logged = false ;
    drive = nullptr ;
}

storageimage_vhd_c::~storageimage_vhd_c()
{
    close() ;
}

// A VHD has a size of its own, which a drive cannot know, so a VHD that is not
// there is not created here; the image library makes a blank one.
bool storageimage_vhd_c::open(storagedrive_c *_drive, bool create)
{
    UNUSED(create) ;
    drive = _drive ;
    if (is_open())
        close() ; // after RL11 INIT
    if (image_fname.empty())
        return true ; // ! is_open
    struct stat st ;
    if (stat(image_fname.c_str(), &st) != 0) {
        if (!expand_gz_image(image_fname)) {
            ERROR("VHD image %s not found", image_fname.c_str()) ;
            return false ;
        }
        write_protected = medium_write_protected(image_fname) ;
    }

    readonly = false ;
    if (!write_protected)
        fd = ::open(image_fname.c_str(), O_RDWR) ;
    if (fd < 0) {
        fd = ::open(image_fname.c_str(), O_RDONLY) ;
        readonly = true ;
    }
    if (fd < 0) {
        ERROR("cannot open VHD image %s: %s", image_fname.c_str(), strerror(errno)) ;
        return false ;
    }
    std::string err ;
    if (!parse(fd, &vhd, &err)) {
        ERROR("%s: %s", image_fname.c_str(), err.c_str()) ;
        ::close(fd) ;
        fd = -1 ;
        return false ;
    }
    // Writing to a dynamic disk appends at the closing footer, so one whose
    // footer is lost is served as it stands.
    if (!vhd.footer_at_end && !readonly) {
        WARNING("%s: the VHD's closing footer is missing, the disk is served read-only",
                image_fname.c_str()) ;
        readonly = true ;
    }
    bitmaps.assign(vhd.bat.size(), std::vector<uint8_t>()) ;
    beyond_end_logged = false ;
    return true ;
}

bool storageimage_vhd_c::is_open(void)
{
    return fd >= 0 ;
}

void storageimage_vhd_c::close(void)
{
    if (fd < 0)
        return ;
    ::close(fd) ;
    fd = -1 ;
    readonly = false ;
    bitmaps.clear() ;
}

uint64_t storageimage_vhd_c::size(void)
{
    return is_open() ? vhd.disk_size : 0 ;
}

const std::vector<uint8_t> &storageimage_vhd_c::bitmap(uint32_t block)
{
    std::vector<uint8_t> &bm = bitmaps[block] ;
    if (bm.empty()) {
        bm.assign(vhd.bitmap_bytes, 0) ;
        if (!read_at(fd, bm.data(), bm.size(), (uint64_t) vhd.bat[block] * SECTOR))
            ERROR("%s: cannot read the bitmap of block %u", image_fname.c_str(), block) ;
    }
    return bm ;
}

bool storageimage_vhd_c::sector_present(uint32_t block, uint32_t sector)
{
    return (bitmap(block)[sector / 8] & (0x80 >> (sector % 8))) != 0 ;
}

// Read len bytes at offset into an allocated block; a sector the block's bitmap
// does not mark was never written and reads as zeros.
bool storageimage_vhd_c::read_in_block(uint32_t block, uint32_t offset, uint8_t *buffer, uint32_t len)
{
    uint64_t data_at = (uint64_t) vhd.bat[block] * SECTOR + vhd.bitmap_bytes ;
    if (!read_at(fd, buffer, len, data_at + offset))
        return false ;
    for (uint32_t s = offset / SECTOR ; s * SECTOR < offset + len ; s++)
        if (!sector_present(block, s)) {
            uint32_t from = std::max(s * SECTOR, offset) ;
            uint32_t to = std::min((s + 1) * SECTOR, offset + len) ;
            memset(buffer + (from - offset), 0, to - from) ;
        }
    return true ;
}

void storageimage_vhd_c::read(uint8_t *buffer, uint64_t position, unsigned len)
{
    assert(is_open()) ;
    assert(buffer != nullptr) ;
    memset(buffer, 0, len) ;
    // past the end of the disk reads as zeros, as past the end of a raw image does
    if (position >= vhd.disk_size)
        return ;
    len = (unsigned) std::min<uint64_t>(len, vhd.disk_size - position) ;
    if (vhd.type == TYPE_FIXED) {
        if (!read_at(fd, buffer, len, position))
            ERROR("%s: read failure at %llu", image_fname.c_str(), (unsigned long long) position) ;
        return ;
    }
    while (len > 0) {
        uint32_t block = (uint32_t) (position / vhd.block_size) ;
        uint32_t offset = (uint32_t) (position % vhd.block_size) ;
        uint32_t n = std::min<uint32_t>(len, vhd.block_size - offset) ;
        if (vhd.bat[block] != BAT_UNUSED && !read_in_block(block, offset, buffer, n)) {
            memset(buffer, 0, n) ;
            ERROR("%s: read failure in block %u", image_fname.c_str(), block) ;
        }
        buffer += n ;
        position += n ;
        len -= n ;
    }
}

// Write into an allocated block. A sector the bitmap does not mark yet reads as
// zeros, so the sectors a partial write touches are written whole - zeros where
// the write does not reach - before their bits are set.
bool storageimage_vhd_c::write_in_block(uint32_t block, uint32_t offset, const uint8_t *buffer, uint32_t len)
{
    uint64_t data_at = (uint64_t) vhd.bat[block] * SECTOR + vhd.bitmap_bytes ;
    uint32_t first = offset / SECTOR, end = (offset + len + SECTOR - 1) / SECTOR ;
    bool all_present = true ;
    for (uint32_t s = first ; s < end && all_present ; s++)
        all_present = sector_present(block, s) ;
    if (all_present)
        return write_at(fd, buffer, len, data_at + offset) ;

    uint32_t span = (end - first) * SECTOR ;
    std::vector<uint8_t> whole(span) ;
    if (!read_in_block(block, first * SECTOR, whole.data(), span))
        return false ;
    memcpy(whole.data() + (offset - first * SECTOR), buffer, len) ;
    if (!write_at(fd, whole.data(), span, data_at + (uint64_t) first * SECTOR))
        return false ;
    std::vector<uint8_t> &bm = bitmaps[block] ;
    for (uint32_t s = first ; s < end ; s++)
        bm[s / 8] |= (uint8_t) (0x80 >> (s % 8)) ;
    return write_at(fd, bm.data(), bm.size(), (uint64_t) vhd.bat[block] * SECTOR) ;
}

// Allocate a block at the end of the file for a write into it. Each step leaves
// a file another reader takes as consistent:
//  1. the footer goes to the new end of the file; the block's room before it
//     is a hole and belongs to no table entry yet
//  2. the bitmap - every sector present - and the written data go into that
//     room, over where the footer was; the rest of the block reads as zeros
//  3. once those are on the medium, the table entry names the block
bool storageimage_vhd_c::allocate_block(uint32_t block, uint32_t offset, const uint8_t *buffer, uint32_t len)
{
    uint64_t at = round_up(vhd.footer_at, SECTOR) ;
    if (at / SECTOR >= BAT_UNUSED) {
        ERROR("%s: the VHD has grown past what its block table can address", image_fname.c_str()) ;
        return false ;
    }
    uint64_t new_footer_at = at + vhd.bitmap_bytes + vhd.block_size ;
    if (!write_at(fd, vhd.footer, SECTOR, new_footer_at))
        return false ;
    vhd.footer_at = new_footer_at ;

    std::vector<uint8_t> bm(vhd.bitmap_bytes, 0) ;
    uint32_t sectors = vhd.block_size / SECTOR ;
    memset(bm.data(), 0xFF, sectors / 8) ;
    if (sectors % 8)
        bm[sectors / 8] = (uint8_t) (0xFF00 >> (sectors % 8)) ;
    // the bitmap, a whole sector or more, covers where the old footer was
    if (!write_at(fd, bm.data(), bm.size(), at)
            || !write_at(fd, buffer, len, at + vhd.bitmap_bytes + offset))
        return false ;
    if (fdatasync(fd) != 0)
        return false ;

    uint8_t entry[4] ;
    be_put(entry, 4, at / SECTOR) ;
    if (!write_at(fd, entry, sizeof entry, vhd.table_at + (uint64_t) block * 4))
        return false ;
    vhd.bat[block] = (uint32_t) (at / SECTOR) ;
    bitmaps[block] = bm ;
    return true ;
}

void storageimage_vhd_c::write(uint8_t *buffer, uint64_t position, unsigned len)
{
    assert(buffer) ;
    assert(is_open()) ;
    assert(!readonly) ; // caller must take care
    // A VHD's disk has the size its footer gives; the part of a write past it
    // has nowhere to go.
    if (position + len > vhd.disk_size) {
        if (!beyond_end_logged) {
            ERROR("%s: write past the end of the %llu-byte disk dropped", image_fname.c_str(),
                  (unsigned long long) vhd.disk_size) ;
            beyond_end_logged = true ;
        }
        if (position >= vhd.disk_size)
            return ;
        len = (unsigned) (vhd.disk_size - position) ;
    }
    if (vhd.type == TYPE_FIXED) {
        if (!write_at(fd, buffer, len, position))
            ERROR("%s: write failure at %llu", image_fname.c_str(), (unsigned long long) position) ;
        return ;
    }
    while (len > 0) {
        uint32_t block = (uint32_t) (position / vhd.block_size) ;
        uint32_t offset = (uint32_t) (position % vhd.block_size) ;
        uint32_t n = std::min<uint32_t>(len, vhd.block_size - offset) ;
        bool ok = vhd.bat[block] == BAT_UNUSED
                  ? allocate_block(block, offset, buffer, n)
                  : write_in_block(block, offset, buffer, n) ;
        if (!ok)
            ERROR("%s: write failure in block %u: %s", image_fname.c_str(), block, strerror(errno)) ;
        buffer += n ;
        position += n ;
        len -= n ;
    }
}

// Empty the disk, keeping its size: a dynamic disk drops every block, a fixed
// one reads as zeros.
bool storageimage_vhd_c::truncate(void)
{
    assert(is_open()) ;
    assert(!readonly) ; // caller must take care
    if (vhd.type == TYPE_FIXED) {
        std::vector<uint8_t> zeros(256 * 1024, 0) ;
        for (uint64_t pos = 0 ; pos < vhd.disk_size ; pos += zeros.size())
            if (!write_at(fd, zeros.data(), (size_t) std::min<uint64_t>(zeros.size(), vhd.disk_size - pos), pos))
                return false ;
        return true ;
    }
    // the table drops every block before the file loses them
    std::vector<uint8_t> table(vhd.bat.size() * 4, 0xFF) ;
    if (!table.empty() && !write_at(fd, table.data(), table.size(), vhd.table_at))
        return false ;
    std::fill(vhd.bat.begin(), vhd.bat.end(), BAT_UNUSED) ;
    bitmaps.assign(vhd.bat.size(), std::vector<uint8_t>()) ;
    uint64_t end = std::max<uint64_t>(SECTOR, vhd.header_at + 1024) ;
    end = std::max(end, round_up(vhd.table_at + table.size(), SECTOR)) ;
    if (!write_at(fd, vhd.footer, SECTOR, end) || ftruncate(fd, (off_t) (end + SECTOR)) != 0)
        return false ;
    vhd.footer_at = end ;
    return true ;
}

void storageimage_vhd_c::get_bytes(byte_buffer_c *byte_buffer, uint64_t byte_offset, uint32_t len)
{
    byte_buffer->set_size(len) ;
    read(byte_buffer->data_ptr(), byte_offset, len) ;
}

void storageimage_vhd_c::set_bytes(byte_buffer_c *byte_buffer, uint64_t byte_offset)
{
    write(byte_buffer->data_ptr(), byte_offset, byte_buffer->size()) ;
}

// A snapshot of a VHD is a copy of its file, which is a VHD again.
void storageimage_vhd_c::save_to_file(std::string _host_filename)
{
    assert(is_open()) ;
    std::string host_filename = absolute_path(&_host_filename) ;
    int out = ::open(host_filename.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0664) ;
    if (out < 0) {
        ERROR("cannot write %s: %s", host_filename.c_str(), strerror(errno)) ;
        return ;
    }
    std::vector<uint8_t> buf(256 * 1024) ;
    uint64_t end = vhd.footer_at + SECTOR ;
    for (uint64_t pos = 0 ; pos < end ; pos += buf.size()) {
        size_t n = (size_t) std::min<uint64_t>(buf.size(), end - pos) ;
        if (!read_at(fd, buf.data(), n, pos) || !write_at(out, buf.data(), n, pos)) {
            ERROR("cannot copy %s to %s", image_fname.c_str(), host_filename.c_str()) ;
            break ;
        }
    }
    ::close(out) ;
}
