/* main_qbone_web.cpp: the QBone as a service, driven only by its web interface

 Copyright (c) 2026, Hans Huebner
 hans@huebner.org
 MIT license.

 This program brings the hardware up, constructs the emulated device set, serves
 the web interface, and then waits to be stopped. It has no menu and reads
 nothing from stdin, which is what a unit under systemd needs: the terminal
 menus of "demo" have no operator there, and their output belongs to a person,
 not to a log.

 Everything an operator does goes through the web interface, and everything
 worth knowing afterwards goes to the log at INFO or above, so the journal
 carries the run: what was configured, what was enabled, what failed.
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>

#include <mutex>
#include <string>
#include <vector>

#include "logger.hpp"
#include "gpios.hpp"
#include "buslatches.hpp"
#include "pru.hpp"
#include "qunibus.h"
#include "ddrmem.h"
#include "application.hpp"
#include "webserver.hpp"
#include "weblog.hpp"
#include "webconfigs.hpp"
#include "webevents.hpp"
#include "websettings.hpp"
#include "webconsole.hpp"
#include "webpower.hpp"
#include "webconsole_ext.hpp"
#include "webauth.hpp"
#include "webshares.hpp"
#include "websystem.hpp"
#include "webversion.hpp"
#include "webseed.hpp"
#include "boardclaim.hpp"
#include "device_configuration.hpp"

static volatile sig_atomic_t terminate_requested = 0;

static void on_terminate_signal(int signum)
{
	terminate_requested = signum;
}

// The document root holds the frontend. Named on the command line by a package
// that installs it; otherwise it sits beside the source tree.
static std::string resolve_docroot(const std::string &opt_root)
{
	if (!opt_root.empty())
		return opt_root;
	const char *root = getenv("QUNILATOR_DIR");
	if (root == nullptr)
		root = getenv("HOME");
	return std::string(root ? root : ".") + "/10.05_web/3_frontend";
}

// Read one line, without the newline. Result false at end of input.
static bool read_line(FILE *f, std::string *out)
{
	char buf[512];
	if (fgets(buf, sizeof(buf), f) == nullptr)
		return false;
	size_t len = strlen(buf);
	while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
		buf[--len] = 0;
	*out = buf;
	return true;
}

static bool read_whole_file(const char *path, std::string *out)
{
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

// The ssh key and the host name, which follow the account whichever shape the
// password arrived in. Neither is required, and a refusal of either is reported
// without taking the account back: it is there, and the rest can be set from
// the System page.
static int apply_the_rest(const std::string &user, const std::string &key,
		const std::string &hostname)
{
	std::string error;
	if (!key.empty()) {
		if (!webshares_set_ssh_key(user, key, &error))
			fprintf(stderr, "the ssh key was not installed: %s\n", error.c_str());
		else
			printf("%s reaches a shell with the given ssh key\n", user.c_str());
	}
	if (!hostname.empty()) {
		if (!websystem_set_hostname(hostname, &error))
			fprintf(stderr, "the host name was not set: %s\n", error.c_str());
		else
			printf("the host name is %s\n", hostname.c_str());
	}
	return 0;
}

// Setting up writes the credentials the service holds in memory, so it belongs
// to root and to a moment when the service is not running - it would write its
// own over them at the next settings change.
static bool setup_is_possible(void)
{
	if (geteuid() != 0) {
		fprintf(stderr, "setting up creates an account, so it must run as root\n");
		return false;
	}
	struct stat st;
	if (stat("/run/qunilator/version", &st) != 0)
		return true; // the unit's runtime directory is gone, so it is not up
	std::string unit = webversion_package() + ".service";
	fprintf(stderr, "%s is running, and would write its own credentials over "
			"these.\n  systemctl stop %s\n  <this command>\n  systemctl start %s\n",
			unit.c_str(), unit.c_str(), unit.c_str());
	return false;
}

// Create the operator - the one account that opens the web interface, the file
// shares and ssh - and exit. Preparing an SD card does this inside the image so
// the card boots ready to use, and it is what gives an operator a way in again
// when the password is gone.
//
// Nothing here touches the bus, and the logger is the only singleton it needs.
static int setup_operator(const std::string &user, const std::string &password,
		const std::string &key, const std::string &hostname, bool adopt)
{
	logger = new logger_c();
	logger->default_level = LL_INFO;
	logger->reset_log_levels();
	// The unit is given QUNILATOR_DIR by a drop-in; a command line has no such
	// thing, and the digest belongs in the state directory the service reads
	// rather than in the home of whoever ran this.
	if (getenv("QUNILATOR_DIR") == nullptr)
		setenv("QUNILATOR_DIR", "/var/lib/qunilator", 1);
	websettings_startup();

	std::string error;
	// An account of that name that nobody made the operator is refused, which
	// is what --adopt-account settles: it keeps the home and the files where
	// they are and makes that account the one.
	if (adopt && !webshares_adopt_account(user, &error)) {
		fprintf(stderr, "%s\n", error.c_str());
		return 1;
	}
	if (!webauth_set_credentials(user, password, &error)) {
		fprintf(stderr, "%s\n", error.c_str());
		return 1;
	}
	printf("%s is the operator: web interface, file shares and ssh\n", user.c_str());
	printf("the credentials are in %s/settings.json\n",
			websettings_state_dir().c_str());
	return apply_the_rest(user, key, hostname);
}

static int setup_operator_hashed(const webseed_c &seed, bool adopt);

// Apply the setup file an SD card carries and remove it. The card is prepared
// on a workstation that has no way to make an account inside the image, so it
// writes what it wants and the first boot that finds the file does the work.
// The same file dropped on the card later is how an operator whose password is
// gone gets back in.
static int setup_from_seed(const std::string &path)
{
	std::string text;
	if (!read_whole_file(path.c_str(), &text)) {
		fprintf(stderr, "cannot read %s: %s\n", path.c_str(), strerror(errno));
		return 1;
	}
	webseed_c seed;
	std::string error;
	if (!webseed_parse(text, &seed, &error)) {
		fprintf(stderr, "%s: %s\n", path.c_str(), error.c_str());
		return 1;
	}
	// A card prepared with a name that is already an account here is asking for
	// that account: there is nobody at the machine to answer the question, and
	// the alternative is an installation that will not come up.
	int result = seed.derived()
			? setup_operator_hashed(seed, /*adopt*/true)
			: setup_operator(seed.user, seed.password, seed.ssh_key, seed.hostname,
					/*adopt*/true);
	if (result != 0)
		return result;
	// The password stood in the file in the clear, so the file goes: overwritten
	// where the filesystem allows it, and unlinked either way.
	FILE *f = fopen(path.c_str(), "r+b");
	if (f != nullptr) {
		for (size_t i = 0; i < text.size(); i++)
			fputc(0, f);
		fflush(f);
		fclose(f);
	}
	if (unlink(path.c_str()) != 0)
		fprintf(stderr, "warning: %s is still there: %s\n", path.c_str(),
				strerror(errno));
	else
		printf("%s is applied and removed\n", path.c_str());
	return 0;
}

