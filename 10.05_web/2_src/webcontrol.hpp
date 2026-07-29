/* webcontrol.hpp: pure decision for POST /api/control actions

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.

   The bus/CPU effect of a control action and the logical power-flag gate,
   factored out of the HTTP handler so the power-flag semantics are host
   testable without civetweb or the bus. The handler in webapi.cpp queries the
   live `powered` flag, asks control_decide() what to do, and carries out the
   effects (init/powercycle/halt/resume) and the power-flag change.
*/
#ifndef _WEBCONTROL_HPP_
#define _WEBCONTROL_HPP_

#include <string>

// what a control action does, given the current logical power state
struct control_decision_c {
	bool known = false;      // false → unknown action, answer 400
	bool allowed = true;     // false → refused while powered off, answer 409
	bool do_init = false;    // pulse bus INIT
	bool do_powercycle = false; // simulated DCOK/POK power cycle
	bool do_halt = false;    // assert the HALT line
	bool do_resume = false;  // release the HALT line (run)
	int set_powered = -1;    // -1 leave, 0 clear, 1 set the power flag
};

// Decide the effect of `action` when the machine's power flag is `powered`.
// Actions: init, powercycle, restart (release HALT + power cycle, so the CPU
// restarts from the power-up vector), halt, continue, dc_on (release HALT +
// power up), dc_off (halt + power down). While powered is false,
// restart/halt/continue are refused (allowed=false); dc_on is the only
// transition back up.
//
// The handler releases HALT (do_resume) before any power-up (do_init /
// do_powercycle) and asserts HALT (do_halt) after it, so a machine brought up
// by dc_on or restart comes up running rather than halted into ODT.
control_decision_c control_decide(const std::string &action, bool powered);

#endif // _WEBCONTROL_HPP_
