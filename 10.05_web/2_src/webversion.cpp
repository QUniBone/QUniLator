/* webversion.cpp: GET /api/version — what this board runs

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.

   The service states its own version and the package that carries it, so an
   update can be reasoned about from both sides:

     - the frontend compares the version it was built from with what this
       reports and reloads when they differ, which carries every open page onto
       the matching bundle after any upgrade, including a hand-run apt upgrade;
     - the updater reads /run/qunilator/version to prove that the instance now
       answering is the one the new package installed - a check that needs no
       credentials, so it works on a password-protected board too.

   The version is compiled in from packaging/debian/changelog (the makefiles'
   -DQUNILATOR_VERSION), so the binary and the package cannot disagree. The
   package name comes from the bus the binary was built for.
*/

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include <string>

#include "civetweb.h"
#include "picojson.h"

#include "logger.hpp"
#include "weblog.hpp"
#include "webversion.hpp"

// The makefiles define this from packaging/debian/changelog. A build outside
// them (an editor's index, a host-side test) says so rather than naming a
// version it cannot have.
#ifndef QUNILATOR_VERSION
#define QUNILATOR_VERSION	"0.0.0-dev"
#endif

// rebuilt on every make run, so it dates this binary rather than a cached object
extern const char *compile_timestamp;

// The runtime directory the unit's RuntimeDirectory= creates. Written at
// startup and gone when the unit stops, so a stale file cannot make a service
// that failed to come up look like one that did.
static const char *RUNTIME_DIR = "/run/qunilator";
static const char *RUNTIME_VERSION_FILE = "/run/qunilator/version";

std::string webversion_package(void) {
#if defined(QBUS)
	return "qbone";
#elif defined(UNIBUS)
	return "unibone";
#else
	return "qunilator";     // host-side test build, owned by no package
#endif
}

std::string webversion_version(void) {
	return QUNILATOR_VERSION;
}

// __DATE__ " " __TIME__ ("Jul 30 2026 09:12:00") as ISO 8601. No zone suffix:
// it is the build machine's local clock, and naming it UTC would be a guess.
std::string webversion_built(void) {
	struct tm tmv;
	memset(&tmv, 0, sizeof(tmv));
	if (strptime(compile_timestamp, "%b %d %Y %H:%M:%S", &tmv) == nullptr)
		return compile_timestamp;
	char iso[32];
	if (strftime(iso, sizeof iso, "%Y-%m-%dT%H:%M:%S", &tmv) == 0)
		return compile_timestamp;
	return iso;
}

// The version of the running instance, for a reader that cannot authenticate.
static void write_runtime_version(void) {
	// RuntimeDirectory= makes this on the board; a hand-started service (a
	// development run, or one outside systemd) has to make it itself.
	if (mkdir(RUNTIME_DIR, 0755) != 0 && errno != EEXIST) {
		WEB_WARNING("%s not writable (%s): an update cannot verify this instance",
				RUNTIME_DIR, strerror(errno));
		return;
	}
	std::string tmp = std::string(RUNTIME_VERSION_FILE) + ".new";
	FILE *f = fopen(tmp.c_str(), "w");
	if (f == nullptr) {
		WEB_WARNING("%s not writable (%s): an update cannot verify this instance",
				RUNTIME_VERSION_FILE, strerror(errno));
		return;
	}
	// Renamed into place, so a reader never sees it half written - the updater
	// polls this file while the service is starting up.
	fprintf(f, "%s\n", webversion_version().c_str());
	bool ok = (fclose(f) == 0);
	if (!ok || rename(tmp.c_str(), RUNTIME_VERSION_FILE) != 0) {
		WEB_WARNING("%s could not be written: %s", RUNTIME_VERSION_FILE, strerror(errno));
		unlink(tmp.c_str());
	}
}

static int api_version_handler(struct mg_connection *conn, void * /*cbdata*/) {
	const struct mg_request_info *ri = mg_get_request_info(conn);
	if (strcmp(ri->request_method, "GET") != 0) {
		mg_printf(conn, "HTTP/1.1 405 Error\r\nContent-Type: application/json\r\n"
				"Cache-Control: no-store\r\nContent-Length: 22\r\n\r\n"
				"{\"error\":\"GET required\"}");
		return 405;
	}
	picojson::object o;
	o["package"] = picojson::value(webversion_package());
	o["version"] = picojson::value(webversion_version());
	o["built"] = picojson::value(webversion_built());
	o["api_version"] = picojson::value((double) 0);
	std::string body = picojson::value(o).serialize();
	mg_printf(conn,
			"HTTP/1.1 200 OK\r\n"
			"Content-Type: application/json\r\n"
			"Cache-Control: no-store\r\n"
			"Content-Length: %u\r\n\r\n", (unsigned) body.size());
	mg_write(conn, body.c_str(), body.size());
	return 200;
}

void webversion_register(struct mg_context *ctx) {
	write_runtime_version();
	mg_set_request_handler(ctx, "/api/version", api_version_handler, nullptr);
	WEB_INFO("%s %s, built %s", webversion_package().c_str(),
			webversion_version().c_str(), webversion_built().c_str());
}
