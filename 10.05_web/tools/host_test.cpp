/* host_test.cpp: run webserver_c on the development host, without hardware

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see any source file header for the full text.

   Serves 10.05_web/3_frontend and the /api/ endpoints so server and frontend
   can be exercised without a BeagleBone. Provides stub implementations of the
   logger/logsource interfaces, which are hardware-free but Linux-specific in
   their real implementation.

   Build & run (from 10.05_web/tools):
     cc  -c -O2 -DNO_SSL -DUSE_WEBSOCKET -DNO_CGI \
         -I ../../91_3rd_party/civetweb ../../91_3rd_party/civetweb/civetweb.c \
         -o civetweb.o
     c++ -std=c++11 -Wall -Wextra \
         -I ../2_src -I ../../91_3rd_party/civetweb -I ../../91_3rd_party/picojson \
         -I ../../90_common/src \
         host_test.cpp ../2_src/webserver.cpp civetweb.o -lpthread -o host_test
     ./host_test [port]      # default 8090, docroot ../3_frontend

   Check:
     curl http://localhost:8090/api/state
     curl -I http://localhost:8090/
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>

#include "logger.hpp"
#include "logsource.hpp"
#include "webserver.hpp"

/*** logger/logsource stubs: print everything to stderr ***/
logger_c *logger = nullptr;

logger_c::logger_c() {
	fifo = nullptr;
	fifo_capacity = fifo_readidx = fifo_writeidx = fifo_fill = 0;
	messagecount = 0;
	life_level = LL_DEBUG;
}
logger_c::~logger_c() {
}
void logger_c::vlog(logsource_c *logsource, unsigned msglevel, bool /*late_evaluation*/,
		const char * /*srcfilename*/, unsigned /*srcline*/, const char *fmt, va_list args) {
	fprintf(stderr, "[%s %u] ", logsource->log_label.c_str(), msglevel);
	vfprintf(stderr, fmt, args);
	fprintf(stderr, "\n");
}

logsource_c::logsource_c() {
	log_level = LL_DEBUG;
	log_level_ptr = &log_level;
	log_id = 0;
}
logsource_c::~logsource_c() {
}
void logsource_c::connect() {
}
void logsource_c::disconnect() {
}

/* Fixture /api/devices and /api/command handlers so the frontend's live mode
   is testable in a browser without hardware. The real implementations live
   in webapi.cpp and need the device registry and application. */
#include "civetweb.h"

/* A representative live device set with categories and the RL lamp
   parameters, so the dashboard widget framework can be exercised offline:
   RL controller + two RL02 disks (bezel widgets), an RK disk (generic disk
   widget), a DL11 (serial → terminal tab), a DELQA (network widget), a line
   clock (no widget), and a disabled MSCP controller (hidden). */
