/* webevents.hpp: /ws/events — live state stream of the web interface

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.
*/
#ifndef _WEBEVENTS_HPP_
#define _WEBEVENTS_HPP_

struct mg_context;

// register /ws/events, install the parameter/logger observers,
// start the broadcast thread
void webevents_register(struct mg_context *ctx);
// remove the observers and stop the broadcast thread
void webevents_shutdown(void);

// record a bus control action (init/powercycle/halt) for the state event
void webevents_note_halt(bool halted);

// set the logical power flag and publish it in the state event. Runtime only:
// a service restart comes up powered on. While powered off the dashboard shows
// a frozen, dark machine and the run controls are refused.
void webevents_note_powered(bool powered);

// current logical power state
bool webevents_is_powered(void);

// Publish a config event now: the current/default configuration changed, or a
// caller wants the modified flag re-evaluated. The 10 Hz poll also emits one
// whenever the computed state flips, so this need only be called on the
// explicit transitions (apply, save, default change, rename).
void webevents_note_config(void);

// current (soft) halt state, as last set via the control API
bool webevents_is_halted(void);

#endif // _WEBEVENTS_HPP_
