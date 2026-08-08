/* application.cpp:  QUniBone "demo" application, global resources

 Copyright (c) 2018, Joerg Hoppe, j_hoppe@t-online.de, www.retrocmp.com

 All rights reserved.

 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions
 are met:

 - Redistributions of source code must retain the above copyright
 notice, this list of conditions and the following disclaimer.

 - Redistributions in binary form must reproduce the above copyright
 notice, this list of conditions and the following disclaimer in the
 documentation and/or other materials provided with the distribution.

 - Neither the name of the copyright holder nor the names of its
 contributors may be used to endorse or promote products derived from
 this software without specific prior written permission.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

 12-nov-2018  JH      entered beta phase
 14-May-2018 	JH      created

 */

#define _MAIN_C_

#include <stdlib.h>
#include <stdio.h>
#include <time.h>
//#include <string.h>
//#include <stdint.h>
#include <unistd.h>
#include <limits.h>
//#include <errno.h>
//#include <ctype.h>
#include <stdarg.h>
#include <strings.h>
#include <string>
#include <iostream>

#include "logsource.hpp"
#include "logger.hpp"
#if defined(WEBUI)
#include "webserver.hpp"
#include "websettings.hpp"
#include "boardclaim.hpp"
#endif
#include "timeout.hpp"
#include "getopt2.hpp"
#include "kbhit.h"
#include "inputline.hpp"
#include "pru.hpp"
#include "mailbox.h"
#include "gpios.hpp"
#include "buslatches.hpp"
#include "qunibussignals.hpp"
#include "memoryimage.hpp"
#include "iopageregister.h"
#include "panel.hpp"
#include "qunibus.h"
#include "qunibusadapter.hpp"

#include "logger.hpp"
#include "application.hpp"   // own

// Singleton
application_c *app;


application_c::application_c() 
{
    log_label = "APP";
}

/*
 * help()
 */
void application_c::help() 
{
    std::cout << "\n";
    std::cout << "NAME\n";
    std::cout << "\n";
    std::cout << version << "\n";
    std::cout << copyright << "\n";
    std::cout << "\n";
    std::cout << "SYNOPSIS\n";
    std::cout << "\n";
    std::cout << "TEST SETUP\n";
    std::cout << "  - UniBone must be plugged into SPC slots C-F on DD11-CK backplane.\n";
    std::cout << "  - 2 passive M903 terminators plugged into backplane.\n";
    std::cout << "  - Short cut BR4,5,6,NPR IN/OUT with jumpers.\n";
    std::cout << "  - Install the \"PRU\" device tree overlay:\n";
    std::cout << "    - cp UniBone-00B0.dtbo /lib/firmware UniBone-00B0.dtbo\n";
    std::cout << "    - reboot\n";
    std::cout << "\n";
//	std::cout << "Command line options are processed strictly left-to-right. \n\n");
    // getopt must be initialized to print the syntax
    getopt_parser.help(std::cout, opt_linewidth, 10, PROGNAME);
    std::cout << "\n";
    std::cout << "EXAMPLES\n";
    std::cout << "\n";
    std::cout << "sudo ./" PROGNAME "\n";
    std::cout << "    Show interactive menus.\n";
    std::cout << "\n";

    exit(1);
}

// show error for one option
void application_c::commandline_error() 
{
    std::cerr << "Error while parsing command line:\n";
    std::cerr << "  " << getopt_parser.curerrortext.c_str() << "\n";
    exit(1);
}

// parameter wrong for currently parsed option
void application_c::commandline_option_error(char *errtext, ...) 
{
    char buffer[1024];
    va_list args;
    std::cerr << "Error while parsing commandline option:\n";
    if (errtext) {
        va_start(args, errtext);
        vsprintf(buffer, errtext, args);
        std::cerr << buffer << "\nSyntax:  ";
        va_end(args);
    } else
        std::cerr << "  " << getopt_parser.curerrortext << "\nSyntax:  ";
    getopt_parser.help_option(std::cerr, 96, 10);
    exit(1);
}

/* check whether the given device parameter configuration
 * my cause problems.
 */

/*
 * read commandline parameters into global "param_" vars
 * result: 0 = OK, 1 = error
 */
