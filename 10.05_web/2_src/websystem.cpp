/* websystem.cpp: the board's own name and the operator's ssh key

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.

   Two settings of the appliance around the emulator, both asked for by the
   first-run dialog and both changeable afterwards from the System page.

     GET /api/hostname   {hostname}
     PUT /api/hostname   {hostname}
     GET /api/sshkey     {user, configured}
     PUT /api/sshkey     {key}

   Boards ship with one hostname, so several on a network are told apart only by
   the mDNS suffix boot order hands out. Naming a board settles which is which:
   <name>.local, the DNS-SD entry, the DHCP lease and the login banner all
   follow the system hostname, and qunilator-rename propagates it.

   The ssh key is what makes the operator's account reachable from a
   workstation. The account itself, its home and the group memberships that
   carry its rights are webshares.cpp's; this only carries the key to it.
*/

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>

#include "utils.hpp"

#include "civetweb.h"
#include "picojson.h"

#include "webauth.hpp"
#include "weblog.hpp"
#include "webshares.hpp"
#include "websystem.hpp"

static const char *RENAME_TOOL = "/usr/sbin/qunilator-rename";

// systemd-networkd reads drop-ins beside the file they belong to, and applies
// them after it. The bridge is where the host's address lives, so that is the
// file this extends.
static const char *NETWORK_DROPIN_DIR = "/etc/systemd/network/br0.network.d";
static const char *NETWORK_DROPIN =
		"/etc/systemd/network/br0.network.d/10-qunilator-address.conf";

/*** plumbing ***/

static void send_json(struct mg_connection *conn, int status,
		const picojson::value &val) {
	std::string body = val.serialize();
	mg_printf(conn,
			"HTTP/1.1 %d %s\r\n"
			"Content-Type: application/json\r\n"
			"Cache-Control: no-store\r\n"
			"Content-Length: %u\r\n\r\n",
			status, status == 200 ? "OK" : "Error", (unsigned) body.size());
	mg_write(conn, body.c_str(), body.size());
}

static void send_error(struct mg_connection *conn, int status,
		const std::string &message) {
	picojson::object err;
	err["error"] = picojson::value(message);
	send_json(conn, status, picojson::value(err));
}

static bool read_json_body(struct mg_connection *conn, picojson::value *out) {
	char body[8192];
	int body_len = mg_read(conn, body, sizeof(body) - 1);
	if (body_len <= 0)
		return false;
	body[body_len] = 0;
	std::string parse_err = picojson::parse(*out, body);
	return parse_err.empty() && out->is<picojson::object>();
}

// Run argv[0] with argv, collecting what it says on stdout and stderr. Result
// is the exit status, or -1 when the program could not be run at all. No shell
// takes part, so an argument is an argument.
static int run_capturing(const char *const argv[], std::string *output) {
	return subprocess_run(argv, -1, output, /*capture_stderr*/true);
}

// The tool prefixes what it says with its own name and ends it with a newline;
// what reaches the interface is the sentence itself.
static std::string tidy(const std::string &text) {
	std::string s = text;
	static const char *prefix = "qunilator-rename: ";
	size_t plen = strlen(prefix);
	if (s.size() >= plen && s.compare(0, plen, prefix) == 0)
		s = s.substr(plen);
	while (!s.empty() && (s[s.size() - 1] == '\n' || s[s.size() - 1] == '\r'))
		s.erase(s.size() - 1);
	size_t eol = s.find('\n');
	if (eol != std::string::npos)
		s = s.substr(0, eol);
	return s;
}

/*** /api/hostname ***/

std::string websystem_hostname(void) {
	char buf[256];
	if (gethostname(buf, sizeof(buf)) != 0)
		return std::string();
	buf[sizeof(buf) - 1] = 0;
	// the board's name is the first label, whatever the resolver appends
	char *dot = strchr(buf, '.');
	if (dot != nullptr)
		*dot = 0;
	return std::string(buf);
}

