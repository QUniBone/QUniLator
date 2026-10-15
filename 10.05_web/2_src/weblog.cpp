/* weblog.cpp: the web interface's log source

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.
*/

#include "civetweb.h"

#include "weblog.hpp"

logsource_c *weblog(void) {
	// Constructed on first use and never destroyed: endpoints log from
	// civetweb's threads, and a static destructor could unregister it from the
	// logger while one of them is still running.
	static logsource_c *instance = nullptr;
	if (instance == nullptr) {
		instance = new logsource_c();
		instance->log_label = "web";
	}
	return instance;
}

int weblog_civetweb_message(const struct mg_connection *conn, const char *message) {
	const struct mg_request_info *ri = conn ? mg_get_request_info(conn) : nullptr;
	if (ri != nullptr && ri->request_method != nullptr)
		WEB_WARNING("%s %s: %s", ri->request_method,
				ri->local_uri ? ri->local_uri : "", message);
	else
		WEB_WARNING("%s", message);
	return 1; // logged; civetweb writes nothing of its own
}
