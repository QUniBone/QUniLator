/* websettings.hpp: /api/settings — global machine settings

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.
*/
#ifndef _WEBSETTINGS_HPP_
#define _WEBSETTINGS_HPP_

#include <string>
#include <vector>

struct mg_context;

// Read $QUNILATOR_DIR/settings.json. Called before the device set is built,
// which is why it is separate from registering the endpoint.
void websettings_startup(void);

// register /api/settings; persisted in $QUNILATOR_DIR/settings.json
void websettings_register(struct mg_context *ctx);

// Whether this board runs the emulated KA11 (PDP-11/20) instead of serving a
// physical CPU. Read at startup; a change takes effect at the next start.

// Whether the board keeps its bus internal instead of driving a backplane. The
// firmware is chosen when the PRU is loaded, so a change takes effect at the
// next start. Independent of the emulated CPU.
bool websettings_internal_bus(void);
void websettings_set_internal_bus(bool on);

// Write settings.json now. The admin password lives in the same file, so
// webauth.cpp calls this when it changes.
void websettings_save(void);

// The update version an operator has told the interface to stop announcing. It
// belongs to the board rather than to one browser, so it is persisted here; a
// later version announces itself again.
std::string websettings_dismissed_version(void);
void websettings_set_dismissed_version(const std::string &version);

// $QUNILATOR_DIR, the state directory the service was given.
std::string websettings_state_dir(void);

// The catalogue index URLs this board subscribes to, in the order the
// interface shows them. Board-level so every operator sees the same list; a
// fresh board carries the project's own catalogue. An operator may empty the
// list, and the empty list persists.
std::vector<std::string> websettings_catalog_sources(void);
void websettings_set_catalog_sources(const std::vector<std::string> &sources);

// external console selection, consumed by the ttyS2 bridge (/ws/console/ext)
struct external_console_c {
	std::string source; // "webserial" | "ttys2" | "off"
	std::string port;   // Linux tty, e.g. "/dev/ttyS2"
	unsigned baud;
};
external_console_c websettings_external_console(void);

#endif // _WEBSETTINGS_HPP_
