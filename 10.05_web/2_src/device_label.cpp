/* device_label.cpp: the static per-type table behind GET /api/devices' label

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.

   One home for the friendly-name table the web UI and the MCP server both read
   from the API. Every label is built the same way — the DEC board type, then
   what the board is, then the instance when a machine can carry more than one:

     RA81 disk 0        RL11 disk controller      DZV11 serial mux 0
     TK50 tape 1        DELQA Ethernet controller KA11 processor

   The table is keyed by type_name; each entry gives the DEC code, the role, and
   how the instance is numbered: drives take their unit number, several boards of
   one type take their ordinal in the registry, and a machine that carries one of
   something takes no number at all. Internal devices with no DEC code render the
   bare role.
*/

#include <cctype>
#include <cstdio>
#include <string>

#include "device_label.hpp"

// The DZ mux is one register model on two boards: the Unibus DZ11 carries eight
// lines, the Q-bus DZV11/DZQ11 four (see dzv11.hpp), and the name follows the
// bus the service was built for.
#if defined(UNIBUS)
#define DZ_BOARD_CODE "DZ11"
#else
#define DZ_BOARD_CODE "DZV11"
#endif

namespace {

enum instancing_e {
	INST_NONE,      // a machine carries one: no number
	INST_UNIT,      // drive instance: its unit number
	INST_INDEX      // several boards of one type: its ordinal in the registry
};

struct label_entry {
	const char *type_name;
	const char *code;       // DEC board type; "" for internal devices with none
	const char *role;       // what the board is, lower case
	instancing_e instancing;
};

const label_entry k_table[] = {
	{ "blinkenbone_c", "",        "front panel",             INST_NONE },
	{ "demo_io_c",     "",        "demo I/O",                INST_NONE },
	{ "RF11",          "RF11",    "disk controller",         INST_NONE },
	{ "RS11",          "RS11",    "disk",                    INST_UNIT },
	{ "RL11",          "RL11",    "disk controller",         INST_NONE },
	{ "RLV11",         "RLV11",   "disk controller",         INST_NONE },
	{ "RLV12",         "RLV12",   "disk controller",         INST_NONE },
	{ "RL01",          "RL01",    "disk",                    INST_UNIT },
	{ "RL02",          "RL02",    "disk",                    INST_UNIT },
	{ "RK11",          "RK11",    "disk controller",         INST_NONE },
	{ "RKV11",         "RKV11",   "disk controller",         INST_NONE },
	{ "RK05",          "RK05",    "disk",                    INST_UNIT },
	{ "UDA50",         "UDA50",   "disk controller",         INST_NONE },
	{ "RA81",          "RA81",    "disk",                    INST_UNIT },
	{ "TQK50",         "TQK50",   "tape controller",         INST_NONE },
	{ "TK50",          "TK50",    "tape",                    INST_UNIT },
	{ "TS11",          "TS11",    "tape controller",         INST_NONE },
	{ "TSV05",         "TSV05",   "tape controller",         INST_NONE },
	{ "TS05",          "TS05",    "tape",                    INST_UNIT },
	{ "slu_c",         "DL11",    "serial line",             INST_INDEX },
	{ "ltc_c",         "KW11",    "line clock",              INST_NONE },
	{ "kw11p_c",       "KW11-P",  "programmable clock",      INST_NONE },
	{ "RX11",          "RX11",    "floppy controller",       INST_NONE },
	{ "RXV11",         "RXV11",   "floppy controller",       INST_NONE },
	{ "RY211",         "RX211",   "floppy controller",       INST_NONE },
	{ "RXV12",         "RXV12",   "floppy controller",       INST_NONE },
	{ "RX0102uCPU",    "",        "floppy microcontroller",  INST_NONE },
	{ "RX01",          "RX01",    "floppy",                  INST_UNIT },
	{ "RX02",          "RX02",    "floppy",                  INST_UNIT },
	{ "DELQA",         "DELQA",   "Ethernet controller",     INST_NONE },
	{ "DEUNA",         "DEUNA",   "Ethernet controller",     INST_NONE },
	{ "VCB01",         "VCB01",   "graphics display",        INST_NONE },
	{ "dzv11_c",       DZ_BOARD_CODE, "serial mux",          INST_INDEX },
	{ "dhv11_c",       "DHV11",   "serial mux",              INST_INDEX },
	{ "MS11",          "MS11",    "memory",                  INST_NONE },
	{ "MSV11",         "MSV11",   "memory",                  INST_NONE },
	{ "PDP-11/20",     "KA11",    "processor",               INST_NONE },
	{ "PDP-11/34",     "KD11-EA", "processor",               INST_NONE },
	{ "VAX-11/780",    "KA780",   "processor",               INST_NONE },
	{ "KE11",          "KE11-A",  "arithmetic element",      INST_NONE },
	{ "m9312_c",       "M9312",   "bootstrap ROM",           INST_NONE },
	{ "MRV11-D",       "MRV11-D", "bootstrap ROM",           INST_NONE },
};

} // namespace

std::string device_label(const std::string &type_name, const std::string &name,
		const std::string &unit, unsigned instance) {
	const label_entry *entry = nullptr;
	for (const label_entry &cand : k_table) {
		if (type_name == cand.type_name) {
			entry = &cand;
			break;
		}
	}
	if (entry == nullptr)
		return name;    // unknown type: the raw handle still renders

	std::string s;
	if (entry->code[0] != '\0') {
		s = entry->code;
		s += " ";
		s += entry->role;
	} else {
		// no board type to lead with, so the role opens the label and carries
		// the capital
		s = entry->role;
		s[0] = (char) toupper((unsigned char) s[0]);
	}

	if (entry->instancing == INST_UNIT && !unit.empty()) {
		s += " ";
		s += unit;
	} else if (entry->instancing == INST_INDEX) {
		char buf[16];
		snprintf(buf, sizeof(buf), " %u", instance);
		s += buf;
	}
	return s;
}
