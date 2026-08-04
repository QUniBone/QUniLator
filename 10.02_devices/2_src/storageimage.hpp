/* storageimage.hpp: geneariv interface and implemention of plain file as storage medium

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

 A disk/tape emulation (storage drive) saves data onto some magnetic surface,
 organized as filesystem.
 On Linux side this is saves a
 - plain binary file (SimH compatible block stream)
 - an unpacked shared directory with file tree


 */
#ifndef _STORAGEIMAGE_HPP_
#define _STORAGEIMAGE_HPP_

#include <stdint.h>
#include <unistd.h>
#include <string>
#include <fstream>
#include <vector>
#include "logsource.hpp"
#include "bytebuffer.hpp"
#include "medium_write_protect.hpp"

class storagedrive_c ;

// generic interface to emulated drive
// storage is accessed as stream of bytes
class storageimage_base_c: public logsource_c  {
public:
	storagedrive_c	*drive ; // uplink to drive managing this image
    // pure interface
    virtual ~storageimage_base_c() {} ; // google for "abstract destructor" for fun
    virtual bool is_readonly() = 0 ;
    virtual bool open(storagedrive_c *drive, bool create) = 0;
    virtual bool is_open(	void)= 0;
    virtual bool truncate(void)= 0 ;
    virtual void read(uint8_t *buffer, uint64_t position, unsigned len)=0;
    virtual void write(uint8_t *buffer, uint64_t position, unsigned len)= 0;
    virtual void set_zero(uint64_t position, unsigned len) ;
    virtual bool is_zero(uint64_t position, unsigned len) ;

    virtual uint64_t size(void)= 0;
    virtual void close(void)= 0;
    virtual void get_bytes(byte_buffer_c *byte_buffer, uint64_t byte_offset, uint32_t data_size) = 0;
    virtual void set_bytes(byte_buffer_c *byte_buffer, uint64_t byte_offset)= 0 ;

    //	bool image_load_from_disk(string host_filename, 		bool allowcreate, bool *filecreated) ;
    virtual void save_to_file(std::string host_filename) = 0 ; // make a snapshot

} ;



// a monolitic binary disk file containing the byte stream, SimH compatible
class storageimage_binfile_c: public storageimage_base_c {
private:
    bool readonly ;
    // The medium's own write protection, read from the image file's mode as the
    // cartridge goes in and kept for as long as it is in the drive. It is taken
    // once because the file's mode moves under the emulator's feet: the web
    // service clears the write bits of an attached image while the machine runs,
    // so that the file shares cannot write a medium the machine is using, and a
    // re-open (an RL11 INIT) would otherwise read that back as a protected pack.
    bool write_protected ;
    std::fstream f; // image file
    std::string image_fname ;

public:
    storageimage_binfile_c(std::string _image_fname) {
        image_fname = _image_fname ;
        readonly = false ;
        write_protected = medium_write_protected(image_fname) ;
    }

    // nothing to free
    virtual ~storageimage_binfile_c() override {
        // handle recreation via param change with open images
        close() ;
    }

    // Write protection belongs to the medium, so a drive holding a cartridge it
    // has not opened - a pack at rest - reports it too.
    virtual bool is_readonly() override {
        if (is_open())
            return readonly ;
        return write_protected ;
    }
    virtual bool open(storagedrive_c *drive, bool create) override;
    virtual bool is_open(	void) override;
    virtual bool truncate(void) override;
    virtual void read(uint8_t *buffer, uint64_t position, unsigned len) override;
    virtual void write(uint8_t *buffer, uint64_t position, unsigned len) override;
    virtual uint64_t size(void) override;
    virtual void close(void) override;
    virtual void get_bytes(byte_buffer_c *byte_buffer, uint64_t byte_offset, uint32_t data_size) override;
    virtual void set_bytes(byte_buffer_c *byte_buffer, uint64_t byte_offset) override ;
    virtual void save_to_file(std::string host_filename) override ;


} ;

// A copy-on-write disk image: a read-only base file plus a sparse overlay that
// holds every 512-byte block written since the base was opened. Reads fall
// through to the base for blocks not yet written; writes only ever touch the
// overlay, so the base stays pristine and an unclean shutdown can only corrupt
// the throw-away overlay. An occupancy bitmap (one bit per block) records which
// blocks live in the overlay; it is persisted to a sidecar and, if the sidecar
// is missing or stale, rebuilt from the overlay's SEEK_DATA/SEEK_HOLE sparseness.
class storageimage_cow_c: public storageimage_base_c {
private:
    static const unsigned BLOCK_SIZE = 512 ;

