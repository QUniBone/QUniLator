/* webserialports.cpp: which UART carries the Linux login, and which the emulator gets

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.

   The board brings out three UARTs. ttyS0 is on the debug header and wants a
   3.3 V TTL cable; ttyS1 and ttyS2 come out on the cape. Each is either a
   Linux login - the way back onto a board whose network has gone - or free for
   the emulator, which binds one to an emulated DL11 or to the console bridge
   that carries a real CPU's own SLU. One port is a login at all times, so a
   board is never left with the network as its only way in.

     GET /api/serialports   {ports:[{device,where,login,kernel_console,used_by}],
                             reboot_required}
     PUT /api/serialports   {logins:["ttyS0",...]}

   A login is a serial-getty@<port> unit, and the answer is what systemd says
   about it now. Turning one off masks the unit as well as disabling it, since
   the getty on the kernel console is generated rather than enabled and a
   disable alone would leave it standing.

   The kernel console follows the first port that carries a login: printk
   writing into a line an emulated device holds would put bytes on the wire
   that no PDP-11 program sent. That is a boot setting - console= in
   /boot/uEnv.txt - so a change to it is reported as needing a reboot rather
   than taken to have happened.
*/

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "civetweb.h"
#include "picojson.h"

#include "device_configuration.hpp"
#include "dl11w.hpp"

#include "weblog.hpp"
#include "webserialports.hpp"
#include "websettings.hpp"

static const char *SYSTEMCTL = "/usr/bin/systemctl";
static const char *UENV = "/boot/uEnv.txt";

// The UARTs and what an operator finds them on. Every board of this design
// carries all three; one whose kernel names fewer reports fewer.
static const struct {
	const char *device;
	const char *where;
} PORTS[] = {
	{ "ttyS0", "debug header, J1 pins 4/5 - needs a 3.3 V TTL adapter" },
	{ "ttyS1", "cape connector" },
	{ "ttyS2", "cape connector" },
};
static const size_t PORT_COUNT = sizeof(PORTS) / sizeof(PORTS[0]);

/*** running systemctl ***/

static bool have(const char *path) {
	return access(path, X_OK) == 0;
}

// Run argv[0] with argv, discarding what it says. Result is the exit status, or
// -1 when the program could not be run at all. execv takes the vector as it
// stands, so no shell parses a port name.
static int run(const char *const argv[]) {
	pid_t pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		int devnull = open("/dev/null", O_RDWR);
		if (devnull >= 0) {
			dup2(devnull, STDIN_FILENO);
			dup2(devnull, STDOUT_FILENO);
			dup2(devnull, STDERR_FILENO);
			close(devnull);
		}
		execv(argv[0], (char *const *) argv);
		_exit(127);
	}
	int status = 0;
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static std::string getty_unit(const std::string &port) {
	return "serial-getty@" + port + ".service";
}

static bool getty_active(const std::string &port) {
	if (!have(SYSTEMCTL))
		return false;
	std::string unit = getty_unit(port);
	const char *argv[] = { SYSTEMCTL, "is-active", "--quiet", unit.c_str(), nullptr };
	return run(argv) == 0;
}

static bool start_getty(const std::string &port) {
	std::string unit = getty_unit(port);
	const char *unmask[] = { SYSTEMCTL, "unmask", unit.c_str(), nullptr };
	const char *enable[] = { SYSTEMCTL, "enable", "--now", unit.c_str(), nullptr };
	run(unmask);
	return run(enable) == 0;
}

static bool stop_getty(const std::string &port) {
	std::string unit = getty_unit(port);
	const char *disable[] = { SYSTEMCTL, "disable", "--now", unit.c_str(), nullptr };
	const char *mask[] = { SYSTEMCTL, "mask", unit.c_str(), nullptr };
	run(disable);
	// masking as well: the getty on the kernel console is generated rather than
	// enabled, and a generated unit comes back at the next boot otherwise
	return run(mask) == 0;
}

/*** the ports ***/

std::vector<std::string> webserialports_all(void) {
	std::vector<std::string> ports;
	for (size_t i = 0; i < PORT_COUNT; i++) {
		std::string node = std::string("/dev/") + PORTS[i].device;
		struct stat st;
		if (stat(node.c_str(), &st) == 0)
			ports.push_back(PORTS[i].device);
	}
	return ports;
}

