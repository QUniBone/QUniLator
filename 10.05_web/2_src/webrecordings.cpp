/* webrecordings.cpp: the recordings API

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.

   See webrecordings.hpp for the endpoints.
*/

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

#include "civetweb.h"
#include "picojson.h"

#include "weblog.hpp"
#include "webrecording.hpp"
#include "webrecordings.hpp"
#include "webconsole.hpp"
#include "webconsole_ext.hpp"

static std::string recordings_dir;

void webrecordings_init(const std::string &dir) {
	recordings_dir = dir;
	mkdir(recordings_dir.c_str(), 0755); // may already exist
}

std::string webrecordings_dir(void) {
	return recordings_dir;
}

static void send_json(struct mg_connection *conn, int status,
		const picojson::value &val) {
	std::string body = val.serialize();
	mg_printf(conn,
			"HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
			"Content-Length: %u\r\nConnection: close\r\n\r\n",
			status, status == 200 ? "OK" : "Error", (unsigned) body.size());
	mg_write(conn, body.data(), body.size());
}

static void send_error(struct mg_connection *conn, int status,
		const std::string &message) {
	picojson::object err;
	err["error"] = picojson::value(message);
	send_json(conn, status, picojson::value(err));
}

// A recording name is a plain file name: no directory parts, no dot files, and
// the .cast suffix this module owns. Anything else is refused rather than
// sanitized, so a request can never reach outside the recordings directory.
static bool valid_name(const std::string &name) {
	if (name.empty() || name.size() > 128)
		return false;
	if (name.find('/') != std::string::npos || name[0] == '.')
		return false;
	for (char c : name)
		if (!(isalnum((unsigned char) c) || c == '-' || c == '_' || c == '.'))
			return false;
	return name.size() > 5 && name.compare(name.size() - 5, 5, ".cast") == 0;
}

static std::string timestamped_name(const std::string &channel) {
	time_t now = time(nullptr);
	struct tm tm_buf;
	localtime_r(&now, &tm_buf);
	char stamp[32];
	strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm_buf);
	return "console-" + channel + "-" + stamp + ".cast";
}

static std::string body_of(struct mg_connection *conn) {
	std::string body;
	char buf[4096];
	int n;
	while ((n = mg_read(conn, buf, sizeof buf)) > 0)
		body.append(buf, (size_t) n);
	return body;
}

// POST /api/console/<channel>/recording
static int api_console_recording_handler(struct mg_connection *conn, void *) {
	const struct mg_request_info *ri = mg_get_request_info(conn);
	std::string uri = ri->local_uri ? ri->local_uri : "";
	// /api/console/<channel>/recording
	const std::string prefix = "/api/console/";
	const std::string suffix = "/recording";
	if (uri.compare(0, prefix.size(), prefix) != 0 ||
			uri.size() <= prefix.size() + suffix.size() ||
			uri.compare(uri.size() - suffix.size(), suffix.size(), suffix) != 0) {
		send_error(conn, 404, "not found");
		return 1;
	}
	std::string channel = uri.substr(prefix.size(),
			uri.size() - prefix.size() - suffix.size());
	console_recorder_c *rec = webconsole_recorder(channel);
	if (rec == nullptr) {
		send_error(conn, 404, "no console channel " + channel);
		return 1;
	}

	if (strcmp(ri->request_method, "GET") == 0) {
		picojson::object o;
		o["recording"] = picojson::value(rec->recording());
		if (rec->recording()) {
			std::string p = rec->path();
			size_t slash = p.rfind('/');
			o["name"] = picojson::value(slash == std::string::npos ? p : p.substr(slash + 1));
			o["bytes"] = picojson::value((double) rec->bytes_written());
		}
		send_json(conn, 200, picojson::value(o));
		return 1;
	}
	if (strcmp(ri->request_method, "POST") != 0) {
		send_error(conn, 405, "method not allowed");
		return 1;
	}

	picojson::value doc;
	std::string err = picojson::parse(doc, body_of(conn));
	if (!err.empty() || !doc.is<picojson::object>()) {
		send_error(conn, 400, "body must be a JSON object");
		return 1;
	}
	const picojson::object &o = doc.get<picojson::object>();
	std::string action = o.count("action") && o.at("action").is<std::string>()
			? o.at("action").get<std::string>() : "";

	if (action == "stop") {
		rec->stop();
		picojson::object out;
		out["ok"] = picojson::value(true);
		out["recording"] = picojson::value(false);
		send_json(conn, 200, picojson::value(out));
		return 1;
	}
	if (action != "start") {
		send_error(conn, 422, "action must be start or stop");
		return 1;
	}
	std::string name = o.count("name") && o.at("name").is<std::string>()
			? o.at("name").get<std::string>() : "";
	if (name.empty())
		name = timestamped_name(channel);
	else if (name.size() < 6 || name.compare(name.size() - 5, 5, ".cast") != 0)
		name += ".cast";
	if (!valid_name(name)) {
		send_error(conn, 422, "invalid recording name");
		return 1;
	}
	std::string reason = rec->start(recordings_dir + "/" + name,
			"console " + channel);
	if (!reason.empty()) {
		send_error(conn, 500, reason);
		return 1;
	}
	WEB_INFO("recording console %s to %s", channel.c_str(), name.c_str());
	picojson::object out;
	out["ok"] = picojson::value(true);
	out["recording"] = picojson::value(true);
	out["name"] = picojson::value(name);
	send_json(conn, 200, picojson::value(out));
	return 1;
}

