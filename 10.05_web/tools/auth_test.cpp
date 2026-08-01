/* auth_test.cpp: host test of the web interface's credentials

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see any source file header for the full text.

   Drives webauth.cpp and the name rules of webshares.cpp on the development
   host: what a user name may be, what basic auth accepts once one is set, what
   survives a round trip through settings.json, and what credentials taken from
   the environment refuse.

   The account provisioning inside webshares.cpp is root-only and does nothing
   here, so a name can be set and changed without an account being created.

   The environment case needs a process that has never seen anything else -
   webauth_init reads WEBUI_PASSWORD once, and the source it decides on cannot
   be unwound - so it runs in a forked child before the parent's own phase.

   Built and run by run_config_test.sh. Exit status is the test result.
*/

#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
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

static void open_board(void) {
	webauth_init();
	check(!webauth_configured(), "a board with no credentials is not configured");
	check(webauth_user().empty(), "and reports no user name");
	check(webauth_verify("anyone", "anything"), "and lets anyone in");
}

static void password_only(void) {
	std::string why;
	check(!webauth_set_credentials("", "short", &why),
			"a password under the minimum is refused");
	check(webauth_set_credentials("", "opensesame", &why),
			"a password on its own is accepted");
	check(webauth_configured(), "the board is configured");
	check(webauth_user().empty(), "with no user name");
	check(webauth_verify("anyone", "opensesame"),
			"an installation with only a password takes any name");
	check(webauth_verify("", "opensesame"), "including an empty one");
	check(!webauth_verify("anyone", "wrong"), "and refuses the wrong password");
}

static void with_a_name(void) {
	std::string why;
	check(!webauth_set_credentials("root", "opensesame", &why),
			"a reserved name is refused as a credential");
	check(webauth_user().empty(), "and leaves the board without a name");

	check(webauth_set_credentials("operators", "opensesame", &why),
			"a name is set alongside the password");
	check(webauth_user() == "operators", "and is reported back");
	check(webauth_verify("operators", "opensesame"), "that pair is accepted");
	check(!webauth_verify("anyone", "opensesame"), "another name is refused");
	check(!webauth_verify("", "opensesame"), "an empty name is refused");
	check(!webauth_verify("operators", "wrong"),
			"the right name with the wrong password is refused");

	check(webauth_set_credentials("dispatch", "opensesame", &why),
			"the name is changed with the password in force");
	check(webauth_user() == "dispatch", "and the new one answers");
	check(!webauth_verify("operators", "opensesame"), "the old one no longer does");
	check(webauth_verify("dispatch", "opensesame"), "the new one does");

	check(webauth_set_credentials("dispatch", "anotherlongone", &why),
			"the password is changed with the name in force");
	check(webauth_verify("dispatch", "anotherlongone"), "the new password is accepted");
	check(!webauth_verify("dispatch", "opensesame"), "the old one is not");

	check(webauth_set_credentials("", "anotherlongone", &why),
			"the name is cleared");
	check(webauth_user().empty(), "and the board reports none");
	check(webauth_verify("anyone", "anotherlongone"), "so any name is taken again");
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

	// An installation made before the name existed: the admin object of an
	// older settings.json, which carries no user member.
	picojson::object older = stored.get<picojson::object>();
	older.erase("user");
	webauth_load(picojson::value(older));
	check(webauth_user().empty(), "an admin object with no user name loads without one");
	check(webauth_verify("anyone", "anotherlongone"), "and the board takes any name");
}

// WEBUI_PASSWORD owns the credentials, so the interface changes neither half.
static int environment_phase(void) {
	setenv("WEBUI_PASSWORD", "fromtheenvironment", 1);
	webauth_init();
	check(webauth_configured(), "WEBUI_PASSWORD configures the board");
	check(webauth_source() == webauth_source_environment, "and owns the credentials");
	check(webauth_user().empty(), "which carry no user name");
	check(webauth_verify("anyone", "fromtheenvironment"), "so any name is taken");
	check(!webauth_verify("anyone", "wrong"), "with that password only");

	std::string why;
	check(!webauth_set_credentials("operators", "anotherlongone", &why),
			"a name change is refused");
	check(!webauth_set_credentials("", "anotherlongone", &why),
			"a password change is refused");
	check(webauth_user().empty(), "and the board still carries no name");

	// A settings.json holding a name is outranked as well, which is what makes
	// WEBUI_PASSWORD the way back into a board whose name was mistyped.
	picojson::object admin;
	admin["algorithm"] = picojson::value("pbkdf2-sha256");
	admin["user"] = picojson::value(std::string("someoneelse"));
	admin["iterations"] = picojson::value((double) 120000);
	admin["salt"] = picojson::value(std::string(32, '0'));
	admin["hash"] = picojson::value(std::string(64, '0'));
	webauth_load(picojson::value(admin));
	check(webauth_user().empty(), "a stored name does not outrank the environment");
	check(webauth_verify("anyone", "fromtheenvironment"),
			"and the environment password still lets anyone in");
	return failures;
}

int main(void) {
	logger = new logger_c();

	printf("--- credentials from the environment\n");
	fflush(stdout); // the child inherits this buffer, and would print it twice
	pid_t child = fork();
	if (child == 0)
		exit(environment_phase() == 0 ? 0 : 1);
	int status = 1;
	if (child < 0 || waitpid(child, &status, 0) < 0 || !WIFEXITED(status)
			|| WEXITSTATUS(status) != 0) {
		printf("the environment phase failed\n");
		failures++;
	}

	printf("--- user names\n");
	name_rules();
	printf("--- a board with no credentials\n");
	open_board();
	printf("--- a password on its own\n");
	password_only();
	printf("--- a user name alongside the password\n");
	with_a_name();
	printf("--- settings.json\n");
	persistence();

	printf("\n%s\n", failures == 0 ? "auth_test: all checks passed"
			: "auth_test: FAILURES");
	return failures == 0 ? 0 : 1;
}
