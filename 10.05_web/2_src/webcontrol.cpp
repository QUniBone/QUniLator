/* webcontrol.cpp: pure decision for POST /api/control actions

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.
*/

#include "webcontrol.hpp"

control_decision_c control_decide(const std::string &action, bool powered) {
	control_decision_c d;

	// While powered off, only dc_on transitions the machine back up; the run
	// controls are refused (and disabled in the UI).
	if (!powered && (action == "restart" || action == "halt"
			|| action == "continue")) {
		d.known = true;
		d.allowed = false;
		return d;
	}

	if (action == "init") {
		d.known = true;
		d.do_init = true;
	} else if (action == "powercycle") {
		d.known = true;
		d.do_powercycle = true;
	} else if (action == "restart") {
		// Restart the machine from its power-up vector, the 11/03 RESTART. A bare
		// bus INIT resets the devices but does not vector the CPU, so a real
		// restart releases the HALT line and runs a power cycle: its DCOK/POK
		// sequence asserts BINIT and brings the CPU up executing from the
		// power-up vector rather than continuing where it was.
		d.known = true;
		d.do_resume = true;
		d.do_powercycle = true;
	} else if (action == "halt") {
		d.known = true;
		d.do_halt = true;
	} else if (action == "continue") {
		d.known = true;
		d.do_resume = true;
	} else if (action == "dc_on") {
		// power up: set the flag, release the HALT the preceding dc_off left on
		// the bus, and bring the machine up running with a power cycle. The
		// handler releases HALT before the cycle so the CPU comes up executing,
		// not halted into ODT.
		d.known = true;
		d.set_powered = 1;
		d.do_resume = true;
		d.do_powercycle = true;
	} else if (action == "dc_off") {
		// power down: halt the CPU and clear the flag
		d.known = true;
		d.do_halt = true;
		d.set_powered = 0;
	}
	return d;
}
