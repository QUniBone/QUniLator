/* webselftest.cpp: /api/selftest and /ws/selftest — hardware self-tests

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.

   The tests run in "<name>-cli --selftest <test>" (see selftest_runner.hpp for
   why a child process). The child takes the board claim, so the service's own
   yield/resume machinery puts the machine down and rebuilds it - nothing here
   touches the hardware. This module is the HTTP face: the catalog, run/stop,
   the output stream on /ws/selftest (a console channel, so a page that
   connects mid-run replays what it missed), and the {"t":"selftest"} frame on
   /ws/events.

   /api/selftest is exempt from the board-held 409 (webserver.cpp): the child
   holding the claim is exactly when Stop must still work. The run handler
   checks the holder itself, so the exemption opens nothing else.
*/

#include <atomic>
#include <string>

#include "civetweb.h"
#include "picojson.h"

#include "qunibus.h"
#include "weblog.hpp"
#include "webevents.hpp"
#include "webws.hpp"
#include "webconsole_channel.hpp"
#include "selftest_runner.hpp"
#include "webselftest.hpp"

// output stream of the running (and last) test; the ring is cleared when a
// run starts, so the replay a late page gets is this run's output
static console_channel_c channel(web_ws_console_send, web_ws_console_send_text);

static selftest_runner_c *runner = nullptr;
static std::atomic<bool> changed(false);

// bound on a caller-supplied --seconds: a day of soak test, not a typo
static const unsigned max_seconds = 86400;

static void send_json(struct mg_connection *conn, int status, const picojson::value &val) {
	std::string body = val.serialize();
	mg_printf(conn,
			"HTTP/1.1 %d %s\r\n"
			"Content-Type: application/json\r\n"
			"Cache-Control: no-store\r\n"
			"Content-Length: %u\r\n\r\n",
			status, status == 200 ? "OK" : (status == 202 ? "Accepted" : "Error"),
			(unsigned) body.size());
	mg_write(conn, body.c_str(), body.size());
}

static void send_error(struct mg_connection *conn, int status, const std::string &message) {
	picojson::object err;
	err["error"] = picojson::value(message);
	send_json(conn, status, picojson::value(err));
}

static std::string cli_path(void) {
	const char *override_path = getenv("QUNILATOR_CLI");
	if (override_path != nullptr && *override_path)
		return override_path;
	return "/usr/bin/" QUNILATOR_CLI_NAME;
}

// {"running":{...}|null,"last":{...}|null} from the runner's status.
// insert() rather than operator[]=: assigning a null picojson::value swaps an
// uninitialized union member, which -O3 reports as maybe-uninitialized.
static void status_members(picojson::object *out) {
	selftest_runner_c::status_t st = runner->status();
	if (st.running) {
		picojson::object run;
		run["test"] = picojson::value(st.test);
		run["started_at"] = picojson::value((double) st.started_at);
		out->insert(std::make_pair("running", picojson::value(run)));
	} else
		out->insert(std::make_pair("running", picojson::value()));
	if (!st.running && !st.test.empty()) {
		picojson::object last;
		last["test"] = picojson::value(st.test);
		last["verdict"] = picojson::value(st.verdict);
		last["exit_code"] = picojson::value((double) st.exit_code);
		last["started_at"] = picojson::value((double) st.started_at);
		last["ended_at"] = picojson::value((double) st.ended_at);
		out->insert(std::make_pair("last", picojson::value(last)));
	} else
		out->insert(std::make_pair("last", picojson::value()));
}

std::string webselftest_event_json(void) {
	if (runner == nullptr)
		return ""; // not registered (a host test's partial assembly)
	picojson::object ev;
	ev["t"] = picojson::value("selftest");
	status_members(&ev);
	return picojson::value(ev).serialize();
}

bool webselftest_poll(void) {
	return changed.exchange(false);
}

static void handle_get(struct mg_connection *conn) {
	picojson::array tests;
	for (const selftest_info_t &t : selftest_catalog()) {
		picojson::object o;
		o["id"] = picojson::value(t.id);
		o["label"] = picojson::value(t.label);
		o["category"] = picojson::value(t.category);
		o["description"] = picojson::value(t.description);
		o["warning"] = picojson::value(t.warning);
		o["unbounded"] = picojson::value(t.unbounded);
		o["default_seconds"] = picojson::value((double) t.default_seconds);
		tests.push_back(picojson::value(o));
	}
	picojson::object out;
	out["tests"] = picojson::value(tests);
	status_members(&out);
	send_json(conn, 200, picojson::value(out));
}

