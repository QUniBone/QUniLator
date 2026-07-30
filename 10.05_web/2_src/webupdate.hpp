/* webupdate.hpp: /api/update — the self-update the interface drives

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.
*/
#ifndef _WEBUPDATE_HPP_
#define _WEBUPDATE_HPP_

#include <string>

struct mg_context;

// register /api/update and its subresources
void webupdate_register(struct mg_context *ctx);

// The current update status, as the /ws/events "update" frame. Empty before the
// first status file has been read.
std::string webupdate_event_json(void);

// Called from the event-poll thread: stat the status file and report whether it
// has changed, so the caller broadcasts a fresh "update" frame. Rate-limits
// itself to one stat a second.
bool webupdate_poll(void);

#endif // _WEBUPDATE_HPP_
