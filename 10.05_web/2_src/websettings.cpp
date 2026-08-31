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
       machine's console (the BeagleBone's /dev/ttyS2 via /ws/console/ext,
       which is the default, or Web Serial in the browser). This is
       persisted in settings.json.

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
static external_console_c ext_console = { "ttys2", "ttyS2", 38400 };
static std::string settings_path;
static std::string state_dir;
// The update version the operator dismissed. On the board rather than in one
// browser, so every page agrees about what is being announced.
static std::string dismissed_version;
// Whether the board keeps its bus to itself instead of driving a backplane.
// Independent of the emulated CPU: a board can be the CPU of a real machine
// full of real cards, or a machine entirely by itself.
static bool internal_bus = false;
// The catalogue index URLs the board subscribes to. A board that has never
// stored the key carries the project's own catalogue; a stored list — the
// empty one included — replaces the default.
static std::vector<std::string> catalog_sources = {
	"https://qunilator.com/catalog/v1/index.json"
};

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
	if (v.get("internal_bus").is<bool>())
		internal_bus = v.get("internal_bus").get<bool>();
	const picojson::value &upd = v.get("update");
	if (upd.is<picojson::object>() && upd.get("dismissed_version").is<std::string>()) {
		std::lock_guard<std::mutex> lock(settings_mutex);
		dismissed_version = upd.get("dismissed_version").get<std::string>();
	}
	const picojson::value &cats = v.get("catalogs");
	if (cats.is<picojson::array>()) {
		std::lock_guard<std::mutex> lock(settings_mutex);
		catalog_sources.clear();
		for (const picojson::value &c : cats.get<picojson::array>())
			if (c.is<std::string>())
				catalog_sources.push_back(c.get<std::string>());
	}
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
		root["internal_bus"] = picojson::value(internal_bus);
		picojson::object upd;
		upd["dismissed_version"] = picojson::value(dismissed_version);
		root["update"] = picojson::value(upd);
		picojson::array cats;
		for (const std::string &c : catalog_sources)
			cats.push_back(picojson::value(c));
		root["catalogs"] = picojson::value(cats);
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

std::string websettings_dismissed_version(void) {
	std::lock_guard<std::mutex> lock(settings_mutex);
	return dismissed_version;
}

void websettings_set_dismissed_version(const std::string &version) {
	{
		std::lock_guard<std::mutex> lock(settings_mutex);
		dismissed_version = version;
	}
	save_settings();
}

std::string websettings_state_dir(void) {
	return state_dir;
}

std::vector<std::string> websettings_catalog_sources(void) {
	std::lock_guard<std::mutex> lock(settings_mutex);
	return catalog_sources;
}

void websettings_set_catalog_sources(const std::vector<std::string> &sources) {
	{
		std::lock_guard<std::mutex> lock(settings_mutex);
		catalog_sources = sources;
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
		o["internal_bus"] = picojson::value(internal_bus);
	}
	send_json(conn, 200, picojson::value(o));
}

static void settings_put(struct mg_connection *conn) {
	picojson::value req;
	if (!read_json_body(conn, &req)) {
		send_error(conn, 400, "body must be a JSON object");
		return;
	}
	picojson::array warnings;
	// whether a setting was actually applied, so the settings event announces a
	// change rather than every PUT that asked for one
	bool changed = false;

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
			changed = true;
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
		changed = true;
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
		changed = true;
	}

	if (changed)
		webevents_note_settings();

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
	state_dir = std::string(base ? base : ".");
	settings_path = state_dir + "/settings.json";
	load_settings();
}

void websettings_register(struct mg_context *ctx) {
	if (settings_path.empty())
		websettings_startup();
	mg_set_request_handler(ctx, "/api/settings", api_settings_handler, nullptr);
}
