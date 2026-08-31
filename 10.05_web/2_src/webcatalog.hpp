/* webcatalog.hpp: /api/catalog — configurations offered by subscribed catalogues

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.
*/
#ifndef _WEBCATALOG_HPP_
#define _WEBCATALOG_HPP_

#include <string>

struct mg_context;

// register /api/catalog; the index cache and the staged download live in
// $QUNILATOR_DIR/catalog
void webcatalog_register(struct mg_context *ctx);

// True when the job status changed since the last call; the events broadcast
// loop asks at 10 Hz and pushes webcatalog_event_json() when it did.
bool webcatalog_poll(void);

// the job status as a {"t":"catalog",...} events frame
std::string webcatalog_event_json(void);

#endif // _WEBCATALOG_HPP_
