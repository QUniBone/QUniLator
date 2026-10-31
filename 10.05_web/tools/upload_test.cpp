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

   A VHD in the library is a medium in its own right: a blank one is made by
   name, the listing says what it is and how big its disk is, and it is offered
   no copy-on-write overlay, even with an ".ovl" lying beside it.

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
#include "picojson.h"
#include "logger.hpp"
#include "device.hpp"
#include "storagedrive.hpp"
#include "storageimage_vhd.hpp"
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
static void check(bool ok, const std::string &what)
{
	checks++;
	if (!ok) {
		failures++;
		fprintf(stderr, "FAIL: %s\n", what.c_str());
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

// One request with an optional body. Returns the HTTP status, 0 for none, and
// the reply's body in *body.
static int http(const std::string &method, const std::string &path,
		const std::string &content, std::string *body)
{
	std::string request = method + " " + path + " HTTP/1.1\r\n"
			"Host: localhost\r\n"
			"Content-Type: application/json\r\n"
			"Content-Length: " + std::to_string(content.size()) + "\r\n"
			"Connection: close\r\n\r\n" + content;
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
	if (send(fd, request.data(), request.size(), 0) != (ssize_t) request.size()) {
		close(fd);
		return 0;
	}
	std::string resp;
	char buf[4096];
	ssize_t r;
	while ((r = recv(fd, buf, sizeof buf, 0)) > 0)
		resp.append(buf, (size_t) r);
	close(fd);
	size_t split = resp.find("\r\n\r\n");
	*body = split == std::string::npos ? "" : resp.substr(split + 4);
	int status = 0;
	if (sscanf(resp.c_str(), "HTTP/1.1 %d", &status) != 1)
		status = 0;
	return status;
}

// The GET /api/images entry for an image, or a null value.
static picojson::value listed(const std::string &path)
{
	std::string body;
	picojson::value v;
	if (http("GET", "/api/images", "", &body) != 200 || !picojson::parse(v, body).empty())
		return picojson::value();
	for (const picojson::value &im : v.get("images").get<picojson::array>())
		if (im.get("path").is<std::string>() && im.get("path").get<std::string>() == path)
			return im;
	return picojson::value();
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
		// What a drive may be given: a stored image, its compressed master, or
		// nothing at all - and not a name with no file behind it.
		check(webstorage_image_exists("images/whole/disk.dsk"), "an uploaded image exists");
		check(!webstorage_image_exists("images/cut/disk.dsk"),
				"an image whose upload was cut off does not exist");
		FILE *gz = fopen((root + "/images/whole/packed.dsk.gz").c_str(), "wb");
		if (gz != nullptr)
			fclose(gz);
		check(webstorage_image_exists("images/whole/packed.dsk"),
				"an image with only its compressed master exists");
		check(webstorage_image_exists(""), "an empty drive needs no file");
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

	{
		// A blank VHD is made by its name, a dynamic disk of the size asked for.
		std::string body;
		int status = http("POST", "/api/images",
				"{\"name\":\"blank.vhd\",\"dir\":\"whole\",\"kind\":\"disk\",\"size\":1048576}", &body);
		check(status == 200, "a blank VHD is created");
		std::string vhd = root + "/images/whole/blank.vhd";
		uint64_t disk_size = 0;
		check(storageimage_vhd_c::is_vhd_file(vhd, &disk_size) && disk_size == 1048576,
				"a blank VHD is a VHD of the size asked for");
		status = http("POST", "/api/images",
				"{\"name\":\"blank.vhd\",\"dir\":\"whole\",\"kind\":\"disk\",\"size\":1048576}", &body);
		check(status == 409, "a blank VHD does not overwrite a file");
		status = http("POST", "/api/images",
				"{\"name\":\"odd.vhd\",\"dir\":\"whole\",\"kind\":\"disk\",\"size\":1000}", &body);
		check(status == 400, "a blank VHD of no whole number of sectors is refused");

		// the listing says what it is, and offers no overlay - not even the
		// one an ".ovl" left beside it would make
		FILE *ovl = fopen((vhd + ".ovl").c_str(), "wb");
		if (ovl != nullptr) {
			fputs("stale", ovl);
			fclose(ovl);
		}
		picojson::value im = listed("whole/blank.vhd");
		check(im.is<picojson::object>(), "a VHD is listed");
		check(im.get("format").is<std::string>() && im.get("format").get<std::string>() == "vhd",
				"a VHD is listed as one");
		check(im.get("disk_size").is<double>() && im.get("disk_size").get<double>() == 1048576,
				"a VHD is listed with its disk's size");
		check(im.get("overlay").is<bool>() && !im.get("overlay").get<bool>(),
				"a VHD is listed with no overlay");
		for (const char *action : {"discard", "commit"}) {
			status = http("POST", std::string("/api/images/whole/blank.vhd/overlay/") + action, "", &body);
			check(status == 404, std::string("a VHD answers no overlay to ") + action);
		}
		check(file_holds(vhd + ".ovl", "stale"), "an overlay op on a VHD leaves the files alone");
		check(storageimage_vhd_c::is_vhd_file(vhd), "an overlay op on a VHD leaves the VHD alone");

		picojson::value raw = listed("whole/disk.dsk");
		check(raw.is<picojson::object>() && !raw.contains("format"),
				"a raw image is listed with no format");
	}

	mg_stop(ctx);
	std::string cleanup = "rm -rf '" + root + "'";
	(void) !system(cleanup.c_str());

	printf("upload_test: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