// One DNS label: letters, digits and inner hyphens, at most 63 characters. The
// tool checks this too; checking here is what turns a refusal into a sentence
// the dialog can show beside the field.
static bool acceptable_hostname(const std::string &name, std::string *error) {
	if (name.empty() || name.size() > 63) {
		*error = "a host name is 1 to 63 characters";
		return false;
	}
	for (size_t i = 0; i < name.size(); i++) {
		char c = name[i];
		bool alnum = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z')
				|| (c >= 'A' && c <= 'Z');
		if (!alnum && !(c == '-' && i > 0 && i + 1 < name.size())) {
			*error = "a host name is letters, digits and inner hyphens";
			return false;
		}
	}
	return true;
}

bool websystem_set_hostname(const std::string &name, std::string *error) {
	if (!acceptable_hostname(name, error))
		return false;
	if (name == websystem_hostname())
		return true;
	if (access(RENAME_TOOL, X_OK) != 0) {
		*error = "this host cannot be renamed from here";
		return false;
	}
	const char *argv[] = { RENAME_TOOL, name.c_str(), nullptr };
	std::string said;
	if (run_capturing(argv, &said) != 0) {
		*error = tidy(said);
		if (error->empty())
			*error = "the host name could not be changed";
		return false;
	}
	WEB_INFO("this QUniLator is now named %s", name.c_str());
	return true;
}

// Nothing here is a shell argument, but an address that came out of a file
// still has to be one before it is written into a configuration systemd reads.
static bool plausible_address(const std::string &s) {
	if (s.empty() || s.size() > 64)
		return false;
	for (size_t i = 0; i < s.size(); i++) {
		char c = s[i];
		bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')
				|| (c >= 'A' && c <= 'F') || c == '.' || c == ':' || c == '/';
		if (!ok)
			return false;
	}
	return true;
}

bool websystem_set_static_address(const std::string &address,
		const std::string &router, const std::string &dns, std::string *error) {
	if (address.find('/') == std::string::npos || !plausible_address(address)) {
		*error = "an address carries its prefix, e.g. 192.168.1.50/24";
		return false;
	}
	if (!router.empty() && !plausible_address(router)) {
		*error = "\"" + router + "\" is not an address";
		return false;
	}
	std::string text = "# Written when this installation was set up. Removing this\n"
			"# file puts the machine back on DHCP.\n"
			"[Network]\n"
			"DHCP=no\n"
			"Address=" + address + "\n";
	if (!router.empty())
		text += "Gateway=" + router + "\n";
	// One DNS server per line, from a value that holds them separated by
	// spaces; a name that is not an address is refused rather than written.
	size_t at = 0;
	while (at < dns.size()) {
		size_t end = dns.find(' ', at);
		std::string one = dns.substr(at, end == std::string::npos
				? std::string::npos : end - at);
		at = end == std::string::npos ? dns.size() : end + 1;
		if (one.empty())
			continue;
		if (!plausible_address(one)) {
			*error = "\"" + one + "\" is not an address";
			return false;
		}
		text += "DNS=" + one + "\n";
	}

	if (mkdir(NETWORK_DROPIN_DIR, 0755) != 0 && errno != EEXIST) {
		*error = std::string("could not create ") + NETWORK_DROPIN_DIR + ": "
				+ strerror(errno);
		return false;
	}
	std::string tmp = std::string(NETWORK_DROPIN) + ".new";
	FILE *f = fopen(tmp.c_str(), "wb");
	if (f == nullptr) {
		*error = std::string("could not write ") + NETWORK_DROPIN + ": "
				+ strerror(errno);
		return false;
	}
	bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
	if (fclose(f) != 0)
		ok = false;
	// networkd reads its configuration as its own user, so the file has to be
	// readable by more than root.
	if (ok)
		ok = chmod(tmp.c_str(), 0644) == 0
				&& rename(tmp.c_str(), NETWORK_DROPIN) == 0;
	if (!ok) {
		*error = std::string("could not write ") + NETWORK_DROPIN + ": "
				+ strerror(errno);
		unlink(tmp.c_str());
		return false;
	}
	WEB_INFO("the machine takes the address %s%s%s", address.c_str(),
			router.empty() ? "" : ", router ", router.c_str());

	// A machine that has already come up on a lease moves to the address when
	// networkd reads this; one still booting finds it there.
	const char *reload[] = { "/usr/bin/networkctl", "reload", nullptr };
	std::string said;
	if (access(reload[0], X_OK) == 0)
		run_capturing(reload, &said);
	return true;
}