static const char *fixture_devices =
	"["
	"{\"name\":\"rl\",\"type\":\"RLV12\",\"category\":\"controller\",\"enabled\":true,\"parent\":null,\"params\":["
	  "{\"name\":\"base_addr\",\"shortname\":\"addr\",\"readonly\":true,\"type\":\"unsigned\","
	  "\"value\":260352,\"base\":8,\"bitwidth\":18,\"info\":\"controller base address in IO page\"}]},"
	"{\"name\":\"rl0\",\"type\":\"RL02\",\"category\":\"disk\",\"removable\":true,\"locked\":false,\"status\":\"ready\",\"enabled\":true,\"parent\":\"rl\",\"params\":["
	  "{\"name\":\"unit\",\"shortname\":\"u\",\"readonly\":true,\"type\":\"unsigned\",\"value\":0,\"base\":10,\"bitwidth\":8,\"info\":\"unit\"},"
	  "{\"name\":\"image\",\"shortname\":\"img\",\"readonly\":false,\"type\":\"string\",\"value\":\"rt11v53.rl02\",\"info\":\"Path to binary image file\"},"
	  "{\"name\":\"loadlamp\",\"shortname\":\"ll\",\"readonly\":true,\"type\":\"bool\",\"value\":false,\"info\":\"LOAD lamp\"},"
	  "{\"name\":\"readylamp\",\"shortname\":\"rl\",\"readonly\":true,\"type\":\"bool\",\"value\":true,\"info\":\"READY lamp\"},"
	  "{\"name\":\"faultlamp\",\"shortname\":\"fl\",\"readonly\":true,\"type\":\"bool\",\"value\":false,\"info\":\"FAULT lamp\"},"
	  "{\"name\":\"writeprotectlamp\",\"shortname\":\"wpl\",\"readonly\":true,\"type\":\"bool\",\"value\":false,\"info\":\"WRITE PROT lamp\"},"
	  "{\"name\":\"runstopbutton\",\"shortname\":\"rsb\",\"readonly\":false,\"type\":\"bool\",\"value\":true,\"info\":\"RUN/STOP\"},"
	  "{\"name\":\"writeprotectbutton\",\"shortname\":\"wpb\",\"readonly\":false,\"type\":\"bool\",\"value\":false,\"info\":\"WRITE PROT\"}]},"
	"{\"name\":\"rl1\",\"type\":\"RL02\",\"category\":\"disk\",\"removable\":true,\"locked\":false,\"status\":\"ready\",\"enabled\":true,\"parent\":\"rl\",\"params\":["
	  "{\"name\":\"unit\",\"shortname\":\"u\",\"readonly\":true,\"type\":\"unsigned\",\"value\":1,\"base\":10,\"bitwidth\":8,\"info\":\"unit\"},"
	  "{\"name\":\"image\",\"shortname\":\"img\",\"readonly\":false,\"type\":\"string\",\"value\":\"games.rl02\",\"info\":\"Path to binary image file\"},"
	  "{\"name\":\"loadlamp\",\"shortname\":\"ll\",\"readonly\":true,\"type\":\"bool\",\"value\":false,\"info\":\"LOAD lamp\"},"
	  "{\"name\":\"readylamp\",\"shortname\":\"rl\",\"readonly\":true,\"type\":\"bool\",\"value\":true,\"info\":\"READY lamp\"},"
	  "{\"name\":\"faultlamp\",\"shortname\":\"fl\",\"readonly\":true,\"type\":\"bool\",\"value\":false,\"info\":\"FAULT lamp\"},"
	  "{\"name\":\"writeprotectlamp\",\"shortname\":\"wpl\",\"readonly\":true,\"type\":\"bool\",\"value\":true,\"info\":\"WRITE PROT lamp\"},"
	  "{\"name\":\"runstopbutton\",\"shortname\":\"rsb\",\"readonly\":false,\"type\":\"bool\",\"value\":true,\"info\":\"RUN/STOP\"},"
	  "{\"name\":\"writeprotectbutton\",\"shortname\":\"wpb\",\"readonly\":false,\"type\":\"bool\",\"value\":true,\"info\":\"WRITE PROT\"}]},"
	"{\"name\":\"rk\",\"type\":\"RKV11\",\"category\":\"controller\",\"enabled\":true,\"parent\":null,\"params\":["
	  "{\"name\":\"base_addr\",\"shortname\":\"addr\",\"readonly\":true,\"type\":\"unsigned\",\"value\":261000,\"base\":8,\"bitwidth\":18,\"info\":\"base address\"}]},"
	"{\"name\":\"rk0\",\"type\":\"RK05\",\"category\":\"disk\",\"removable\":true,\"locked\":false,\"status\":\"idle\",\"enabled\":true,\"parent\":\"rk\",\"params\":["
	  "{\"name\":\"unit\",\"shortname\":\"u\",\"readonly\":true,\"type\":\"unsigned\",\"value\":0,\"base\":10,\"bitwidth\":8,\"info\":\"unit\"},"
	  "{\"name\":\"image\",\"shortname\":\"img\",\"readonly\":false,\"type\":\"string\",\"value\":\"\",\"info\":\"Path to binary image file\"}]},"
	"{\"name\":\"DL11\",\"type\":\"slu_c\",\"category\":\"serial\",\"enabled\":true,\"parent\":null,\"params\":["
	  "{\"name\":\"base_addr\",\"shortname\":\"addr\",\"readonly\":true,\"type\":\"unsigned\",\"value\":777560,\"base\":8,\"bitwidth\":18,\"info\":\"console SLU\"},"
	  "{\"name\":\"serialport\",\"shortname\":\"p\",\"readonly\":false,\"type\":\"string\",\"value\":\"ttyS2\",\"info\":\"Linux serial port\"}]},"
	"{\"name\":\"delqa\",\"type\":\"DELQA\",\"category\":\"network\",\"enabled\":true,\"parent\":null,\"params\":["
	  "{\"name\":\"base_addr\",\"shortname\":\"addr\",\"readonly\":true,\"type\":\"unsigned\",\"value\":774440,\"base\":8,\"bitwidth\":18,\"info\":\"DELQA base\"},"
	  "{\"name\":\"mac\",\"shortname\":\"mac\",\"readonly\":false,\"type\":\"string\",\"value\":\"08:00:2b:24:8d:47\",\"info\":\"station address\"}]},"
	"{\"name\":\"KW11\",\"type\":\"ltc_c\",\"category\":\"other\",\"enabled\":true,\"parent\":null,\"params\":["
	  "{\"name\":\"base_addr\",\"shortname\":\"addr\",\"readonly\":true,\"type\":\"unsigned\",\"value\":777546,\"base\":8,\"bitwidth\":18,\"info\":\"line clock\"}]},"
	"{\"name\":\"uda\",\"type\":\"UDA50\",\"category\":\"controller\",\"enabled\":false,\"parent\":null,\"params\":["
	  "{\"name\":\"base_addr\",\"shortname\":\"addr\",\"readonly\":false,\"type\":\"unsigned\",\"value\":772150,\"base\":8,\"bitwidth\":18,\"info\":\"MSCP base\"}]}"
	"]";

