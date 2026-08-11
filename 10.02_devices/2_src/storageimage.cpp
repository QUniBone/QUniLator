/* storageimage.cpp: bianry SimH comatible image file as storage medium.

 Copyright (c) 2021, Joerg Hoppe
 j_hoppe@t-online.de, www.retrocmp.com

 Permission is hereby granted, free of charge, to any person obtaining a
 copy of this software and associated documentation files (the "Software"),
 to deal in the Software without restriction, including without limitation
 the rights to use, copy, modify, merge, publish, distribute, sublicense,
 and/or sell copies of the Software, and to permit persons to whom the
 Software is furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 JOERG HOPPE BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.


 07-mar-2021	JH      start

 A storagedrive is a disk or tape drive, with an image file as storage medium.
 a couple of these are connected to a single "storagecontroler"
 supports the "attach" command
 */
// SEEK_DATA/SEEK_HOLE (used by the COW overlay's sparseness scan) are glibc
// extensions gated on _GNU_SOURCE, which must be set before any libc header.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include <algorithm>
#include <fstream>
#include <ios>
#include <vector>
#include <sys/stat.h>
#include <fcntl.h>

#ifndef O_BINARY
#define O_BINARY 0		// for linux compatibility
#endif

#include "logger.hpp"
#include "utils.hpp"
#include "storageimage.hpp"


// BIG use of memory
void storageimage_base_c::set_zero(uint64_t position, unsigned len)
{
    uint8_t *zeros = (uint8_t*) malloc(len) ;
    memset(zeros, 0, len) ;
    write(zeros, position, len);
    free(zeros) ;
}

bool storageimage_base_c::is_zero(uint64_t position, unsigned len)
{
    bool result = true ;
    uint8_t *buffer= (uint8_t*) malloc(len) ;
    read(buffer, position, len);
    for (unsigned i=0 ; i < len ; i++)
        if (buffer[i] != 0)
            result = false ;
    free(buffer) ;
    return result ;
}


// Expand <image>.gz into <image>, without a shell.
//
// The image path is settable through the web API, and it used to be pasted
// into the command line "zcat <path>.gz ><path>" and handed to system(). A
// name carrying a space breaks that command; one carrying a semicolon or a
// backtick runs what it names, as the operator, who has sudo on this board.
// Here zcat gets the path as an argument rather than as text in a line, and
// the output file as a file descriptor, so there is no line for anything to be
// quoted into and no shell to interpret it. subprocess_run() does the fork and
// the wait; see utils.hpp.
//
// A failed expansion takes the half-written file with it: an image left
// truncated is one the retry above would open as a valid, empty disk.
static bool uncompress_gz(const std::string &gz_path, const std::string &out_path)
{
    int out_fd = ::open(out_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0666);
    if (out_fd < 0)
        return false;

    // "--" so a path starting with '-' stays a path
    const char *argv[] = { "zcat", "--", gz_path.c_str(), NULL };
    int rc = subprocess_run(argv, out_fd);
    ::close(out_fd);
    if (rc == 0)
        return true;
    unlink(out_path.c_str());
    return false;
}

// http://www.cplusplus.com/doc/tutorial/files/

