/* webselftest.hpp: /api/selftest and /ws/selftest — hardware self-tests

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.
*/
#ifndef _WEBSELFTEST_HPP_
#define _WEBSELFTEST_HPP_

#include <string>

struct mg_context;

// register /api/selftest (catalog, run, stop) and /ws/selftest (the running
// test's output stream, with replay)
void webselftest_register(struct mg_context *ctx);

// stop a running test child and wait it out, so the service's shutdown closes
// the child's board claim in order
void webselftest_shutdown(void);

// For the events broadcast loop, like webupdate: poll() says whether a run
// started or ended since the last call, event_json() is the {"t":"selftest"}
// frame. The frame is also sent to every /ws/events client on connect.
bool webselftest_poll(void);
std::string webselftest_event_json(void);

#endif // _WEBSELFTEST_HPP_