bool webserialports_has_login(const std::string &port) {
	for (size_t i = 0; i < PORT_COUNT; i++)
		if (port == PORTS[i].device)
			return getty_active(port);
	return false;
}

// What the emulator has bound to a port, if anything: an enabled DL11 whose
// serialport names it, or the console bridge carrying a real CPU's SLU.
static std::string emulator_holder(const std::string &port) {
	device_configuration_c *dc = device_configuration;
	if (dc != nullptr) {
		slu_c *slus[] = { dc->DL11, dc->DL11b };
		for (slu_c *slu : slus)
			if (slu != nullptr && slu->enabled.value && slu->serialport.value == port)
				return slu->name.value;
	}
	external_console_c ec = websettings_external_console();
	if (ec.source == "ttys2" && ec.port == port)
		return "the external console bridge";
	return "";
}

/*** the kernel console ***/

static bool read_file(const char *path, std::string *out) {
	FILE *f = fopen(path, "rb");
	if (f == nullptr)
		return false;
	char buf[4096];
	size_t n;
	out->clear();
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		out->append(buf, n);
	fclose(f);
	return true;
}

static bool write_file(const char *path, const std::string &content) {
	std::string tmp = std::string(path) + ".qunilator-new";
	FILE *f = fopen(tmp.c_str(), "wb");
	if (f == nullptr)
		return false;
	bool ok = fwrite(content.data(), 1, content.size(), f) == content.size();
	if (fclose(f) != 0)
		ok = false;
	if (ok)
		ok = chmod(tmp.c_str(), 0644) == 0 && rename(tmp.c_str(), path) == 0;
	if (!ok)
		unlink(tmp.c_str());
	return ok;
}

// The port a "console=<port>[,speed]" setting names, wherever the setting is
// written. Empty when it names none of the UARTs.
static std::string console_port_in(const std::string &text) {
	for (size_t i = 0; i < PORT_COUNT; i++) {
		std::string want = std::string("console=") + PORTS[i].device;
		size_t at = text.find(want);
		// a whole setting, so console=ttyS1 does not match console=ttyS12
		if (at != std::string::npos && (at == 0 || text[at - 1] == ' '
				|| text[at - 1] == '\n')) {
			char after = at + want.size() < text.size() ? text[at + want.size()] : ' ';
			if (after == ' ' || after == ',' || after == '\n')
				return PORTS[i].device;
		}
	}
	return "";
}

// The port the running kernel prints on, read from the command line it booted
// with.
static std::string kernel_console_port(void) {
	std::string cmdline;
	if (!read_file("/proc/cmdline", &cmdline))
		return "";
	return console_port_in(cmdline);
}

// The port the next boot will print on.
static std::string boot_console_port(void) {
	std::string uenv;
	if (!read_file(UENV, &uenv))
		return "";
	return console_port_in(uenv);
}

// A boot setting written but not yet booted with, which is what a reboot is
// wanted for. Derived from the two rather than remembered, so it survives a
// restart of the service and clears when the reboot happens.
static bool kernel_console_pending(void) {
	std::string boot = boot_console_port();
	return !boot.empty() && boot != kernel_console_port();
}

// Point the boot setting at a port, keeping the speed and framing the line
// already carries. Result is true when the file changed.
static bool set_boot_console(const std::string &port) {
	std::string text;
	if (!read_file(UENV, &text))
		return false;
	std::string out;
	bool changed = false;
	size_t pos = 0;
	while (pos <= text.size()) {
		size_t eol = text.find('\n', pos);
		size_t end = eol == std::string::npos ? text.size() : eol;
		std::string line = text.substr(pos, end - pos);
		if (line.compare(0, 8, "console=") == 0) {
			std::string tail; // ",115200n8" and anything else after the device
			size_t comma = line.find(',');
			if (comma != std::string::npos)
				tail = line.substr(comma);
			std::string want = "console=" + port + tail;
			if (line != want) {
				line = want;
				changed = true;
			}
		}
		out += line;
		if (eol == std::string::npos)
			break;
		out += '\n';
		pos = eol + 1;
	}
	if (!changed)
		return false;
	if (!write_file(UENV, out)) {
		WEB_WARNING("could not write %s: the kernel console still prints on %s",
				UENV, kernel_console_port().c_str());
		return false;
	}
	WEB_INFO("the kernel console prints on %s from the next boot", port.c_str());
	return true;
}

