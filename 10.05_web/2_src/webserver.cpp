/* webserver.cpp: embedded HTTP/WebSocket server for the web interface

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
*/

#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <sys/stat.h>
#include <string>

#include "civetweb.h"
#include "picojson.h"

#include "logger.hpp"
#include "webauth.hpp"
#include "webserver.hpp"
#include "webstorage.hpp"
#include "qunibusadapter.hpp"
#include "qunibus.h"		// QUNIBONE_NAME (the display brand)

webserver_c *webserver = nullptr;

// force every civetweb thread to time-share scheduling. The device workers
// run SCHED_RR on this single-core machine; a web thread must never share
// that real-time band, or a dashboard request round-robins with the bus
// servicer and the DELQA reflection worker, blowing the firmware's ~33ms
// self-test poll window.
static void *webserver_init_thread(const struct mg_context *ctx, int thread_type) {
	(void) ctx;
	(void) thread_type;
	struct sched_param param;
	param.sched_priority = 0;
	pthread_setschedparam(pthread_self(), SCHED_OTHER, &param);
	return nullptr;
}

/*** HTTP basic auth. Any user name is accepted, the password must match the
     one webauth.cpp holds - set through the web interface, or given as
     WEBUI_PASSWORD in the environment. A board with no password yet is open,
     which is how the frontend reaches /api/auth to set one.
     Browsers replay the credentials on the WebSocket handshakes, so /ws/
     is covered as well. ***/

// decode base64 into out; result false on illegal input
static bool base64_decode(const char *in, std::string *out) {
	static const char *alphabet =
			"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	unsigned bits = 0, value = 0;
	out->clear();
	for (; *in && *in != '='; in++) {
		const char *pos = strchr(alphabet, *in);
		if (pos == nullptr)
			return false;
		value = (value << 6) | (pos - alphabet);
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out->push_back((char) (value >> bits));
		}
	}
	return true;
}

// The single-page frontend routes on History-API paths, so a reload or a deep
// link to a client route (e.g. /config/211bsd) must return the SPA shell for
// the client router to resolve. This is scoped so it never shadows the API or
// the WebSockets: /api/ and /ws/ are left to their handlers, an existing static
// asset is served normally, and only an otherwise-unresolved GET falls back to
// index.html. mg_send_file routes through civetweb's static handler, so the
// shell keeps its ETag and static_file_max_age revalidation. Non-GET methods on
// unknown paths still 404 through civetweb's default handling.
static int spa_shell_fallback(struct mg_connection *conn) {
	const struct mg_request_info *ri = mg_get_request_info(conn);
	if (ri->request_method == nullptr || strcmp(ri->request_method, "GET") != 0)
		return 0;
	const char *uri = ri->local_uri != nullptr ? ri->local_uri : "/";
	if (strncmp(uri, "/api/", 5) == 0 || strncmp(uri, "/ws/", 4) == 0)
		return 0; // belongs to a registered handler
	const char *root = mg_get_option(mg_get_context(conn), "document_root");
	if (root == nullptr)
		return 0;
	// civetweb cleans local_uri of "." and ".." segments, so this cannot escape
	// the document root
	std::string path = std::string(root) + uri;
	struct stat st;
	if (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode))
		return 0; // an existing asset: let civetweb serve it
	std::string shell = std::string(root) + "/index.html";
	mg_send_file(conn, shell.c_str());
	return 1;
}

// result 0: request continues (authorized or auth disabled), 1: handled here
static int begin_request_handler(struct mg_connection *conn) {
	if (webauth_configured()) {
		const char *auth = mg_get_header(conn, "Authorization");
		bool ok = false;
		if (auth != nullptr && strncmp(auth, "Basic ", 6) == 0) {
			std::string credentials; // "user:password", any user accepted
			if (base64_decode(auth + 6, &credentials)) {
				size_t colon = credentials.find(':');
				if (colon != std::string::npos
						&& webauth_verify(credentials.substr(colon + 1)))
					ok = true;
			}
		}
		if (!ok) {
			mg_printf(conn,
					"HTTP/1.1 401 Unauthorized\r\n"
					"WWW-Authenticate: Basic realm=\"" QUNIBONE_NAME "\"\r\n"
					"Content-Length: 0\r\n\r\n");
			return 1;
		}
	}
	return spa_shell_fallback(conn);
}

#if defined(QBUS)
static const char *platform_name = "QBUS";
#elif defined(UNIBUS)
static const char *platform_name = "UNIBUS";
#else
static const char *platform_name = "HOST"; // host-side test build
#endif

