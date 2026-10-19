/* rk0607.cpp: RK06/RK07 cartridge disk drive, on an RK611 controller

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see any device source header for the full text.

   Timing is the UG's (EK-RK067-UG-001 chapter 1): 2400 rpm, seeks from 8 ms
   for one cylinder to 75 ms across an RK06 and from 6.5 ms to 71 ms across an
   RK07, and a 30 second start and stop. Status message bit positions are those
   of UG figure 2-7b.
*/

#include <assert.h>
#include <string.h>
#include <strings.h>

#include "logger.hpp"
#include "timeout.hpp"
#include "utils.hpp"
#include "rk611.hpp"
#include "rk0607.hpp"

// Message A0 (UG figure 2-7b, chapter 7 text)
#define A0_DRA          0000040 // drive available
#define A0_VV           0000100 // volume valid
#define A0_DRDY         0000200 // drive ready
#define A0_DDT          0000400 // drive type, 1 = RK07
#define A0_FMT20        0001000 // 20-sector format
#define A0_OFST         0002000 // offset mode
#define A0_WRL          0004000 // write lock
#define A0_SPO          0010000 // spindle on
#define A0_PIP          0020000 // positioning in progress
#define A0_DSC          0040000 // drive status change
// Message A1
#define A1_SERVO        0000020 // servo signal present
#define A1_HEADS_HOME   0000040
#define A1_BRUSH_HOME   0000100
#define A1_DOOR_LATCHED 0000200
#define A1_CARTRIDGE    0000400
#define A1_SPEED_OK     0001000
#define A1_FWD          0002000
#define A1_REV          0004000
#define A1_LOADING      0010000
#define A1_RTZ          0020000
#define A1_UNLOADING    0040000
// Message B0
#define B0_IAE          0000040 // invalid address
#define B0_ACLO         0000100
#define B0_FAULT        0000200
#define B0_NXF          0000400 // nonexecutable function
#define B0_PAR          0001000 // controller-to-drive parity error
#define B0_SKI          0002000 // seek incomplete
#define B0_WLE          0004000 // write lock error
#define B0_SPLS         0010000 // speed loss
#define B0_DROT         0020000 // drive off track
#define B0_UNS          0040000 // read/write unsafe

#define PARITY_BIT      0100000

// odd parity over the 16 bits of a message
static uint16_t with_parity(uint16_t v)
{
	v &= 077777;
	unsigned ones = __builtin_popcount(v);
	return (ones & 1) ? v : (v | PARITY_BIT);
}

rk0607_c::rk0607_c(storagecontroller_c *_controller) :
		storagedrive_c(_controller)
{
	log_label = "rk0607";
	drive_type = drive_type_e::RK067;
	set_type(drive_type_e::RK067);
	type_name.readonly = false;
	runstop_switch.value = true;
	writeprotect_switch.value = false;
	spinup_delay.value = false;
	ready_lamp.kind = parameter_c::PARAM_STATUS;
	fault_lamp.kind = parameter_c::PARAM_STATUS;
	writeprotect_lamp.kind = parameter_c::PARAM_STATUS;
	cylinder_param.kind = parameter_c::PARAM_STATUS;
}

rk611_c *rk0607_c::rk611(void)
{
	return static_cast<rk611_c *>(controller);
}

// The type parameter names the drive: RK06 or RK07.
void rk0607_c::set_type(enum drive_type_e type)
{
	UNUSED(type);
	drive_type = drive_type_e::RK067;
	geometry.cylinder_count = cylinder_count();
	geometry.head_count = tracks_per_cylinder;
	geometry.sector_count = sectors_per_track;
	geometry.sector_size_bytes = 2 * words_per_sector;
	// the bad sector file is on the last track
	geometry.bad_sector_file_offset = (uint64_t) (geometry.cylinder_count * tracks_per_cylinder - 1)
			* sectors_per_track * geometry.sector_size_bytes;
	type_name.value = rk07 ? "RK07" : "RK06";
	capacity.value = geometry.get_raw_capacity();
}

