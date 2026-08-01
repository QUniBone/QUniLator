/* rx0102drive.cpp: implementation of RX01/RX02 disk drive, attached to RX0102 uCPU

 Copyright (c) 2020, Joerg Hoppe
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


 5-jan-2020	JH      start

 The microCPU board contains all logic and state for the RX01/02 subsystem.
 It is connected on one side two to "dump" electro-mechanical drives,
 on the other side two a RX11/RXV11/RX211/RXV21 UNIBUS/QBUS interface.
 */

#include <assert.h>

#include <array>

#include "logger.hpp"
#include "timeout.hpp"
#include "utils.hpp"
#include "rx0102ucpu.hpp"
#include "rx0102drive.hpp"

RX0102drive_c::RX0102drive_c(RX0102uCPU_c *_uCPU, bool _is_RX02) :
    // no controller assigned, command by box uCPU
    storagedrive_c(nullptr)
{
    geometry.cylinder_count = cylinder_count_const ;
	geometry.head_count = 1 ;
    geometry.sector_count = sector_count_const ;
	// sector_size_bytes is 128/256 depending on density

    uCPU = _uCPU ; // link to micro CPU board
    is_RX02 = _is_RX02 ;
    if (!is_RX02) {
        log_label = "RXDRV"; // to be overwritten by RX11 on create
        type_name.set("RX01") ;
        set_density(false) ;
        density_name.readonly = true ;
    } else {
        log_label = "RYDRV"; // to be overwritten by RX11 on create
        type_name.set("RX02") ;
        set_density(true) ; // default: start as 512k MFM
        density_name.readonly = false ;
    }
	assert(drive_type_c::is_RX(drive_type)) ; // set_density() OK?

    // some rxdp floppy images start at track #1 instead of #0
    imagetrack0.value = true ;

    // the layout is read off the medium as it is attached
    layout.value = "auto" ;
    layout_in_use.value = "physical" ;
    layout_in_use.kind = parameter_c::PARAM_STATUS ;

    // "enable" and power switch are controlled by uCPU in case box
    enabled.readonly = true ;

    write_protect_lamp.kind = parameter_c::PARAM_STATUS ;
}

// Runs on the lamp poll: age the ACCESS lamp (base class) and track the WRITE
// PROTECT lamp against the mounted image's read-only state.
void RX0102drive_c::refresh_activity(void)
{
    storagedrive_c::refresh_activity() ;
    write_protect_lamp.value = image_is_readonly() ;
}

// return false, if illegal parameter value.
// verify "new_value", must output error messages
//
// density control &conversion:
// SD/DD can be set when image loaded, reinterprets image. User must know!
// over-long images may be generated
// load of image: SD/DD determined from file size
bool RX0102drive_c::on_param_changed(parameter_c *param) 
{
    if (param == &density_name) {
        // toupper() ... really!
        std::transform(density_name.new_value.begin(), density_name.new_value.end(), density_name.new_value.begin(), ::toupper);
        if (!strcasecmp(density_name.new_value.c_str(), "SD"))
            set_density(false);
        else if (!strcasecmp(density_name.new_value.c_str(), "DD"))
            set_density(true);
        else {
            // throw bad_parameter_check("drive type must be RX01 or RX02") ;
            ERROR("drive double_density SD or DD");
            return false;
        }
        resolve_layout(layout.value) ; // the sector size moved: the probe reads elsewhere
    } else if (param == &layout) {
        std::string wanted = layout.new_value ;
        std::transform(wanted.begin(), wanted.end(), wanted.begin(), ::tolower);
        if (wanted != "physical" && wanted != "logical" && wanted != "auto") {
            ERROR("drive layout must be physical, logical or auto");
            return false;
        }
        layout.new_value = wanted ;
        resolve_layout(wanted) ;
    } else if (image_is_param(param)
               && image_recreate_on_param_change(param) ) {
        // change of file image changes state, results also in close()
        // assume new "floppy" has no delete marks set
        memset(deleted_data_marks, 0, sizeof(deleted_data_marks)) ;

        if (imagetrack0.value)  // image contains unused track 0
        	geometry.filesystem_offset = geometry.sector_count * geometry.sector_size_bytes ; // disk data, boot loader at track #1
        else
            geometry.filesystem_offset = 0 ; // disk data, boot loader at image start
		
        image_open(true) ; // may fail
        // file size determines double_density
        if (is_RX02 && image_is_open()) {
            // RX02: user can set density only if not file loaded
            // if file: double_density set by file_size
            // RX02DD ?
            unsigned single_density_image_size = 128 * geometry.cylinder_count * geometry.sector_count ; // 256.256
            if (image_size() > single_density_image_size)
                set_density(true) ;
            else set_density(false) ;
        }
        resolve_layout(layout.value) ;
    }
    return storagedrive_c::on_param_changed(param); // more actions (for enable)
}