// The same from a card that carries the password already derived: three hashes
// instead of one password, each going where only that shape is understood.
static int setup_operator_hashed(const webseed_c &seed, bool adopt)
{
	logger = new logger_c();
	logger->default_level = LL_INFO;
	logger->reset_log_levels();
	if (getenv("QUNILATOR_DIR") == nullptr)
		setenv("QUNILATOR_DIR", "/var/lib/qunilator", 1);
	websettings_startup();

	std::string error;
	std::string previous = webauth_user();
	if (adopt && !webshares_adopt_account(seed.user, &error)) {
		fprintf(stderr, "%s\n", error.c_str());
		return 1;
	}
	if (!webauth_set_digest(seed.user, seed.web_salt, seed.web_hash,
			seed.web_iterations, &error)) {
		fprintf(stderr, "%s\n", error.c_str());
		return 1;
	}
	webshares_apply_hashed(previous, seed.user, seed.unix_hash, seed.nt_hash);
	printf("%s is the operator: web interface, file shares and ssh\n",
			seed.user.c_str());
	printf("the credentials are in %s/settings.json, and no password was read\n",
			websettings_state_dir().c_str());
	return apply_the_rest(seed.user, seed.ssh_key, seed.hostname);
}

static void usage(const char *progname)
{
	printf("%s - " QUNILATOR_NAME " " QUNIBUS_NAME " emulator, served over its web interface\n\n", progname);
	printf("usage: %s [options]\n", progname);
	printf("  --port <n>          TCP port of the web interface (default 80)\n");
	printf("  --webroot <dir>     directory holding the frontend\n");
#if defined(QBUS)
	printf("  --addresswidth <n>  " QUNIBUS_NAME " address width: 16, 18 or 22 (default 22)\n");
#else
	printf("  --addresswidth <n>  " QUNIBUS_NAME " address width: 18\n");
#endif
	printf("  --config <name>     saved configuration to apply at startup\n");
	printf("  --loglevel <n>      %d fatal, %d error, %d warning, %d info (default), %d debug\n",
			LL_FATAL, LL_ERROR, LL_WARNING, LL_INFO, LL_DEBUG);
	printf("  --help              this text\n");
	printf("\nsetting up, instead of serving:\n");
	printf("  --setup-operator <name>  create the operator account, reading its password\n");
	printf("                           from stdin, and exit\n");
	printf("  --ssh-key <file>         public key the operator reaches a shell with\n");
	printf("  --hostname <name>        the name this " QUNILATOR_NAME " has on the network\n");
	printf("  --adopt-account          make an account that exists already the operator's,\n");
	printf("                           keeping its home, its files and its shell\n");
	printf("  --seed <file>            apply a setup file an SD card carries, and\n");
	printf("                           remove it; " WEBSEED_PATH " is where\n");
	printf("                           the card carries one\n");
}