bool rk0607_c::on_param_changed(parameter_c *param)
{
	if (param == &enabled) {
		std::lock_guard<std::mutex> guard(rk611()->state_mutex);
		if (!enabled.new_value) {
			// a drive switched off answers nothing and holds nothing
			spindle = SPINDLE_STOPPED;
			position_op = POSITION_NONE;
			volume_valid = false;
			status_change = false;
			medium_was_present = false;
			update_lamps();
		}
	} else if (param == &type_name) {
		bool new_rk07;
		if (!strcasecmp(type_name.new_value.c_str(), "RK06"))
			new_rk07 = false;
		else if (!strcasecmp(type_name.new_value.c_str(), "RK07"))
			new_rk07 = true;
		else {
			ERROR("drive type must be RK06 or RK07");
			return false;
		}
		std::lock_guard<std::mutex> guard(rk611()->state_mutex);
		if (new_rk07 != rk07 && spindle != SPINDLE_STOPPED) {
			ERROR("%s: the drive type can be changed only with the spindle stopped",
					name.value.c_str());
			return false;
		}
		rk07 = new_rk07;
		set_type(drive_type_e::RK067);
		type_name.new_value = type_name.value;
	} else if (param == &runstop_switch) {
		std::lock_guard<std::mutex> guard(rk611()->state_mutex);
		// pressing RUN brings a drive taken off line by an Unload back
		if (runstop_switch.new_value && !runstop_switch.value)
			offline_by_command = false;
	} else if (image_is_param(param)) {
		if (!image_recreate_on_param_change(param))
			return false;
		if (param == &image_filepath && !image_filepath.new_value.empty()) {
			if (!image_open(true))
				ERROR("%s: could not open image \"%s\"", name.value.c_str(),
						image_filepath.new_value.c_str());
			else if (image_size() != geometry.get_raw_capacity())
				WARNING("%s: image \"%s\" holds %llu bytes, an %s pack %llu",
						name.value.c_str(), image_filepath.new_value.c_str(),
						(unsigned long long) image_size(), type_name.value.c_str(),
						(unsigned long long) geometry.get_raw_capacity());
		}
		std::lock_guard<std::mutex> guard(rk611()->state_mutex);
		// a cartridge change: the heads come off the old pack, and a pack put
		// in loads like one inserted by hand
		medium_was_present = false;
		custom_headers.clear();
		bad_ecc.clear();
		volume_valid = false;
		if (spindle == SPINDLE_LOADED || spindle == SPINDLE_STARTING) {
			spindle = SPINDLE_UNLOADING;
			spindle_until_ns = now_ns();
			position_op = POSITION_NONE;
			set_status_change();
		}
		return true;
	}
	return storagedrive_c::on_param_changed(param);
}

void rk0607_c::on_power_changed(signal_edge_enum aclo_edge, signal_edge_enum dclo_edge)
{
	UNUSED(aclo_edge);
	UNUSED(dclo_edge);
	// the drive has its own power; the controller's DC LO is its INIT
}

// INIT reaches the drive through the controller, which calls init_line()
// under its own lock.
void rk0607_c::on_init_changed(void)
{
}

uint64_t rk0607_c::now_ns(void) const
{
	return timeout_c::abstime_ns();
}

// a mechanical time, at the drive's emulation speed
uint64_t rk0607_c::scaled_ns(uint64_t ns) const
{
	double speed = emulation_speed.value > 0 ? emulation_speed.value : 1;
	return (uint64_t) (ns / speed);
}

bool rk0607_c::fault(void) const
{
	return err_invalid_address || err_nxf || err_parity || err_seek_incomplete || err_write_lock
			|| err_off_track;
}

bool rk0607_c::write_locked(void)
{
	return writeprotect_switch.value || image_is_readonly();
}

// The disk turns whether or not anyone looks: the sector under the heads
// follows from the time.
unsigned rk0607_c::sector_under_heads(uint64_t now, unsigned sectors) const
{
	uint64_t rev = scaled_ns(revolution_ns);
	uint64_t disk_now = now - rotation_offset_ns;
	return (unsigned) ((disk_now % rev) * sectors / rev);
}

// when the given sector next begins to pass the heads
uint64_t rk0607_c::sector_start_ns(unsigned sector, uint64_t now, unsigned sectors) const
{
	uint64_t rev = scaled_ns(revolution_ns);
	uint64_t disk_now = now - rotation_offset_ns;
	uint64_t rev_start = disk_now - (disk_now % rev);
	uint64_t start = rev_start + rev * sector / sectors;
	if (start < disk_now)
		start += rev;
	return start + rotation_offset_ns;
}