void RX0102drive_c::set_density(bool _double_density) 
{
    double_density = _double_density;
    if (!_double_density) {
        // RX01, RX02 FM encoding
        geometry.sector_size_bytes = 128; // in byte
        density_name.value = "SD"; // no "on change" callbacks
        drive_type = drive_type_e::RX01 ;
    } else {
        // RX02 MFM encoding = Double Density floppy
        geometry.sector_size_bytes = 256; // in byte
        density_name.value = "DD"; // no "on change" callbacks
        drive_type = drive_type_e::RX02 ;
    }
	capacity.value = geometry.get_raw_capacity() ;

//    uCPU->on_drive_state_changed(this) ;
}


// uCPU may query the "ready/ door open state"
// true: file image loaded: "door closed" + "floppy inserted"
bool RX0102drive_c::check_ready(void) 
{
    return image_is_open() ;
}


void RX0102drive_c::on_power_changed(signal_edge_enum aclo_edge, signal_edge_enum dclo_edge) 
{
    UNUSED(dclo_edge) ;
    UNUSED(aclo_edge) ;
    uCPU->on_drive_state_changed(this) ; // not needed, uCPU controls power
}

// if seeking or ontrack: retrack head to 0
void RX0102drive_c::on_init_changed(void) 
{
}

unsigned RX0102drive_c::get_rotation_ms() 
{
    return (1000*60) / full_rpm ; // time for index hole to pass
}

unsigned RX0102drive_c::get_cylinder() {
    return cylinder ;
}

void RX0102drive_c::set_cylinder(unsigned cyl) 
{
    cylinder = cyl ;
    current_track.set(cyl) ;
}


bool RX0102drive_c::check_disk_address(unsigned track, unsigned sector) 
{
    error_illegal_track = (track >= geometry.cylinder_count) ;
    error_illegal_sector = (sector < 1 || sector > geometry.sector_count) ;
    return !error_illegal_track && !error_illegal_sector ;
}

// sector offset in image file in bytes
int RX0102drive_c::get_sector_image_offset(unsigned track, unsigned sector)
{
    return ((track * geometry.sector_count) + (sector-1)) * geometry.sector_size_bytes ;
}

// A handler reaches a volume's blocks through a 2:1 sector interleave with six
// sectors of skew per track, so a block is two (RX02) or four (RX01) sectors
// spread across a track. These two functions are that mapping and its inverse.
// Tracks are numbered from 1, as the skew counts from the first data track;
// sectors are numbered from 1 and the index runs 0..25 over a track's blocks.

// The sector of `track` carrying the track's `index`-th logical sector.
static unsigned interleaved_sector(unsigned track, unsigned index)
{
    const unsigned n = RX0102drive_c::sector_count_const ;
    unsigned sector = (2 * index) % n + (index >= 13 ? 1 : 0) ;
    return (sector + 6 * (track - 1)) % n + 1 ;
}

// The place in `track`'s logical order that `sector` holds.
static unsigned interleave_index(unsigned track, unsigned sector)
{
    const unsigned n = RX0102drive_c::sector_count_const ;
    unsigned skewed = (sector - 1 + n - (6 * (track - 1)) % n) % n ;
    return (skewed & 1) ? (skewed - 1) / 2 + 13 : skewed / 2 ;
}

// Where the sector the controller names lands in the image file, and -1 for a
// sector the file does not carry.
//
// A logical image holds the volume's blocks in order from block 0, so it starts
// at track 1 and the sector is found by undoing the interleave; track 0 lies
// outside the block space. A physical image holds the surface as written, and
// `imagetrack0` says whether it opens at track 0 or at track 1.
int RX0102drive_c::sector_file_offset(unsigned track, unsigned sector, bool logical)
{
    if (logical) {
        if (track == 0)
            return -1 ;
        return get_sector_image_offset(track - 1, interleave_index(track, sector) + 1) ;
    }
    if (!imagetrack0.value) {
        if (track == 0)
            return -1 ;
        track-- ;
    }
    return get_sector_image_offset(track, sector) ;
}

// Block 1 of the volume, gathered out of the sectors the given layout puts it
// in. Reads past the end of a short image come back as 00s.
void RX0102drive_c::read_home_block(uint8_t *block, bool logical)
{
    unsigned sector_size = geometry.sector_size_bytes ;
    unsigned sectors_per_block = 512 / sector_size ;
    memset(block, 0, 512) ;
    for (unsigned i = 0 ; i < sectors_per_block ; i++) {
        unsigned logical_sector = sectors_per_block + i ; // block 1 follows block 0
        unsigned track = logical_sector / geometry.sector_count + 1 ;
        unsigned index = logical_sector % geometry.sector_count ;
        int offset = sector_file_offset(track, interleaved_sector(track, index), logical) ;
        if (offset < 0)
            return ;
        image_read(block + i * sector_size, (unsigned) offset, sector_size) ;
    }
}