    std::string base_fname ;    // read-only base image
    std::string overlay_fname ; // sparse overlay holding written blocks
    std::string map_fname ;     // occupancy-bitmap sidecar

    int base_fd ;               // O_RDONLY
    int overlay_fd ;            // O_RDWR|O_CREAT

    uint64_t base_size_bytes ;  // size of the base file (pristine size)
    uint64_t image_size_bytes ; // logical disk size (grows if a write extends it)
    uint64_t block_count ;      // ceil(image_size_bytes / BLOCK_SIZE)

    std::vector<uint8_t> occupancy ; // 1 bit per block; set => block is in overlay
    bool opened ;               // between open() and close()

    // bitmap helpers
    bool bit_get(uint64_t block) const ;
    void bit_set(uint64_t block) ;
    void ensure_blocks(uint64_t needed_block_count) ; // grow bitmap + logical size

    // read one whole block: from the overlay if occupied, else the base; the
    // buffer is zero-filled first so a short read past EOF yields 00s.
    void cow_read_block(uint64_t block, uint8_t *block_buffer) ;

    bool load_bitmap_sidecar() ;      // true if a valid sidecar was loaded
    void rebuild_bitmap_from_overlay() ; // crash recovery from overlay sparseness
    void write_bitmap_sidecar() ;

public:
    storageimage_cow_c(std::string _base_fname, std::string _overlay_fname,
                       std::string _map_fname) {
        base_fname = _base_fname ;
        overlay_fname = _overlay_fname ;
        map_fname = _map_fname ;
        base_fd = -1 ;
        overlay_fd = -1 ;
        base_size_bytes = 0 ;
        image_size_bytes = 0 ;
        block_count = 0 ;
        opened = false ;
    }

    virtual ~storageimage_cow_c() override {
        close() ;
    }

    virtual bool is_readonly() override {
        return false ; // writes always land in the overlay
    }
    virtual bool open(storagedrive_c *drive, bool create) override;
    virtual bool is_open(void) override;
    virtual bool truncate(void) override; // resets the disk to the pristine base
    virtual void read(uint8_t *buffer, uint64_t position, unsigned len) override;
    virtual void write(uint8_t *buffer, uint64_t position, unsigned len) override;
    virtual uint64_t size(void) override;
    virtual void close(void) override;
    virtual void get_bytes(byte_buffer_c *byte_buffer, uint64_t byte_offset, uint32_t data_size) override;
    virtual void set_bytes(byte_buffer_c *byte_buffer, uint64_t byte_offset) override ;
    virtual void save_to_file(std::string host_filename) override ; // flattened export

    // overlay management, driven by the images API
    void discard() ;             // throw the overlay away, back to pristine base
    bool commit() ;              // fold the overlay into the base, then discard()
    void export_to(std::string host_filename) { save_to_file(host_filename) ; }

    // status for the API
    bool has_overlay(void) const { return opened ; }
    uint64_t dirty_block_count(void) const ; // occupied (written) blocks
    uint64_t overlay_allocated_bytes(void) const ; // overlay's on-disk footprint
} ;

// in-memory version of disk image file
class storageimage_memory_c: public storageimage_base_c {
private:
    bool readonly ;
    std::fstream f; // image file
    // std::string image_fname ;
    uint8_t 	*data ; // the disk image content
    uint64_t	data_size ;
    bool opened ; // between open() and close()

public:
    storageimage_memory_c(unsigned _capacity) {
        opened = false;
        data = nullptr ;
        data_size = _capacity ;
    }

    // nothing to free
    virtual ~storageimage_memory_c() override {
        close() ;
    }

    virtual bool is_readonly() override {
        return readonly ;
    }
    virtual bool open(storagedrive_c *drive, bool create) override;
    virtual bool is_open(	void) override;
    virtual bool truncate(void) override;
    virtual void read(uint8_t *buffer, uint64_t position, unsigned len) override;
    virtual void write(uint8_t *buffer, uint64_t position, unsigned len) override;
    virtual uint64_t size(void) override;
    virtual void close(void) override;
    virtual void get_bytes(byte_buffer_c *byte_buffer, uint64_t byte_offset, uint32_t data_size) override;
    virtual void set_bytes(byte_buffer_c *byte_buffer, uint64_t byte_offset) override ;
    bool load_from_file(std::string _host_filename,  	 bool allowcreate, bool *result_file_created);
    void save_to_file(std::string _host_filename) override ;


} ;

#endif

