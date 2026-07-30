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
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>

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
#include "websettings.hpp"

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
#if defined(UNIBUS)
	printf("  --emulated-cpu      run the emulated KA11 (PDP-11/20) for this run\n");
#endif
	printf("  --loglevel <n>      %d fatal, %d error, %d warning, %d info (default), %d debug\n",
			LL_FATAL, LL_ERROR, LL_WARNING, LL_INFO, LL_DEBUG);
	printf("  --help              this text\n");
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
	bool opt_emulated_cpu = false;

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
		else if (!strcmp(argv[i], "--emulated-cpu"))
			opt_emulated_cpu = true;
		else if (!strcmp(argv[i], "--help")) {
			usage(argv[0]);
			return 0;
		} else {
			fprintf(stderr, "%s: unknown option \"%s\"\n", argv[0], argv[i]);
			usage(argv[0]);
			return 2;
		}
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

	// A board is either a peripheral of a physical PDP-11, which arbitrates the
	// bus, or the machine itself with the emulated KA11 doing that. The device
	// set is built for one of the two, so the setting is read before it exists;
	// --emulated-cpu selects the KA11 for a run without changing what is stored.
	websettings_startup();
	bool emulated_cpu = websettings_emulated_cpu() || opt_emulated_cpu;
#if !defined(UNIBUS)
	if (emulated_cpu) {
		WEB_WARNING("No emulated CPU on " QUNIBUS_NAME "; running as a peripheral.");
		emulated_cpu = false;
	}
#endif

	// Brings the PRU up with the emulation code and constructs the device set,
	// which lives for the process lifetime. This starts the hardware; the PRU
	// must not be started separately.
	// The bus mode is independent of the CPU: a board can be the CPU of a real
	// machine full of real cards, or a machine entirely by itself.
	bool internal_bus = websettings_internal_bus();
	WEB_INFO("%s bus.", internal_bus ? "Internal" : "Physical");
	app->devices_startup(emulated_cpu, internal_bus);

	// The emulated CPU has no machine around it, so the board supplies the
	// memory too: everything below the I/O page that no physical card answers.
	if (emulated_cpu) {
		WEB_INFO("Emulated CPU: KA11 (PDP-11/20).");
		unsigned first_invalid = qunibus->test_sizer();
		if (first_invalid >= qunibus->iopage_start_addr)
			WEB_INFO("Physical memory answers up to the I/O page; none emulated.");
		else if (ddrmem->set_range(DDRMEM_RANGE_MEMORY, first_invalid, qunibus->iopage_start_addr - 2))
			WEB_INFO("Emulating memory %s..%s.", qunibus->addr2text(first_invalid),
					qunibus->addr2text(qunibus->iopage_start_addr - 2));
		else
			WEB_ERROR("Memory emulation for %s..%s refused.",
					qunibus->addr2text(first_invalid),
					qunibus->addr2text(qunibus->iopage_start_addr - 2));
	}

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

	WEB_INFO(QUNILATOR_NAME " ready. Every operator action arrives through the web interface.");

	// Nothing to do here: the web server serves on its own threads and the
	// emulated devices run on theirs.
	while (!terminate_requested)
		pause();

	WEB_INFO("Signal %d received, shutting down.", (int) terminate_requested);
	web_server.stop();
	app->devices_shutdown();
	WEB_INFO(QUNILATOR_NAME " web service stopped.");
	return 0;
}
