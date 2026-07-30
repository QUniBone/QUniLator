/* webupdate.cpp: /api/update — the self-update the interface drives

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.

   The service does not run apt. /usr/sbin/qunilator-update does, in its own
   systemd unit, because an install stops and restarts this very service: a dpkg
   running as a child of it would be killed with it, mid-install. So the division
   is:

     - the updater owns updates/status.json and writes each step to it as it goes;
     - this file publishes that status - over GET /api/update and as an "update"
       frame on /ws/events whenever the file's mtime moves - and starts the
       updater's units with "systemctl start --no-block";
     - a requested version is checked against the candidate the updater reported
       and written to updates/request.json. No string from an HTTP request ever
       reaches a command line.

   Which version an operator has dismissed belongs to the board rather than to one
   browser, so it lives in settings.json and is merged into what is published
   here.
*/

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

#include "civetweb.h"
#include "picojson.h"

#include "logger.hpp"
#include "weblog.hpp"
#include "websettings.hpp"
#include "webversion.hpp"
#include "webupdate.hpp"

// The updater's state directory, inside the state directory the service was
// given. The unit sets QUNILATOR_DIR=/var/lib/qunilator, which is the path
// qunilator-update uses.
static std::string state_dir(void) {
	return websettings_state_dir() + "/updates";
}

static std::string status_path(void) {
	return state_dir() + "/status.json";
}

static std::string request_path(void) {
	return state_dir() + "/request.json";
}

static const char *UPDATER = "/usr/sbin/qunilator-update";

// The status file as last read, and the mtime it was read at. Guarded by
// status_mutex; the poll thread writes, request handlers read.
static std::mutex status_mutex;
static picojson::object status_obj;
static bool status_known = false;
static time_t status_mtime = 0;
static off_t status_size = 0;

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

static bool read_json_body(struct mg_connection *conn, picojson::value *out) {
	char body[4096];
	int n = mg_read(conn, body, sizeof(body) - 1);
	if (n <= 0)
		return false;
	body[n] = 0;
	return picojson::parse(*out, body).empty() && out->is<picojson::object>();
}

// A version string as it may appear in a request: what dpkg allows in a version,
// and nothing else. It is written to a file the updater reads, and refused
// outright if it does not look like a version - belt to the braces of checking it
// against the candidate.
static bool version_is_plausible(const std::string &v) {
	if (v.empty() || v.size() > 64)
		return false;
	for (size_t i = 0; i < v.size(); i++) {
		char c = v[i];
		bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
				|| c == '.' || c == '+' || c == '-' || c == '~' || c == ':';
		if (!ok)
			return false;
	}
	return true;
}

// ---- the status file ----

// Read updates/status.json. Returns false when it is absent or unparsable, which
// is the state of a board where the check has never run.
static bool read_status(picojson::object *out) {
	std::ifstream f(status_path().c_str());
	if (!f)
		return false;
	std::stringstream ss;
	ss << f.rdbuf();
	picojson::value v;
	if (!picojson::parse(v, ss.str()).empty() || !v.is<picojson::object>())
		return false;
	*out = v.get<picojson::object>();
	return true;
}

// The board's own view of the status: what the updater wrote, plus the dismissed
// version, which is this side's to keep. A board where the check has never run
// still gets an answer, so the interface has something to show.
static picojson::value status_json_locked(void) {
	picojson::object o = status_obj;
	if (!status_known) {
		o["state"] = picojson::value(std::string("idle"));
		o["package"] = picojson::value(webversion_package());
		o["installed"] = picojson::value(webversion_version());
		o["candidate"] = picojson::value(std::string(""));
		o["checked_at"] = picojson::value(std::string(""));
		o["source_configured"] = picojson::value(false);
		o["rollback"] = picojson::value(false);
		o["needs_repair"] = picojson::value(false);
		o["error"] = picojson::value(std::string(""));
		o["journal"] = picojson::value(picojson::array());
		picojson::object os;
		os["count"] = picojson::value((double) 0);
		os["packages"] = picojson::value(picojson::array());
		os["held_back"] = picojson::value(picojson::array());
		os["reboot_required"] = picojson::value(false);
		o["os"] = picojson::value(os);
		o["last"] = picojson::value(picojson::object());
	}
	o["dismissed"] = picojson::value(websettings_dismissed_version());
	return picojson::value(o);
}

std::string webupdate_event_json(void) {
	std::lock_guard<std::mutex> lock(status_mutex);
	picojson::value v = status_json_locked();
	picojson::object o = v.get<picojson::object>();
	o["t"] = picojson::value(std::string("update"));
	return picojson::value(o).serialize();
}

// The state the last frame reported, so a change can be logged once.
static std::string last_logged_state;