// A program that issues its next command as soon as the last one is done -
// back-to-back Read Headers that walk a track, consecutive transfers - does so
// on a real PDP-11 within a few tens of microseconds, before the next header
// has passed. An emulated processor takes longer than that, and a disk turning
// by the wall clock alone would show it a sector or more further on each time.
// So a pause shorter than two revolutions between two operations of the
// controller on this drive turns the disk by no more than 50 microseconds; a
// longer one turns it by the time it took.
void rk0607_c::begin_access(uint64_t now)
{
	const uint64_t think_ns = 50000;
	if (last_access_end_ns == 0 || now < last_access_end_ns)
		return;
	uint64_t gap = now - last_access_end_ns;
	if (gap > think_ns && gap < 2 * scaled_ns(revolution_ns))
		rotation_offset_ns += gap - think_ns;
	last_access_end_ns = 0;
}

// UG 1.x: one cylinder 8 ms (RK06) / 6.5 ms (RK07), full stroke 75 / 71 ms,
// linear in between
uint64_t rk0607_c::seek_time_ns(unsigned distance) const
{
	uint64_t min_ns = rk07 ? 6500000 : 8000000;
	uint64_t max_ns = rk07 ? 71000000 : 75000000;
	unsigned max_distance = cylinder_count() - 1;
	if (distance == 0)
		return 0;
	uint64_t t = min_ns + (max_ns - min_ns) * (distance - 1) / (max_distance - 1);
	return scaled_ns(t);
}

// The DSC flip-flop drives the polled Attention line.
void rk0607_c::set_status_change(void)
{
	status_change = true;
	rk611()->drive_attention(this);
}

void rk0607_c::update_lamps(void)
{
	ready_lamp.value = enabled.value && drive_ready();
	fault_lamp.value = enabled.value && fault();
	writeprotect_lamp.value = enabled.value && write_locked();
	cylinder_param.value = cylinder;
}

void rk0607_c::step_mechanics(uint64_t now)
{
	bool medium = image_is_open();
	if (!medium)
		medium_was_present = false;
	bool want_spinning = enabled.value && medium && runstop_switch.value && !offline_by_command;
	bool modeled = spinup_delay.value && !config_apply_immediate;
	uint64_t start_stop_ns = modeled ? scaled_ns(30000000000ULL) : 0;

	switch (spindle) {
	case SPINDLE_STOPPED:
		if (want_spinning) {
			spindle = SPINDLE_STARTING;
			spindle_until_ns = now + start_stop_ns;
			medium_was_present = true;
		}
		break;
	case SPINDLE_STARTING:
		if (!want_spinning)
			spindle = SPINDLE_STOPPED;
		else if (now >= spindle_until_ns) {
			// heads loaded over cylinder 0: Start Spindle complete
			spindle = SPINDLE_LOADED;
			cylinder = target_cylinder = 0;
			offset_mode = false;
			offset_value = 0;
			DEBUG_FAST("%s: heads loaded", name.value.c_str());
			set_status_change();
		}
		break;
	case SPINDLE_LOADED:
		if (!want_spinning) {
			// UG 2.5.1: Attention at the beginning of an Unload Heads
			spindle = SPINDLE_UNLOADING;
			spindle_until_ns = now + start_stop_ns;
			position_op = POSITION_NONE;
			volume_valid = false;
			offset_mode = false;
			DEBUG_FAST("%s: heads unloading", name.value.c_str());
			set_status_change();
		}
		break;
	case SPINDLE_UNLOADING:
		if (now >= spindle_until_ns)
			spindle = SPINDLE_STOPPED;
		break;
	}

	if (position_op != POSITION_NONE && now >= position_until_ns) {
		if (position_op != POSITION_OFFSET)
			cylinder = target_cylinder;
		position_op = POSITION_NONE;
		// seek, recalibrate and offset complete with Attention
		set_status_change();
	}
	update_lamps();
}

void rk0607_c::worker(unsigned instance)
{
	UNUSED(instance);
	worker_init_realtime_priority(rt_device);
	while (!workers_terminate) {
		{
			std::lock_guard<std::mutex> guard(rk611()->state_mutex);
			step_mechanics(now_ns());
		}
		timeout_c::wait_us(500);
	}
}

/*** status messages ***/

