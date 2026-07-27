/* websettings.hpp: /api/settings — global machine settings

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.
*/
#ifndef _WEBSETTINGS_HPP_
#define _WEBSETTINGS_HPP_

#include <string>

struct mg_context;

// register /api/settings; persisted in $QUNIBONE_DIR/settings.json
void websettings_register(struct mg_context *ctx);

// Write settings.json now. The admin password lives in the same file, so
// webauth.cpp calls this when it changes.
void websettings_save(void);

// external console selection, consumed by the ttyS2 bridge (/ws/console/ext)
struct external_console_c {
	std::string source; // "webserial" | "ttys2" | "off"
	std::string port;   // Linux tty, e.g. "/dev/ttyS2"
	unsigned baud;
};
external_console_c websettings_external_console(void);

#endif // _WEBSETTINGS_HPP_
