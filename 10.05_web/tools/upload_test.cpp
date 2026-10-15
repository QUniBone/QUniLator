/* upload_test.cpp: host test of the image upload (POST /api/images, webstorage.cpp)

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see any source file header for the full text.

   Runs the real webstorage.cpp under a real civetweb on a loopback port, with
   the images root in a temp directory, and talks HTTP to it over a socket. An
   upload is judged by what it leaves on disk as well as by what it answers: a
   complete one stores the file byte for byte, and one whose body ends early is
   refused and leaves no file behind.

   webstorage.cpp names devices, drives and the configuration model only for the
   endpoints this test does not call, so those are stubbed below.

   civetweb's own complaints go through the same callback the service installs,
   and the test reads them back from the logger's file sink.

   Build & run: 10.05_web/tools/run_config_test.sh
*/

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <mutex>
#include <string>
#include <vector>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "civetweb.h"
#include "logger.hpp"
#include "device.hpp"
#include "storagedrive.hpp"
#include "webconfigs.hpp"
#include "webevents.hpp"
#include "weblog.hpp"
#include "webstorage.hpp"

/*** what webstorage.cpp links against outside the upload path ***/
std::list<device_c *> device_c::mydevices;
std::mutex device_c::mydevices_mutex;
device_c::~device_c() {}
bool device_c::on_param_changed(parameter_c *) { return true; }
void storagedrive_c::refresh_activity(void) {}
storagedrive_c::~storagedrive_c() {}
bool storagedrive_c::on_param_changed(parameter_c *) { return true; }
std::string webconfigs_image_referenced(const std::string &) { return ""; }
std::vector<config_image_use_t> webconfigs_image_uses(const std::string &) { return {}; }
bool webevents_is_halted(void) { return false; }
bool webevents_is_powered(void) { return false; }

static int failures = 0;
static int checks = 0;
static void check(bool ok, const char *what)
{
	checks++;
	if (!ok) {
		failures++;
		fprintf(stderr, "FAIL: %s\n", what);
	}
}

static const int PORT = 18193;

static bool file_holds(const std::string &path, const std::string &expect)
{
	FILE *f = fopen(path.c_str(), "rb");
	if (f == nullptr)
		return false;
	std::string got;
	char buf[65536];
	size_t n;
	while ((n = fread(buf, 1, sizeof buf, f)) > 0)
		got.append(buf, n);
	fclose(f);
	return got == expect;
}

static std::string read_file(const std::string &path)
{
	std::string got;
	FILE *f = fopen(path.c_str(), "rb");
	if (f == nullptr)
		return got;
	char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof buf, f)) > 0)
		got.append(buf, n);
	fclose(f);
	return got;
}

// The logger writes on its own thread, so a line arrives a moment after it is
// logged.
static bool log_shows(const std::string &logfile, const std::string &text)
{
	for (int i = 0; i < 200; i++) {
		if (read_file(logfile).find(text) != std::string::npos)
			return true;
		usleep(10000);
	}
	return false;
}

static bool exists(const std::string &path)
{
	struct stat st;
	return stat(path.c_str(), &st) == 0;
}

// One multipart request carrying a single file field. The header announces
// the whole body; only the first `send_bytes` of it are sent before the
// connection's sending side is shut, which is what a client that goes away
// mid-upload looks like to the server. Returns the HTTP status, 0 for none.
static int upload(const std::string &dir, const std::string &filename,
		const std::string &content, size_t send_bytes, std::string *reply)
{
	const std::string boundary = "----qunilatoruploadtest";
	std::string body = "--" + boundary + "\r\n"
			"Content-Disposition: form-data; name=\"file\"; filename=\""
			+ filename + "\"\r\n"
			"Content-Type: application/octet-stream\r\n\r\n"
			+ content + "\r\n--" + boundary + "--\r\n";
	std::string head = "POST /api/images?dir=" + dir + " HTTP/1.1\r\n"
			"Host: localhost\r\n"
			"Content-Type: multipart/form-data; boundary=" + boundary + "\r\n"
			"Content-Length: " + std::to_string(body.size()) + "\r\n"
			"Connection: close\r\n\r\n";
	std::string request = head + body.substr(0, std::min(send_bytes, body.size()));

	int fd = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in sa;
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_port = htons(PORT);
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (connect(fd, (struct sockaddr *) &sa, sizeof sa) != 0) {
		close(fd);
		return 0;
	}
	size_t off = 0;
	while (off < request.size()) {
		ssize_t w = send(fd, request.data() + off, request.size() - off, 0);
		if (w <= 0)
			break;
		off += (size_t) w;
	}
	shutdown(fd, SHUT_WR);
	std::string resp;
	char buf[4096];
	ssize_t r;
	while ((r = recv(fd, buf, sizeof buf, 0)) > 0)
		resp.append(buf, (size_t) r);
	close(fd);
	*reply = resp;
	int status = 0;
	if (sscanf(resp.c_str(), "HTTP/1.1 %d", &status) != 1)
		status = 0;
	return status;
}

int main()
{
	logger = new logger_c();

	char tmpl[] = "/tmp/upload_test.XXXXXX";
	std::string root = mkdtemp(tmpl);
	setenv("QUNILATOR_DIR", root.c_str(), 1);
	std::string logfile = root + "/log";
	logger->set_file_sink(logfile.c_str());

	std::string port = std::to_string(PORT);
	const char *options[] = {
		"listening_ports", port.c_str(),
		"num_threads", "4",
		"request_timeout_ms", "2000",
		nullptr };
	struct mg_callbacks callbacks;
	memset(&callbacks, 0, sizeof callbacks);
	callbacks.log_message = weblog_civetweb_message;
	mg_init_library(0);
	struct mg_context *ctx = mg_start(&callbacks, nullptr, options);
	check(ctx != nullptr, "the server starts");
	if (ctx == nullptr)
		return 1;
	webstorage_register(ctx);

	// A disk image's worth of bytes that are not all the same, so a store that
	// drops or repeats a buffer shows.
	std::string content(3 * 1024 * 1024 + 517, '\0');
	for (size_t i = 0; i < content.size(); i++)
		content[i] = (char) (i * 7 + i / 4096);

	{
		std::string reply;
		int status = upload("whole", "disk.dsk", content, (size_t) -1, &reply);
		check(status == 200, "a complete upload is accepted");
		check(reply.find("\"ok\":true") != std::string::npos, "a complete upload answers ok");
		check(file_holds(root + "/images/whole/disk.dsk", content),
				"a complete upload stores the file byte for byte");
	}

	{
		std::string reply;
		int status = upload("cut", "disk.dsk", content, content.size() / 2, &reply);
		check(status != 200, "an upload whose body ends early is refused");
		check(reply.find("\"ok\":true") == std::string::npos,
				"an upload whose body ends early does not answer ok");
		check(!exists(root + "/images/cut/disk.dsk"),
				"an upload whose body ends early leaves no file");
	}

	{
		// A file that cannot be created is civetweb's to report; it reaches
		// the log, and the upload is refused.
		mkdir((root + "/images/blocked").c_str(), 0755);
		mkdir((root + "/images/blocked/disk.dsk").c_str(), 0755);
		std::string reply;
		int status = upload("blocked", "disk.dsk", content, (size_t) -1, &reply);
		check(status != 200, "an upload that cannot be written is refused");
		check(log_shows(logfile, "Cannot create file"),
				"civetweb's own error reaches the log");
	}

	mg_stop(ctx);
	std::string cleanup = "rm -rf '" + root + "'";
	(void) !system(cleanup.c_str());

	printf("upload_test: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