// open a file, if possible.
// set the file_readonly flag
// creates file, if not existing
// result: OK= true, else false
bool storageimage_binfile_c::open(storagedrive_c *_drive, bool create) 
{
	drive = _drive ;
    // 1st: if file not exists, try to unzip it from <image_fname>.gz
    int retries = 2 ;
    while (retries > 0) {
        readonly = false;
        if (is_open())
            close(); // after RL11 INIT
        if (image_fname.empty())
            return true ; // ! is_open
        if (!write_protected) {
            f.open(image_fname, std::ios::in | std::ios::out | std::ios::binary | std::ios::ate);
            if (f.is_open())
                return true;
        }

        // a protected medium, or one this process may not write: read only
        f.open(image_fname, std::ios::in | std::ios::binary | std::ios::ate);
        if (f.is_open()) {
            readonly = true;
            return true;
        }

        retries-- ;
        if (retries > 0) {
            // file could not be opened, neither rw nor read only
            // try to unzip, then retry opening
            std::string compressed_image_fname = image_fname + ".gz" ;
            if (FILE *fz = fopen(compressed_image_fname.c_str(), "r")) {
                fclose(fz);
                printf("Only compressed image file %s found, expanding %s ...\n",
                       image_fname.c_str(), compressed_image_fname.c_str()) ;
                if (!uncompress_gz(compressed_image_fname, image_fname)) {
                    printf(" FAILED!\n") ;
                    retries = 0 ; // not again
                } else
                    printf("... complete.\n") ;

            } else
                retries = 0 ; // not again
        }
    }

    // definitely no image file neither plain nor zipped
    // create one?
    if (!create)
        return false;

    // try to create
    // https://stackoverflow.com/questions/17260394/fstream-not-creating-new-file/18160837
    f.open(image_fname, std::ios::out);
    f.close();
    f.open(image_fname, std::ios::in | std::ios::out | std::ios::binary | std::ios::ate);
    if (f.is_open()) {
        INFO("Created empty image file %s.", image_fname.c_str()) ;
        return true ;
    } else {
        INFO("Creating empty image file %s FAILED.", image_fname.c_str()) ;
        return false;
    }
}

bool storageimage_binfile_c::is_open() 
{
    return f.is_open();
}

