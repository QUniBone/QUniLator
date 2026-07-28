/* websettings.cpp: /api/settings — global machine settings

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.

   Machine-global settings that are neither per-device parameters nor part
   of a device configuration snapshot:

     - address_width: the CPU address width (16/18/22, QBUS only for 16/22).
       A live property of the qunibus; the boot value comes from the launch
       flag (--addresswidth). Changing it re-bases the I/O page, so it is
       only applied when the bus is (soft-)halted; otherwise the request is
       accepted with a warning and left unchanged.
     - external_console: which physical port, if any, backs the real
       machine's console (Mac Web Serial in the browser, or the BeagleBone's
       /dev/ttyS2 via /ws/console/ext). This is persisted in settings.json.

       GET /api/settings   {platform, address_width, external_console:{...}}
       PUT /api/settings   {address_width?, external_console?:{...}}
*/

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

#include "civetweb.h"
#include "picojson.h"

#include "logger.hpp"
#include "qunibus.h"
#include "device_configuration.hpp"

#include "weblog.hpp"
#include "webauth.hpp"
#include "webevents.hpp"
#include "webconsole_ext.hpp"
#include "weblogging.hpp"
#include "websettings.hpp"

#if defined(QBUS)
static const char *platform_name = "QBUS";
#elif defined(UNIBUS)
static const char *platform_name = "UNIBUS";
#else
static const char *platform_name = "HOST";
#endif

static std::mutex settings_mutex; // guards ext_console
// port is a bare tty name (rs232_c prepends /dev/), matching the SLU convention
static external_console_c ext_console = { "webserial", "ttyS2", 38400 };
static std::string settings_path;
// Whether the board runs the emulated KA11. Read once at startup, before the
// device set is built, and honoured on the UNIBUS build alone.
static bool emulated_cpu = false;
// Whether the board keeps its bus to itself instead of driving a backplane.
// Independent of the emulated CPU: a board can be the CPU of a real machine
// full of real cards, or a machine entirely by itself.
static bool internal_bus = false;

static void send_json(struct mg_connection *conn, int status, const picojson::value &val) {
	std::string body = val.serialize();
	mg_printf(conn,
			"HTTP/1.1 %d %s\r\n"
			"Content-Type: application/json\r\n"
			"Cache-Control: no-store\r\n"
			"Content-Length: %u\r\n\r\n",
			status, status == 200 ? "OK" : "Error", (unsigned) body.size());
	mg_write(conn, body.c_str(), body.size());
}

static void send_error(struct mg_connection *conn, int status, const std::string &message) {
	picojson::object err;
	err["error"] = picojson::value(message);
	send_json(conn, status, picojson::value(err));
}

static bool read_json_body(struct mg_connection *conn, picojson::value *out) {
	char body[4096];
	int body_len = mg_read(conn, body, sizeof(body) - 1);
	if (body_len <= 0)
		return false;
	body[body_len] = 0;
	std::string parse_err = picojson::parse(*out, body);
	return parse_err.empty() && out->is<picojson::object>();
}

// caller holds settings_mutex
static picojson::value external_console_json(void) {
	picojson::object ec;
	ec["source"] = picojson::value(ext_console.source);
	ec["port"] = picojson::value(ext_console.port);
	ec["baud"] = picojson::value((double) ext_console.baud);
	return picojson::value(ec);
}

static void load_settings(void) {
	std::ifstream f(settings_path.c_str());
	if (!f)
		return;
	std::stringstream ss;
	ss << f.rdbuf();
	picojson::value v;
	if (!picojson::parse(v, ss.str()).empty() || !v.is<picojson::object>())
		return;
	webauth_load(v.get("admin"));
	weblogging_load(v.get("log_levels"));
	if (v.get("emulated_cpu").is<bool>())
		emulated_cpu = v.get("emulated_cpu").get<bool>();
	if (v.get("internal_bus").is<bool>())
		internal_bus = v.get("internal_bus").get<bool>();
	const picojson::value &ec = v.get("external_console");
	if (!ec.is<picojson::object>())
		return;
	std::lock_guard<std::mutex> lock(settings_mutex);
	if (ec.get("source").is<std::string>())
		ext_console.source = ec.get("source").get<std::string>();
	if (ec.get("port").is<std::string>())
		ext_console.port = ec.get("port").get<std::string>();
	if (ec.get("baud").is<double>())
		ext_console.baud = (unsigned) ec.get("baud").get<double>();
}

