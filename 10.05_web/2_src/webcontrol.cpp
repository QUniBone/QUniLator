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
		// A DCOK/POK cycle that resets the devices and CPU but keeps the running
		// configuration. The DIP switches are read only when the backend starts;
		// a power cycle does not re-select from them. To load the machine the
		// switches name, change them and restart the backend.
		//
		// The cards are rebuilt with the cycle: every device is taken out of the
		// machine and put back, which drops the emulated state a card loses with
		// its supply — worker threads, controller state machines, drive
		// mechanics and the media those drives hold. A machine already switched
		// off has its cards out and keeps them out; the bus edges alone reach
		// the CPU.
		d.known = true;
		d.do_powercycle = true;
		d.devices_off = d.devices_on = powered;
	} else if (action == "restart") {
		// Restart the machine from its power-up vector, the 11/03 RESTART. A bare
		// bus INIT resets the devices but does not vector the CPU, so a real
		// restart releases the HALT line and runs a power cycle: its DCOK/POK
		// sequence asserts BINIT and brings the CPU up executing from the
		// power-up vector rather than continuing where it was.
		d.known = true;
		d.do_resume = true;
		d.do_powercycle = true;
		d.devices_off = d.devices_on = true;
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
		// not halted into ODT. The running configuration is kept: the AUX ON/OFF
		// switch powers the machine, it does not re-read the DIP switches.
		// The cards dc_off took out come back with the supply, each holding the
		// medium it held, and the CPU gets its power-up edge after them.
		d.known = true;
		d.set_powered = 1;
		d.do_resume = true;
		d.devices_on = true;
		d.do_powercycle = true;
	} else if (action == "dc_off") {
		// power down: halt the CPU, take the cards out of the machine and clear
		// the flag, so a switched-off machine reads as one — lamps dark, drives
		// spun down, units offline
		d.known = true;
		d.do_halt = true;
		d.devices_off = true;
		d.set_powered = 0;
	}
	return d;
}
