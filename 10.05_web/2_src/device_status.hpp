/* device_status.hpp: verbal runtime status of a disk drive

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.

   The single source of the disk-drive status word exposed as the `status`
   field in GET /api/devices and read by both the dashboard and the MCP
   server, so the two never drift. The derivation is a pure function of the
   parameters a drive already publishes (enabled, image, the drive state
   machine, the ready lamp, the activity/access lamp); the device-aware
   extraction that feeds it lives in webapi.cpp, keeping this translation unit
   free of device dependencies so it links into the host test unchanged.
*/
#ifndef _DEVICE_STATUS_HPP_
#define _DEVICE_STATUS_HPP_

// Drive families that need different treatment for the loaded/ready split.
// Most drives report ready as soon as a medium is present (no modelled
// spin-up); the RL01/RL02 walk a spin-up/seek/lock-on state machine, so they
// pass through `loaded` while coming online.
enum class disk_family_e {
	generic, // RK05, RX01/02, MSCP (RA81 ...): ready once media is present
	rl       // RL01/RL02: distinguish loaded vs ready by the state machine
};

// Mirror of the RL0102_STATE_* values (rl0102.hpp); webapi.cpp static_asserts
// they agree, so this file needs no device header. The pack spins up through
// spin_up..load_heads and spins down through unload_heads..spin_down; it is
// online only at lock_on.
static const int DISK_STATUS_RL_SPIN_UP = 1;    // spin_up
static const int DISK_STATUS_RL_LOAD_HEADS = 3; // load_heads (end of spin-up)
static const int DISK_STATUS_RL_LOCK_ON = 5;
static const int DISK_STATUS_RL_UNLOAD_HEADS = 6; // unload_heads (start of spin-down)
static const int DISK_STATUS_RL_SPIN_DOWN = 7;

// Signals read from the drive's parameters.
struct disk_signals_c {
	bool enabled = false;   // device enabled
	bool has_image = false; // image parameter non-empty (a medium is loaded)
	bool activity = false;  // access/ACCESS lamp lit (a transfer just ran)
	int rl_state = -1;      // RL0102_STATE_* raw value; used only for family rl
};

// Verbal status:
//   off          — device disabled
//   idle         — enabled, no medium
//   spinning up  — RL pack coming up to speed / loading heads
//   spinning down— RL pack unloading heads / stopping
//   loaded       — medium present, drive coming online (seek/load), not spinning
//   ready        — online, ready for I/O
//   busy         — actively transferring
const char *disk_status(disk_family_e family, const disk_signals_c &s);

#endif // _DEVICE_STATUS_HPP_