// The RT-11 home block, which an RT-11 volume carries as block 1: the system
// identification "DECRT11A" at offset 0760, a printable volume identification at
// 0730 and the block number of the first directory segment at 0724. The checksum
// word is stale on the RT-11 V5.1 distribution media, so the structure
// identifies the block.
bool RX0102drive_c::is_rt11_home_block(const uint8_t *block)
{
    if (memcmp(block + 0760, "DECRT11A", 8) != 0)
        return false ;
    unsigned first_directory_block = block[0724] | (block[0725] << 8) ;
    if (first_directory_block < 1
            || first_directory_block > geometry.get_raw_capacity() / 512)
        return false ;
    for (unsigned i = 0 ; i < 12 ; i++) {
        uint8_t c = block[0730 + i] ;
        if (c < 0x20 || c > 0x7e)
            return false ;
    }
    return true ;
}

// Settle the layout the drive serves the mounted medium in. An operator's choice
// of "physical" or "logical" stands; "auto" reads the image and takes the layout
// the RT-11 home block turns up in. Media the probe cannot place - a volume in
// another file system, a blank scratch image - is physical, which is the form a
// newly created image takes.
void RX0102drive_c::resolve_layout(const std::string &choice)
{
    bool logical = (choice == "logical") ;
    if (choice == "auto") {
        uint8_t block[512] ;
        if (image_is_open()) {
            read_home_block(block, /*logical*/true) ;
            logical = is_rt11_home_block(block) ;
            if (!logical) {
                read_home_block(block, /*logical*/false) ;
                if (is_rt11_home_block(block))
                    INFO("RT-11 home block found in physical sector order") ;
                else
                    INFO("no RT-11 home block: the image is taken as physical") ;
            } else
                INFO("RT-11 home block found in logical block order") ;
        }
    }
    logical_layout = logical ;
    layout_in_use.value = logical ? "logical" : "physical" ;
}

// false: error
bool RX0102drive_c::sector_read(uint8_t *sector_buffer, bool *deleted_data_mark, unsigned track, unsigned sector, bool with_delay) 
{
    if (!check_disk_address(track,sector))
        return false ;
    if (!check_ready())
        return false ; // no floppy image

    // wait for 1 sector to pass, else ZRXB failures
    unsigned sector_us = 1000 * get_rotation_ms() / geometry.sector_count ; // about 6.5 ms
    if (with_delay)
        timeout_c().wait_us(sector_us / emulation_speed.value) ;

    *deleted_data_mark = deleted_data_marks[track][sector] ;
    DEBUG_FAST("sector_read(): delmark=%d, track=%d, sector=%d", (unsigned)*deleted_data_mark, (unsigned)track, (unsigned)sector) ;

    int offset = sector_file_offset(track, sector, logical_layout) ;
    if (offset < 0) {
        // a sector the image does not carry reads back blank
        memset(sector_buffer, 0, geometry.sector_size_bytes);
        return true ;
    }
    DEBUG_FAST("sector_read(): reading 0x%03x bytes from file offset 0x%06x", (unsigned) geometry.sector_size_bytes, (unsigned) offset);
    image_read(sector_buffer, (unsigned) offset, geometry.sector_size_bytes) ;
    // logger->debug_hexdump(this, "image_read():", (uint8_t *) sector_buffer, sector_size_bytes, NULL);
    return true ;
}

// false: error
bool RX0102drive_c::sector_write(uint8_t *sector_buffer, bool deleted_data_mark, unsigned track, unsigned sector, bool with_delay) 
{
    if (!check_disk_address(track,sector))
        return false ;
    if (!check_ready())
        return false ; // no floppy image

    if (image_is_readonly()) {
        // No means to detect write-protected floppies?
        WARNING("Write access to readonly floppy image file ignored") ;
        return true ;
    }

    // wait for 1 sector to pass, else ZRXB failures
    unsigned rotation_ms =  (1000*60) / full_rpm ; // time for index hole to pass
    unsigned sector_us = 1000 * rotation_ms / geometry.sector_count ; // about 6.5 ms
    if (with_delay)
        timeout_c().wait_us(sector_us / emulation_speed.value) ;

    deleted_data_marks[track][sector] = deleted_data_mark ;

    DEBUG_FAST("sector_write(): delmark=%d, track=%d, sector=%d", (unsigned)deleted_data_mark, (unsigned)track, (unsigned)sector) ;

    int offset = sector_file_offset(track, sector, logical_layout) ;
    if (offset < 0)
        return true ; // a sector the image does not carry takes no data
    DEBUG_FAST("sector_write(): writing 0x%03x bytes to file offset 0x%06x", (unsigned) geometry.sector_size_bytes, (unsigned) offset);
    image_write(sector_buffer, (unsigned) offset, geometry.sector_size_bytes) ;
    // logger->debug_hexdump(this, "image_write():", (uint8_t *) sector_buffer, sector_size_bytes, NULL);
    return true ;
}


// no thread, just passive mechnical device
void RX0102drive_c::worker(unsigned instance) 
{
    UNUSED(instance); // only one
}

