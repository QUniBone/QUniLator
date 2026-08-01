/* rl02.cpp: implementation of RL01/RL02 disk drive, attached to RL11 controller

 Copyright (c) 2018, Joerg Hoppe
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


 12-nov-2018  JH      entered beta phase
 */

#include <assert.h>

#include "logger.hpp"
#include "timeout.hpp"
#include "utils.hpp"
#include "rl11.hpp"
#include "rl0102.hpp"

RL0102_c::RL0102_c(storagecontroller_c *_controller) :    storagedrive_c(_controller) 
{
    log_label = "RL0102"; // to be overwritten by RL11 on create
    status_word = 0;
    seek_settle_pending = false;
    set_type(drive_type_e::RL02); // default
    runstop_button.value = false; // force user to load file assume drive is LOAD
    fault_lamp.value = false;
    cover_open.value = false;
    spinup_delay.value = false; // ready at once unless the delay is asked for

    // running state the drive drives on its own — the spindle speed, the
    // internal state machine and the front-panel lamps — is not configuration.
    rotation_umin.kind = parameter_c::PARAM_STATUS;
    state.kind = parameter_c::PARAM_STATUS;
    load_lamp.kind = parameter_c::PARAM_STATUS;
    ready_lamp.kind = parameter_c::PARAM_STATUS;
    fault_lamp.kind = parameter_c::PARAM_STATUS;
    writeprotect_lamp.kind = parameter_c::PARAM_STATUS;
}

// return false, if illegal parameter value.
// verify "new_value", must output error messages
bool RL0102_c::on_param_changed(parameter_c *param) 
{
    if (param == &enabled) {
        if (!enabled.new_value) {
            // disable switches power OFF and releases the door lock at once. The
            // unlock normally happens in state_power_off() on the worker thread,
            // but a disabled drive's worker is not running, so unlock here: a
            // powered-off drive holds no medium, and a configuration apply must be
            // able to load a new image before the drive is enabled again.
            power_switch.value = false;
            image_params_readonly(false);
            change_state(RL0102_STATE_power_off);
        } else
            // A drive is powered with the box it sits in: enabling it closes
            // the POWER switch, so it reaches "load cartridge" and the LOAD
            // button spins it up. Powered off, the worker parks the drive on
            // every pass and LOAD has no effect.
            power_switch.value = true;
    } else if (param == &type_name) {
        if (!strcasecmp(type_name.new_value.c_str(), "RL01"))
            set_type(drive_type_e::RL01);
        else if (!strcasecmp(type_name.new_value.c_str(), "RL02"))
            set_type(drive_type_e::RL02);
        else {
//		throw bad_parameter_check("drive type must be RL01 or RL02") ;
            ERROR("drive type must be RL01 or RL02");
            return false;
        }
    } else if (image_is_param(param)
               && image_recreate_on_param_change(param) ) {
        // A drive with a cartridge comes up loaded: attaching an image presses
        // the LOAD button, so it spins up to READY without a manual runstop
        // toggle. An empty image (detach) leaves it in LOAD.
        //
        // A drive already spinning takes the change too. A real one would have
        // to bring the pack to rest before the cartridge came out, so the drive
        // does that as part of the change: LOAD is released here, and the
        // "load cartridge" state it settles into presses it again for the new
        // pack. The medium is what the operator names; the mechanics of getting
        // to it are the drive's business.
        if (param == &image_filepath) {
            bool spinning = state.value != RL0102_STATE_power_off
                            && state.value != RL0102_STATE_load_cartridge;
            bool loading = !image_filepath.new_value.empty();
            media_change_pending = spinning && loading;
            runstop_button.value = loading && !spinning;
        }
        return true ; // param accepted
    }

    // more actions (enable)
    return storagedrive_c::on_param_changed(param); // more actions (for enable)
}