/* in-memory /api/settings state */
static std::string settings_source = "webserial";
static unsigned settings_baud = 38400;
static unsigned settings_addrwidth = 22;

static const char *fixture_images =
	"[{\"name\":\"rt11v53.rl02\",\"path\":\"rt11v53.rl02\",\"size\":10485760,\"attached\":[\"rl0\"],\"mtime\":\"2026-07-14 09:12\"},"
	"{\"name\":\"games.rl02\",\"path\":\"games.rl02\",\"size\":10485760,\"attached\":[\"rl1\"],\"mtime\":\"2026-07-10 21:40\"},"
	"{\"name\":\"xxdp25.rl02\",\"path\":\"xxdp25.rl02\",\"size\":10485760,\"attached\":[],\"mtime\":\"2026-05-02 18:03\"},"
	"{\"name\":\"rkboot.rk05\",\"path\":\"rkboot.rk05\",\"size\":2494464,\"attached\":[],\"mtime\":\"2026-06-01 10:00\"}]";

static void fixture_send(struct mg_connection *conn, const char *body) {
	mg_printf(conn, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
			"Content-Length: %u\r\n\r\n", (unsigned) strlen(body));
	mg_write(conn, body, strlen(body));
}

// GET /api/devices → fixture list; PUT /api/devices/<d>/params/<p> → log + ok
static int fixture_devices_handler(struct mg_connection *conn, void *) {
	const struct mg_request_info *ri = mg_get_request_info(conn);
	if (strcmp(ri->request_method, "GET") == 0) {
		fixture_send(conn, fixture_devices);
		return 200;
	}
	char body[1024];
	int n = mg_read(conn, body, sizeof(body) - 1);
	body[n > 0 ? n : 0] = 0;
	fprintf(stderr, "fixture %s %s: %s\n", ri->request_method, ri->local_uri, body);
	fixture_send(conn, "{\"ok\":true}");
	return 200;
}

static int fixture_control_handler(struct mg_connection *conn, void *) {
	char body[1024];
	int n = mg_read(conn, body, sizeof(body) - 1);
	body[n > 0 ? n : 0] = 0;
	fprintf(stderr, "fixture /api/control: %s\n", body);
	fixture_send(conn, "{\"ok\":true}");
	return 200;
}

// GET /api/settings → current state; PUT → crude parse of the fields the
// frontend sends, enough to exercise the Machine page offline
static int fixture_settings_handler(struct mg_connection *conn, void *) {
	const struct mg_request_info *ri = mg_get_request_info(conn);
	if (strcmp(ri->request_method, "PUT") == 0 || strcmp(ri->request_method, "POST") == 0) {
		char body[1024];
		int n = mg_read(conn, body, sizeof(body) - 1);
		body[n > 0 ? n : 0] = 0;
		std::string b(body);
		std::string warn;
		size_t p;
		if ((p = b.find("\"address_width\"")) != std::string::npos)
			settings_addrwidth = (unsigned) atoi(b.c_str() + b.find(':', p) + 1);
		if ((p = b.find("\"source\"")) != std::string::npos) {
			size_t q = b.find('"', b.find(':', p) + 1);
			size_t e = b.find('"', q + 1);
			settings_source = b.substr(q + 1, e - q - 1);
		}
		if ((p = b.find("\"baud\"")) != std::string::npos)
			settings_baud = (unsigned) atoi(b.c_str() + b.find(':', p) + 1);
		fprintf(stderr, "fixture PUT /api/settings: %s\n", body);
		std::string res = "{\"ok\":true,\"warnings\":[]}";
		fixture_send(conn, res.c_str());
		return 200;
	}
	char buf[512];
	snprintf(buf, sizeof(buf),
		"{\"platform\":\"QBUS\",\"address_width\":%u,"
		"\"external_console\":{\"source\":\"%s\",\"port\":\"ttyS2\",\"baud\":%u}}",
		settings_addrwidth, settings_source.c_str(), settings_baud);
	fixture_send(conn, buf);
	return 200;
}

