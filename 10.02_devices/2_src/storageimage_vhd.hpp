/* storageimage_vhd.hpp: a Virtual Hard Disk (VHD) file as storage medium

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see storageimage.hpp for the full text.

   SIMH and other emulators keep disks as VHD files, in the format Microsoft
   published for Virtual PC. A drive holding one reads and writes it in place:
   the file stays a VHD, so it goes back to the emulator it came from as it is.

   Two of the format's disk types are served:
   - fixed (2): the disk's bytes, front to back, followed by a 512-byte footer
   - dynamic (3): a copy of the footer, a dynamic disk header, a block
     allocation table and the blocks written so far, each a sector bitmap and
     the block's data, then the footer. A block never written reads as zeros.
   A differencing VHD (4) holds only the changes to a parent disk and is
   refused.

   A VHD is written in place and never through the copy-on-write overlay of
   storageimage_cow_c: the overlay sits beside a raw base and reads it as the
   disk's bytes, which a VHD's file is not.
*/

#ifndef _STORAGEIMAGE_VHD_HPP_
#define _STORAGEIMAGE_VHD_HPP_

#include <stdint.h>
#include <string>
#include <vector>

#include "storageimage.hpp"

class storageimage_vhd_c: public storageimage_base_c {
public:
    static const unsigned SECTOR = 512 ;

    // The parts of a VHD's file that say where the disk's bytes are, read by
    // parse() and kept for as long as the file is open.
    struct layout_t {
        uint8_t footer[SECTOR] ;    // as found, written back unchanged when it moves
        uint32_t type = 0 ;         // 2 fixed, 3 dynamic
        uint64_t disk_size = 0 ;    // the footer's current size: the disk's size
        uint64_t footer_at = 0 ;    // where the closing footer sits in the file
        bool footer_at_end = true ; // false: only the dynamic disk's leading copy is valid
        // dynamic disks only
        uint64_t header_at = 0 ;
        uint64_t table_at = 0 ;
        uint32_t block_size = 0 ;
        uint32_t bitmap_bytes = 0 ; // a block's sector bitmap, in whole sectors
        std::vector<uint32_t> bat ; // per block: its first sector in the file, or BAT_UNUSED
    } ;
    static const uint32_t BAT_UNUSED = 0xFFFFFFFFu ;

private:
    std::string image_fname ;
    int fd ;
    bool readonly ;
    // The medium's write protection, read from the file's mode as the medium
    // goes in; see storageimage_binfile_c for why it is taken only once.
    bool write_protected ;
    layout_t vhd ;
    // Sector bitmaps of the dynamic disk's allocated blocks, read on first use.
    std::vector<std::vector<uint8_t> > bitmaps ;
    bool beyond_end_logged ;

    const std::vector<uint8_t> &bitmap(uint32_t block) ;
    bool sector_present(uint32_t block, uint32_t sector) ;
    bool read_in_block(uint32_t block, uint32_t offset, uint8_t *buffer, uint32_t len) ;
    bool write_in_block(uint32_t block, uint32_t offset, const uint8_t *buffer, uint32_t len) ;
    bool allocate_block(uint32_t block, uint32_t offset, const uint8_t *buffer, uint32_t len) ;

public:
    storageimage_vhd_c(std::string _image_fname) ;
    virtual ~storageimage_vhd_c() override ;

    virtual bool is_readonly() override {
        if (is_open())
            return readonly ;
        return write_protected ;
    }
    virtual bool open(storagedrive_c *drive, bool create) override ;
    virtual bool is_open(void) override ;
    virtual bool truncate(void) override ; // every block reads as zeros again; the size stays
    virtual void read(uint8_t *buffer, uint64_t position, unsigned len) override ;
    virtual void write(uint8_t *buffer, uint64_t position, unsigned len) override ;
    virtual uint64_t size(void) override ; // the disk's size, not the file's
    virtual void close(void) override ;
    virtual void get_bytes(byte_buffer_c *byte_buffer, uint64_t byte_offset, uint32_t data_size) override ;
    virtual void set_bytes(byte_buffer_c *byte_buffer, uint64_t byte_offset) override ;
    virtual void save_to_file(std::string host_filename) override ; // a copy of the VHD file

    // Read the layout of the VHD open on fd. false with *err saying why, for a
    // file that is no VHD, a differencing one, or one too damaged to serve.
    static bool parse(int fd, layout_t *layout, std::string *err) ;

    // Whether the file at path is a VHD: a footer with the "conectix" cookie
    // and a valid checksum closes it, or a dynamic disk's leading copy opens
    // it. *disk_size, when given, is set to the disk's size.
    static bool is_vhd_file(const std::string &path, uint64_t *disk_size = nullptr) ;

    // Whether a drive given `fname` as its medium takes it as a VHD. The file's
    // content decides, the way SIMH recognises a VHD whatever its name: a VHD
    // arrives as .vhd, .dsk or .rl02 alike, and a raw image cannot end in a
    // footer whose checksum holds by accident. A name with no file behind it
    // yet - only its ".gz", which open() expands - is taken by its ".vhd".
    static bool takes(const std::string &fname) ;

    // Write a new dynamic VHD of disk_size bytes, a whole number of sectors,
    // with no block allocated. The file must not exist.
    static bool create_dynamic(const std::string &path, uint64_t disk_size, std::string *err) ;
} ;

#endif