static void list_recordings(struct mg_connection *conn) {
	picojson::array arr;
	DIR *d = opendir(recordings_dir.c_str());
	if (d != nullptr) {
		struct dirent *e;
		std::vector<std::string> names;
		while ((e = readdir(d)) != nullptr) {
			std::string n = e->d_name;
			if (valid_name(n))
				names.push_back(n);
		}
		closedir(d);
		std::sort(names.begin(), names.end());
		for (const std::string &n : names) {
			struct stat st;
			if (stat((recordings_dir + "/" + n).c_str(), &st) != 0)
				continue;
			picojson::object o;
			o["name"] = picojson::value(n);
			o["bytes"] = picojson::value((double) st.st_size);
			char when[32];
			struct tm tm_buf;
			localtime_r(&st.st_mtime, &tm_buf);
			strftime(when, sizeof when, "%Y-%m-%d %H:%M", &tm_buf);
			o["mtime"] = picojson::value(std::string(when));
			arr.push_back(picojson::value(o));
		}
	}
	picojson::object out;
	out["recordings"] = picojson::value(arr);
	send_json(conn, 200, picojson::value(out));
}

// GET/DELETE /api/recordings[/<name>]
static int api_recordings_handler(struct mg_connection *conn, void *) {
	const struct mg_request_info *ri = mg_get_request_info(conn);
	std::string uri = ri->local_uri ? ri->local_uri : "";
	const std::string prefix = "/api/recordings";
	std::string name;
	if (uri.size() > prefix.size() + 1)
		name = uri.substr(prefix.size() + 1);

	if (name.empty()) {
		if (strcmp(ri->request_method, "GET") != 0) {
			send_error(conn, 405, "method not allowed");
			return 1;
		}
		list_recordings(conn);
		return 1;
	}
	if (!valid_name(name)) {
		send_error(conn, 422, "invalid recording name");
		return 1;
	}
	std::string path = recordings_dir + "/" + name;
	if (strcmp(ri->request_method, "DELETE") == 0) {
		if (unlink(path.c_str()) != 0) {
			send_error(conn, 404, "no recording " + name);
			return 1;
		}
		picojson::object out;
		out["ok"] = picojson::value(true);
		send_json(conn, 200, picojson::value(out));
		return 1;
	}
	if (strcmp(ri->request_method, "GET") != 0) {
		send_error(conn, 405, "method not allowed");
		return 1;
	}
	// A cast is text; serve it as such so a browser can open it directly.
	mg_send_mime_file(conn, path.c_str(), "text/plain; charset=utf-8");
	return 1;
}

void webrecordings_register(struct mg_context *ctx) {
	const char *base = getenv("QUNILATOR_DIR");
	if (base == nullptr)
		base = getenv("HOME");
	webrecordings_init(std::string(base ? base : ".") + "/recordings");
	mg_set_request_handler(ctx, "/api/console/", api_console_recording_handler,
			nullptr);
	mg_set_request_handler(ctx, "/api/recordings", api_recordings_handler,
			nullptr);
}

// The recorder behind a channel name. Kept here so the routing module knows
// the channel names and the console backends stay unaware of the API.
console_recorder_c *webconsole_recorder(const std::string &channel) {
	if (channel == "ext")
		return webconsole_ext_recorder();
	if (channel == "0")
		return webconsole_channel_recorder(0);
	if (channel == "1")
		return webconsole_channel_recorder(1);
	if (channel == "vax")
		return webconsole_channel_recorder(2);
	return nullptr;
}
