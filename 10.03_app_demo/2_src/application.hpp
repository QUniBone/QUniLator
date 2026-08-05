/* application.cpp:  UniBone "demo" application, global resources

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
 */
#ifndef _APPLICATION_H_
#define _APPLICATION_H_
#include <string>

#include "logsource.hpp"
#include "getopt2.hpp"
#include "inputline.hpp"
#include "pru.hpp"
#include "parameter.hpp"
#include "qunibus.h"
#include "qunibusdevice.hpp"

#define PROGNAME	"demo"

// The makefiles define this from packaging/debian/changelog, so the binary and
// the package that ships it always name the same version. A build outside them
// (an editor's index, a host-side test) says so instead of naming a version it
// cannot have.
#ifndef QUNILATOR_VERSION
#define QUNILATOR_VERSION	"0.0.0-dev"
#endif
#define VERSION	QUNILATOR_VERSION

class application_c: public logsource_c {
public:
	const std::string copyright = std::string("(C) 2018-2020 Joerg Hoppe <j_hoppe@t-online.de>\n");
	const std::string version = std::string(PROGNAME "  - QUniBone " QUNIBUS_NAME " test application.\n"
	"    Version "
#ifdef DBG
			"DBG "
#endif
			VERSION ", compile " __DATE__ " " __TIME__ ".");

// global options
	unsigned opt_testnumber = 0; // test to perform

	// command line args
	unsigned opt_linewidth = 80;
	std::string opt_cmdfilename;
#if defined(WEBUI)
	unsigned opt_web_port = 0; // 0 = web interface not requested
	std::string opt_web_root; // frontend directory, empty = beside QUNILATOR_DIR
#endif
	getopt_c getopt_parser;
	void help(void);
	void commandline_error(void);
	void commandline_option_error(char *errtext, ...);
	void parse_commandline(int argc, char **argv);

	void hardware_startup(enum pru_c::prucode_enum prucode_id);

	void hardware_shutdown(void);

	// bring up PRU emulation code and the device set (device_configuration),
	// as the devices menu does on entry; used at startup in web mode
	// internal_bus: load the firmware that keeps the bus inside the PRU, for a
	// board that is a machine by itself rather than a card in a backplane
	void devices_startup(bool internal_bus);
	void devices_shutdown(void);

	// QUniBone should emulate this address range
	uint32_t emulated_memory_start_addr; // even
	uint32_t emulated_memory_end_addr; // even (last word)

	void print_arbitration_info(		const char *indent);
	void print_params(parameterized_c *parameterized, parameter_c *p);

	inputline_c	inputline ;
	bool script_active() {return inputline.is_file_open();}
	char *getchoice(const char *menu_code);

	bool emulate_memory(uint32_t endaddr = 0) ;

	qunibusdevice_register_t * device_register_by_id(qunibusdevice_c *device, char *specifier);

	void menu_info(const char *menu_code);
	void menu_gpio(const char *menu_code);
	void menu_panel(const char *menu_code);
	void menu_mailbox(const char *menu_code);
	void menu_buslatches(const char *menu_code);
	void menu_qunibus_signals(const char *menu_code);
	void menu_ddrmem_slave_only(const char *menu_code);
	void menu_masterslave(const char *menu_code, bool with_CPU);
	void menu_interrupts(const char *menu_code);
	void menu_devices(const char *menu_code);
	void menu_device_exercisers(const char *menu_code);

	void menu_main(void);

public:
	application_c();

	int run(int argc, char *argv[]);

};

extern application_c *app;	// Singleton

// construct the singletons every program built on this code needs
void qunibone_factory(void);


#endif

