/* status_power_test.cpp: host test of the disk-status and power-flag helpers

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see any source file header for the full text.

   The verbal disk status (web-dashboard-plan.md §5) and the control-action
   power gate (§3) are pure functions of the parameters a drive publishes and
   of the logical power flag, so they run on the development host with no
   hardware. This links the real device_status.cpp and webcontrol.cpp and drives
   them over the parameter combinations for each drive type and every control
   action.

   Build & run: 10.05_web/tools/run_config_test.sh
*/

#include <cstdio>
#include <cstring>

#include "device_status.hpp"
#include "webcontrol.hpp"

static int failures = 0;
static int checks = 0;

static void check(bool ok, const char *what) {
	checks++;
	if (!ok) {
		failures++;
		fprintf(stderr, "FAIL: %s\n", what);
	}
}

// RL0102_STATE_* raw values (mirrored from rl0102.hpp; DISK_STATUS_RL_LOCK_ON
// is verified against the header at the board build in webapi.cpp)
static const int RL_LOAD_CARTRIDGE = 0;
static const int RL_SPIN_UP = 1;
static const int RL_LOAD_HEADS = 3;
static const int RL_SEEK = 4;
static const int RL_LOCK_ON = 5;
static const int RL_UNLOAD_HEADS = 6;
static const int RL_SPIN_DOWN = 7;

static disk_signals_c sig(bool enabled, bool has_image, bool activity, int rl_state) {
	disk_signals_c s;
	s.enabled = enabled;
	s.has_image = has_image;
	s.activity = activity;
	s.rl_state = rl_state;
	return s;
}

static bool is(const char *a, const char *b) {
	return strcmp(a, b) == 0;
}

static void test_disk_status(void) {
	// Generic drives (RK05, RX01/02, MSCP RA81): ready as soon as a medium is
	// present, no modelled spin-up.
	disk_family_e g = disk_family_e::generic;
	check(is(disk_status(g, sig(false, false, false, -1)), "off"),
			"generic disabled, no image -> off");
	check(is(disk_status(g, sig(false, true, true, -1)), "off"),
			"generic disabled with media -> off");
	check(is(disk_status(g, sig(true, false, false, -1)), "idle"),
			"generic enabled, no image -> idle");
	check(is(disk_status(g, sig(true, true, false, -1)), "ready"),
			"generic enabled with media -> ready (non-spinning fast path)");
	check(is(disk_status(g, sig(true, true, true, -1)), "busy"),
			"generic transferring -> busy");

	// RL01/RL02: the state machine splits loaded (coming online) from ready.
	disk_family_e r = disk_family_e::rl;
	check(is(disk_status(r, sig(false, false, false, RL_LOAD_CARTRIDGE)), "off"),
			"rl disabled -> off");
	check(is(disk_status(r, sig(true, false, false, RL_LOAD_CARTRIDGE)), "idle"),
			"rl enabled, no image -> idle");
	check(is(disk_status(r, sig(true, true, false, RL_LOAD_CARTRIDGE)), "loaded"),
			"rl media loaded, cartridge not spun up -> loaded");
	check(is(disk_status(r, sig(true, true, false, RL_SPIN_UP)), "spinning up"),
			"rl spin_up -> spinning up");
	check(is(disk_status(r, sig(true, true, false, RL_LOAD_HEADS)), "spinning up"),
			"rl loading heads -> spinning up");
	check(is(disk_status(r, sig(true, true, false, RL_SEEK)), "loaded"),
			"rl seeking -> loaded");
	check(is(disk_status(r, sig(true, true, false, RL_UNLOAD_HEADS)), "spinning down"),
			"rl unloading heads -> spinning down");
	check(is(disk_status(r, sig(true, true, false, RL_SPIN_DOWN)), "spinning down"),
			"rl spin_down -> spinning down");
	// a spin transition outranks a stale activity flag: no transfer mid-spin
	check(is(disk_status(r, sig(true, true, true, RL_SPIN_DOWN)), "spinning down"),
			"rl spinning down with stale activity -> spinning down");
	check(is(disk_status(r, sig(true, true, false, RL_LOCK_ON)), "ready"),
			"rl locked on -> ready");
	check(is(disk_status(r, sig(true, true, true, RL_LOCK_ON)), "busy"),
			"rl transferring -> busy");
	// spin-up disabled: the drive reaches lock-on at once, so it never lingers
	// in loaded — the mapping tolerates the fast path.
	check(is(disk_status(r, sig(true, true, false, RL_LOCK_ON)), "ready"),
			"rl non-spinning fast path -> ready");
}

