/* device_label.cpp: the static per-type table behind GET /api/devices' label

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.

   One home for the friendly-name table the web UI and the MCP server both read
   from the API. The table is keyed by type_name; each entry gives a role
   description and the DEC code, and marks how the instance is disambiguated:
   drives append their unit number, the serial lines append their CSR address,
   controllers and standalone devices show the bare role.
*/

#include <cstdint>
#include <cstdio>
#include <string>

#include "device_label.hpp"

namespace {

enum instancing_e {
	INST_NONE,      // controller or standalone device: bare role
	INST_UNIT,      // drive instance: append the unit number
	INST_ADDR       // same-type instances told apart by CSR address
};

struct label_entry {
	const char *type_name;
	const char *role;
	const char *code;       // "" for internal devices with no DEC code
	instancing_e instancing;
};

const label_entry k_table[] = {
	{ "blinkenbone_c", "Front panel",            "",      INST_NONE },
	{ "demo_io_c",     "Demo I/O",               "",      INST_NONE },
	{ "RF11",          "DECdisk controller",     "RF11",  INST_NONE },
	{ "RS11",          "Fixed-head disk",        "RS11",  INST_UNIT },
	{ "RLV12",         "RL disk controller",     "RLV12", INST_NONE },
	{ "RL02",          "RL02 cartridge disk",    "RL02",  INST_UNIT },
	{ "RKV11",         "RK disk controller",     "RKV11", INST_NONE },
	{ "RK05",          "RK05 cartridge disk",    "RK05",  INST_UNIT },
	{ "UDA50",         "MSCP disk controller",   "UDA50", INST_NONE },
	{ "RA81",          "MSCP disk",              "RA81",  INST_UNIT },
	{ "slu_c",         "Serial line unit",       "DL11",  INST_ADDR },
	{ "ltc_c",         "Line-time clock",        "KW11",  INST_NONE },
	{ "RXV11",         "RX01 floppy controller", "RXV11", INST_NONE },
	{ "RXV12",         "RX02 floppy controller", "RXV12", INST_NONE },
	{ "RX0102uCPU",    "Floppy microcontroller", "",      INST_NONE },
	{ "RX01",          "RX01 floppy",            "RX01",  INST_UNIT },
	{ "RX02",          "RX02 floppy",            "RX02",  INST_UNIT },
	{ "DELQA",         "Ethernet controller",    "DELQA", INST_UNIT },
	{ "VCB01",         "Graphics display",       "VCB01", INST_NONE },
};

} // namespace

std::string device_label(const std::string &type_name, const std::string &name,
		const std::string &unit, uint32_t base_addr) {
	const label_entry *entry = nullptr;
	for (const label_entry &cand : k_table) {
		if (type_name == cand.type_name) {
			entry = &cand;
			break;
		}
	}
	if (entry == nullptr)
		return name;    // unknown type: the raw handle still renders

	std::string role = entry->role;
	if (entry->instancing == INST_UNIT && !unit.empty()) {
		role += " ";
		role += unit;
	} else if (entry->instancing == INST_ADDR) {
		char buf[16];
		snprintf(buf, sizeof(buf), "@%o", (unsigned) base_addr);
		role += " ";
		role += buf;
	}

	if (entry->code[0] == '\0')
		return role;
	return role + " (" + entry->code + ")";
}