// Reread the status file when it has moved. Size as well as mtime: two writes in
// the same second are what a fast install produces, and the file is replaced by
// rename each time, so its size almost always changes with its content.
static bool reload_status(void) {
	struct stat st;
	if (stat(status_path().c_str(), &st) != 0)
		return false;
	std::string state, error;
	bool changed = false;
	{
		std::lock_guard<std::mutex> lock(status_mutex);
		if (status_known && st.st_mtime == status_mtime && st.st_size == status_size)
			return false;
		picojson::object fresh;
		if (!read_status(&fresh))
			return false;   // mid-rename or malformed: try again next tick
		status_obj = fresh;
		status_known = true;
		status_mtime = st.st_mtime;
		status_size = st.st_size;
		changed = true;
		if (status_obj["state"].is<std::string>())
			state = status_obj["state"].get<std::string>();
		if (status_obj["error"].is<std::string>())
			error = status_obj["error"].get<std::string>();
	}
	// The journal carries what the interface shows, so an update can be read back
	// afterwards from the board rather than from a browser that has moved on.
	if (changed && state != last_logged_state) {
		last_logged_state = state;
		if (!error.empty())
			WEB_WARNING("update: %s (%s)", state.c_str(), error.c_str());
		else
			WEB_INFO("update: %s", state.c_str());
	}
	return changed;
}

bool webupdate_poll(void) {
	// The caller runs at 10 Hz; one stat a second is enough to follow an install
	// that writes a line per step.
	static unsigned tick = 0;
	if (++tick < 10)
		return false;
	tick = 0;
	return reload_status();
}

// ---- starting the updater's units ----

// systemctl start --no-block <unit>: the unit runs in its own cgroup, so it
// survives this service being stopped and restarted by the install it performs.
// No shell, and the argument list is fixed.
static bool start_unit(const char *unit) {
	pid_t pid = fork();
	if (pid < 0) {
		WEB_ERROR("update: fork failed: %s", strerror(errno));
		return false;
	}
	if (pid == 0) {
		execl("/bin/systemctl", "systemctl", "start", "--no-block", unit, (char *) nullptr);
		execl("/usr/bin/systemctl", "systemctl", "start", "--no-block", unit,
				(char *) nullptr);
		_exit(127);
	}
	int wstatus = 0;
	// --no-block returns as soon as the job is enqueued, so this does not wait
	// for the update itself
	while (waitpid(pid, &wstatus, 0) < 0 && errno == EINTR)
		;
	if (!WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 0) {
		WEB_ERROR("update: could not start %s", unit);
		return false;
	}
	WEB_INFO("update: started %s", unit);
	return true;
}

// ---- endpoints ----

static void update_get(struct mg_connection *conn) {
	std::lock_guard<std::mutex> lock(status_mutex);
	send_json(conn, 200, status_json_locked());
}

// The changelog of the staged candidate, as plain text: the stanzas the package
// itself ships, newer than the installed version. The updater stages the package
// to read it, which takes a moment, so this is its own request rather than part
// of the status.
static void update_changelog(struct mg_connection *conn) {
	// Bounded: reading the changelog downloads the candidate, and an apt that
	// hangs would otherwise hold this civetweb worker thread for as long as it
	// felt like. The command line is fixed - nothing from the request reaches it.
	std::string cmd = std::string("timeout 90 ") + UPDATER + " --changelog 2>/dev/null";
	FILE *p = popen(cmd.c_str(), "r");
	if (p == nullptr) {
		send_error(conn, 500, "the updater could not be run");
		return;
	}
	std::string text;
	char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof buf, p)) > 0) {
		text.append(buf, n);
		if (text.size() > 512 * 1024) // a changelog, not a log file
			break;
	}
	int rc = pclose(p);
	picojson::object o;
	if (rc != 0 && text.empty()) {
		send_error(conn, 502, "the candidate's changelog could not be read");
		return;
	}
	o["changelog"] = picojson::value(text);
	send_json(conn, 200, picojson::value(o));
}

static void update_check(struct mg_connection *conn) {
	if (!start_unit("qunilator-update-check.service")) {
		send_error(conn, 500, "the check could not be started");
		return;
	}
	picojson::object o;
	o["ok"] = picojson::value(true);
	send_json(conn, 202, picojson::value(o));
}

