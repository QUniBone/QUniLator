/* rk0607.hpp: RK06/RK07 cartridge disk drive, on an RK611 controller

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see any device source header for the full text.

   The drive as DEC's RK06/RK07 Disk Drive User's Guide (EK-RK067-UG-001)
   describes it: a spindle that spins up and loads its heads, a positioner that
   seeks, recalibrates and offsets, a Volume Valid flip-flop the program sets
   with Pack Acknowledge, a Drive Status-Change flip-flop behind the polled
   Attention line, and the eight 16-bit status messages A0-A3/B0-B3 it returns
   to the controller (UG figure 2-7b).

   An RK06 has 411 cylinders, an RK07 815, both 3 tracks of 22 sectors of 256
   16-bit words, and the image is a plain dump of those sectors in cylinder,
   track, sector order - the layout SimH's HK device uses. Sector headers are
   not stored in the image: a track reads back the headers a standard format
   gives it until a Write Header formats it otherwise, and such a track's
   headers are held for as long as the medium stays in the drive.

   All mutable state is guarded by the controller's state mutex; the drive's
   own worker runs the mechanics (spin-up, seek and offset timing) under it.
*/
#ifndef _RK0607_HPP_
#define _RK0607_HPP_

#include <stdint.h>
#include <array>
#include <map>
#include <set>

#include "storagedrive.hpp"

class rk611_c;

class rk0607_c: public storagedrive_c {
	friend class rk611_c;
public:
	bool removable(void) const override { return true; }
	bool opens_medium_on_attach(void) const override { return true; }

	static const unsigned sectors_per_track = 22;
	static const unsigned tracks_per_cylinder = 3;
	static const unsigned words_per_sector = 256;
	static const unsigned rk06_cylinders = 411;
	static const unsigned rk07_cylinders = 815;

	// UG 1.x specifications: 2400 rpm, so one revolution is 25 ms
	static const uint64_t revolution_ns = 25000000;

	/// The drive's front panel. `runstop` is the RUN/STOP switch: in RUN, a
	/// drive holding a cartridge spins up, loads its heads and comes READY;
	/// in STOP it unloads them and spins down. An Unload command takes the
	/// drive off line as STOP does, and Start Spindle brings it back while the
	/// switch is in RUN. Setting `image` on a drive in RUN loads the new pack
	/// the same way.
	///
	/// `writeprotect` is the WRITE PROT switch. A protected drive refuses
	/// every write with a Write Lock Error, and an image the board can only
	/// read is protected the same way.
	///
	/// The lamps follow the drive: READY with the heads loaded and on a
	/// cylinder, FAULT while the drive holds an error, WRITE PROT while the
	/// pack is protected.
	parameter_bool_c runstop_switch = parameter_bool_c(this, "runstop", "rs", /*readonly*/
			false, "RUN/STOP switch: 1 = RUN");
	parameter_bool_c writeprotect_switch = parameter_bool_c(this, "writeprotect", "wp", /*readonly*/
			false, "WRITE PROT switch");
	parameter_bool_c ready_lamp = parameter_bool_c(this, "readylamp", "rl", /*readonly*/
			true, "State of READY lamp");
	parameter_bool_c fault_lamp = parameter_bool_c(this, "faultlamp", "fl", /*readonly*/
			true, "State of FAULT lamp");
	parameter_bool_c writeprotect_lamp = parameter_bool_c(this, "writeprotectlamp", "wpl", /*readonly*/
			true, "State of WRITE PROT lamp");

	/// With 1, the drive takes the 30 seconds a real one does to spin up, run
	/// its brush cycle and load the heads.
	/// With 0, it is ready as soon as it is loaded. Seek, offset and rotation
	/// times are modelled either way.
	parameter_bool_c spinup_delay = parameter_bool_c(this, "spinupdelay", "sud", /*readonly*/
			false, "1 = model the drive's spin-up/spin-down delay; 0 = ready at once");

	/// The cylinder the heads are over.
	parameter_unsigned_c cylinder_param = parameter_unsigned_c(this, "cylinder", "cyl", /*readonly*/
			true, "", "%d", "Cylinder the heads are over", 16, 10);

	rk0607_c(storagecontroller_c *controller);

	bool on_param_changed(parameter_c *param) override;
	void on_power_changed(signal_edge_enum aclo_edge, signal_edge_enum dclo_edge) override;
	void on_init_changed(void) override;
	void worker(unsigned instance) override;

	void set_type(enum drive_type_e type);
	bool is_rk07(void) const { return rk07; }
	unsigned cylinder_count(void) const { return rk07 ? rk07_cylinders : rk06_cylinders; }

private:
	rk611_c *rk611(void);
	bool rk07 = true;