// GET /api/state — phase 0: identifies the platform and the API generation.
// Bus/device state fields are added with the corresponding phases.
// GET  /api/latency  - how long the PRU was left holding the bus
// POST /api/latency  - start a fresh measurement
//
// The maximum is the number that matters: one late wakeup stretches a QBUS
// cycle far enough for the PDP-11 to call it a timeout, and an average would
// bury it. The histogram shows whether the tail is a cliff or a slope.
static int api_latency_handler(struct mg_connection *conn, void * /*cbdata*/) {
	const struct mg_request_info *ri = mg_get_request_info(conn);
	event_latency_c &lat = qunibusadapter->event_latency;

	if (!strcmp(ri->request_method, "POST")) {
		lat.reset();
	}

	picojson::object o;
	o["available"] = picojson::value(lat.counter != NULL);
	o["count"] = picojson::value((double) lat.count);
	o["max_us"] = picojson::value((double) event_latency_c::cycles_to_us(lat.max_cycles));
	o["mean_us"] = picojson::value((double) event_latency_c::cycles_to_us(lat.mean_cycles()));

	picojson::array hist;
	for (unsigned b = 0; b < EVENT_LATENCY_BUCKETS; b++) {
		if (lat.bucket[b] == 0)
			continue;
		picojson::object e;
		e["from_us"] = picojson::value((double) event_latency_c::bucket_floor_us(b));
		e["count"] = picojson::value((double) lat.bucket[b]);
		hist.push_back(picojson::value(e));
	}
	o["histogram"] = picojson::value(hist);

	std::string body = picojson::value(o).serialize();
	mg_printf(conn,
			"HTTP/1.1 200 OK\r\n"
			"Content-Type: application/json\r\n"
			"Cache-Control: no-store\r\n"
			"Content-Length: %u\r\n\r\n", (unsigned) body.size());
	mg_write(conn, body.c_str(), body.size());
	return 200;
}

static int api_state_handler(struct mg_connection *conn, void * /*cbdata*/) {
	picojson::object state;
	state["platform"] = picojson::value(platform_name);
	state["api_version"] = picojson::value((double)0);
	// the directory the interface manages: a drive holding an image from
	// anywhere else is shown by its full path, not by name alone
	state["images_dir"] = picojson::value(webstorage_images_dir());
	std::string body = picojson::value(state).serialize();
	mg_printf(conn,
			"HTTP/1.1 200 OK\r\n"
			"Content-Type: application/json\r\n"
			"Cache-Control: no-store\r\n"
			"Content-Length: %u\r\n\r\n", (unsigned) body.size());
	mg_write(conn, body.c_str(), body.size());
	return 200;
}

webserver_c::webserver_c(unsigned listen_port, std::string document_root) :
		port(listen_port), docroot(document_root) {
	log_label = "websrv";
}

webserver_c::~webserver_c() {
	stop();
}

bool webserver_c::start(void) {
	char portstr[16];
	snprintf(portstr, sizeof(portstr), "%u", port);
	// each connected WebSocket occupies one worker thread for its lifetime
	// (2 per open page: events + console), so the pool must cover several
	// browser sessions plus concurrent REST requests
	//
	// the frontend is a single file that changes with every deploy, so browsers
	// must revalidate it rather than serve an hour-old copy from cache; the
	// ETag makes that a 304 in the common case
	const char *options[] = { //
			"document_root", docroot.c_str(), //
			"listening_ports", portstr, //
			// One worker thread is tied up for the life of every WebSocket, so
			// the pool must cover several open pages (each holds a few) plus
			// concurrent REST requests without starving new connections.
			"num_threads", "48", //
			// A page that navigates away or a laptop that sleeps leaves a
			// WebSocket half-open with no close. Ping it, and drop it after a
			// few unanswered pings, so its worker thread returns to the pool
			// instead of leaking until the pool is exhausted.
			"enable_websocket_ping_pong", "yes", //
			"websocket_timeout_ms", "5000", //
			// Bounds any socket write that does block (the broadcasters avoid it,
			// but this backstops a client that stalls mid-message) so it cannot
			// hold a worker for the 30 s default.
			"request_timeout_ms", "5000", //
			"enable_directory_listing", "no", //
			"static_file_max_age", "0", //
			// civetweb does not know the extension, and would serve the PWA
			// manifest as text/plain
			"extra_mime_types", ".webmanifest=application/manifest+json", //
			nullptr };

	// before the settings file is read, so WEBUI_PASSWORD outranks what it holds
	webauth_init();

	struct mg_callbacks callbacks;
	memset(&callbacks, 0, sizeof(callbacks));
	callbacks.begin_request = begin_request_handler;
	callbacks.init_thread = webserver_init_thread;

	mg_init_library(0);
	ctx = mg_start(&callbacks, nullptr, options);
	if (ctx == nullptr) {
		ERROR("web server failed to start on port %u, document root %s", port, docroot.c_str());
		mg_exit_library();
		return false;
	}
	mg_set_request_handler(ctx, "/api/state", api_state_handler, nullptr);
	mg_set_request_handler(ctx, "/api/latency", api_latency_handler, nullptr);
	webauth_register(ctx);
	webapi_register(ctx);
	INFO("web server listening on port %u, document root %s, %s", port, docroot.c_str(),
			webauth_configured() ? "basic auth enabled"
					: "open until an admin password is set");
	return true;
}

void webserver_c::stop(void) {
	if (ctx == nullptr)
		return;
	webapi_shutdown();
	mg_stop(ctx);
	ctx = nullptr;
	mg_exit_library();
	INFO("web server stopped");
}
