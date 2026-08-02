/* rx0102drive.hpp: implementation of RX01/RX02 disk drive, attached to RX0102 uCPU

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

 10-jan-2020  JH      start

 The microCPU board contains all logic and state for the RX01/02 subsystem.
 It is connected on one side two to "dump" electro-mechanical drives,
 on the other side two a RX11/RXV11/RX211/RXV21 UNIBUS/QBUS interface.
 */
#ifndef _RX0102DRIVE_HPP_
#define _RX0102DRIVE_HPP_

#include <stdint.h>
#include <string.h>
#include <atomic>
#include <vector>

#include "storagedrive.hpp"


class RX0102uCPU_c;
class RX0102drive_c: public storagedrive_c {
public:
    bool removable(void) const override { return true; }
private:

    volatile unsigned cylinder; // current head position

public:
	static const unsigned cylinder_count_const = 77 ; // for array declaration 
	static const unsigned sector_count_const = 26 ;
	
	
    // the RX11 controller may see everything
    // dynamic state
    bool is_RX02 ; // false: RX01, true: FM/MFM RX02 drive
    bool double_density ; // true = RX02 and MFM

    unsigned full_rpm = 360; // normal rotation speed
    // track-to-track time is 5ms, head settle is 25ms
    unsigned track_step_time_ms = 5 ;
    // disk is always spinning
    unsigned head_settle_time_ms = 25 ;

    unsigned get_cylinder() ;
    void set_cylinder(unsigned cyl) ;

    bool error_illegal_track  ;
    bool error_illegal_sector ;

    parameter_string_c density_name = parameter_string_c(this, "density", "d",/*readonly*/
                                      false, "SD for RX01 & RX02 FM; DD for RX02 MFM");

    parameter_bool_c imagetrack0 = parameter_bool_c(this, "imagetrack0", "it0",/*readonly*/
                                   false, "true: File image contains track 0-76 (std), else only 1..76");

    // The order the image file holds the medium's sectors in. "physical" is the
    // surface as it was written, track by track with sector 1..26 in place.
    // "logical" is the volume's blocks in order from block 0, the form an
    // archived RT-11 distribution comes in; the drive maps every access through
    // the 2:1 interleave with six sectors of skew per track that a handler
    // addresses a volume through. "auto" reads the image when it is attached and
    // takes the layout the RT-11 home block turns up in.
    parameter_string_c layout = parameter_string_c(this, "layout", "lay", /*readonly*/
                                false, "Sector order in the image file: physical, logical or auto");

    // The layout in force, which is what "auto" settled on. Named apart from
    // "layout" so a configuration keeps the operator's choice and the dashboard
    // still shows what the drive is doing with the medium it holds.
    parameter_string_c layout_in_use = parameter_string_c(this, "layoutinuse", "lyu", /*readonly*/
                                       true, "Sector order the drive serves the mounted image in");

    // current head position , info only
    parameter_unsigned_c current_track = parameter_unsigned_c(this, "track", "tr", /*readonly*/
                                         true, "", "%d", "Track # of current head position", 77, 10);

    // WRITE PROTECT lamp for the dashboard, lit when the mounted image is
    // read-only — the write-protect state the drive enforces, ignoring writes to
    // a read-only floppy. Named "...lamp" so the webevents lamp poll picks it up.
    parameter_bool_c write_protect_lamp = parameter_bool_c(this, "writeprotectlamp", "wpl",
                                          /*readonly*/true, "State of WRITE PROTECT lamp");

private:

    // IBM floppy format allows a "delete mark" for each sector.
    // DEC : "Delete data mark is not used during normal operation,
    // but the RX01 can identify and write deleted data marks under program control,
    // The deleted data mark is only included in the RX11 to be IBM compatible."
    //
    // These sector marks are persistent on the floppy disk, but not saved in the
    // SimH-compatible image file format.
    // To pass the ZRX* diags, sector marks are held volatile "per drive".
    bool deleted_data_marks[cylinder_count_const][sector_count_const] ;

    // the layout in force, read by the uCPU worker on every sector access
    std::atomic<bool> logical_layout{false} ;

    bool check_disk_address(unsigned track, unsigned sector) ;

    // Where a sector of the medium lands in the image file, or -1 for a sector
    // the file does not carry.
    int sector_file_offset(unsigned track, unsigned sector, bool logical) ;

    // Read the volume's block 1 - the RT-11 home block, when the medium holds an
    // RT-11 volume - as the given layout places it in the file.
    void read_home_block(uint8_t *block, bool logical) ;
    bool is_rt11_home_block(const uint8_t *block) ;

    void resolve_layout(const std::string &choice) ;

public:
    RX0102uCPU_c *uCPU ; // link to micro CPU board

    // no user controls!

    RX0102drive_c(RX0102uCPU_c *uCPU, bool is_RX02);

    bool on_param_changed(parameter_c *param) override;

    void on_power_changed(signal_edge_enum aclo_edge, signal_edge_enum dclo_edge) override;
    void on_init_changed(void) override;

    // ages the ACCESS lamp (base class) and tracks the WRITE PROTECT lamp
    void refresh_activity(void) override;

    unsigned get_rotation_ms() ;

    // door closed, flopyp inserted?
    bool check_ready(void) ;

    void set_density(bool double_density);

    int get_sector_image_offset(unsigned track, unsigned sector) ;
    bool sector_read(uint8_t *sector_buffer, bool *deleted_data_mark, unsigned track, unsigned sector, bool with_delay) ;
    bool sector_write(uint8_t *sector_buffer, bool deleted_data_mark, unsigned track, unsigned sector, bool with_delay) ;


    // background worker function
    void worker(unsigned instance) override;
};

#endif