static void save_settings(void) {
	picojson::object root;
	{
		std::lock_guard<std::mutex> lock(settings_mutex);
		root["external_console"] = external_console_json();
		root["emulated_cpu"] = picojson::value(emulated_cpu);
		root["internal_bus"] = picojson::value(internal_bus);
	}
	picojson::value admin = webauth_json();
	if (!admin.is<picojson::null>())
		root["admin"] = admin;
	root["log_levels"] = weblogging_json();

	// The file carries a password digest, so it is written through a private
	// temporary and renamed: readable only by the emulator's user, and never
	// seen truncated by a reader that opens it mid-write.
	std::string tmp_path = settings_path + ".new";
	{
		std::ofstream f(tmp_path.c_str());
		if (!f)
			return;
		f << picojson::value(root).serialize();
		if (!f)
			return;
	}
	chmod(tmp_path.c_str(), S_IRUSR | S_IWUSR);
	rename(tmp_path.c_str(), settings_path.c_str());
}

void websettings_save(void) {
	save_settings();
}

external_console_c websettings_external_console(void) {
	std::lock_guard<std::mutex> lock(settings_mutex);
	return ext_console;
}

bool websettings_emulated_cpu(void) {
	std::lock_guard<std::mutex> lock(settings_mutex);
	return emulated_cpu;
}

bool websettings_internal_bus(void) {
	std::lock_guard<std::mutex> lock(settings_mutex);
	return internal_bus;
}

void websettings_set_internal_bus(bool on) {
	{
		std::lock_guard<std::mutex> lock(settings_mutex);
		internal_bus = on;
	}
	save_settings();
}

void websettings_set_emulated_cpu(bool on) {
	{
		std::lock_guard<std::mutex> lock(settings_mutex);
		emulated_cpu = on;
	}
	save_settings();
}

static void settings_get(struct mg_connection *conn) {
	picojson::object o;
	o["platform"] = picojson::value(platform_name);
	o["address_width"] = picojson::value((double) qunibus->addr_width);
	{
		std::lock_guard<std::mutex> lock(settings_mutex);
		o["external_console"] = external_console_json();
		o["emulated_cpu"] = picojson::value(emulated_cpu);
		o["internal_bus"] = picojson::value(internal_bus);
	}
#if defined(UNIBUS)
	o["emulated_cpu_available"] = picojson::value(true);
#else
	o["emulated_cpu_available"] = picojson::value(false);
#endif
	send_json(conn, 200, picojson::value(o));
}

