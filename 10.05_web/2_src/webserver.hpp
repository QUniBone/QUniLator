/* webserver.hpp: embedded HTTP/WebSocket server for the web interface

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org

   Permission is hereby granted, free of charge, to any person obtaining a
   copy of this software and associated documentation files (the "Software"),
   to deal in the Software without restriction, including without limitation
   the rights to use, copy, modify, merge, publish, distribute, sublicense,
   and/or sell copies of the Software, and to permit persons to whom the
   Software is furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
   THE AUTHOR BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
   IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
   CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

   The server (civetweb) runs in its own worker threads. It serves the static
   frontend from the document root and the JSON API under /api/. See
   10.05_web/docs/plan.md for the architecture.
*/
#ifndef _WEBSERVER_HPP_
#define _WEBSERVER_HPP_

#include <string>

#include "logsource.hpp"

struct mg_context; // civetweb, opaque here

// implemented in webapi.cpp: registers the /api/ and /ws/ handlers that need
// the device registry and the application (the host test build stubs these)
void webapi_register(struct mg_context *ctx);
void webapi_shutdown(void);

class webserver_c: public logsource_c {
private:
	struct mg_context *ctx = nullptr;
	unsigned port;
	std::string docroot;

public:
	webserver_c(unsigned listen_port, std::string document_root);
	~webserver_c();

	// starts the worker threads; false on error (port in use, bad docroot)
	bool start(void);
	void stop(void);
	bool is_running(void) {
		return ctx != nullptr;
	}
	// The civetweb context the handlers are registered against, for a component
	// that is registered again after having been shut down mid-run.
	struct mg_context *context(void) {
		return ctx;
	}
};

extern webserver_c *webserver; // singleton, created by application on --web

#endif // _WEBSERVER_HPP_