// helpers to read a decision compactly
static control_decision_c dec(const char *action, bool powered) {
	return control_decide(action, powered);
}

static void test_power_gate(void) {
	// Powered on: every action is known and allowed.
	control_decision_c d;

	// a bus INIT resets registers; it is not a power event, so the cards stay
	// where they are
	d = dec("init", true);
	check(d.known && d.allowed && d.do_init && !d.do_powercycle
			&& !d.do_halt && !d.do_resume && d.set_powered == -1,
			"init -> pulse INIT only");
	check(!d.devices_off && !d.devices_on, "init leaves the cards in the machine");

	// a power cycle keeps the running configuration: the DIP switches are read
	// when the backend starts and nowhere else. Every card comes out and goes
	// back, which is what drops the state a card loses with its supply.
	d = dec("powercycle", true);
	check(d.known && d.allowed && d.do_powercycle && !d.do_init
			&& d.set_powered == -1,
			"powercycle -> power cycle, keeping the running configuration");
	check(d.devices_off && d.devices_on, "powercycle rebuilds every card");

	// restart reboots the CPU from its power-up vector: release HALT, then power
	// cycle. A bare INIT would not restart the CPU, so restart drives the power
	// cycle rather than a lone bus INIT.
	d = dec("restart", true);
	check(d.known && d.allowed && d.do_resume && d.do_powercycle
			&& !d.do_init && !d.do_halt && d.set_powered == -1,
			"restart -> release HALT then power cycle");
	check(d.devices_off && d.devices_on, "restart rebuilds every card");

	d = dec("halt", true);
	check(d.known && d.allowed && d.do_halt && !d.do_resume
			&& d.set_powered == -1, "halt -> assert HALT");
	check(!d.devices_off && !d.devices_on, "halt leaves the cards in the machine");

	d = dec("continue", true);
	check(d.known && d.allowed && d.do_resume && !d.do_halt
			&& d.set_powered == -1, "continue -> release HALT");
	check(!d.devices_off && !d.devices_on,
			"continue leaves the cards in the machine");

	// dc_off halts the CPU, takes the cards out and clears the power flag, so
	// the machine reads as switched off: lamps dark, drives spun down, units
	// offline.
	d = dec("dc_off", true);
	check(d.known && d.allowed && d.do_halt && d.set_powered == 0
			&& !d.do_powercycle, "dc_off -> halt and clear powered");
	check(d.devices_off && !d.devices_on, "dc_off takes the cards out and leaves them out");

	// dc_on sets the flag, releases HALT, puts the cards back and power cycles
	// the machine up. The resume clears the HALT a preceding dc_off left
	// asserted, so the CPU comes up running rather than halted into ODT.
	d = dec("dc_on", true);
	check(d.known && d.allowed && d.do_powercycle && d.do_resume
			&& !d.do_halt && d.set_powered == 1,
			"dc_on -> release HALT, power cycle, set powered");
	check(!d.devices_off && d.devices_on, "dc_on brings the cards back up");

	d = dec("bogus", true);
	check(!d.known, "unknown action -> not known");

	// Powered off: only dc_on transitions back up; the run controls are refused.
	d = dec("restart", false);
	check(d.known && !d.allowed, "restart refused while powered off");
	d = dec("halt", false);
	check(d.known && !d.allowed, "halt refused while powered off");
	d = dec("continue", false);
	check(d.known && !d.allowed, "continue refused while powered off");

	d = dec("dc_on", false);
	check(d.known && d.allowed && d.do_powercycle && d.do_resume
			&& d.set_powered == 1,
			"dc_on allowed while powered off, releases HALT and sets powered");

	d = dec("dc_on", false);
	check(!d.devices_off && d.devices_on,
			"dc_on brings the cards back up from a dark machine");

	// init/powercycle are not run controls; they are not gated.
	d = dec("init", false);
	check(d.known && d.allowed && d.do_init, "init allowed while powered off");
	d = dec("powercycle", false);
	check(d.known && d.allowed && d.do_powercycle,
			"powercycle allowed while powered off");
	// A machine with no power has its cards out already, and they stay out: the
	// bus edges alone reach the CPU, and only dc_on switches the machine on.
	check(!d.devices_off && !d.devices_on,
			"a power cycle of a dark machine leaves the cards out");
}

int main(void) {
	test_disk_status();
	test_power_gate();

	printf("status_power_test: %d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