void application_c::parse_commandline(int argc, char **argv) 
{
    int res;

    // define commandline syntax
    getopt_parser.init(/*ignore_case*/1);

//	getopt_def(&getopt_parser, NULL, NULL, "hostname", NULL, NULL, "Connect to the Blinkenlight API server on <hostname>\n"
//		"<hostname> may be numerical or ar DNS name",
//		"127.0.0.1", "connect to the server running on the same machine.",
//		"raspberrypi", "connected to a RaspberryPi with default name.");

    // !!!1 Do not define any defaults... else these will be set very time!!!

    getopt_parser.ignore_case = 1;
    getopt_parser.define("?", "help", "", "", "", "Print help.", "", "", "", "");
    getopt_parser.define("v", "verbose", "", "", "", "Print info about operation.", "", "", "",
                         "");
    getopt_parser.define("dbg", "debug", "", "", "", "Print debug messages.\n"
                         // getopt_parser.define("dbg", "debug", "", "channelmask", "",
                         //		"Print debug messages. Optional reduces to channels wit <channelmask>.\n"
                         "Outputfile is \"unibone.log\"", "", "", "", "");
    getopt_parser.define("cf", "cmdfile", "cmdfilename", "", "",
                         "File from which commands are read.\n"
                         "Lines are processed as if typed in.", "testseq",
                         "read commands from file \"testseq\" and execute line by line", "", "");
#if defined(QBUS)
    getopt_parser.define("aw", "addresswidth", "addresswidth", "", "",
                         "Mandatory address width of QBUS CPU: 16, 18, 22.\nCan not be auto-probed from backplane address width.", "",
                         "", "", "");
#endif
    getopt_parser.define("leds", "leds", "ledcode", "", "",
                         "<decimal number>: Display number 0..15 on 4 binary LEDs.\n"
                         "\"debug\": LEDs not used, free for internal debugging.", "",
                         "", "", "");
#if defined(WEBUI)
    getopt_parser.define("web", "web", "", "port", "",
                         "Start the web interface.\n"
                         "Serves the frontend from ~/10.05_web/3_frontend on <port> (default 80).", "",
                         "", "8080", "web interface on port 8080");
    getopt_parser.define("webroot", "webroot", "directory", "", "",
                         "Serve the web frontend from <directory>.\n"
                         "Default is 10.05_web/3_frontend under QUNILATOR_DIR, which is the\n"
                         "source tree layout; an installed frontend lives somewhere else.", "",
                         "", "/usr/share/qunilator/frontend", "installed frontend");
#endif

	// test options

    getopt_parser.define("t", "test", "iarg1,iarg2", "soptarg", "8 15",
                         "Tests the new c++ getop2.cpp\n"
                         "Multiline info, fix and optional args, short and long examples", "1,2",
                         "simple sets both mandatory int args", "1 2 hello",
                         "Sets integer args and option string arg");
//	if (argc < 2)
//		help(); // at least 1 required
    logger->default_level = LL_WARNING;
    res = getopt_parser.first(argc, argv);
    while (res > 0) {
        if (getopt_parser.isoption("help")) {
            help();
        } else if (getopt_parser.isoption("verbose")) {
            logger->default_level = LL_INFO;
        } else if (getopt_parser.isoption("debug")) {
            logger->default_level = LL_DEBUG;
        } else if (getopt_parser.isoption("cmdfile")) {
            if (getopt_parser.arg_s("cmdfilename", opt_cmdfilename) < 0)
                commandline_option_error(NULL);
#if defined(QBUS)
        } else if (getopt_parser.isoption("addresswidth")) {
            unsigned aw ;
            if (getopt_parser.arg_u("addresswidth", &aw) < 0)
                commandline_option_error(NULL);
            if (aw != 16 && aw != 18 && aw != 22)
                commandline_option_error((char *)"Number of address bits must ne 16, 18 or 22");
            qunibus->set_addr_width(aw) ;
            // now iopageregisters_init() possible
#endif
        } else if (getopt_parser.isoption("leds")) {
            std::string s ;
            // Option "debug" ?
            if (getopt_parser.arg_s("ledcode", s) < 0)
                commandline_option_error(NULL);
            if (getopt_parser.stringcmp(s, "debug") == 0)
                gpios->leds_for_debug = true ;
            else {
                unsigned n ;
                if (getopt_parser.arg_u("ledcode", &n) < 0)
                    commandline_option_error(NULL);
                if (n > 15)
                    commandline_option_error((char *)"4 LEDs can only display values 0..15");
                gpios->cmdline_leds = n ;
            }
#if defined(WEBUI)
        } else if (getopt_parser.isoption("web")) {
            unsigned n;
            int argres = getopt_parser.arg_u("port", &n);
            if (argres == GETOPT_STATUS_OK)
                opt_web_port = n;
            else if (argres < 0)
                commandline_option_error(NULL);
            else
                opt_web_port = 80;
        } else if (getopt_parser.isoption("webroot")) {
            if (getopt_parser.arg_s("directory", opt_web_root) != GETOPT_STATUS_OK)
                commandline_option_error(NULL);
#endif
        } else if (getopt_parser.isoption("test")) {
            int i1, i2;
            std::string s;
            if (getopt_parser.arg_i("iarg1", &i1) < 0)
                commandline_option_error(NULL);
            if (getopt_parser.arg_i("iarg2", &i2) < 0)
                commandline_option_error(NULL);
            std::cout << "iarg1=" << i1 << ", iarg2=" << i2;
            if (getopt_parser.arg_s("soptarg", s))
                std::cout << ", soptarg=" << s;
            std::cout << "\n";
        }
        res = getopt_parser.next();
    }
    if (res == GETOPT_STATUS_MINARGCOUNT || res == GETOPT_STATUS_MAXARGCOUNT)
        // known option, but wrong number of arguments
        commandline_option_error(NULL);
    else if (res < 0)
        commandline_error();
}