static void settings_put(struct mg_connection *conn) {
	picojson::value req;
	if (!read_json_body(conn, &req)) {
		send_error(conn, 400, "body must be a JSON object");
		return;
	}
	picojson::array warnings;

	// address width — validate, then apply only while halted
	const picojson::value &aw = req.get("address_width");
	if (aw.is<double>()) {
		unsigned w = (unsigned) aw.get<double>();
		bool valid = (w == 18);
#if defined(QBUS)
		valid = valid || w == 16 || w == 22;
#endif
		if (!valid) {
			send_error(conn, 422, "address_width must be 16, 18 or 22");
			return;
		}
		if (!webevents_is_halted()) {
			warnings.push_back(picojson::value(std::string(
				"address width unchanged: halt the bus (or powercycle) before changing it")));
		} else {
			std::lock_guard<std::mutex> ops(device_configuration_c::operations_mutex);
			qunibus->set_addr_width(w);
			WEB_INFO("address width %u", w);
		}
	}

	// external console — validate before mutating
	const picojson::value &ec = req.get("external_console");
	if (ec.is<picojson::object>()) {
		if (ec.get("source").is<std::string>()) {
			std::string s = ec.get("source").get<std::string>();
			if (s != "webserial" && s != "ttys2" && s != "off") {
				send_error(conn, 422, "external_console.source must be webserial, ttys2 or off");
				return;
			}
		}
		{
			std::lock_guard<std::mutex> lock(settings_mutex);
			if (ec.get("source").is<std::string>())
				ext_console.source = ec.get("source").get<std::string>();
			if (ec.get("port").is<std::string>())
				ext_console.port = ec.get("port").get<std::string>();
			if (ec.get("baud").is<double>())
				ext_console.baud = (unsigned) ec.get("baud").get<double>();
		}
		save_settings();
		// (re)open or close the ttyS2 bridge; report any refusal as a warning
		external_console_c now = websettings_external_console();
		std::string reason = webconsole_ext_configure(now.source, now.port, now.baud);
		if (!reason.empty())
			warnings.push_back(picojson::value(reason));
	}

	// the emulated CPU — the board is either a peripheral of a real PDP-11 or
	// the machine itself, and which one it is decides who arbitrates the bus.
	// That is settled when the device set is built, so the switch takes effect
	// at the next start of the service.
	const picojson::value &ecpu = req.get("emulated_cpu");
	if (ecpu.is<bool>()) {
#if defined(UNIBUS)
		if (ecpu.get<bool>() != websettings_emulated_cpu()) {
			websettings_set_emulated_cpu(ecpu.get<bool>());
			WEB_INFO("emulated CPU %s", ecpu.get<bool>() ? "on" : "off");
			warnings.push_back(picojson::value(std::string(
				"emulated CPU setting stored; restart the service to build the "
				"device set with it")));
		}
#else
		send_error(conn, 422, "no emulated CPU on this bus");
		return;
#endif
	}

	// the bus mode — which peripherals the machine can reach: the cards in a
	// real backplane, or the board's own emulated devices. Independent of the
	// emulated CPU, and settled when the PRU is loaded, so it takes effect at
	// the next start of the service.
	const picojson::value &ibus = req.get("internal_bus");
	if (ibus.is<bool>() && ibus.get<bool>() != websettings_internal_bus()) {
		websettings_set_internal_bus(ibus.get<bool>());
		WEB_INFO("bus %s", ibus.get<bool>() ? "internal" : "physical");
		warnings.push_back(picojson::value(std::string(
			"bus mode stored; restart the service to load the firmware for it")));
	}

	picojson::object res;
	res["ok"] = picojson::value(true);
	res["warnings"] = picojson::value(warnings);
	send_json(conn, 200, picojson::value(res));
}

static int api_settings_handler(struct mg_connection *conn, void * /*cbdata*/) {
	const struct mg_request_info *ri = mg_get_request_info(conn);
	if (strcmp(ri->request_method, "GET") == 0)
		settings_get(conn);
	else if (strcmp(ri->request_method, "PUT") == 0 || strcmp(ri->request_method, "POST") == 0)
		settings_put(conn);
	else {
		send_error(conn, 405, "GET or PUT required");
		return 405;
	}
	return 200;
}

// Read settings.json. Separate from registering the endpoint, because the
// device set is built before the web server starts and the emulated-CPU
// setting decides how it is built.
void websettings_startup(void) {
	const char *base = getenv("QUNILATOR_DIR");
	if (base == nullptr)
		base = getenv("HOME");
	settings_path = std::string(base ? base : ".") + "/settings.json";
	load_settings();
}

void websettings_register(struct mg_context *ctx) {
	if (settings_path.empty())
		websettings_startup();
	mg_set_request_handler(ctx, "/api/settings", api_settings_handler, nullptr);
}