int main(int argc, char *argv[])
{
	unsigned port = 80;
	std::string webroot;
#if defined(QBUS)
	unsigned addresswidth = 22; // QBUS default of this cape
#else
	unsigned addresswidth = 18; // the UNIBUS is 18 bit
#endif
	unsigned loglevel = LL_INFO;
	std::string startup_config;
	std::string operator_name, ssh_key_file, hostname, seed_file;
	bool adopt_account = false;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--port") && i + 1 < argc)
			port = strtoul(argv[++i], nullptr, 10);
		else if (!strcmp(argv[i], "--webroot") && i + 1 < argc)
			webroot = argv[++i];
		else if (!strcmp(argv[i], "--addresswidth") && i + 1 < argc)
			addresswidth = strtoul(argv[++i], nullptr, 10);
		else if (!strcmp(argv[i], "--loglevel") && i + 1 < argc)
			loglevel = strtoul(argv[++i], nullptr, 10);
		else if (!strcmp(argv[i], "--config") && i + 1 < argc)
			startup_config = argv[++i];
		else if (!strcmp(argv[i], "--setup-operator") && i + 1 < argc)
			operator_name = argv[++i];
		else if (!strcmp(argv[i], "--ssh-key") && i + 1 < argc)
			ssh_key_file = argv[++i];
		else if (!strcmp(argv[i], "--hostname") && i + 1 < argc)
			hostname = argv[++i];
		else if (!strcmp(argv[i], "--adopt-account"))
			adopt_account = true;
		else if (!strcmp(argv[i], "--seed") && i + 1 < argc)
			seed_file = argv[++i];
		else if (!strcmp(argv[i], "--help")) {
			usage(argv[0]);
			return 0;
		} else {
			fprintf(stderr, "%s: unknown option \"%s\"\n", argv[0], argv[i]);
			usage(argv[0]);
			return 2;
		}
	}

	// Setting up is a run of its own: it creates the account and returns,
	// leaving the hardware alone, so it works in an SD-card image as well as on
	// a machine that is switched off.
	if (!seed_file.empty()) {
		if (!setup_is_possible())
			return 1;
		return setup_from_seed(seed_file);
	}
	if (!operator_name.empty()) {
		if (!setup_is_possible())
			return 1;
		// The password comes on stdin, where a process listing cannot see it.
		std::string password;
		if (!read_line(stdin, &password) || password.empty()) {
			fprintf(stderr, "--setup-operator reads the password from stdin, and "
					"read none\n");
			return 1;
		}
		std::string key;
		if (!ssh_key_file.empty() && !read_whole_file(ssh_key_file.c_str(), &key)) {
			fprintf(stderr, "cannot read the ssh key in %s: %s\n",
					ssh_key_file.c_str(), strerror(errno));
			return 1;
		}
		return setup_operator(operator_name, password, key, hostname, adopt_account);
	}
	if (!ssh_key_file.empty() || !hostname.empty() || adopt_account) {
		fprintf(stderr, "%s: --ssh-key, --hostname and --adopt-account belong to "
				"--setup-operator\n", argv[0]);
		return 2;
	}

	qunibone_factory();

	// A service reports what it did, so the run can be read back from the
	// journal afterwards. The menu program leaves this at warnings, where the
	// operator sees everything on screen as it happens anyway.
	logger->default_level = loglevel;
	logger->reset_log_levels();

	// systemd stops a unit with SIGTERM; answering it is what makes the stop
	// clean instead of a kill, so the device set is shut down in order.
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_terminate_signal;
	sigaction(SIGTERM, &sa, nullptr);
	sigaction(SIGINT, &sa, nullptr);

	WEB_INFO(QUNILATOR_NAME " web service starting, " QUNIBUS_NAME " emulation.");

	// The address width decides the layout of the IO page, so the emulation
	// needs it before the PRU is started.
	bool addresswidth_valid = (addresswidth == 18);