uint16_t rk0607_c::message_a(unsigned n)
{
	uint16_t v = unitno.value & 7;
	bool medium = image_is_open();
	switch (n & 3) {
	case 0:
		v |= A0_DRA;
		if (volume_valid)
			v |= A0_VV;
		if (drive_ready())
			v |= A0_DRDY;
		if (rk07)
			v |= A0_DDT;
		if (sector_format_20)
			v |= A0_FMT20;
		if (offset_mode)
			v |= A0_OFST;
		if (write_locked())
			v |= A0_WRL;
		if (spindle != SPINDLE_STOPPED)
			v |= A0_SPO;
		if (positioning())
			v |= A0_PIP;
		if (status_change)
			v |= A0_DSC;
		break;
	case 1:
		v |= A1_BRUSH_HOME | A1_DOOR_LATCHED;
		if (medium)
			v |= A1_CARTRIDGE;
		if (spindle == SPINDLE_LOADED)
			v |= A1_SERVO | A1_SPEED_OK;
		else
			v |= A1_HEADS_HOME;
		if (spindle == SPINDLE_STARTING)
			v |= A1_LOADING;
		if (spindle == SPINDLE_UNLOADING)
			v |= A1_UNLOADING;
		if (position_op == POSITION_RECAL)
			v |= A1_RTZ;
		else if (position_op == POSITION_SEEK) {
			if (target_cylinder > cylinder)
				v |= A1_FWD;
			else if (target_cylinder < cylinder)
				v |= A1_REV;
		}
		break;
	case 2: {
		// the cylinder difference of the last seek, or the offset in offset mode
		unsigned value = offset_mode ? offset_value : (unsigned) cylinder_difference;
		v |= (value & (rk07 ? 01777 : 0777)) << 4;
		break;
	}
	case 3: {
		// three BCD digits of the drive's serial number
		unsigned serial = 100 + (unitno.value & 7);
		unsigned bcd = ((serial / 100) % 10) << 8 | ((serial / 10) % 10) << 4 | (serial % 10);
		v |= (bcd & 07777) << 3;
		break;
	}
	}
	return with_parity(v);
}

uint16_t rk0607_c::message_b(unsigned n)
{
	uint16_t v = n & 3; // message ID
	switch (n & 3) {
	case 0:
		if (err_invalid_address)
			v |= B0_IAE;
		if (fault())
			v |= B0_FAULT;
		if (err_nxf)
			v |= B0_NXF;
		if (err_parity)
			v |= B0_PAR;
		if (err_seek_incomplete)
			v |= B0_SKI;
		if (err_write_lock)
			v |= B0_WLE;
		if (err_off_track)
			v |= B0_DROT;
		break;
	case 1:
		// none of the B1 conditions (servo, head and write-circuit faults)
		// arise in an emulated drive
		break;
	case 2:
		v |= (cylinder & (rk07 ? 01777 : 0777)) << 4;
		break;
	case 3: {
		unsigned sectors = sector_format_20 ? 20 : sectors_per_track;
		unsigned sector = sector_under_heads(now_ns(), sectors);
		v |= (sector & 037) << 4;
		v |= 1 << (9 + ((selected_head & 3) < 3 ? (selected_head & 3) : 0));
		break;
	}
	}
	return with_parity(v);
}

/*** commands ***/

void rk0607_c::cmd_pack_acknowledge(void)
{
	// the program acknowledges the medium the heads are on
	if (heads_loaded())
		volume_valid = true;
}

void rk0607_c::cmd_drive_clear(void)
{
	// the errors are cleared once their conditions are gone
	err_invalid_address = err_nxf = err_parity = err_seek_incomplete = err_write_lock
			= err_off_track = false;
	status_change = false;
}

void rk0607_c::cmd_unload(void)
{
	if (spindle == SPINDLE_LOADED || spindle == SPINDLE_STARTING)
		offline_by_command = true; // step_mechanics() unloads, with Attention
}

void rk0607_c::cmd_start_spindle(void)
{
	// heads already loaded: no Attention follows (UG 7.7)
	if (spindle == SPINDLE_LOADED)
		return;
	offline_by_command = false;
}

void rk0607_c::cmd_recalibrate(void)
{
	// UG 7.7: a head motion command is ignored while the drive is not ready
	if (!drive_ready())
		return;
	target_cylinder = 0;
	cylinder_difference = -(int) cylinder;
	offset_mode = false;
	position_op = POSITION_RECAL;
	// the positioner returns to its reference and comes back to cylinder 0:
	// it takes the full-stroke time wherever the heads were
	position_until_ns = now_ns() + seek_time_ns(cylinder_count() - 1);
}

// An offset moves the heads a fraction of a track; it takes as long as a
// one-cylinder seek.
void rk0607_c::cmd_offset(uint8_t value)
{
	if (!drive_ready())
		return;
	offset_value = value;
	offset_mode = (value & 077) != 0;
	position_op = POSITION_OFFSET;
	position_until_ns = now_ns() + seek_time_ns(1);
}