void RL0102_c::set_type(enum drive_type_e _drivetype) 
{
	assert(drive_type_c::is_RL(_drivetype)) ;
    drive_type = _drivetype;
    switch (drive_type) {
    case drive_type_e::RL01:
        geometry.cylinder_count = 256;
        geometry.head_count = 2;
        geometry.sector_count = 40;
        type_name.value = "RL01";
        break;
    case drive_type_e::RL02:
        geometry.cylinder_count = 512;
        geometry.head_count = 2;
        geometry.sector_count = 40;
        type_name.value = "RL02";
        break;
	default: FATAL("illegal drive_type") ;
    }
    geometry.sector_size_bytes = 256; // in byte
    
    capacity.value = geometry.get_raw_capacity() ;

	// last track
	geometry.bad_sector_file_offset = (geometry.head_count*geometry.cylinder_count-1) * geometry.sector_count * geometry.sector_size_bytes ;
}

/* CRC16 as implemented by the DEC 9401 chip
 * simh/PDP11\pdp11_rl.c
 */
uint16_t RL0102_c::calc_crc(const int wc, const uint16_t *data) 
{
    uint32_t crc, j, d;
    int32_t i;

    crc = 0;
    for (i = 0; i < wc; i++) {
        d = *data++;
        /* cribbed from KG11-A */
        for (j = 0; j < 16; j++) {
            crc = (crc & ~01) | ((crc & 01) ^ (d & 01));
            crc = (crc & 01) ? (crc >> 1) ^ 0120001 : crc >> 1;
            d >>= 1;
        }
    }
    return (uint16_t) crc;
}

// after QBUS/UNIBUS install, device is reset by DCLO/DCOK cycle
void RL0102_c::on_power_changed(signal_edge_enum aclo_edge, signal_edge_enum dclo_edge) 
{
    UNUSED(aclo_edge) ;
    // called at high priority.
    // mutex?
    if (dclo_edge == SIGNAL_EDGE_RAISING) {
        // FAULT lamp, while RL11 evals DC_LO
        // power-on defaults
        change_state( RL0102_STATE_power_off);
        update_status_word(drive_ready_line, true); // RL11 starts with error
    }
}

// if seeking or ontrack: retrack head to 0
void RL0102_c::on_init_changed(void) 
{
    // called at high priority.
    // mutex?

    if (init_asserted) {
        // seems not to retract head on INIT
        /*
         if (state.value == RL0102_STATE_seek || state.value == RL0102_STATE_lock_on) {
         seek_destination_cylinder = 0;
         seek_destination_head = 0;
         change_state( RL0102_STATE_seek) ;
         }
         */
        // clear_error_register(); !NO! voume check remains
    }
}

// seek is only possible if ready
bool RL0102_c::cmd_seek(unsigned destination_cylinder, unsigned destination_head) 
{
    assert(destination_cylinder < geometry.cylinder_count); // RL11 must calc correct #

    if (state.value != RL0102_STATE_lock_on) {
//	if (state.value != RL0102_STATE_seek && state.value != RL0102_STATE_lock_on) {
        WARNING("Drive seek to cyl.head=%d.%d failed, wrong state %d!", destination_cylinder,
                destination_head, state.value);
        return false; // illegal state
    }

    DEBUG_FAST("Drive start seek from cyl.head %d.%d to %d.%d", cylinder, head, destination_cylinder,
          destination_head);

    this->seek_destination_cylinder = destination_cylinder;
    this->seek_destination_head = destination_head;

    // EK-RL012-TM-PRE §4.2.1: any seek command drops Drive Ready immediately for
    // the 6.5 ms positioner-settle timeout. RL11 must see READY=false at once.
    seek_settle_pending = true;
    update_status_word(/*drive_ready_line*/false, drive_error_line);

    if (destination_cylinder == cylinder) {
        // Zero track difference. EK-RL012-TM-PRE §4.2: the State Control Logic
        // leaves Velocity (Seek) Mode for Position Mode only when Track Count
        // reaches 0; with a zero difference the positioner never leaves Position
        // Mode, so the drive stays in Lock On (state 5) and does not report Seek
        // (state 4). Head selection is electronic within Position Mode. The drive
        // holds Lock On and re-settles in place - state_lock_on() times the 6.5 ms
        // Drive-Ready drop and only the selected head changes.
        head = destination_head;
        return true;
    }

    // Nonzero track difference: enter Velocity (Seek) Mode. state_seek() applies
    // the head-switch settle only when the selected head actually changes, then
    // re-enters Lock On where the 6.5 ms settle completes before Drive Ready.
    change_state(RL0102_STATE_seek);
    return true;
}

