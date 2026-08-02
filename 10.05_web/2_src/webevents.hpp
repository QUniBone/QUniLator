/* webevents.hpp: /ws/events — live state stream of the web interface

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.
*/
#ifndef _WEBEVENTS_HPP_
#define _WEBEVENTS_HPP_

#include <cstdint>
#include <string>

#include "picojson.h"

struct mg_context;

// register /ws/events, install the parameter/logger observers,
// start the broadcast thread
void webevents_register(struct mg_context *ctx);
// remove the observers and stop the broadcast thread
void webevents_shutdown(void);

// record a bus control action (init/powercycle/halt) for the state event
void webevents_note_halt(bool halted);

// Publish a committed parameter change. The device parameters publish
// themselves through the change hook; this is for a value an endpoint commits
// somewhere other than the parameter — an edit to the machine the board carries
// while its power is off, which reaches no device until the machine comes up.
void webevents_note_param(const std::string &dev, const std::string &param,
		const picojson::value &value);

// set the logical power flag and publish it in the state event. Runtime only:
// a service restart comes up powered on. While powered off the dashboard shows
// a frozen, dark machine and the run controls are refused.
void webevents_note_powered(bool powered);

// current logical power state
bool webevents_is_powered(void);

// Hold the board for work no interface may act during, naming what holds it:
// the checks a power-up runs before it drives the bus, or the interactive menu
// having the hardware. The reason reaches every connected page in the state
// frame, and the API answers 409 to the requests that would change the machine
// while it is held. Prefer board_hold_c, which releases on every path out.
void webevents_hold_board(const std::string &reason);
void webevents_release_board(void);
// what holds the board, "" when nothing does
std::string webevents_board_held_by(void);

// Holds the board for as long as it is in scope. `take` false leaves the board
// free, so a caller whose work only sometimes needs the hold writes one object
// rather than a pointer to one.
class board_hold_c {
	bool held;
public:
	board_hold_c(const std::string &reason, bool take = true) : held(take) {
		if (held)
			webevents_hold_board(reason);
	}
	~board_hold_c() {
		if (held)
			webevents_release_board();
	}
	board_hold_c(const board_hold_c &) = delete;
	board_hold_c &operator=(const board_hold_c &) = delete;
};

// Publish a config event now: the current/default configuration changed, or a
// caller wants the modified flag re-evaluated. Call this on the explicit
// transitions (apply, save, default change, rename).
void webevents_note_config(void);

// Publish a settings event: a machine setting changed. The frame carries no
// payload, so a page rereads /api/settings for what they now are.
void webevents_note_settings(void);

// Something that could have moved the "modified" flag has happened, so the poll
// should recompute it once. Answering that question is expensive - a snapshot of
// every enabled device compared against the saved file - so it is not asked on a
// timer. A committed parameter change marks it here on its own.
void webevents_note_config_dirty(void);

// current (soft) halt state, as last set via the control API
bool webevents_is_halted(void);

// The board's 4 DIP switches (SW0..SW3) read as one value, 0..15, SW0 the least
// significant bit. -1 when there is no GPIO hardware (the host build), so a
// caller can fall back rather than treat "no switches" as switch value 0.
int webevents_dip_value(void);

// Load the persisted log journal (<dir>/log.jsonl) and open it for append.
// Called once at startup before the log sink is installed.
void webevents_log_init(const std::string &dir);

// A page of the log journal as JSON {entries:[…newest first…], more}: up to
// `limit` (default 200, max 1000) entries with id < `before` (0 → the latest).
std::string webevents_log_page_json(uint64_t before, unsigned limit);

#endif // _WEBEVENTS_HPP_