// set file size to 0
bool storageimage_binfile_c::truncate() 
{
    assert(is_open());
    assert(!readonly); // caller must take care

    f.close();
    // reopen with "trunc" option
    f.open(image_fname, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
    return  f.is_open() ;
}


/* read "len" bytes from file into buffer
 * if file is too short, 00s are read
 * it is assumed that buffer has at least a size of "len"
 */
void storageimage_binfile_c::read(uint8_t *buffer, uint64_t position, unsigned len) 
{
    assert(is_open());
    assert(buffer != nullptr) ;
    assert(len) ;
    // 1. fill the buffer with 00s
    memset(buffer, 0, len);

    // 2. move read pointer
    f.seekg(position);
    // may be at eof now, doesn't matter

    // 3. read as byte count, may abort at end of file
    f.read((char *) buffer, len);
}

/* write "len" bytes from buffer into file at position "offset"
 * if file too short, it is extended
 */
void storageimage_binfile_c::write(uint8_t *buffer, uint64_t position, unsigned len) 
{
    int64_t write_pos = (int64_t) position;  // unsigned-> int
    const int max_chunk_size = 0x40000; //256KB: trade-off between performance and mem usage
    uint8_t *fillbuff = NULL;
    int64_t file_size, p;

    assert(buffer);
    assert(is_open());
    assert(!readonly); // caller must take care

    // enlarge file in chunks until filled up to "position"
    f.clear(); // clear fail bit
    f.seekp(0, std::ios::end); // move to current EOF
    file_size = f.tellp(); // current file len
    if (file_size < 0)
        file_size = 0; // -1 on emtpy files
    while (file_size < write_pos) {
        // fill in '00' 'chunks up to desired end, but limit to max_chunk_size
        int chunk_size = std::min(max_chunk_size, (int) (write_pos - file_size));
        if (!fillbuff) {
            // allocate 00-buffer only once
            fillbuff = (uint8_t *) malloc(max_chunk_size);
            assert(fillbuff);
            memset(fillbuff, 0, max_chunk_size);
        }
        f.clear(); // clear fail bit
        f.seekp(file_size, std::ios::beg); // move to end
        f.write((const char *) fillbuff, chunk_size);
        file_size += chunk_size;
    }
    if (fillbuff)
        free(fillbuff); // has been used, discard

    if (file_size == 0)
        // p = -1 error after seekp(0) on empty files?
        assert(write_pos == 0);
    else {
        // move write pointer to target position
        f.clear(); // clear fail bit
        f.seekp(write_pos, std::ios::beg);
        p = f.tellp(); // position now < target?
        assert(p == write_pos);
    }

    // 3. write data
    f.write((const char*) buffer, len);
    if (f.fail())
        ERROR("storageimage_binfile_c.write() failure on %s", image_fname.c_str());
    f.flush();
}

uint64_t storageimage_binfile_c::size(void) 
{
    f.seekp(0, std::ios::end);
    return f.tellp();
}

void storageimage_binfile_c::close(void) 
{
    if (!is_open())
        return ;
    f.close();
    readonly = false;
}

// read data from image into memory buffer (cache)
void storageimage_binfile_c::get_bytes(byte_buffer_c* byte_buffer, uint64_t byte_offset, uint32_t len)
{
    byte_buffer->set_size(len) ;
    read(byte_buffer->data_ptr(), byte_offset, len) ;
}

// write cache buffer to image
void storageimage_binfile_c::set_bytes(byte_buffer_c *byte_buffer, uint64_t byte_offset)
{
    write(byte_buffer->data_ptr(), byte_offset, byte_buffer->size()) ;
}


// make a snapshot
// must be locked against parallel read()/write()/close()
void storageimage_binfile_c::save_to_file(std::string _host_filename)
{
    std::string host_filename = absolute_path(&_host_filename) ;
    assert(is_open()) ;

    try {
        std::streampos current_pos = f.tellg() ;

        // make a stream copy, then resore the position
        std::ofstream dest(host_filename, std::ios::binary);
        f.seekg(0) ; // pos source to begin

        // copy
        dest << f.rdbuf();

        // restore pos
        f.seekg(current_pos) ;
    }
    catch(std::exception& e) {
        ERROR(e.what()) ;
    }
}



// ----------------------------------------------------------------------------
// storageimage_cow_c: copy-on-write base + sparse overlay
// ----------------------------------------------------------------------------

// Sidecar layout: an 8-byte magic, the block count and the logical size the
// bitmap describes, then ceil(block_count/8) bitmap bytes. block_count and
// image_size let a stale sidecar (base resized under it) be detected and the
// bitmap rebuilt from the overlay instead.
static const char COW_MAP_MAGIC[8] = { 'Q','C','O','W','M','A','P','1' } ;

bool storageimage_cow_c::bit_get(uint64_t block) const
{
    if (block >= block_count)
        return false ;
    return (occupancy[block >> 3] >> (block & 7)) & 1 ;
}

void storageimage_cow_c::bit_set(uint64_t block)
{
    assert(block < block_count) ;
    occupancy[block >> 3] |= (uint8_t)(1u << (block & 7)) ;
}

// grow the logical disk so that at least needed_block_count blocks exist, and
// size the bitmap to match. A write past the current end extends the image the
// same way the plain binfile grows on write.
void storageimage_cow_c::ensure_blocks(uint64_t needed_block_count)
{
    if (needed_block_count <= block_count)
        return ;
    block_count = needed_block_count ;
    if (image_size_bytes < block_count * BLOCK_SIZE)
        image_size_bytes = block_count * BLOCK_SIZE ;
    size_t need_bytes = (size_t)((block_count + 7) / 8) ;
    if (occupancy.size() < need_bytes)
        occupancy.resize(need_bytes, 0) ;
}

// read one whole 512-byte block, overlay if written else base, zero past EOF
void storageimage_cow_c::cow_read_block(uint64_t block, uint8_t *block_buffer)
{
    memset(block_buffer, 0, BLOCK_SIZE) ;
    off_t pos = (off_t)(block * BLOCK_SIZE) ;
    int fd = bit_get(block) ? overlay_fd : base_fd ;
    // a short read (block sits past this file's EOF) leaves the tail as the
    // zeros already written, matching the plain binfile's read behaviour
    ssize_t got = pread(fd, block_buffer, BLOCK_SIZE, pos) ;
    (void) got ;
}

bool storageimage_cow_c::load_bitmap_sidecar()
{
    int fd = ::open(map_fname.c_str(), O_RDONLY) ;
    if (fd < 0)
        return false ;
    bool ok = false ;
    char magic[8] ;
    uint64_t stored_block_count = 0, stored_image_size = 0 ;
    if (pread(fd, magic, sizeof magic, 0) == (ssize_t)sizeof magic
            && memcmp(magic, COW_MAP_MAGIC, sizeof magic) == 0
            && pread(fd, &stored_block_count, sizeof stored_block_count, 8)
                    == (ssize_t)sizeof stored_block_count
            && pread(fd, &stored_image_size, sizeof stored_image_size, 16)
                    == (ssize_t)sizeof stored_image_size) {
        // a stored image smaller than the base means the base changed under the
        // overlay; reject and rebuild
        if (stored_image_size >= base_size_bytes && stored_block_count > 0) {
            size_t bitmap_bytes = (size_t)((stored_block_count + 7) / 8) ;
            std::vector<uint8_t> buf(bitmap_bytes, 0) ;
            if (pread(fd, buf.data(), bitmap_bytes, 24) == (ssize_t)bitmap_bytes) {
                image_size_bytes = stored_image_size ;
                block_count = stored_block_count ;
                occupancy.swap(buf) ;
                ok = true ;
            }
        }
    }
    ::close(fd) ;
    return ok ;
}

// crash recovery: a written block is allocated data in the sparse overlay (even
// all-zero data, since it arrived via pwrite), so walk the overlay's data
// regions with SEEK_DATA/SEEK_HOLE and mark every allocated block occupied.
void storageimage_cow_c::rebuild_bitmap_from_overlay()
{
    for (uint64_t i = 0 ; i < (block_count + 7) / 8 ; i++)
        occupancy[i] = 0 ;

    off_t end = (off_t) image_size_bytes ;
    off_t pos = 0 ;
    while (pos < end) {
        off_t data = lseek(overlay_fd, pos, SEEK_DATA) ;
        if (data < 0)
            break ; // no more data (ENXIO past the last data region)
        off_t hole = lseek(overlay_fd, data, SEEK_HOLE) ;
        if (hole < 0)
            hole = end ;
        if (hole > end)
            hole = end ;
        uint64_t first = (uint64_t)data / BLOCK_SIZE ;
        uint64_t last = (uint64_t)(hole - 1) / BLOCK_SIZE ; // inclusive
        for (uint64_t b = first ; b <= last && b < block_count ; b++)
            bit_set(b) ;
        pos = hole ;
    }
    // restore the file offset the SEEK_DATA/SEEK_HOLE walk moved
    lseek(overlay_fd, 0, SEEK_SET) ;
}

void storageimage_cow_c::write_bitmap_sidecar()
{
    int fd = ::open(map_fname.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666) ;
    if (fd < 0) {
        ERROR("storageimage_cow_c: cannot write bitmap sidecar %s", map_fname.c_str()) ;
        return ;
    }
    (void) !::write(fd, COW_MAP_MAGIC, sizeof COW_MAP_MAGIC) ;
    (void) !::write(fd, &block_count, sizeof block_count) ;
    (void) !::write(fd, &image_size_bytes, sizeof image_size_bytes) ;
    (void) !::write(fd, occupancy.data(), (size_t)((block_count + 7) / 8)) ;
    ::close(fd) ;
}

bool storageimage_cow_c::open(storagedrive_c *_drive, bool create)
{
    drive = _drive ;
    if (opened)
        close() ; // after an INIT that re-opens

    if (base_fname.empty())
        return true ; // detached: ! is_open

    base_fd = ::open(base_fname.c_str(), O_RDONLY) ;
    if (base_fd < 0 && create) {
        // no base yet: let a "create a new disk" attach still work by starting
        // from an empty base, exactly as the plain binfile would create one
        int cfd = ::open(base_fname.c_str(), O_RDWR | O_CREAT, 0666) ;
        if (cfd >= 0) {
            ::close(cfd) ;
            INFO("Created empty base image file %s.", base_fname.c_str()) ;
            base_fd = ::open(base_fname.c_str(), O_RDONLY) ;
        }
    }
    if (base_fd < 0) {
        INFO("storageimage_cow_c: base image %s not found.", base_fname.c_str()) ;
        return false ;
    }

    struct stat st ;
    if (fstat(base_fd, &st) != 0) {
        ::close(base_fd) ;
        base_fd = -1 ;
        return false ;
    }
    base_size_bytes = (uint64_t) st.st_size ;

    overlay_fd = ::open(overlay_fname.c_str(), O_RDWR | O_CREAT, 0666) ;
    if (overlay_fd < 0) {
        ERROR("storageimage_cow_c: cannot open overlay %s", overlay_fname.c_str()) ;
        ::close(base_fd) ;
        base_fd = -1 ;
        return false ;
    }

    // the overlay's own size may exceed the base if an earlier run's writes
    // extended the disk; the logical size is the larger of the two
    uint64_t overlay_size = 0 ;
    if (fstat(overlay_fd, &st) == 0)
        overlay_size = (uint64_t) st.st_size ;
    image_size_bytes = std::max(base_size_bytes, overlay_size) ;
    if (overlay_size < base_size_bytes) {
        // keep the overlay at least base-sized and sparse
        if (ftruncate(overlay_fd, (off_t) base_size_bytes) != 0)
            ERROR("storageimage_cow_c: cannot size overlay %s", overlay_fname.c_str()) ;
    }
    block_count = (image_size_bytes + BLOCK_SIZE - 1) / BLOCK_SIZE ;
    occupancy.assign((size_t)((block_count + 7) / 8), 0) ;

    // prefer the persisted bitmap; if it is absent or stale, reconstruct it
    // from the overlay's allocated (written) regions
    if (!load_bitmap_sidecar())
        rebuild_bitmap_from_overlay() ;

    opened = true ;
    return true ;
}

bool storageimage_cow_c::is_open(void)
{
    return opened ;
}

// A truncate on a COW image means "reset the medium": the overlay is thrown
// away so the disk reads back the pristine base. There is no separate
// zero-length state — the base is the floor. (The RX02 SET_MEDIA_DENSITY
// reformat relies only on the subsequent full-track writes to repopulate.)
bool storageimage_cow_c::truncate(void)
{
    assert(opened) ;
    discard() ;
    return true ;
}

void storageimage_cow_c::read(uint8_t *buffer, uint64_t position, unsigned len)
{
    assert(opened) ;
    assert(buffer != nullptr) ;
    assert(len) ;
    uint8_t block[BLOCK_SIZE] ;
    uint64_t pos = position ;
    unsigned remaining = len ;
    uint8_t *dst = buffer ;
    while (remaining > 0) {
        uint64_t block_idx = pos / BLOCK_SIZE ;
        unsigned off = (unsigned)(pos % BLOCK_SIZE) ;
        unsigned n = std::min((unsigned)(BLOCK_SIZE - off), remaining) ;
        cow_read_block(block_idx, block) ; // zero-fills past EOF
        memcpy(dst, block + off, n) ;
        dst += n ;
        pos += n ;
        remaining -= n ;
    }
}

void storageimage_cow_c::write(uint8_t *buffer, uint64_t position, unsigned len)
{
    assert(opened) ;
    assert(buffer != nullptr) ;
    assert(len) ;
    uint8_t block[BLOCK_SIZE] ;
    uint64_t pos = position ;
    unsigned remaining = len ;
    const uint8_t *src = buffer ;
    while (remaining > 0) {
        uint64_t block_idx = pos / BLOCK_SIZE ;
        unsigned off = (unsigned)(pos % BLOCK_SIZE) ;
        unsigned n = std::min((unsigned)(BLOCK_SIZE - off), remaining) ;
        ensure_blocks(block_idx + 1) ;
        if (off == 0 && n == BLOCK_SIZE) {
            // whole block: write it straight to the overlay
            if (pwrite(overlay_fd, src, BLOCK_SIZE, (off_t)(block_idx * BLOCK_SIZE))
                    != (ssize_t) BLOCK_SIZE)
                ERROR("storageimage_cow_c: overlay write failed at block %" PRIu64, block_idx) ;
        } else {
            // partial block: COW-read the whole block, patch, write it back
            cow_read_block(block_idx, block) ;
            memcpy(block + off, src, n) ;
            if (pwrite(overlay_fd, block, BLOCK_SIZE, (off_t)(block_idx * BLOCK_SIZE))
                    != (ssize_t) BLOCK_SIZE)
                ERROR("storageimage_cow_c: overlay write failed at block %" PRIu64, block_idx) ;
        }
        bit_set(block_idx) ;
        src += n ;
        pos += n ;
        remaining -= n ;
    }
    if (position + len > image_size_bytes) {
        image_size_bytes = position + len ;
        // keep the block count (and the occupancy bitmap that covers it) in step
        // with a write that extended the disk, so commit/export/save_to_file and
        // dirty_block_count iterate the whole grown image
        block_count = (image_size_bytes + BLOCK_SIZE - 1) / BLOCK_SIZE ;
        size_t need_bytes = (size_t)((block_count + 7) / 8) ;
        if (occupancy.size() < need_bytes)
            occupancy.resize(need_bytes, 0) ;
    }
}

uint64_t storageimage_cow_c::size(void)
{
    return image_size_bytes ;
}

void storageimage_cow_c::close(void)
{
    if (!opened)
        return ;
    write_bitmap_sidecar() ;
    if (overlay_fd >= 0) {
        ::close(overlay_fd) ;
        overlay_fd = -1 ;
    }
    if (base_fd >= 0) {
        ::close(base_fd) ;
        base_fd = -1 ;
    }
    opened = false ;
}

void storageimage_cow_c::get_bytes(byte_buffer_c *byte_buffer, uint64_t byte_offset, uint32_t len)
{
    byte_buffer->set_size(len) ;
    read(byte_buffer->data_ptr(), byte_offset, len) ;
}

void storageimage_cow_c::set_bytes(byte_buffer_c *byte_buffer, uint64_t byte_offset)
{
    write(byte_buffer->data_ptr(), byte_offset, byte_buffer->size()) ;
}

// Export: write a flattened plain image (base + overlay merged) to host_filename.
// Base and overlay are left untouched.
void storageimage_cow_c::save_to_file(std::string _host_filename)
{
    std::string host_filename = absolute_path(&_host_filename) ;
    assert(opened) ;
    int fd = ::open(host_filename.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666) ;
    if (fd < 0) {
        ERROR("storageimage_cow_c::save_to_file cannot open %s", host_filename.c_str()) ;
        return ;
    }
    uint8_t block[BLOCK_SIZE] ;
    for (uint64_t b = 0 ; b < block_count ; b++) {
        cow_read_block(b, block) ;
        if (pwrite(fd, block, BLOCK_SIZE, (off_t)(b * BLOCK_SIZE)) != (ssize_t) BLOCK_SIZE) {
            ERROR("storageimage_cow_c::save_to_file write failed on %s", host_filename.c_str()) ;
            break ;
        }
    }
    // trim the trailing block padding to the exact logical size
    if (ftruncate(fd, (off_t) image_size_bytes) != 0)
        ERROR("storageimage_cow_c::save_to_file cannot size %s", host_filename.c_str()) ;
    ::close(fd) ;
}

// Throw the overlay away: clear the bitmap, punch the overlay back to all-holes
// at the base size, and drop the sidecar. The disk reads the pristine base again.
void storageimage_cow_c::discard()
{
    assert(opened) ;
    image_size_bytes = base_size_bytes ;
    block_count = (image_size_bytes + BLOCK_SIZE - 1) / BLOCK_SIZE ;
    occupancy.assign((size_t)((block_count + 7) / 8), 0) ;
    if (ftruncate(overlay_fd, 0) != 0
            || ftruncate(overlay_fd, (off_t) base_size_bytes) != 0)
        ERROR("storageimage_cow_c::discard cannot reset overlay %s", overlay_fname.c_str()) ;
    unlink(map_fname.c_str()) ;
    INFO("storageimage_cow_c: overlay for %s discarded", base_fname.c_str()) ;
}

// Fold the overlay into the base: this is the only path that writes the base.
// Every occupied block is copied from the overlay into the base, then the
// overlay is discarded so the (now updated) base is again the pristine floor.
bool storageimage_cow_c::commit()
{
    assert(opened) ;

    // Consolidating writes into the base needs it writable. A base the user has
    // made read-only cannot be committed into; fail cleanly rather than touch
    // its permissions. (The overlay itself already keeps the emulator off the
    // base, so nothing else needs write here.)
    if (medium_write_protected(base_fname)) {
        ERROR("storageimage_cow_c::commit: the base %s is write protected",
              base_fname.c_str()) ;
        return false ;
    }
    int base_rw_fd = ::open(base_fname.c_str(), O_RDWR) ;
    if (base_rw_fd < 0) {
        ERROR("storageimage_cow_c::commit cannot open base %s for writing (%s)",
              base_fname.c_str(), strerror(errno)) ;
        return false ;
    }
    bool ok = true ;
    uint8_t block[BLOCK_SIZE] ;
    for (uint64_t b = 0 ; b < block_count ; b++) {
        if (!bit_get(b))
            continue ;
        memset(block, 0, BLOCK_SIZE) ;
        (void) pread(overlay_fd, block, BLOCK_SIZE, (off_t)(b * BLOCK_SIZE)) ;
        if (pwrite(base_rw_fd, block, BLOCK_SIZE, (off_t)(b * BLOCK_SIZE)) != (ssize_t) BLOCK_SIZE) {
            ERROR("storageimage_cow_c::commit write failed at block %" PRIu64, b) ;
            ok = false ;
            break ;
        }
    }
    // match the base to the (possibly grown) logical size before closing
    if (ok && ftruncate(base_rw_fd, (off_t) image_size_bytes) != 0)
        ERROR("storageimage_cow_c::commit cannot size base %s", base_fname.c_str()) ;
    fsync(base_rw_fd) ;
    ::close(base_rw_fd) ;

    if (ok) {
        // the base is now the merged content; make it the pristine floor
        base_size_bytes = image_size_bytes ;
        INFO("storageimage_cow_c: overlay for %s committed into base", base_fname.c_str()) ;
        discard() ;
    }
    return ok ;
}

uint64_t storageimage_cow_c::dirty_block_count(void) const
{
    uint64_t n = 0 ;
    for (uint64_t b = 0 ; b < block_count ; b++)
        if (bit_get(b))
            n++ ;
    return n ;
}

// the overlay's real footprint on disk (allocated blocks * 512), which is far
// smaller than its apparent size while it stays sparse
uint64_t storageimage_cow_c::overlay_allocated_bytes(void) const
{
    struct stat st ;
    if (overlay_fd >= 0 && fstat(overlay_fd, &st) == 0)
        return (uint64_t) st.st_blocks * 512 ;
    return 0 ;
}


// result: OK= true, else false
bool storageimage_memory_c::open(storagedrive_c *_drive, bool create)
{
	drive = _drive ;
    UNUSED(create);
    if (is_open())
        close(); // after RL11 INIT
    if (data_size)
        data = (uint8_t *)malloc(data_size) ;
    opened = true ;
    return true ;
}

bool storageimage_memory_c::is_open(	void)
{
    return opened ;
}

bool storageimage_memory_c::truncate(void)
{
    assert(is_open()) ;
    // fixed size

    free(data) ;
    data = nullptr ;
    data_size = 0;
    return true ;
}

void storageimage_memory_c::read(uint8_t *buffer, uint64_t position, unsigned len)
{
    assert(is_open()) ;
    assert(buffer != nullptr) ;
    assert(len) ;
    // if len > data_size, fill up with 00s

    uint8_t *dest = buffer ;
    unsigned bytes_copied = 0 ;
    if (position < data_size) {
        // copy bytes from data[] to buffer
        uint8_t *src = &data[position] ;
        uint64_t stop_pos = position + len ; // idx of byte after last byte to fill
        if (stop_pos <= data_size)
            bytes_copied = len ; // all wanted bytes in data[]
        else
            bytes_copied = data_size - position ;
        // copy
        memcpy(dest, src, bytes_copied) ;
        dest += bytes_copied ;
    }
    // fill up 00s
    assert (bytes_copied <= len) ;
    if (bytes_copied != len)
        memset(dest, 0, len - bytes_copied) ;
}


void storageimage_memory_c::write(uint8_t *buffer, uint64_t position, unsigned len)
{
    assert(buffer) ;
    assert(is_open()) ;
    // re-allocate?
    uint64_t new_size = position + len - 1 ;
    if (new_size > data_size) {
        data = (uint8_t *)realloc(data, data_size) ; // conent preserved
        data_size = new_size ;
    }
    uint8_t *dest = &(data[position]);
    memcpy(dest, buffer, len) ;
}

uint64_t storageimage_memory_c::size(void)
{
    assert(is_open()) ;
    return data_size ;
}

// data volatile
void storageimage_memory_c::close(void)
{
    assert(is_open()) ;
    free(data) ;
    data = nullptr ;
    // data size remains for next open()
    opened = false ;
}

// extract a smaller buffer, required by storage_image_base_c
void storageimage_memory_c::get_bytes(byte_buffer_c* byte_buffer, uint64_t byte_offset, uint32_t len)
{
    byte_buffer->set_size(len) ;
    assert(byte_offset < data_size) ;
    uint8_t *src = &(data[byte_offset]) ;
    uint8_t *dest = byte_buffer->data_ptr() ;
    assert(src+len <= data+size()) ; // no overrun allowed
    memcpy(dest, src, len) ;
}

// write and free cache buffer
void storageimage_memory_c::set_bytes(byte_buffer_c *byte_buffer, uint64_t byte_offset)
{
    uint8_t *src = byte_buffer->data_ptr() ;
    assert(byte_offset < data_size) ;
    uint8_t *dest = &(data[byte_offset]);
    memcpy(dest, src, byte_buffer->size()) ;
}



// load complete image from a host file
// if result_file_created:
bool storageimage_memory_c::load_from_file(std::string _host_filename,
        bool allowcreate, bool *result_file_created)
{
    std::string host_filename ;

    bool result ;
    bool file_created = false ;
    try {
        host_filename = absolute_path(&_host_filename) ;

        // opens image file or creates it
        int32_t file_descriptor;
        struct stat file_status; // timestamps and size

        if (!readonly) // check writability here.
            file_descriptor = ::open(host_filename.c_str(), O_BINARY | O_RDWR, 0666);
        else
            file_descriptor = ::open(host_filename.c_str(), O_BINARY | O_RDONLY);

        // create file if it does not exist
        if (file_descriptor < 0 && allowcreate) {
            file_descriptor = ::creat(host_filename.c_str(), 0666);
            file_created = true;
        }
        if (file_descriptor < 0)
            throw printf_exception("image_data_load_from_disk(): cannot open or create \"%s\"", host_filename.c_str()) ;

        // get timestamps, to monitor changes
        ::stat(host_filename.c_str(), &file_status);

        // clear image
        memset(data, 0, data_size);

        if (!file_created) {
            // read existing file
            if (!is_fileset(&host_filename, 0, data_size))
                if (file_status.st_size > (off_t)data_size) { // trunc ?
                    FATAL("storageimage_memory_c::load_from_disk(): File \"%s\" is %" PRId64 " bytes, shall be trunc'd to %" PRId64 " bytes, non-zero data would be lost",
                          host_filename.c_str(), (uint64_t)file_status.st_size, data_size) ;
                }

            int res = ::read(file_descriptor, data, data_size);
            ::close(file_descriptor) ;

            // read file to memory
            if (res < 0)
                throw printf_exception("storageimage_memory_c::load_from_disk(): cannot read \"%s\"", host_filename.c_str()) ;
            if (res < file_status.st_size)
                throw printf_exception("storageimage_memory_c::load_from_disk(): cannot read %" PRId64 " bytes from \"%s\"",
                                       (uint64_t)file_status.st_size, host_filename.c_str()) ;
        }

        result = true ;
    }
    catch(std::exception& e) {
        ERROR(e.what()) ;
        result = false ;
    }
    if (result_file_created)
        *result_file_created = file_created ;
    return result ;
}

// needs to be locked against image changes
void storageimage_memory_c::save_to_file(std::string _host_filename)
{
    std::string host_filename = absolute_path(&_host_filename) ;

    try {
        // opens image file for full rewrite or creates it
        int32_t file_descriptor;
        file_descriptor = ::open(host_filename.c_str(), O_BINARY | O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (file_descriptor < 0)
            throw printf_exception("storageimage_memory_c::data_save_to_disk() cannot open \"%s\"",
                                   host_filename.c_str());
        ::write(file_descriptor, data, data_size);
        ::close(file_descriptor);
    }
    catch(std::exception& e) {
        ERROR(e.what()) ;
    }
}



