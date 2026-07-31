/* webconsole_ext.hpp: /ws/console/ext — external tty console over WebSocket

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.
*/
#ifndef _WEBCONSOLE_EXT_HPP_
#define _WEBCONSOLE_EXT_HPP_

#include <string>

struct mg_context;

// register /ws/console/ext (raw tty bridge) and start the reader thread
void webconsole_ext_register(struct mg_context *ctx);
void webconsole_ext_shutdown(void);

// Forget what the external console has printed (see webconsole_clear).
void webconsole_ext_clear(void);

// apply the external-console setting: open/reopen the tty when
// source == "ttys2", close it otherwise. Returns "" on success, or a
// human-readable reason on failure (port in use by an enabled DL11,
// open failed) — surfaced to the client as a warning.
std::string webconsole_ext_configure(const std::string &source,
		const std::string &port, unsigned baud);

#endif // _WEBCONSOLE_EXT_HPP_