// separate proc, to have a testpoint
void RL0102_c::change_state(unsigned new_state) 
{
    unsigned old_state = state.value;
    uint16_t old_status_word = status_word;
    // TODO: only in update_status_word() the visible state of state of ready line and error line should be set.!
    //renome "update_status_word()" to "update_visible_status()"
    state.value = new_state;
    update_status_word(); // contains state
    if (old_state != new_state)
        DEBUG_FAST("Change drive %s state from %d to %d. Status word %06o -> %06o.",
              name.value.c_str(), old_state, state.value, old_status_word, status_word);
}

/*** state functions, called repeatedly ***/
void RL0102_c::state_power_off() 
{
    // drive_ready_line = false; // verified
    // drive_error_line = true; // real RL02: RL11 show a DRIVE ERROR after power on / DC_LO
    type_name.readonly = false; // may be changed between RL01/RL02
    volume_check = true; // starts with volume check?
    cover_open.readonly = true;
    update_status_word(/*drive_ready_line*/false, /*drive_error_line*/true);
    ready_lamp.value = false;
    load_lamp.value = false;
    fault_lamp.value = false;
    writeprotect_lamp.value = false;
//	image_filepath.readonly = true ; // "door locked", disk can not be changed
    image_params_readonly(false) ;// don't be so complicated
    if (power_switch.value == true)
        change_state(RL0102_STATE_load_cartridge);
    state_timeout.wait_ms(100);

}

// drive stop, door unlocked, cartridge can be loaded
void RL0102_c::state_load_cartridge() 
{
    // drive_ready_line = false; // verified
    type_name.readonly = true; // must be powered of to changed between RL01/RL02
    update_status_word(/*drive_ready_line*/false, drive_error_line);
    load_lamp.value = 1;
    ready_lamp.value = 0;
    writeprotect_lamp.value = writeprotect_button.value;
    cover_open.readonly = false; // can be changed ("opened") only in LOAD state
    // The pack is at rest and the new medium is in: press LOAD for it, the way
    // the drive would have been loaded by hand.
    if (media_change_pending) {
        media_change_pending = false;
        runstop_button.value = true;
    }
    // Path to FAULT: try to load illegal file
    // Path out of FAULT: File OK, or RUN runstop_button released
    // FAULT => state is "load cartridge"
    if (runstop_button.value == true && cover_open.value == false) {
        // LOAD released: start spinning, if file image OK
        // this test is repeated endlessly if button pressed and filename illegal
        if (image_open(/*create*/true)) {
            fault_lamp.value = false;
            change_state(RL0102_STATE_spin_up);
            return; // no wait
        } else {
            if (!fault_lamp.value) // error message only once
                ERROR("Could not open/create file \"%s\".", image_filepath.value.c_str());
            fault_lamp.value = true; // disables effect of runstop button
        }
    } else {
        // load cartridge: unlock file
        fault_lamp.value = false;
        if (image_is_open())
            image_close();
    }
    state_timeout.wait_ms(100);
}

void RL0102_c::state_spin_up() 
{
    unsigned calcperiod_ms = 100;
    // change of rpm in 0.1 secs
    unsigned rpm_increment = full_rpm / (time_spinup_sec * (1000 / calcperiod_ms));

    volume_check = true; // SIMH RLDS_VCK
    cover_open.readonly = true; // can be changed ("opened") only in LOAD state

    // drive_ready_line = false; // verified
    update_status_word(/*drive_ready_line*/false, drive_error_line);

    if (runstop_button.value == false || fault_lamp.value == true) { // stop spinning
        change_state(RL0102_STATE_spin_down);
        return;
    }

    if (!spinup_delay.value || config_apply_immediate) { // skip the modeled spin-up
        rotation_umin.value = full_rpm;
        cylinder = 0;
        change_state(RL0102_STATE_brush_cycle);
        return;
    }

    rpm_increment *= emulation_speed.value;
    INFO("Spin up drive speed = %d", rotation_umin.value);

    rotation_umin.value += rpm_increment;
    if (rotation_umin.value > full_rpm) {
        rotation_umin.value = full_rpm;
        cylinder = 0;
        change_state(RL0102_STATE_brush_cycle);
        return;
    }

    load_lamp.value = 0;
    ready_lamp.value = 0;
    writeprotect_lamp.value = writeprotect_button.value || image_is_readonly();

    state_timeout.wait_ms(calcperiod_ms);
}

