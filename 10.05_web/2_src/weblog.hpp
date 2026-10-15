/* weblog.hpp: the web interface's log source

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.

   The API endpoints are free functions, while the logging macros address the
   logsource_c of the surrounding object. They log through the one below, so
   what an operator does over the web appears in the log under "web" with a
   timestamp, next to what the devices report.

   Everything routed here goes through the logger's own output thread, which
   drops messages rather than making the caller wait for a slow sink. That
   matters for the endpoints that write while holding the device operations
   lock: a printf straight to stdout blocks there when the journal is behind,
   and every other request waits with it.
*/
#ifndef _WEBLOG_HPP_
#define _WEBLOG_HPP_

#include "logger.hpp"
#include "logsource.hpp"

// the log source shared by the web API endpoints
logsource_c *weblog(void);

// civetweb's log_message callback: what the server itself reports - a file an
// upload could not create or write, a request it could not read - is logged
// here under "web" with the request it belongs to.
struct mg_connection;
int weblog_civetweb_message(const struct mg_connection *conn, const char *message);

#define WEB_INFO(...)	\
	logger->log(weblog(), LL_INFO, false, __FILE__, __LINE__, __VA_ARGS__)
#define WEB_WARNING(...)	\
	logger->log(weblog(), LL_WARNING, false, __FILE__, __LINE__, __VA_ARGS__)
#define WEB_ERROR(...)	\
	logger->log(weblog(), LL_ERROR, false, __FILE__, __LINE__, __VA_ARGS__)

#endif // _WEBLOG_HPP_