static void hostname_get(struct mg_connection *conn) {
	picojson::object o;
	o["hostname"] = picojson::value(websystem_hostname());
	send_json(conn, 200, picojson::value(o));
}

static void hostname_put(struct mg_connection *conn) {
	picojson::value body;
	if (!read_json_body(conn, &body)) {
		send_error(conn, 400, "expected a JSON object");
		return;
	}
	if (!body.get("hostname").is<std::string>()) {
		send_error(conn, 400, "hostname is required");
		return;
	}
	std::string name = body.get("hostname").get<std::string>();
	std::string error;
	if (!websystem_set_hostname(name, &error)) {
		send_error(conn, 422, error);
		return;
	}
	picojson::object o;
	o["ok"] = picojson::value(true);
	o["hostname"] = picojson::value(websystem_hostname());
	send_json(conn, 200, picojson::value(o));
}

static int api_hostname_handler(struct mg_connection *conn, void * /*cbdata*/) {
	const struct mg_request_info *ri = mg_get_request_info(conn);
	if (!strcmp(ri->request_method, "GET")) {
		hostname_get(conn);
		return 200;
	}
	if (!strcmp(ri->request_method, "PUT") || !strcmp(ri->request_method, "POST")) {
		hostname_put(conn);
		return 200;
	}
	send_error(conn, 405, "method not allowed");
	return 405;
}

/*** /api/sshkey ***/

static void sshkey_get(struct mg_connection *conn) {
	std::string user = webauth_user();
	picojson::object o;
	o["user"] = picojson::value(user);
	o["configured"] = picojson::value(!user.empty() && webshares_has_ssh_key(user));
	send_json(conn, 200, picojson::value(o));
}

static void sshkey_put(struct mg_connection *conn) {
	picojson::value body;
	if (!read_json_body(conn, &body)) {
		send_error(conn, 400, "expected a JSON object");
		return;
	}
	if (!body.get("key").is<std::string>()) {
		send_error(conn, 400, "key is required");
		return;
	}
	std::string user = webauth_user();
	if (user.empty()) {
		send_error(conn, 409, "set a user name before installing a key");
		return;
	}
	std::string error;
	if (!webshares_set_ssh_key(user, body.get("key").get<std::string>(), &error)) {
		send_error(conn, 422, error);
		return;
	}
	picojson::object o;
	o["ok"] = picojson::value(true);
	o["user"] = picojson::value(user);
	send_json(conn, 200, picojson::value(o));
}

static int api_sshkey_handler(struct mg_connection *conn, void * /*cbdata*/) {
	const struct mg_request_info *ri = mg_get_request_info(conn);
	if (!strcmp(ri->request_method, "GET")) {
		sshkey_get(conn);
		return 200;
	}
	if (!strcmp(ri->request_method, "PUT") || !strcmp(ri->request_method, "POST")) {
		sshkey_put(conn);
		return 200;
	}
	send_error(conn, 405, "method not allowed");
	return 405;
}

void websystem_register(struct mg_context *ctx) {
	mg_set_request_handler(ctx, "/api/hostname", api_hostname_handler, nullptr);
	mg_set_request_handler(ctx, "/api/sshkey", api_sshkey_handler, nullptr);
}