// configure all hardware related subsystems:
// PRU, shard memory, GPIOs
void application_c::hardware_startup(enum pru_c::prucode_enum prucode_id) 
{
    INFO("Connecting to PRU.");
    /* initialize the library, PRU and interrupt; launch our PRU program */

    // No mailbox_connect() here. pru->start() does it, before it loads the
    // PRUs - which is the only order that works, because mailbox_connect()
    // clears the whole mailbox. Called again afterwards it wipes whatever the
    // PRUs have written in the meantime, and by then they have been running for
    // the 100 ms start() waits out plus the NOP handshake it ends with. It cost
    // the diagnostic counters their magic word, written once at PRU startup and
    // erased before anything could read it; an event signalled that early would
    // go the same way.
    pru->start(prucode_id);

    INFO("Registering non-PRU pins.");
    gpios->init();
    INFO("Disable DS8641 drivers.");
    buslatches.output_enable(0);
    INFO("Leave SYSBOOT mode.");
    GPIO_SETVAL(gpios->reg_enable, 1);
    // input registers can now be read

    INFO("Registering multiplex bus latches, initialized later by PRU code.");
    // INFO("Setup bus multiplex latches.");
    buslatches.setup();

    //Todo:  iopageregisters_init() only after bus width known, and only in emulation-menus
    INFO("Initializing device register maps.");
    iopageregisters_init();

}


// disable all hardware related subsystems:
void application_c::hardware_shutdown() 
{
    pru->stop();
}