#if defined(QBUS)
	addresswidth_valid = addresswidth_valid || addresswidth == 16 || addresswidth == 22;
	if (!addresswidth_valid) {
		WEB_ERROR("Address width must be 16, 18 or 22 bits, not %u.", addresswidth);
		return 2;
	}
#else
	if (!addresswidth_valid) {
		WEB_ERROR("The " QUNIBUS_NAME " is 18 bit; address width %u is invalid.", addresswidth);
		return 2;
	}
#endif
	qunibus->set_addr_width(addresswidth);
	WEB_INFO("Address width %u bit.", qunibus->addr_width);

	gpios->init();
	buslatches.output_enable(0); // DS8641 drivers off until the bus is ours
	GPIO_SETVAL(gpios->reg_enable, 1); // leave SYSBOOT mode

	websettings_startup();

	// Brings the PRU up with the emulation code and constructs the device set,
	// which lives for the process lifetime. This starts the hardware; the PRU
	// must not be started separately.
	// A board is a peripheral of whatever machine it was fitted to until a
	// configuration enables a processor of its own; that is what takes the bus
	// over, and what claims the memory a machine with no cards of its own
	// needs. The bus mode is a separate question: it decides which peripherals
	// the machine can reach, the cards in a real backplane or the board's own.
	bool internal_bus = websettings_internal_bus();
	WEB_INFO("%s bus.", internal_bus ? "Internal" : "Physical");
	app->devices_startup(internal_bus);

	std::string docroot = resolve_docroot(webroot);
	webserver_c web_server(port, docroot);
	if (!web_server.start()) {
		WEB_ERROR("Web server failed to start on port %u, document root %s.", port,
				docroot.c_str());
		app->devices_shutdown();
		return 1;
	}

	// The machine a board comes up as is the designated default configuration,
	// applied here. It is applied after the server has started, because
	// registering the endpoints is what locates the configuration directory and
	// captures the parameter defaults an apply resets to. --config overrides the
	// default for bring-up and testing.
	webconfigs_startup(startup_config);

	// The interactive menu drives the same hardware this service does, so it
	// asks for it rather than fighting for it: the machine is put down, the
	// board handed over for the length of the session, and rebuilt afterwards
	// as the configuration says. The web interface keeps serving throughout,
	// locked, so an operator looking at it is told where the board went.
	boardclaim_handlers_c handlers;
	// The machine to rebuild is the one that was running, named while it still
	// is. The DIP switches choose a configuration once, when the backend starts;
	// a session at the menu is not a start, so re-reading them here would hand
	// back a different machine than the one the operator gave up - or none at
	// all, when the switches name nothing.
	std::string held_config;
	handlers.yield = [&]() {
		held_config = webconfigs_current();
		std::lock_guard<std::mutex> ops_lock(device_configuration_c::operations_mutex);

		// Switch the machine off first, the way the panel switch does. Taking
		// the hardware from a running machine means pulling its cards out from
		// under a guest mid-transfer; switching it off stops the CPU and takes
		// the cards out through the path every power-down uses, leaving nothing
		// of the emulation running to be surprised by what follows.
#if defined(QBUS)
		qunibus->set_halt(1);
#endif
		webevents_note_halt(true);
		webpower_devices_off();
		webevents_note_powered(false);
		// The record of the dark machine holds each card by address, and those
		// cards are about to be freed.
		webpower_forget();

		webconsole_shutdown();
		webconsole_ext_shutdown();
		app->devices_shutdown();
	};
	handlers.resume = [&]() {
		{
			std::lock_guard<std::mutex> ops_lock(device_configuration_c::operations_mutex);
			app->devices_startup(internal_bus);
			webconsole_register(web_server.context());
			webconsole_ext_register(web_server.context());
			// Registering starts the bridge's threads; the tty it carries is
			// opened by the setting, the same way bringing the API up does it.
			// Without this the board comes back with a dead console.
			external_console_c ec = websettings_external_console();
			webconsole_ext_configure(ec.source, ec.port, ec.baud);

			// The board comes back with the machine switched off. Whoever took
			// the hardware was driving bus latches and exercisers with it, and
			// starting a machine on what they left behind is not the service's
			// decision to make: the configuration is loaded into a dark machine
			// and the panel switch is the operator's.
			webpower_devices_off();
			webevents_note_powered(false);
#if defined(QBUS)
			qunibus->set_halt(1);
#endif
			webevents_note_halt(true);
		}
		// What the dark machine carries is its configuration, loaded into it
		// without reaching the emulation. Applying it takes the operations lock
		// itself, so it is done outside the one above.
		std::vector<std::string> rejections;
		std::string apply_error;
		if (held_config.empty())
			WEB_WARNING("No configuration was current; the board comes back with "
					"an empty machine.");
		else if (webconfigs_apply(held_config, &rejections, &apply_error))
			WEB_INFO("Configuration \"%s\" applied again, %u rejections.",
					held_config.c_str(), (unsigned) rejections.size());
		else
			WEB_ERROR("Configuration \"%s\" not applied after the menu session: %s",
					held_config.c_str(), apply_error.c_str());
		for (const std::string &r : rejections)
			WEB_WARNING("Configuration \"%s\": %s", held_config.c_str(), r.c_str());
	};
	std::string claim_error;
	if (!boardclaim_serve(handlers, &claim_error))
		WEB_WARNING("The interactive menu cannot ask for the board: %s.",
				claim_error.c_str());

	WEB_INFO(QUNILATOR_NAME " ready. Every operator action arrives through the web interface.");

	// Nothing to do here: the web server serves on its own threads and the
	// emulated devices run on theirs.
	while (!terminate_requested)
		pause();

	WEB_INFO("Signal %d received, shutting down.", (int) terminate_requested);
	boardclaim_stop_serving();
	web_server.stop();
	app->devices_shutdown();
	WEB_INFO(QUNILATOR_NAME " web service stopped.");
	return 0;
}
