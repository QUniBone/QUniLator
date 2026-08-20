/* auth_test.cpp: host test of the web interface's credentials

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see any source file header for the full text.

   Drives webauth.cpp and the name rules of webshares.cpp on the development
   host: what a user name may be, what basic auth accepts once an operator is
   set, and what survives a round trip through settings.json.

   The account provisioning inside webshares.cpp is root-only and does nothing
   here, so a name can be set and changed without an account being created.

   Built and run by run_config_test.sh. Exit status is the test result.
*/

#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "picojson.h"

#include "logger.hpp"
#include "logsource.hpp"
#include "webauth.hpp"
#include "websettings.hpp"
#include "webshares.hpp"

/*** logger stub: quiet unless something logs an error ***/

logger_c *logger = nullptr;

logger_c::logger_c() {
	fifo = nullptr;
	fifo_capacity = fifo_readidx = fifo_writeidx = fifo_fill = 0;
	messagecount = 0;
}
logger_c::~logger_c() {}
void logger_c::vlog(logsource_c *logsource, unsigned msglevel, bool,
		const char *, unsigned, const char *fmt, va_list args) {
	fprintf(stderr, "[%s %u] ", logsource->log_label.c_str(), msglevel);
	vfprintf(stderr, fmt, args);
	fprintf(stderr, "\n");
}
void logger_c::add_source(logsource_c *logsource) {
	*(logsource->log_level_ptr) = default_level;
	logsources.push_back(logsource);
}
void logger_c::remove_source(logsource_c *logsource) {
	for (logsource_c *&s : logsources)
		if (s == logsource)
			s = nullptr;
}
void logger_c::reset_log_levels(void) {
	for (logsource_c *s : logsources)
		if (s != nullptr)
			*(s->log_level_ptr) = default_level;
}
std::vector<logger_c::logsource_ref_t> logger_c::list_logsources(void) {
	std::vector<logsource_ref_t> result;
	for (logsource_c *s : logsources)
		if (s != nullptr)
			result.push_back({ s, s->log_label, s->log_level_ptr });
	return result;
}

logsource_c::logsource_c() {
	log_level = LL_ERROR;
	log_level_ptr = &log_level;
	log_id = 0;
}
logsource_c::~logsource_c() {}
void logsource_c::connect() {}
void logsource_c::disconnect() {}

/*** settings stub: webauth persists through this, and the test reads back
     what it would have written by asking webauth_json() directly ***/

void websettings_save(void) {}

/*** the test ***/

static int failures = 0;

