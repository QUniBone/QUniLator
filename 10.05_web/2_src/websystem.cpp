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
#include <sys/wait.h>
#include <unistd.h>

#include <string>

#include "civetweb.h"
#include "picojson.h"

#include "webauth.hpp"
#include "weblog.hpp"
#include "webshares.hpp"
#include "websystem.hpp"

static const char *RENAME_TOOL = "/usr/sbin/qunilator-rename";

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
	int fds[2];
	if (pipe(fds) != 0)
		return -1;
	pid_t pid = fork();
	if (pid < 0) {
		close(fds[0]);
		close(fds[1]);
		return -1;
	}
	if (pid == 0) {
		close(fds[0]);
		if (dup2(fds[1], STDOUT_FILENO) < 0 || dup2(fds[1], STDERR_FILENO) < 0)
			_exit(127);
		close(fds[1]);
		execv(argv[0], (char *const *) argv);
		_exit(127);
	}
	close(fds[1]);
	char buf[512];
	ssize_t n;
	output->clear();
	while ((n = read(fds[0], buf, sizeof(buf))) > 0)
		output->append(buf, (size_t) n);
	close(fds[0]);
	int status = 0;
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
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
		*error = "a board name is 1 to 63 characters";
		return false;
	}
	for (size_t i = 0; i < name.size(); i++) {
		char c = name[i];
		bool alnum = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z')
				|| (c >= 'A' && c <= 'Z');
		if (!alnum && !(c == '-' && i > 0 && i + 1 < name.size())) {
			*error = "a board name is letters, digits and inner hyphens";
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
			*error = "the board could not be renamed";
		return false;
	}
	WEB_INFO("the board is now named %s", name.c_str());
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
		send_error(conn, 409, "set a user name before giving the board a key");
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
