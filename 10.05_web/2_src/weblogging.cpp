/* weblogging.cpp: /api/logging — runtime log-level control

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.

   The log level is adjustable at runtime: a global default plus per-target
   overrides, for devices and non-device subsystems alike. The levels are the
   logger's five (LL_FATAL..LL_DEBUG), exposed at the API as lowercase names.

   The persistent authority is the "log_levels" object in settings.json, held
   here and serialized by websettings.cpp. It is independent of the
   configuration, so switching configurations never disturbs the levels; a
   device verbosity is not baked into a saved config. The stored levels are
   applied to the logger at startup and re-asserted after every configuration
   apply.

     GET /api/logging                  {default, sources:[{label,level,kind}]}
     PUT /api/logging/default          {"level": "info"}
     PUT /api/logging/sources/<label>  {"level": "debug"} or {"level": null}
*/

#include <string.h>

#include <map>
#include <mutex>
#include <set>
#include <string>

#include "civetweb.h"
#include "picojson.h"

#include "logger.hpp"
#include "device.hpp"

#include "weblog.hpp"
#include "websettings.hpp"
#include "weblogging.hpp"

// The stored model: the global default and the per-label overrides. An
// override for a label with no registered source is retained here and applied
// if that source later appears.
static std::mutex log_mutex;
static unsigned model_default_level = LL_WARNING;
static std::map<std::string, unsigned> model_overrides; // label -> LL_*

static const char *level_name(unsigned level) {
	switch (level) {
	case LL_FATAL:   return "fatal";
	case LL_ERROR:   return "error";
	case LL_WARNING: return "warning";
	case LL_INFO:    return "info";
	case LL_DEBUG:   return "debug";
	default:         return "warning";
	}
}

static bool level_from_name(const std::string &name, unsigned *out) {
	if (name == "fatal")        *out = LL_FATAL;
	else if (name == "error")   *out = LL_ERROR;
	else if (name == "warning") *out = LL_WARNING;
	else if (name == "info")    *out = LL_INFO;
	else if (name == "debug")   *out = LL_DEBUG;
	else return false;
	return true;
}

void weblogging_load(const picojson::value &v) {
	if (!v.is<picojson::object>())
		return;
	std::lock_guard<std::mutex> lock(log_mutex);
	unsigned level;
	if (v.get("default").is<std::string>()
			&& level_from_name(v.get("default").get<std::string>(), &level))
		model_default_level = level;
	model_overrides.clear();
	if (v.get("sources").is<picojson::object>())
		for (const std::pair<const std::string, picojson::value> &kv :
				v.get("sources").get<picojson::object>())
			if (kv.second.is<std::string>()
					&& level_from_name(kv.second.get<std::string>(), &level))
				model_overrides[kv.first] = level;
}

picojson::value weblogging_json(void) {
	picojson::object o;
	std::lock_guard<std::mutex> lock(log_mutex);
	o["default"] = picojson::value(std::string(level_name(model_default_level)));
	picojson::object sources;
	for (const std::pair<const std::string, unsigned> &kv : model_overrides)
		sources[kv.first] = picojson::value(std::string(level_name(kv.second)));
	o["sources"] = picojson::value(sources);
	return picojson::value(o);
}

unsigned weblogging_default_level(void) {
	std::lock_guard<std::mutex> lock(log_mutex);
	return model_default_level;
}

void weblogging_apply(void) {
	// snapshot the model, then walk the logger's sources without holding
	// log_mutex (list_logsources takes the logger's own lock)
	unsigned def;
	std::map<std::string, unsigned> overrides;
	{
		std::lock_guard<std::mutex> lock(log_mutex);
		def = model_default_level;
		overrides = model_overrides;
	}
	logger->default_level = def;
	for (const logger_c::logsource_ref_t &ref : logger->list_logsources()) {
		std::map<std::string, unsigned>::iterator it = overrides.find(ref.label);
		*(ref.level_ptr) = (it != overrides.end()) ? it->second : def;
	}
}

std::vector<weblogging_source_t> weblogging_sources(void) {
	// A source is a device when its level lives in a device's verbosity
	// parameter; everything else (web layer, PRU and bus, GPIOS, ...) is a
	// subsystem. Collect the device verbosity addresses to compare against.
	std::set<unsigned *> device_levels;
	{
		std::lock_guard<std::mutex> lock(device_c::mydevices_mutex);
		for (device_c *d : device_c::mydevices)
			device_levels.insert(&d->verbosity.value);
	}
	std::vector<weblogging_source_t> result;
	for (const logger_c::logsource_ref_t &ref : logger->list_logsources()) {
		weblogging_source_t s;
		s.label = ref.label;
		s.level = *(ref.level_ptr);
		s.kind = device_levels.count(ref.level_ptr) ? "device" : "subsystem";
		result.push_back(s);
	}
	return result;
}

