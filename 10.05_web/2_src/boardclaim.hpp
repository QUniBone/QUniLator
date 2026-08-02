/* boardclaim.hpp: one program drives the board at a time

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.
*/
#ifndef _BOARDCLAIM_HPP_
#define _BOARDCLAIM_HPP_

#include <functional>
#include <string>

// A board is one set of hardware: one PRU, one bus interface, one set of
// latches. The service holds it while it serves, and the interactive menu takes
// it over for the length of a session.
//
// The rendezvous is a Unix socket the service listens on, and a claim lives
// exactly as long as the connection: the menu holds it open for its run, so a
// menu that exits, is killed or dies takes the board's return with it. Nothing
// has to notice a process is gone.
//
// One menu runs at a time whether or not a service is there to yield to it: the
// lock file is taken before the socket, because a board with no service running
// has nobody to arbitrate between two menus.

// Where the two meet. QUNILATOR_BOARD_SOCKET and QUNILATOR_BOARD_LOCK override
// them, which is what lets a test run against paths of its own.
std::string boardclaim_socket_path(void);
std::string boardclaim_lock_path(void);

// ---- the service side ----------------------------------------------------

// What the service does with the hardware when a menu asks for it. `yield`
// gives the board up - the device set down, the PRU released - and `resume`
// takes it back and rebuilds the machine. Neither is called with the other
// running, and `resume` follows every `yield`.
struct boardclaim_handlers_c {
	std::function<void()> yield;
	std::function<void()> resume;
};

// Listen for a menu asking for the board. Answers each claim by yielding, and
// takes the board back when the claim's connection closes. False when the
// socket cannot be served, with the reason in `error`; the service still runs,
// it just cannot hand the board over.
bool boardclaim_serve(const boardclaim_handlers_c &handlers, std::string *error);
void boardclaim_stop_serving(void);

// ---- the menu side -------------------------------------------------------

// Take the board for this process, waiting out the service's teardown. False
// when another menu already holds it, or the service will not give it up, with
// the reason in `error`. The claim is held until the process ends.
bool boardclaim_take(std::string *error);
// Give the board back. Called on the way out; the claim ends with the process
// either way.
void boardclaim_release(void);

#endif // _BOARDCLAIM_HPP_