static int fixture_images_handler(struct mg_connection *conn, void *) {
	fixture_send(conn, fixture_images);
	return 200;
}

/* Two saved snapshots and the live setup, in the shape webconfigs.cpp emits:
   "rt11" is what the fixture device set is currently running, so the frontend
   marks it as the loaded configuration; "xxdp" differs. */
static const char *fixture_config_rt11 =
	"{\"devices\":["
	"{\"name\":\"rl\",\"enabled\":true,\"params\":{}},"
	"{\"name\":\"rl0\",\"enabled\":true,\"params\":{\"image\":\"rt11v53.rl02\"}},"
	"{\"name\":\"rl1\",\"enabled\":true,\"params\":{\"image\":\"games.rl02\",\"writeprotectbutton\":\"1\"}},"
	"{\"name\":\"DL11\",\"enabled\":true,\"params\":{\"serialport\":\"ttyS2\"}},"
	"{\"name\":\"delqa\",\"enabled\":true,\"params\":{\"mac\":\"08:00:2b:24:8d:47\"}}"
	"]}";
static const char *fixture_config_xxdp =
	"{\"devices\":["
	"{\"name\":\"rl\",\"enabled\":true,\"params\":{}},"
	"{\"name\":\"rl0\",\"enabled\":true,\"params\":{\"image\":\"xxdp25.rl02\"}},"
	"{\"name\":\"DL11\",\"enabled\":true,\"params\":{\"serialport\":\"ttyS2\"}}"
	"]}";
// the object form: current/default pointers, the live modified flag, and the
// saved configurations. The live setup matches "rt11", so it is current and
// unmodified; "rt11" is also the default.
static const char *fixture_configs_list =
	"{\"current\":\"rt11\",\"default\":\"rt11\",\"modified\":false,\"configs\":["
	"{\"name\":\"rt11\",\"mtime\":\"2026-07-19 09:12\",\"default\":true,\"enabled\":[\"rl\",\"rl0\",\"rl1\",\"DL11\",\"delqa\"]},"
	"{\"name\":\"xxdp\",\"mtime\":\"2026-07-18 21:40\",\"default\":false,\"enabled\":[\"rl\",\"rl0\",\"DL11\"]}]}";

// GET /api/configs[?current=1], GET/PUT/DELETE /api/configs/<name>,
// POST /api/configs/<name>/apply
static int fixture_configs_handler(struct mg_connection *conn, void *) {
	const struct mg_request_info *ri = mg_get_request_info(conn);
	std::string rest = std::string(ri->local_uri ? ri->local_uri : "")
			.substr(strlen("/api/configs"));
	if (rest.empty() || rest == "/") {
		const char *q = ri->query_string;
		// the live setup matches the "rt11" snapshot
		fixture_send(conn, q != nullptr && strstr(q, "current") != nullptr
				? fixture_config_rt11 : fixture_configs_list);
		return 200;
	}
	if (strcmp(ri->request_method, "GET") == 0) {
		fixture_send(conn, rest == "/xxdp" ? fixture_config_xxdp : fixture_config_rt11);
		return 200;
	}
	fprintf(stderr, "fixture %s %s\n", ri->request_method, ri->local_uri);
	fixture_send(conn, "{\"ok\":true,\"errors\":[]}");
	return 200;
}

void webapi_register(struct mg_context *ctx) {
	mg_set_request_handler(ctx, "/api/devices", fixture_devices_handler, nullptr);
	mg_set_request_handler(ctx, "/api/control", fixture_control_handler, nullptr);
	mg_set_request_handler(ctx, "/api/settings", fixture_settings_handler, nullptr);
	mg_set_request_handler(ctx, "/api/images", fixture_images_handler, nullptr);
	mg_set_request_handler(ctx, "/api/configs", fixture_configs_handler, nullptr);
}

void webapi_shutdown(void) {
}

int main(int argc, char **argv) {
	unsigned port = (argc > 1) ? (unsigned) atoi(argv[1]) : 8090;
	logger = new logger_c();
	webserver_c srv(port, "../3_frontend");
	if (!srv.start()) {
		fprintf(stderr, "server start failed\n");
		return 1;
	}
	printf("serving ../3_frontend on http://localhost:%u — Ctrl+C to stop\n", port);
	for (;;)
		pause();
	return 0;
}