bool weblogging_set_default(const std::string &level, std::string *error) {
	unsigned l;
	if (!level_from_name(level, &l)) {
		if (error != nullptr)
			*error = "unknown level \"" + level + "\"";
		return false;
	}
	{
		std::lock_guard<std::mutex> lock(log_mutex);
		model_default_level = l;
	}
	weblogging_apply();
	websettings_save();
	WEB_INFO("log default level = %s", level.c_str());
	return true;
}

bool weblogging_set_source(const std::string &label, const picojson::value &level,
		std::string *error) {
	if (level.is<picojson::null>()) {
		{
			std::lock_guard<std::mutex> lock(log_mutex);
			model_overrides.erase(label);
		}
		weblogging_apply();
		websettings_save();
		WEB_INFO("log level of %s cleared to default", label.c_str());
		return true;
	}
	if (!level.is<std::string>()) {
		if (error != nullptr)
			*error = "\"level\" must be a level name or null";
		return false;
	}
	unsigned l;
	std::string name = level.get<std::string>();
	if (!level_from_name(name, &l)) {
		if (error != nullptr)
			*error = "unknown level \"" + name + "\"";
		return false;
	}
	{
		std::lock_guard<std::mutex> lock(log_mutex);
		model_overrides[label] = l;
	}
	weblogging_apply();
	websettings_save();
	WEB_INFO("log level of %s = %s", label.c_str(), name.c_str());
	return true;
}

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

static void logging_get(struct mg_connection *conn) {
	picojson::object root;
	root["default"] = picojson::value(std::string(level_name(weblogging_default_level())));
	picojson::array sources;
	for (const weblogging_source_t &s : weblogging_sources()) {
		picojson::object o;
		o["label"] = picojson::value(s.label);
		o["level"] = picojson::value(std::string(level_name(s.level)));
		o["kind"] = picojson::value(s.kind);
		sources.push_back(picojson::value(o));
	}
	root["sources"] = picojson::value(sources);
	send_json(conn, 200, picojson::value(root));
}

// /api/logging, /api/logging/default, /api/logging/sources/<label>
static int api_logging_handler(struct mg_connection *conn, void * /*cbdata*/) {
	const struct mg_request_info *ri = mg_get_request_info(conn);
	std::string uri = ri->local_uri ? ri->local_uri : "";
	std::string rest = uri.substr(strlen("/api/logging"));
	std::string method = ri->request_method;

	if (rest.empty() || rest == "/") {
		if (method != "GET") {
			send_error(conn, 405, "GET required");
			return 405;
		}
		logging_get(conn);
		return 200;
	}

	if (rest == "/default") {
		if (method != "PUT" && method != "POST") {
			send_error(conn, 405, "PUT required");
			return 405;
		}
		picojson::value req;
		if (!read_json_body(conn, &req) || !req.get("level").is<std::string>()) {
			send_error(conn, 400, "body must be a JSON object with a string \"level\"");
			return 400;
		}
		std::string error;
		if (!weblogging_set_default(req.get("level").get<std::string>(), &error)) {
			send_error(conn, 422, error);
			return 422;
		}
		picojson::object res;
		res["ok"] = picojson::value(true);
		send_json(conn, 200, picojson::value(res));
		return 200;
	}

	const std::string prefix = "/sources/";
	if (rest.compare(0, prefix.size(), prefix) == 0) {
		std::string label = rest.substr(prefix.size());
		if (label.empty() || label.find('/') != std::string::npos) {
			send_error(conn, 404, "unknown logging target");
			return 404;
		}
		if (method != "PUT" && method != "POST") {
			send_error(conn, 405, "PUT required");
			return 405;
		}
		picojson::value req;
		if (!read_json_body(conn, &req)
				|| req.get<picojson::object>().count("level") == 0) {
			send_error(conn, 400,
					"body must be a JSON object with a \"level\" of a level name or null");
			return 400;
		}
		std::string error;
		if (!weblogging_set_source(label, req.get("level"), &error)) {
			send_error(conn, 422, error);
			return 422;
		}
		picojson::object res;
		res["ok"] = picojson::value(true);
		send_json(conn, 200, picojson::value(res));
		return 200;
	}

	send_error(conn, 404, "unknown logging path");
	return 404;
}

void weblogging_register(struct mg_context *ctx) {
	// settings (with the persisted levels) are loaded before this runs, so
	// apply them to the logger now; the per-configuration re-assert follows in
	// webconfigs_apply().
	weblogging_apply();
	mg_set_request_handler(ctx, "/api/logging", api_logging_handler, nullptr);
}
