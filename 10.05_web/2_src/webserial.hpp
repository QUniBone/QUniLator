/* webserial.hpp: /ws/serial/<dev>/<line> — mux/SLU serial lines over WebSocket

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.
*/
#ifndef _WEBSERIAL_HPP_
#define _WEBSERIAL_HPP_

struct mg_context;

// register /ws/serial/<dev>/<line> and start the transmit-flush thread
void webserial_register(struct mg_context *ctx);
// stop the flush thread and drop all clients
void webserial_shutdown(void);

#endif // _WEBSERIAL_HPP_