static void handle_run(struct mg_connection *conn) {
	char body[4096];
	int n = mg_read(conn, body, sizeof(body) - 1);
	if (n <= 0) {
		send_error(conn, 400, "a JSON body naming the test is required");
		return;
	}
	body[n] = 0;
	picojson::value req;
	if (!picojson::parse(req, body).empty() || !req.is<picojson::object>()) {
		send_error(conn, 400, "the body is not a JSON object");
		return;
	}
	std::string test = req.get("test").is<std::string>()
			? req.get("test").get<std::string>() : "";
	const selftest_info_t *info = selftest_info_by_id(test);
	if (info == nullptr) {
		send_error(conn, 400, "unknown self-test \"" + test + "\"");
		return;
	}
	unsigned seconds = info->default_seconds;
	if (req.get("seconds").is<double>()) {
		double s = req.get("seconds").get<double>();
		if (s < 0 || s > max_seconds) {
			send_error(conn, 400, "seconds must be 0 (until stopped) to 86400");
			return;
		}
		seconds = (unsigned) s;
	}

	if (runner->status().running) {
		send_error(conn, 409, "a self-test is already running");
		return;
	}
	// The exemption from the global board-held 409 makes this check ours: the
	// interactive menu (or a power-up's checks) holding the board refuses a
	// run the same way any other mutation is refused.
	std::string held = webevents_board_held_by();
	if (!held.empty()) {
		send_error(conn, 409, held);
		return;
	}

	unsigned addr_width = 0;
#if defined(QBUS)
	if (info->needs_addr_width) {
		addr_width = qunibus->addr_width;
		if (addr_width == 0) {
			send_error(conn, 409,
					"the QBUS address width is not set; set it in the machine settings first");
			return;
		}
	}
#endif

	channel.clear_ring();
	std::string banner = std::string("$ ") + QUNILATOR_CLI_NAME " --selftest "
			+ test + " " + std::to_string(seconds)
			+ (addr_width ? " --addresswidth " + std::to_string(addr_width) : "")
			+ "\n";
	channel.append(banner.c_str(), banner.size());

	std::string error;
	if (!runner->start(test, seconds, addr_width, &error)) {
		send_error(conn, 500, error);
		return;
	}
	WEB_INFO("selftest: started %s (%u s)", test.c_str(), seconds);
	picojson::object out;
	out["started"] = picojson::value(true);
	send_json(conn, 202, picojson::value(out));
}

static void handle_stop(struct mg_connection *conn) {
	if (!runner->stop()) {
		send_error(conn, 409, "no self-test is running");
		return;
	}
	WEB_INFO("selftest: stop requested");
	picojson::object out;
	out["stopping"] = picojson::value(true);
	send_json(conn, 202, picojson::value(out));
}

static int api_selftest_handler(struct mg_connection *conn, void *) {
	const struct mg_request_info *ri = mg_get_request_info(conn);
	std::string uri = ri->local_uri ? ri->local_uri : "";
	std::string tail = uri.substr(strlen("/api/selftest"));
	std::string method = ri->request_method ? ri->request_method : "";

	if (method == "GET" && (tail.empty() || tail == "/")) {
		handle_get(conn);
		return 200;
	}
	if (method == "POST" && tail == "/run") {
		handle_run(conn);
		return 200;
	}
	if (method == "POST" && tail == "/stop") {
		handle_stop(conn);
		return 200;
	}
	send_error(conn, 404, "unknown selftest endpoint");
	return 404;
}

// ---- /ws/selftest: the output stream -------------------------------------

static int ws_connect_handler(const struct mg_connection *, void *) {
	return 0; // accept; an idle channel just replays the last run
}

static void ws_ready_handler(struct mg_connection *conn, void *) {
	channel.add_client(conn);
}

static int ws_data_handler(struct mg_connection *, int, char *, size_t, void *) {
	return 1; // the stream is one-way; client input (pings) ignored
}

static void ws_close_handler(const struct mg_connection *conn, void *) {
	channel.remove_client(const_cast<struct mg_connection *>(conn));
}

// ---- wiring --------------------------------------------------------------

void webselftest_register(struct mg_context *ctx) {
	runner = new selftest_runner_c(cli_path(),
			[](const char *data, size_t len) { channel.append(data, len); },
			[]() { changed = true; });
	mg_set_request_handler(ctx, "/api/selftest", api_selftest_handler, nullptr);
	mg_set_websocket_handler(ctx, "/ws/selftest", ws_connect_handler,
			ws_ready_handler, ws_data_handler, ws_close_handler, nullptr);
}

void webselftest_shutdown(void) {
	if (runner == nullptr)
		return;
	runner->shutdown();
	channel.clear_clients();
}