void RL0102_c::state_brush_cycle()
{
    // a real brush was used only on early RL01
    // drive_ready_line = false ;
    update_status_word(/*drive_ready_line*/false, drive_error_line);

    state_timeout.wait_ms(spinup_delay.value ? 100 : 0);
    change_state(RL0102_STATE_load_heads);
}

void RL0102_c::state_load_heads() 
{
    // drive_ready_line = false ;
    update_status_word(/*drive_ready_line*/false, drive_error_line);
    state_timeout.wait_ms(spinup_delay.value ? time_heads_out_ms : 0);

    cylinder = 0;
    this->seek_destination_cylinder = 0;
    this->seek_destination_head = 0; // time needed to center on track
    head = 0xff; // invalid, to get extra seek time

    // now perform a "guard band seek": seek head 0, track 0
    change_state(RL0102_STATE_seek);

//	next_sector_segment_under_heads = 0; // init rotation angle
    next_sector_segment_under_heads = 12; // next header is 6
}

// EK-RL012-TM-PRE seek timing. The positioner accelerates, slews and
// decelerates across the requested cylinder distance, dwelling in Seek
// (velocity) mode - drive state 4 - for the whole move. It then enters Position
// mode (Lock On), where state_lock_on() applies the 6.5 ms settle before Drive
// Ready. A head switch to the other surface is itself a servo seek (UG-005:
// "the seek and the head switching are normally combined"), so it is folded
// into the same move with an 8 ms floor.
void RL0102_c::state_seek()
{
    update_status_word(/*drive_ready_line*/false, drive_error_line); // reports state 4

    if (runstop_button.value == false || fault_lamp.value == true) { // stop spinning
        change_state(RL0102_STATE_spin_down);
        return;
    }

    load_lamp.value = 0;
    ready_lamp.value = 0;
    writeprotect_lamp.value = writeprotect_button.value || image_is_readonly();

    // cylinder distance for the requested seek
    unsigned dist = (seek_destination_cylinder > cylinder)
                    ? (seek_destination_cylinder - cylinder)
                    : (cylinder - seek_destination_cylinder);

    // Linear seek time between the TM-PRE endpoints: one-track = 5 ms,
    // full stroke (cylinder_count-1 tracks) = 100 ms.
    unsigned seek_ms = 0;
    if (dist > 0) {
        if (geometry.cylinder_count > 1)
            seek_ms = time_seek_one_track_ms
                      + (time_seek_full_ms - time_seek_one_track_ms) * (dist - 1)
                        / (geometry.cylinder_count - 1);
        else
            seek_ms = time_seek_one_track_ms;
    }
    // a head switch is a combined servo seek: at least the 8 ms head-switch time
    if (seek_destination_head != head && seek_ms < time_head_switch_ms)
        seek_ms = time_head_switch_ms;

    DEBUG_FAST("Drive seek cyl %d->%d (dist %d), head %d->%d, %d ms",
          cylinder, seek_destination_cylinder, dist, head, seek_destination_head, seek_ms);

    head = seek_destination_head;
    cylinder = seek_destination_cylinder;

    // held in Seek (state 4) for the positioner move, then Lock On
    state_timeout.wait_ms(seek_ms);
    change_state(RL0102_STATE_lock_on);
}