// The version to install, checked against the candidate the updater reported and
// written to request.json for qunilator-update.service to read. Refusing a
// version the repository does not offer here is what keeps the interface from
// asking for an install that can only fail.
static void update_install(struct mg_connection *conn) {
	picojson::value req;
	if (!read_json_body(conn, &req) || !req.get("version").is<std::string>()) {
		send_error(conn, 400, "body must be a JSON object with a \"version\" string");
		return;
	}
	std::string want = req.get("version").get<std::string>();
	if (!version_is_plausible(want)) {
		send_error(conn, 422, "\"version\" is not a version string");
		return;
	}

	std::string candidate, state;
	{
		std::lock_guard<std::mutex> lock(status_mutex);
		if (status_known) {
			if (status_obj["candidate"].is<std::string>())
				candidate = status_obj["candidate"].get<std::string>();
			if (status_obj["state"].is<std::string>())
				state = status_obj["state"].get<std::string>();
		}
	}
	if (state == "downloading" || state == "installing" || state == "verifying"
			|| state == "os-upgrading") {
		send_error(conn, 409, "an update is already running");
		return;
	}
	if (candidate.empty()) {
		send_error(conn, 409, "no candidate version — run a check first");
		return;
	}
	if (want != candidate) {
		send_error(conn, 422, "version " + want + " is not the candidate the "
				"repository offers (" + candidate + ")");
		return;
	}

	// Written through a temporary and renamed, so the updater cannot read it half
	// written even though it is started only afterwards. The directory is the
	// package's, 0700 root; made here as well so a service started outside the
	// package's layout still works.
	if (mkdir(state_dir().c_str(), 0700) != 0 && errno != EEXIST) {
		send_error(conn, 500, "the update state directory is not writable");
		return;
	}
	std::string path = request_path();
	std::string tmp = path + ".new";
	{
		std::ofstream f(tmp.c_str());
		picojson::object r;
		r["version"] = picojson::value(want);
		r["requested_at"] = picojson::value(std::string(""));
		if (!f || !(f << picojson::value(r).serialize())) {
			send_error(conn, 500, "the request could not be written");
			return;
		}
	}
	chmod(tmp.c_str(), S_IRUSR | S_IWUSR);
	if (rename(tmp.c_str(), path.c_str()) != 0) {
		unlink(tmp.c_str());
		send_error(conn, 500, "the request could not be written");
		return;
	}
	WEB_INFO("update: %s requested", want.c_str());
	if (!start_unit("qunilator-update.service")) {
		unlink(path.c_str());
		send_error(conn, 500, "the install could not be started");
		return;
	}
	picojson::object o;
	o["ok"] = picojson::value(true);
	o["version"] = picojson::value(want);
	send_json(conn, 202, picojson::value(o));
}

static void update_os(struct mg_connection *conn) {
	std::string state;
	{
		std::lock_guard<std::mutex> lock(status_mutex);
		if (status_known && status_obj["state"].is<std::string>())
			state = status_obj["state"].get<std::string>();
	}
	if (state == "downloading" || state == "installing" || state == "verifying"
			|| state == "os-upgrading") {
		send_error(conn, 409, "an update is already running");
		return;
	}
	if (!start_unit("qunilator-update-os.service")) {
		send_error(conn, 500, "the upgrade could not be started");
		return;
	}
	picojson::object o;
	o["ok"] = picojson::value(true);
	send_json(conn, 202, picojson::value(o));
}

static void update_dismiss(struct mg_connection *conn) {
	picojson::value req;
	if (!read_json_body(conn, &req) || !req.get("version").is<std::string>()) {
		send_error(conn, 400, "body must be a JSON object with a \"version\" string");
		return;
	}
	std::string version = req.get("version").get<std::string>();
	if (!version.empty() && !version_is_plausible(version)) {
		send_error(conn, 422, "\"version\" is not a version string");
		return;
	}
	websettings_set_dismissed_version(version);
	WEB_INFO("update: %s dismissed", version.empty() ? "nothing" : version.c_str());
	picojson::object o;
	o["ok"] = picojson::value(true);
	o["dismissed"] = picojson::value(version);
	send_json(conn, 200, picojson::value(o));
}

static int api_update_handler(struct mg_connection *conn, void * /*cbdata*/) {
	const struct mg_request_info *ri = mg_get_request_info(conn);
	std::string uri = ri->local_uri ? ri->local_uri : "";
	std::string rest = uri.substr(strlen("/api/update"));
	if (!rest.empty() && rest[rest.size() - 1] == '/')
		rest.erase(rest.size() - 1);
	bool is_get = strcmp(ri->request_method, "GET") == 0;
	bool is_post = strcmp(ri->request_method, "POST") == 0;

	if (rest.empty()) {
		if (!is_get) {
			send_error(conn, 405, "GET required");
			return 405;
		}
		update_get(conn);
		return 200;
	}
	if (rest == "/changelog") {
		if (!is_get) {
			send_error(conn, 405, "GET required");
			return 405;
		}
		update_changelog(conn);
		return 200;
	}
	if (!is_post) {
		send_error(conn, 405, "POST required");
		return 405;
	}
	if (rest == "/check")
		update_check(conn);
	else if (rest == "/install")
		update_install(conn);
	else if (rest == "/os")
		update_os(conn);
	else if (rest == "/dismiss")
		update_dismiss(conn);
	else {
		send_error(conn, 404, "unknown path");
		return 404;
	}
	return 200;
}

void webupdate_register(struct mg_context *ctx) {
	// The status file the previous instance left is how a page that reconnects
	// after an install is told how it went, so it is read before the first
	// request can arrive.
	reload_status();
	mg_set_request_handler(ctx, "/api/update", api_update_handler, nullptr);
}