	// Spindle and heads. A drive in RUN with a cartridge goes STARTING (spin
	// up, brush cycle, heads loading) -> LOADED; leaving RUN, losing the
	// cartridge or an Unload goes UNLOADING -> STOPPED.
	enum spindle_e {
		SPINDLE_STOPPED, SPINDLE_STARTING, SPINDLE_LOADED, SPINDLE_UNLOADING
	};
	volatile spindle_e spindle = SPINDLE_STOPPED;
	uint64_t spindle_until_ns = 0;
	// set by an Unload command, cleared by Start Spindle and by RUN
	bool offline_by_command = false;

	// the positioner
	enum position_op_e {
		POSITION_NONE, POSITION_SEEK, POSITION_RECAL, POSITION_OFFSET
	};
	position_op_e position_op = POSITION_NONE;
	uint64_t position_until_ns = 0;
	unsigned cylinder = 0;          // where the heads are
	unsigned target_cylinder = 0;   // where a seek takes them
	int cylinder_difference = 0;    // of the last seek, Message A2
	uint8_t offset_value = 0;       // RKAS/OF bits <7:0> of the last Offset
	bool offset_mode = false;

	bool volume_valid = false;
	bool status_change = false;     // DSC flip-flop, the polled Attention line
	unsigned sector_format_20 = 0;  // the controller's last format bit
	unsigned selected_head = 0;     // head select code of the last message

	// errors the drive holds (Message B0/B1), cleared by Drive Clear or INIT
	bool err_invalid_address = false;  // B0:T5, with Seek Incomplete
	bool err_nxf = false;              // B0:T8
	bool err_parity = false;           // B0:T9, controller-to-drive parity
	bool err_seek_incomplete = false;  // B0:T10
	bool err_write_lock = false;       // B0:T11
	bool err_off_track = false;        // B0:T13, Write Gate off the centerline

	// Tracks formatted with headers other than the standard ones, by track
	// number (cylinder * 3 + track): 20 or 22 sectors, three words each.
	struct track_format_t {
		unsigned sectors;
		std::array<uint16_t, 3 * sectors_per_track> headers;
	};
	std::map<unsigned, track_format_t> custom_headers;

	// Sectors whose write stopped before the ECC was recorded, by
	// (cylinder * 3 + track) * 22 + sector slot. They read with Data Check.
	std::set<unsigned> bad_ecc;

	bool medium_was_present = false;

	// The disk's angular position runs on a clock of its own: wall time less
	// the part of each short pause between two controller operations that a
	// real processor would not have taken. See begin_access().
	uint64_t rotation_offset_ns = 0;
	uint64_t last_access_end_ns = 0;
	void begin_access(uint64_t now);
	void end_access(uint64_t now) { last_access_end_ns = now; }

	// all below run under the controller's state mutex
	uint64_t now_ns(void) const;
	uint64_t scaled_ns(uint64_t ns) const;
	bool fault(void) const;
	bool heads_loaded(void) const { return spindle == SPINDLE_LOADED; }
	bool positioning(void) const { return position_op != POSITION_NONE; }
	bool drive_ready(void) const { return heads_loaded() && !positioning(); }
	bool write_locked(void);
	// the sector counter, for a track of 22 sectors or of 20
	unsigned sector_under_heads(uint64_t now, unsigned sectors = sectors_per_track) const;
	uint64_t sector_start_ns(unsigned sector, uint64_t now,
			unsigned sectors = sectors_per_track) const;
	uint64_t seek_time_ns(unsigned distance) const;

	void set_status_change(void);
	void step_mechanics(uint64_t now);
	void update_lamps(void);

	// status messages, parity included
	uint16_t message_a(unsigned n);
	uint16_t message_b(unsigned n);

	// commands from the controller, after the drive accepted the message
	void cmd_pack_acknowledge(void);
	void cmd_drive_clear(void);
	void cmd_unload(void);
	void cmd_start_spindle(void);
	void cmd_recalibrate(void);
	void cmd_offset(uint8_t value);
	enum seek_result_e {
		SEEK_REFUSED, // the drive holds an error for it
		SEEK_IGNORED, // a drive not ready ignores head motion (UG 7.7)
		SEEK_STARTED
	};
	seek_result_e cmd_seek(unsigned desired_cylinder, bool return_to_centerline);
	void cmd_write_gate(void);  // a write began: Write Lock check
	void init_line(void);
	void parity_error(bool head_motion);

	// the header the controller reads at a physical sector position
	unsigned track_sectors(unsigned cyl, unsigned track);
	void header_at(unsigned cyl, unsigned track, unsigned position, uint16_t *header);
	void set_track_headers(unsigned cyl, unsigned track, const uint16_t *headers,
			unsigned sectors);
	unsigned sector_key(unsigned cyl, unsigned track, unsigned slot) const {
		return (cyl * tracks_per_cylinder + track) * sectors_per_track + slot;
	}
	static void standard_header(unsigned cyl, unsigned track, unsigned sector, uint16_t *header);
};

#endif