void RL0102_c::state_lock_on()
{
    if (runstop_button.value == false || fault_lamp.value == true) { // stop spinning
        change_state(RL0102_STATE_unload_heads);
        return;
    }

    if (seek_settle_pending) {
        // EK-RL012-TM-PRE §4.2.1: after reaching Position Mode a read/write
        // timeout allows 6.5 ms for the positioner to settle before Drive Ready
        // is asserted. The drive stays in Lock On (state 5) throughout, including
        // a zero-difference seek. Hold not-ready for the settle, then re-assert.
        update_status_word(/*drive_ready_line*/false, drive_error_line);
        load_lamp.value = 0;
        ready_lamp.value = 0;
        writeprotect_lamp.value = writeprotect_button.value || image_is_readonly();
        state_timeout.wait_us(time_seek_settle_us);
        seek_settle_pending = false;
        return;
    }

    // drive_ready_line = true;
    update_status_word(/*drive_ready_line*/true, drive_error_line);

    load_lamp.value = 0;
    ready_lamp.value = 1;
    writeprotect_lamp.value = writeprotect_button.value || image_is_readonly();

    // fast polling, if ZRLI tests time of 0 cly seek with head switch
    state_timeout.wait_ms(1);
//	state_wait_ms = 100 ;
}

void RL0102_c::state_unload_heads() 
{
    drive_ready_line = false;
    state_timeout.wait_ms(spinup_delay.value ? time_heads_out_ms : 0);
    change_state(RL0102_STATE_spin_down);
}

void RL0102_c::state_spin_down() 
{
    unsigned calcperiod_ms = 100;
    // change of rpm in 0.1 secs
    unsigned rpm_increment = full_rpm / (time_spinup_sec * (1000 / calcperiod_ms));
    rpm_increment *= emulation_speed.value;

    // drive_ready_line = false; // verified
    update_status_word(/*drive_ready_line*/false, drive_error_line);

    INFO("Spin down drive speed = %d", rotation_umin.value);

    if (config_apply_immediate) {       // a configuration apply skips the spin-down delay
        rotation_umin.value = 0;
        change_state(RL0102_STATE_load_cartridge);
        return;
    }

    if (rotation_umin.value <= rpm_increment) {
        rotation_umin.value = 0;
        change_state(RL0102_STATE_load_cartridge);
        return;
    } else
        rotation_umin.value -= rpm_increment;

    load_lamp.value = 0;
    ready_lamp.value = 0;
    writeprotect_lamp.value = writeprotect_button.value || image_is_readonly();

    state_timeout.wait_ms(calcperiod_ms);
}

// clear volatile error conditions in status word
void RL0102_c::clear_error_register(void) 
{
    error_wge = false;
    volume_check = false;
    update_status_word(drive_ready_line, /*drive_error_line*/false);
}

// return drive status word for controller MP registers
void RL0102_c::update_status_word(bool new_drive_ready_line, bool new_drive_error_line) 
{
    uint16_t tmp = 0;
    if (state.value != RL0102_STATE_power_off)
        tmp |= state.value;
    if (state.value != RL0102_STATE_brush_cycle)
        tmp |= RL0102_STATUS_BH; // brush home
    if (state.value == RL0102_STATE_load_heads || state.value == RL0102_STATE_seek
            || state.value == RL0102_STATE_lock_on)
        tmp |= RL0102_STATUS_HO; // heads out
    if (cover_open.value == true)
        tmp |= RL0102_STATUS_CO;
    if (head == 1) // which head is selected after last seek/read/write?
        tmp |= RL0102_STATUS_HS;
    if (drive_type == drive_type_e::RL02)
        tmp |= RL0102_STATUS_DT; // rl02
    /* OPI on RL11 controller
     if (state.value == RL0102_STATE_power_off) {
     tmp |= RL0102_STATUS_DSE; // drive select
     drive_error_line = true;
     }
     */
    if (volume_check) {
        tmp |= RL0102_STATUS_VC;
        new_drive_error_line = true; // VC is error, tested on real RL02
    }
    if (error_wge) {
        tmp |= RL0102_STATUS_WGE; // write not possible
        new_drive_error_line = true;
    }
    if (image_is_readonly() || writeprotect_button.value == true) {
        // writeprotect_lamp.value = true ; not here!!
        tmp |= RL0102_STATUS_WL;
    }

    // notify the RL11 CSR?
    if (new_drive_ready_line != drive_ready_line || new_drive_error_line != drive_error_line
            || tmp != status_word) {
        drive_ready_line = new_drive_ready_line;
        drive_error_line = new_drive_error_line;
        status_word = tmp;
        controller->on_drive_status_changed(this);
    }
}