rk0607_c::seek_result_e rk0607_c::cmd_seek(unsigned desired_cylinder, bool return_to_centerline)
{
	if (!volume_valid) {
		// UG 7.2.7: a Seek received while Volume Valid is reset
		err_nxf = true;
		set_status_change();
		return SEEK_REFUSED;
	}
	// An RK06 decodes nine cylinder bits, an RK07 ten, and both two bits of
	// the head select code: head 3 does not exist.
	unsigned cyl = desired_cylinder & (rk07 ? 01777 : 0777);
	if (cyl >= cylinder_count() || (selected_head & 3) >= tracks_per_cylinder) {
		err_invalid_address = true;
		err_seek_incomplete = true;
		set_status_change();
		return SEEK_REFUSED;
	}
	if (!drive_ready())
		return SEEK_IGNORED;
	if (return_to_centerline || cyl != cylinder)
		offset_mode = false;
	cylinder_difference = (int) cyl - (int) cylinder;
	target_cylinder = cyl;
	if (cyl == cylinder) {
		// no motion: the seek is complete as the message is taken
		set_status_change();
		return SEEK_STARTED;
	}
	position_op = POSITION_SEEK;
	position_until_ns = now_ns() + seek_time_ns(cyl > cylinder ? cyl - cylinder : cylinder - cyl);
	return SEEK_STARTED;
}

// The controller asserts Write Gate: a protected drive refuses it, and one
// whose heads are offset from the centerline reports Drive Off Track.
void rk0607_c::cmd_write_gate(void)
{
	if (write_locked()) {
		err_write_lock = true;
		set_status_change();
	}
	if (offset_mode) {
		err_off_track = true;
		set_status_change();
	}
}

// the Initialize line: deselect, reset errors, clear the status change
void rk0607_c::init_line(void)
{
	err_invalid_address = err_nxf = err_parity = err_seek_incomplete = err_write_lock
			= err_off_track = false;
	status_change = false;
}

// A controller-to-drive message arrived with even parity. The drive acts on
// none of it; a seek it carried is a seek not completed.
void rk0607_c::parity_error(bool seek)
{
	err_parity = true;
	if (seek)
		err_seek_incomplete = true;
	set_status_change();
}

/*** headers ***/

// UG figure 2-3: word 1 the cylinder, word 2 the sector in <4:0>, the track
// in <6:5>, the format bit <9> (0 for 16-bit data) and the two Good Sector
// flags <15:14>, word 3 the exclusive OR of the two.
void rk0607_c::standard_header(unsigned cyl, unsigned track, unsigned sector, uint16_t *header)
{
	header[0] = cyl;
	header[1] = (sector & 037) | ((track & 3) << 5) | 0140000;
	header[2] = header[0] ^ header[1];
}

// sectors on a track: 22 unless a Write Header formatted it with 20
unsigned rk0607_c::track_sectors(unsigned cyl, unsigned track)
{
	auto it = custom_headers.find(cyl * tracks_per_cylinder + track);
	return it == custom_headers.end() ? sectors_per_track : it->second.sectors;
}

void rk0607_c::header_at(unsigned cyl, unsigned track, unsigned position, uint16_t *header)
{
	auto it = custom_headers.find(cyl * tracks_per_cylinder + track);
	if (it == custom_headers.end())
		standard_header(cyl, track, position, header);
	else
		memcpy(header, &it->second.headers[3 * position], 3 * sizeof(uint16_t));
}

// A 20-sector track holds its sectors in the first 20 sector slots of the
// track in the image; on a UNIBUS the two bits an 18-bit word has beyond the
// memory word are the bus parity lines, so a sector still holds 256 words.
void rk0607_c::set_track_headers(unsigned cyl, unsigned track, const uint16_t *headers,
		unsigned sectors)
{
	bool standard = sectors == sectors_per_track;
	for (unsigned s = 0; s < sectors && standard; s++) {
		uint16_t h[3];
		standard_header(cyl, track, s, h);
		standard = !memcmp(h, &headers[3 * s], sizeof h);
	}
	unsigned index = cyl * tracks_per_cylinder + track;
	for (unsigned s = 0; s < sectors_per_track; s++)
		bad_ecc.erase(sector_key(cyl, track, s));
	if (standard)
		custom_headers.erase(index);
	else {
		track_format_t f;
		f.sectors = sectors;
		f.headers.fill(0);
		memcpy(f.headers.data(), headers, sizeof(uint16_t) * 3 * sectors);
		custom_headers[index] = f;
	}
}
