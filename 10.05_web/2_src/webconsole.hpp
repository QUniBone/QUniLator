/* webconsole.hpp: /ws/console/0 — DL11 console over WebSocket

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.
*/
#ifndef _WEBCONSOLE_HPP_
#define _WEBCONSOLE_HPP_

struct mg_context;

// register /ws/console/0, install the xmt tap on the console DL11,
// start the flush thread
void webconsole_register(struct mg_context *ctx);
// remove the tap and stop the flush thread
void webconsole_shutdown(void);

// Forget what every emulated console has printed, so a machine powered on does
// not hand a reconnecting terminal the last machine's screen.
void webconsole_clear(void);


class console_recorder_c;
// The session recorder for an emulated console line: 0 and 1 are the DL11
// taps, 2 the emulated VAX console where the platform has one.
console_recorder_c *webconsole_channel_recorder(unsigned index);

#endif // _WEBCONSOLE_HPP_