// update, if neither error nor ready changed
void RL0102_c::update_status_word(void) 
{
    update_status_word(drive_ready_line, drive_error_line);
}

// is sector with given header on current track?
bool RL0102_c::header_on_track(uint16_t header) 
{
    // fields of disk address word (read/write data, read header)
    unsigned header_cyl = (header >> 7) & 0x1ff; // bits <15:7>
    unsigned header_hd = (header >> 6) & 0x01; // bit 6
    unsigned header_sec = header & 0x03f; // bit <5:0>
    if (header_cyl != cylinder || header_hd != head || header_sec >= 40)
        return false;
    else
        return true;
}

/*
 simulate an alternating stream of sector_headers and sector_data on the rotating platter.
 sector_segment_under_heads counts 0..79
 if even: sector header under head
 if odd: data under heads
 time for one sector on platter:
 1 rotation = 40 sectors = (60/2400)= 25ms
 1 sector (header+data( = 25ms/40 = 625 us.
 */

#define NEXT_SECTOR_SEGMENT_ADVANCE do {	\
	next_sector_segment_under_heads = (next_sector_segment_under_heads + 1) % 80 ;	\
} while(0)

#ifdef OLD
#define NEXT_SECTOR_SEGMENT_ADVANCE do {	\
 	next_sector_segment_under_heads = (next_sector_segment_under_heads + 1) % 80 ;	\
 	if (next_sector_segment_under_heads & 1)		\
 		/* time to pass one sector 600 data, 25 header? */			\
 		rotational_timeout.wait_us(600) ;		\
 		else rotational_timeout.wait_us(25) ;		\
 } while(0)
#endif

// read next sector header from rotating platter
// then increments "sector_segment_under_heads" to next data
// sector header has the format
// 3 words: diskaddress, 0x0000, CRC
// samples from real RL02: cyl=0,head. each header(=sector) and crc
// (3,042000);(4,030001);(6,104000);(7,072001);(16,164002);(17,012003);(24,170005);
// (25,006004);(26,044004);(27,132005);(31,056007);(33,162006);(34,110007);
// (37,152007);(40,140013);(43,102013);(44,170012);(46,044013)
bool RL0102_c::cmd_read_next_sector_header(uint16_t *buffer, unsigned buffer_size_words) 
{
    if (state.value != RL0102_STATE_lock_on)
        return false; // wrong state

    assert(buffer_size_words >= 3);

    if (next_sector_segment_under_heads & 1) {
        // odd: next is data, let it pass the head
        NEXT_SECTOR_SEGMENT_ADVANCE
        ;
        // nanosleep() for rotational delay?
    }

    unsigned sectorno = next_sector_segment_under_heads >> 1; // LSB is header/data phase

    // bits<0:5>=sector, bit<6>=head, bit<7:15>=cylinder
    assert(cylinder < 512);
    assert(head < 2);
    assert(sectorno < 40);
    // fill 3 word header
    buffer[0] = (cylinder << 7) | (head << 6) | sectorno;
    buffer[1] = 0x0000;
    buffer[2] = calc_crc(2, &buffer[0]); // header CRC

    // circular advance to next header: 40x headers, 40x data
    NEXT_SECTOR_SEGMENT_ADVANCE
    ;
    // nanosleep() for rotational delay?
    return true;
}

// read next data block from rotating platter
// then increments "sector_segment_under_heads" to next header
// controller must address sector by waiting for it with cmd_read_next_sector_header()
bool RL0102_c::cmd_read_next_sector_data(uint16_t *buffer, unsigned buffer_size_words) 
{
    if (state.value != RL0102_STATE_lock_on)
        return false; // wrong state

    assert(buffer_size_words * 2 >= geometry.sector_size_bytes);

    if (!(next_sector_segment_under_heads & 1)) {
        // even: next segment is header, let it pass the head
        NEXT_SECTOR_SEGMENT_ADVANCE
        ;
        // nanosleep() for rotational delay?
    }
    unsigned sectorno = next_sector_segment_under_heads >> 1; // LSB is header/data phase
    unsigned track_size_bytes = geometry.sector_count * geometry.sector_size_bytes;
    uint64_t offset = (uint64_t) (geometry.head_count * cylinder + head) * track_size_bytes
                      + sectorno * geometry.sector_size_bytes;

    // access image file
    // LSB saved before MSB -> word/byte conversion on ARM (little endian) is easy
    image_read((uint8_t *) buffer, offset, geometry.sector_size_bytes);
    DEBUG_FAST("File Read 0x%x words from c/h/s=%d/%d/%d, file pos=0x%llx, words = %06o, %06o, ...",
          geometry.sector_size_bytes / 2, cylinder, head, sectorno, offset, (unsigned )(buffer[0]),
          (unsigned )(buffer[1]));

    // circular advance to next header: 40x headers, 40x data
    NEXT_SECTOR_SEGMENT_ADVANCE
    ;
    // nanosleep() for rotational delay?
    return true;
}