// Keep printk off a line the emulator has: the kernel console follows the first
// port that carries a login. A setting that already names one is left alone.
static void follow_kernel_console(const std::vector<std::string> &logins) {
	if (logins.empty())
		return;
	std::string boot = boot_console_port();
	if (!boot.empty() && webserialports_has_login(boot))
		return;
	set_boot_console(logins[0]);
}

/*** the API ***/

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

static const char *where_of(const std::string &port) {
	for (size_t i = 0; i < PORT_COUNT; i++)
		if (port == PORTS[i].device)
			return PORTS[i].where;
	return "";
}

static picojson::value ports_json(void) {
	std::string console = kernel_console_port();
	picojson::array ports;
	for (const std::string &port : webserialports_all()) {
		picojson::object o;
		o["device"] = picojson::value(port);
		o["where"] = picojson::value(where_of(port));
		o["login"] = picojson::value(getty_active(port));
		o["kernel_console"] = picojson::value(port == console);
		o["used_by"] = picojson::value(emulator_holder(port));
		ports.push_back(picojson::value(o));
	}
	picojson::object o;
	o["ports"] = picojson::value(ports);
	o["reboot_required"] = picojson::value(kernel_console_pending());
	return picojson::value(o);
}

static void serialports_get(struct mg_connection *conn) {
	send_json(conn, 200, ports_json());
}

static void serialports_put(struct mg_connection *conn) {
	picojson::value body;
	if (!read_json_body(conn, &body)) {
		send_error(conn, 400, "expected a JSON object");
		return;
	}
	if (!body.get("logins").is<picojson::array>()) {
		send_error(conn, 400, "logins is required, as an array of port names");
		return;
	}
	if (!have(SYSTEMCTL)) {
		send_error(conn, 501, "this host has no systemd to move a login with");
		return;
	}
	std::vector<std::string> all = webserialports_all();
	std::vector<std::string> wanted;
	const picojson::array &arr = body.get("logins").get<picojson::array>();
	for (const picojson::value &v : arr) {
		if (!v.is<std::string>()) {
			send_error(conn, 400, "logins holds port names");
			return;
		}
		std::string port = v.get<std::string>();
		bool known = false;
		for (const std::string &p : all)
			if (p == port)
				known = true;
		if (!known) {
			send_error(conn, 422, "\"" + port + "\" is not a serial port of this board");
			return;
		}
		wanted.push_back(port);
	}
	if (wanted.empty()) {
		send_error(conn, 422, "one port keeps its login: it is the way back onto "
				"a board whose network has gone");
		return;
	}
	// A port the emulator holds cannot take a login: the getty and the device
	// would read each other's bytes off the same line.
	for (const std::string &port : wanted) {
		if (getty_active(port))
			continue;
		std::string holder = emulator_holder(port);
		if (!holder.empty()) {
			send_error(conn, 409, "/dev/" + port + " is in use by " + holder);
			return;
		}
	}

	picojson::array warnings;
	for (const std::string &port : all) {
		bool want = false;
		for (const std::string &p : wanted)
			if (p == port)
				want = true;
		if (want == getty_active(port))
			continue;
		if (want) {
			if (start_getty(port))
				WEB_INFO("a Linux login answers on /dev/%s", port.c_str());
			else
				warnings.push_back(picojson::value(
						"the login on /dev/" + port + " did not start"));
		} else {
			if (stop_getty(port))
				WEB_INFO("/dev/%s is free for the emulator", port.c_str());
			else
				warnings.push_back(picojson::value(
						"the login on /dev/" + port + " did not stop"));
		}
	}
	follow_kernel_console(wanted);

	picojson::value answer = ports_json();
	answer.get<picojson::object>()["warnings"] = picojson::value(warnings);
	send_json(conn, 200, answer);
}

static int api_serialports_handler(struct mg_connection *conn, void * /*cbdata*/) {
	const struct mg_request_info *ri = mg_get_request_info(conn);
	if (!strcmp(ri->request_method, "GET")) {
		serialports_get(conn);
		return 200;
	}
	if (!strcmp(ri->request_method, "PUT") || !strcmp(ri->request_method, "POST")) {
		serialports_put(conn);
		return 200;
	}
	send_error(conn, 405, "method not allowed");
	return 405;
}

void webserialports_register(struct mg_context *ctx) {
	mg_set_request_handler(ctx, "/api/serialports", api_serialports_handler, nullptr);
}