static void check(bool ok, const char *what) {
	printf("%-68s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok)
		failures++;
}

static void name_rules(void) {
	std::string why;
	check(webshares_name_acceptable("operators", &why),
			"a plain lower case name is taken");
	check(webshares_name_acceptable("pdp-11_op", &why),
			"digits, hyphens and underscores are taken");
	check(webshares_name_acceptable("_x", &why), "a leading underscore is taken");

	check(!webshares_name_acceptable("", &why), "an empty name is refused");
	check(!webshares_name_acceptable("Hans", &why), "an upper case name is refused");
	check(!webshares_name_acceptable("2fast", &why), "a leading digit is refused");
	check(!webshares_name_acceptable("has space", &why), "a space is refused");
	check(!webshares_name_acceptable("semi;colon", &why), "punctuation is refused");
	check(!webshares_name_acceptable("../etc/passwd", &why), "a path is refused");
	check(!webshares_name_acceptable(std::string(33, 'a'), &why),
			"a name over 32 characters is refused");
	check(webshares_name_acceptable(std::string(32, 'a'), &why),
			"a name of 32 characters is taken");

	check(!webshares_name_acceptable("root", &why), "root is refused");
	check(!webshares_name_acceptable("qunilator", &why),
			"the service account is refused");
	check(!webshares_name_acceptable("daemon", &why), "a system account is refused");
	check(!webshares_name_acceptable("nobody", &why), "nobody is refused");

	// Whatever runs this test has an account of its own, and no operator mark
	// on it, so the name it logs in under stands for every account this
	// service did not create.
	const char *me = getenv("USER");
	if (me != nullptr && *me != 0 && getpwnam(me) != nullptr
			&& strcmp(me, "root") != 0)
		check(!webshares_name_acceptable(me, &why),
				"an account this service did not create is refused");
}

static void not_set_up(void) {
	webauth_init();
	check(!webauth_configured(), "an installation with no operator is not configured");
	check(webauth_user().empty(), "and reports no user name");
	check(webauth_verify("anyone", "anything"), "and lets anyone in");
}

// A name is half of the credential, so there is no way to set one without it.
static void a_name_is_required(void) {
	std::string why;
	check(!webauth_set_credentials("", "opensesame", &why),
			"a password with no name is refused");
	check(!webauth_configured(), "which leaves the installation not set up");
	check(!webauth_set_credentials("operators", "short", &why),
			"a password under the minimum is refused");
	check(!webauth_set_credentials("root", "opensesame", &why),
			"a reserved name is refused");
	check(!webauth_configured(), "and none of that set anything up");
}

static void with_a_name(void) {
	std::string why;
	check(webauth_set_credentials("operators", "opensesame", &why),
			"a name and a password set the operator");
	check(webauth_configured(), "which is what being set up means");
	check(webauth_user() == "operators", "and the name is reported back");
	check(webauth_verify("operators", "opensesame"), "that pair is accepted");
	check(!webauth_verify("anyone", "opensesame"), "another name is refused");
	check(!webauth_verify("", "opensesame"), "an empty name is refused");
	check(!webauth_verify("operators", "wrong"),
			"the right name with the wrong password is refused");

	check(webauth_set_credentials("dispatch", "opensesame", &why),
			"the name is changed with the password in force");
	check(webauth_user() == "dispatch", "and the new one answers");
	check(!webauth_verify("operators", "opensesame"), "the name it replaced does not");
	check(webauth_verify("dispatch", "opensesame"), "the new one does");

	check(webauth_set_credentials("dispatch", "anotherlongone", &why),
			"the password is changed with the name in force");
	check(webauth_verify("dispatch", "anotherlongone"), "the new password is accepted");
	check(!webauth_verify("dispatch", "opensesame"), "the one it replaced is not");

	check(!webauth_set_credentials("", "anotherlongone", &why),
			"the name cannot be cleared");
	check(webauth_user() == "dispatch", "so the operator stands");
}

static void persistence(void) {
	std::string why;
	check(webauth_set_credentials("archivist", "anotherlongone", &why),
			"credentials are set for a round trip");
	picojson::value stored = webauth_json();
	check(stored.is<picojson::object>(), "settings.json carries an admin object");
	check(stored.get("user").is<std::string>()
			&& stored.get("user").get<std::string>() == "archivist",
			"holding the user name");
	check(stored.get("hash").is<std::string>() && stored.get("salt").is<std::string>(),
			"and the salted digest");

	// What a restart does: the same digest and name read back into a process
	// that knows nothing else.
	webauth_load(stored);
	check(webauth_user() == "archivist", "a reload restores the name");
	check(webauth_verify("archivist", "anotherlongone"),
			"and the credentials still verify");

	// A record from before an operator existed: an admin object with no user
	// member. It names no account, so it does not put anyone in force.
	picojson::object nameless = stored.get<picojson::object>();
	nameless.erase("user");
	webauth_load(picojson::value(nameless));
	check(webauth_user() == "archivist",
			"an admin object with no user name leaves the operator alone");

	picojson::object empty_name = stored.get<picojson::object>();
	empty_name["user"] = picojson::value(std::string());
	webauth_load(picojson::value(empty_name));
	check(webauth_user() == "archivist", "and so does an empty one");
}

/*** the session cookie ***/

// What a browser would send back: the value out of a Set-Cookie header line.
static std::string cookie_header_of(const std::string &set_cookie) {
	size_t eq = set_cookie.find('=');
	size_t semi = set_cookie.find(';', eq);
	if (eq == std::string::npos || semi == std::string::npos)
		return std::string();
	size_t name = set_cookie.find(':');
	std::string pair = set_cookie.substr(name + 2, semi - name - 2);
	return pair; // "qunilator_session=1.user.expiry.mac"
}

static void sessions(void) {
	std::string why;
	check(webauth_set_credentials("archivist", "anotherlongone", &why),
			"an operator is set for the session tests");
	std::string set_cookie = webauth_session_cookie();
	check(!set_cookie.empty(), "an authenticated answer carries a Set-Cookie");
	check(set_cookie.find("HttpOnly") != std::string::npos
			&& set_cookie.find("SameSite=Lax") != std::string::npos
			&& set_cookie.find("Max-Age=432000") != std::string::npos,
			"good for five days, HttpOnly and same-site");

	std::string cookie = cookie_header_of(set_cookie);
	check(webauth_verify_session(cookie.c_str()), "the cookie signs the holder in");
	check(webauth_verify_session(("other=1; " + cookie + "; more=2").c_str()),
			"and is found among other cookies");
	check(!webauth_verify_session(nullptr), "a request with no cookies is refused");
	check(!webauth_verify_session("other=1; more=2"),
			"and so is one carrying somebody else's");

	// Every field is under the mac, so no part of it can be edited.
	std::string tampered = cookie;
	tampered[tampered.size() - 1] = tampered[tampered.size() - 1] == 'a' ? 'b' : 'a';
	check(!webauth_verify_session(tampered.c_str()), "a changed mac is refused");
	std::string forged = "qunilator_session=1.archivist.99999999999."
			+ std::string(64, '0');
	check(!webauth_verify_session(forged.c_str()),
			"a token signed by nobody is refused");

	// What a restart does: the secret comes back out of settings.json with the
	// digest, so the session a browser is holding still opens the board.
	picojson::value stored = webauth_json();
	check(stored.get("session_secret").is<std::string>(),
			"settings.json carries the signing secret");
	webauth_load(stored);
	check(webauth_verify_session(cookie.c_str()),
			"a session survives the service being restarted");

	// The key follows the digest, so new credentials end what was open.
	check(webauth_set_credentials("archivist", "yetanotherone", &why),
			"the password is changed");
	check(!webauth_verify_session(cookie.c_str()),
			"which closes the sessions that were open");
	check(webauth_verify_session(cookie_header_of(webauth_session_cookie()).c_str()),
			"and the answer that changed it opens a new one");
}

int main(void) {
	logger = new logger_c();

	printf("--- user names\n");
	name_rules();
	printf("--- before anyone is set up\n");
	not_set_up();
	printf("--- half a credential\n");
	a_name_is_required();
	printf("--- the operator\n");
	with_a_name();
	printf("--- settings.json\n");
	persistence();
	printf("--- the browser's session\n");
	sessions();

	printf("\n%s\n", failures == 0 ? "auth_test: all checks passed"
			: "auth_test: FAILURES");
	return failures == 0 ? 0 : 1;
}