// write data for current sector under head
// then increments "stuff_under_heads" to next header
// controller must address sector by waiting for it with cmd_read_next_sector_header()
bool RL0102_c::cmd_write_next_sector_data(uint16_t *buffer, unsigned buffer_size_words) 
{
    if (state.value != RL0102_STATE_lock_on)
        return false; // wrong state

    assert(buffer_size_words * 2 >= geometry.sector_size_bytes);

    // error: write can not be executed, different reasons
    if (image_is_readonly() || writeprotect_button.value == true || !drive_ready_line) {
        error_wge = true;
        update_status_word();
        return true; // function did not fail, WGE is valid result
    }
    error_wge = false; // can read

    if (!(next_sector_segment_under_heads & 1)) {
        // even: next segment is header, let it pass the head
        NEXT_SECTOR_SEGMENT_ADVANCE
        ;
        // nanosleep() for rotational delay?
    }
    unsigned sectorno = next_sector_segment_under_heads >> 1; // LSB is header/data phase
    unsigned track_size_bytes = geometry.sector_count * geometry.sector_size_bytes;
    uint64_t offset = (uint64_t) (geometry.head_count * cylinder + head) * track_size_bytes
                      + sectorno * geometry.sector_size_bytes;

    // access image file
    // LSB saved before MSB -> word/byte conversion on ARM (little endian) is easy
    image_write((uint8_t *) buffer, offset, geometry.sector_size_bytes);
    DEBUG_FAST("File Write 0x%x words from c/h/s=%d/%d/%d, file pos=0x%llx, words = %06o, %06o, ...",
          geometry.sector_size_bytes / 2, cylinder, head, sectorno, offset, (unsigned )(buffer[0]),
          (unsigned )(buffer[1]));

    // circular advance to next header: 40x headers, 40x data
    NEXT_SECTOR_SEGMENT_ADVANCE
    ;
    // nanosleep() for rotational delay?

    return true;
}

// thread
void RL0102_c::worker(unsigned instance) 
{
    UNUSED(instance); // only one
    timeout_c timeout;

    // set prio to RT, but less than RL11 controller
    worker_init_realtime_priority(rt_device);

    while (!workers_terminate) {
//		worker_mutex.lock() ; // collision with cmd_seek() and on_xxx_changed()
        // states have set error flags not in RL11 CSR: just update
        update_status_word(drive_ready_line, drive_error_line);

        // global stuff for all states
        if (enabled.value && (!controller || !controller->enabled.value))
            // RL drive powered, but no controller: no clock -> FAULT
            fault_lamp.value = true;

        if (power_switch.value == false)
            change_state(RL0102_STATE_power_off);

        switch (state.value) {
        case RL0102_STATE_power_off:
            state_power_off();
            break;
        case RL0102_STATE_load_cartridge:
            state_load_cartridge();
            break;
        case RL0102_STATE_spin_up:
            state_spin_up();
            break;
        case RL0102_STATE_brush_cycle:
            state_brush_cycle();
            break;
        case RL0102_STATE_load_heads:
            state_load_heads();
            break;
        case RL0102_STATE_seek:
            state_seek();
            break;
        case RL0102_STATE_lock_on:
            state_lock_on();
            break;
        case RL0102_STATE_unload_heads:
            state_unload_heads();
            break;
        case RL0102_STATE_spin_down:
            state_spin_down();
            break;
        }
//	worker_mutex.unlock() ;

    }
}