int application_c::run(int argc, char *argv[]) 
{
    void error_clear(void);

    opt_linewidth = 80;
    /* Intializes random number generator */
    {
        time_t t;
        srand((unsigned) time(&t));
    }

    // returns only if everything is OK
    // Std options already executed
    parse_commandline(argc, argv);

    logger->reset_log_levels(); // logger.default_level maybe info or debug
    // QUNILATOR_LOG=<path> additionally appends every message to a file,
    // flushed per line - reliable capture for tail/grep
    if (getenv("QUNILATOR_LOG"))
        logger->set_file_sink(getenv("QUNILATOR_LOG"));
    logger->default_filepath = "qunibone.log.csv";

    // the device workers run SCHED_RR and saturate the CPU during
    // register-poll-heavy phases; the kernel's RT throttling would then
    // suspend the whole RT class for 50ms of every second, long enough to
    // miss device-firmware poll windows (the DELQA self-test allows ~33ms
    // per reflected frame)
    {
        FILE *f = fopen("/proc/sys/kernel/sched_rt_runtime_us", "w");
        if (f) {
            fputs("-1\n", f);
            fclose(f);
        } else
            WARNING("cannot disable RT throttling (sched_rt_runtime_us)");
    }

    // pin the CPU at its top frequency: the ondemand governor idles the
    // single core down to 300-720MHz between poll bursts and ramps only
    // after several sample periods, adding scheduling latency in exactly
    // the phases where device firmware polls with a tight timeout
    {
        FILE *f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "w");
        if (f) {
            fputs("performance\n", f);
            fclose(f);
        } else
            WARNING("cannot pin CPU governor to performance");
    }

    // Test messages: visible if -verbose, -debug set.
    INFO("Printing verbose output.");
    DEBUG("Printing DEBUG output. Log file = \"%s\"", logger->default_filepath.c_str());

    /* the PRU interfaces need physical memory, so root or nothing */
    if (geteuid()) {
        FATAL("%s must be run as root to reach the PRUs\n", argv[0]);
    }

    inputline.init();
    if (!opt_cmdfilename.empty()) {
        // read commands from file
        if (!inputline.open_file((char*) opt_cmdfilename.c_str())) {
            printf("%s\n",
                   fileErrorText("Could not open command file \"%s\"",
                                 opt_cmdfilename.c_str()));
            return -1;
        }
    }

    std::cout << version << "\n";

    // The board is one set of hardware and this program is about to take it:
    // the GPIOs below and the PRU behind them are held by whatever is driving
    // the board now. Claimed after the options are read, so asking for --help
    // does not take a machine away from anyone, and before the first pin is
    // touched. A service holding it puts its machine down and locks its web
    // interface for the length of this session; a second menu is turned away.
#if defined(WEBUI)
    {
        std::string claim_error;
        if (!boardclaim_take(&claim_error)) {
            printf("Cannot take the board: %s\n", claim_error.c_str());
            return -1;
        }
    }
#endif

    // Multiplex latches are initialized by PRU code after each code download
    INFO("Registering Non-PRU GPIO pins.");
    gpios->init();
    INFO("Disable DS8641 drivers.");
    buslatches.output_enable(0);
    INFO("Leave SYSBOOT mode.");
    GPIO_SETVAL(gpios->reg_enable, 1);
    // input registers can now be read

#if defined(WEBUI)
    if (opt_web_port) {
        // installation root: QUNILATOR_DIR (see compile-bbb.env), fallback $HOME
        std::string docroot = opt_web_root;
        if (docroot.empty()) {
            const char *root = getenv("QUNILATOR_DIR");
            if (root == nullptr)
                root = getenv("HOME");
            docroot = std::string(root ? root : ".") + "/10.05_web/3_frontend";
        }
        // The device set lives for the process lifetime; menus only borrow it.
        websettings_startup();
        devices_startup(/*internal_bus*/false);
        webserver = new webserver_c(opt_web_port, docroot);
        webserver->start();
    }
#endif

    menu_main();

#if defined(WEBUI)
    if (opt_web_port) {
        webserver->stop();
        devices_shutdown();
    }
#endif

//	hardware_shutdown();

#if defined(WEBUI)
    // Hands the board back, which is what lets a waiting service rebuild its
    // machine. The claim ends with the process either way; this is the orderly
    // way out of it.
    boardclaim_release();
#endif

    return 0;
}


/* construct all singletons in proper order
 */
void qunibone_factory() 
{
    // logger first, all logsource_c connect to it.
    logger = new logger_c();

    the_flexi_timeout_controller = new flexi_timeout_controller_c() ;

    pru = new pru_c();
    gpios = new gpios_c();
    //qunibus_signals = new qunibus_signals_c();
    ddrmem = new ddrmem_c();

    // paneldriver before all devices who use lamps or buttons
    paneldriver = new paneldriver_c();

    membuffer = new memoryimage_c();

    qunibus = new qunibus_c();
    // qunibusadapter.worker() needs initialized mailbox
    qunibusadapter = new qunibusadapter_c();

    app = new application_c();
}


