/* webapi.cpp: JSON API of the web interface

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.

   Reads are snapshots of the device/parameter registry taken under
   device_c::mydevices_mutex. Writes go directly through the parameter
   system — the same calls the devices menu makes — serialized against the
   menu thread by device_configuration_c::operations_mutex:

     PUT  /api/devices/<device>/params/<param>   {"value": "..."}
     POST /api/control                           {"action": "init" | ...}

   Enable/disable is the "enabled" parameter, so the params endpoint covers
   it too.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <cstdint>
#include <map>
#include <set>
#include <vector>
#include <mutex>

#include "civetweb.h"
#include "picojson.h"

#include "logger.hpp"
#include "device.hpp"
#include "storagedrive.hpp"
#include "storagecontroller.hpp"
#include "timeout.hpp"     // timeout_c, used by rl0102.hpp
#include "rl0102.hpp"
#include "parameter.hpp"
#include "qunibus.h"
#include "ddrmem.h"
#include "qunibusadapter.hpp"
#include "panel.hpp"
#include "mscp_server.hpp"
#include "device_configuration.hpp"
#include "device_label.hpp"
#include "device_status.hpp"
#include "webcontrol.hpp"

#include "weblog.hpp"
#include "webevents.hpp"
#include "webconsole.hpp"
#include "webserial.hpp"
#include "webconsole_ext.hpp"
#include "webvcb01.hpp"
#include "webstorage.hpp"
#include "webconfigs.hpp"
#include "websettings.hpp"
#include "weblogging.hpp"
#include "webversion.hpp"
#include "webupdate.hpp"

static void send_json(struct mg_connection *conn, int status, const picojson::value &val) {
	std::string body = val.serialize();
	mg_printf(conn,
			"HTTP/1.1 %d %s\r\n"
			"Content-Type: application/json\r\n"
			"Cache-Control: no-store\r\n"
			"Content-Length: %u\r\n\r\n",
			status, status == 200 ? "OK" : "Error", (unsigned) body.size());
	mg_write(conn, body.c_str(), body.size());
}

static void send_error(struct mg_connection *conn, int status, const std::string &message) {
	picojson::object err;
	err["error"] = picojson::value(message);
	send_json(conn, status, picojson::value(err));
}

// read and parse a JSON object request body, of any size - a bulk memory write
// carries thousands of words, far past one read buffer
static bool read_json_body(struct mg_connection *conn, picojson::value *out) {
	std::string body;
	char chunk[8192];
	int n;
	while ((n = mg_read(conn, chunk, sizeof(chunk))) > 0) {
		body.append(chunk, (size_t) n);
		if (body.size() > 64 * 1024 * 1024)     // a sane ceiling
			return false;
	}
	if (body.empty())
		return false;
	std::string parse_err = picojson::parse(*out, body);
	return parse_err.empty() && out->is<picojson::object>();
}

// one parameter with metadata, typed values serialized from the value
// members (render() mutates shared state and is left to the menu thread)
static picojson::value param_to_json(parameter_c *p) {
	picojson::object o;
	o["name"] = picojson::value(p->name);
	o["shortname"] = picojson::value(p->shortname);
	o["readonly"] = picojson::value(p->readonly);
	if (!p->info.empty())
		o["info"] = picojson::value(p->info);
	if (!p->unit.empty())
		o["unit"] = picojson::value(p->unit);

	if (parameter_string_c *ps = dynamic_cast<parameter_string_c *>(p)) {
		o["type"] = picojson::value("string");
		o["value"] = picojson::value(ps->value);
	} else if (parameter_bool_c *pb = dynamic_cast<parameter_bool_c *>(p)) {
		o["type"] = picojson::value("bool");
		o["value"] = picojson::value(pb->value);
	} else if (parameter_unsigned_c *pu = dynamic_cast<parameter_unsigned_c *>(p)) {
		o["type"] = picojson::value("unsigned");
		o["value"] = picojson::value((double) pu->value);
		o["base"] = picojson::value((double) pu->base);
		o["bitwidth"] = picojson::value((double) pu->bitwidth);
	} else if (parameter_unsigned64_c *pu64 = dynamic_cast<parameter_unsigned64_c *>(p)) {
		o["type"] = picojson::value("unsigned64");
		o["value"] = picojson::value((double) pu64->value);
		o["base"] = picojson::value((double) pu64->base);
		o["bitwidth"] = picojson::value((double) pu64->bitwidth);
	} else if (parameter_double_c *pd = dynamic_cast<parameter_double_c *>(p)) {
		o["type"] = picojson::value("double");
		o["value"] = picojson::value(pd->value);
	} else {
		o["type"] = picojson::value("unknown");
	}
	return picojson::value(o);
}

// infrastructure: part of the bridge or of a controller's implementation,
// not of the emulated configuration — not exposed to the web interface.
// mscp_disk_server / mscp_tape_server are the MSCP/TMSCP protocol engines,
// device_c only so the logging macros work.
static bool device_is_infrastructure(device_c *d) {
	return dynamic_cast<qunibusadapter_c *>(d) != nullptr
			|| dynamic_cast<paneldriver_c *>(d) != nullptr
			|| dynamic_cast<mscp_disk_server *>(d) != nullptr
			|| dynamic_cast<mscp_tape_server *>(d) != nullptr;
}

// caller holds device_c::mydevices_mutex
static device_c *find_device(const std::string &name) {
	for (device_c *d : device_c::mydevices) {
		if (device_is_infrastructure(d))
			continue;
		if (strcasecmp(d->name.value.c_str(), name.c_str()) == 0)
			return d;
	}
	return nullptr;
}

// the RL state values feed the verbal status; keep the mirror constants in
// device_status.hpp honest against the device header
static_assert(RL0102_STATE_lock_on == DISK_STATUS_RL_LOCK_ON,
		"DISK_STATUS_RL_LOCK_ON out of sync with rl0102.hpp");
static_assert(RL0102_STATE_spin_up == DISK_STATUS_RL_SPIN_UP,
		"DISK_STATUS_RL_SPIN_UP out of sync with rl0102.hpp");
static_assert(RL0102_STATE_load_heads == DISK_STATUS_RL_LOAD_HEADS,
		"DISK_STATUS_RL_LOAD_HEADS out of sync with rl0102.hpp");
static_assert(RL0102_STATE_unload_heads == DISK_STATUS_RL_UNLOAD_HEADS,
		"DISK_STATUS_RL_UNLOAD_HEADS out of sync with rl0102.hpp");
static_assert(RL0102_STATE_spin_down == DISK_STATUS_RL_SPIN_DOWN,
		"DISK_STATUS_RL_SPIN_DOWN out of sync with rl0102.hpp");

// Computed verbal status for a disk drive, one of off/idle/loaded/ready/busy.
// Reads only the parameters the drive already publishes and defers the mapping
// to disk_status(); "" for a non-disk device, which omits the field.
std::string device_status_for(device_c *d) {
	storagedrive_c *drv = dynamic_cast<storagedrive_c *>(d);
	if (drv == nullptr)
		return "";
	disk_signals_c s;
	s.enabled = d->enabled.value;
	s.has_image = !drv->image_filepath.value.empty();
	s.activity = drv->access_lamp.value;
	disk_family_e fam = disk_family_e::generic;
	if (RL0102_c *rl = dynamic_cast<RL0102_c *>(drv)) {
		fam = disk_family_e::rl;
		s.rl_state = (int) rl->state.value;
	}
	return disk_status(fam, s);
}

// A device type's standard bus placements, gathered from the construction
// defaults of every instance of that type in the registry (the pool the DEC
// floating-address scheme lays down: e.g. the four DZV11 addresses). The UI
// offers these as the address/vector menu for a device of that type.
static picojson::array sorted_options(const std::set<uint32_t> &vals) {
	picojson::array a;
	for (uint32_t v : vals) // std::set iterates ascending
		a.push_back(picojson::value((double) v));
	return a;
}

// GET /api/devices — snapshot of the device registry
static void devices_list(struct mg_connection *conn) {
	picojson::array devices;
	{
		std::lock_guard<std::mutex> lock(device_c::mydevices_mutex);
		// per-type standard address/vector sets, from all instances' defaults
		std::map<std::string, std::set<uint32_t> > addr_opts, vec_opts;
		for (device_c *d : device_c::mydevices) {
			qunibusdevice_c *qd = dynamic_cast<qunibusdevice_c *>(d);
			if (qd == nullptr || device_is_infrastructure(d))
				continue;
			addr_opts[d->type_name.value].insert(qd->default_base_addr);
			if (qd->default_intr_vector != 0)
				vec_opts[d->type_name.value].insert(qd->default_intr_vector);
		}
		// how many of each type have been labelled so far, which numbers the
		// boards a machine can carry several of ("DZV11 serial mux 0..3") in
		// registry order — the order the pool was built in
		std::map<std::string, unsigned> type_seen;
		for (device_c *d : device_c::mydevices) {
			if (device_is_infrastructure(d))
				continue;
			picojson::object o;
			o["name"] = picojson::value(d->name.value);
			o["type"] = picojson::value(d->type_name.value);
			// friendly name from the static per-type table (read-only, derived)
			std::string unit;
			if (parameter_c *up = d->param_by_name("unit")) {
				if (parameter_unsigned_c *uu = dynamic_cast<parameter_unsigned_c *>(up)) {
					char buf[16];
					snprintf(buf, sizeof(buf), "%u", (unsigned) uu->value);
					unit = buf;
				}
			}
			if (dynamic_cast<qunibusdevice_c *>(d) != nullptr) {
				// the standard address/vector menu for this device's type, so the
				// UI offers a dropdown rather than a free octal field
				o["address_options"] = picojson::value(
						sorted_options(addr_opts[d->type_name.value]));
				o["vector_options"] = picojson::value(
						sorted_options(vec_opts[d->type_name.value]));
			}
			unsigned instance = type_seen[d->type_name.value]++;
			o["label"] = picojson::value(
					device_label(d->type_name.value, d->name.value, unit, instance));
			o["category"] = picojson::value(std::string(d->category()));
			storagedrive_c *drv = dynamic_cast<storagedrive_c *>(d);
			if (drv != nullptr) {
				o["removable"] = picojson::value(drv->removable());
				o["locked"] = picojson::value(drv->locked());
				// computed verbal status, shared with the dashboard and MCP
				o["status"] = picojson::value(device_status_for(d));
			}
			o["enabled"] = picojson::value(d->enabled.value);
			o["parent"] = d->parent ?
					picojson::value(d->parent->name.value) : picojson::value();
			// two collections split by parameter kind: "params" is the
			// configuration an operator sets and a snapshot captures;
			// "statusparams" is running state the emulator drives (lamps,
			// activity LEDs, the drive state machine, mirrored registers),
			// read-only for display. Entry shape is the same in both. This is
			// distinct from the "status" string above, the disk drive's
			// computed verbal state.
			picojson::array params, statusparams;
			for (parameter_c *p : d->parameter) {
				if (p->kind == parameter_c::PARAM_STATUS)
					statusparams.push_back(param_to_json(p));
				else
					params.push_back(param_to_json(p));
			}
			o["params"] = picojson::value(params);
			o["statusparams"] = picojson::value(statusparams);
			devices.push_back(picojson::value(o));
		}
	}
	send_json(conn, 200, picojson::value(devices));
}

// The device's bus placement — where it sits in the I/O page and how it
// interrupts. Locked while the device is on the bus (readonly), so changing one
// re-registers the device, which is only safe with the CPU halted.
static bool is_bus_placement_param(const std::string &n) {
	return n == "base_addr" || n == "intr_vector" || n == "intr_level" || n == "slot";
}

// The enabled device whose register window overlaps [addr, addr + 2*count), or
// "" when the range is free. register_device() aborts the emulator on an I/O
// page collision, so a re-address is checked against this first.
// Caller holds device_c::mydevices_mutex.
static std::string address_range_owner(qunibusdevice_c *self, uint32_t addr,
		unsigned count) {
	uint32_t end = addr + 2 * count;
	for (device_c *d : device_c::mydevices) {
		if (d == self || device_is_infrastructure(d))
			continue;
		qunibusdevice_c *qd = dynamic_cast<qunibusdevice_c *>(d);
		if (qd == nullptr || !qd->enabled.value)
			continue;
		uint32_t o = qd->base_addr.value, oend = o + 2 * qd->register_count;
		if (addr < oend && o < end)
			return qd->name.value;
	}
	return "";
}

// PUT /api/devices/<device>/params/<param> {"value": ...} — set a parameter
static void device_param_set(struct mg_connection *conn, const std::string &devname,
		const std::string &paramname) {
	picojson::value req;
	if (!read_json_body(conn, &req) || req.get("value").is<picojson::null>()) {
		send_error(conn, 400, "body must be a JSON object with a \"value\" member");
		return;
	}
	// accept string, number and bool values; the parameter parses text
	std::string value;
	picojson::value v = req.get("value");
	if (v.is<std::string>())
		value = v.get<std::string>();
	else if (v.is<bool>())
		value = v.get<bool>() ? "1" : "0";
	else if (v.is<double>()) {
		char buff[40];
		snprintf(buff, sizeof(buff), "%g", v.get<double>());
		value = buff;
	} else {
		send_error(conn, 400, "\"value\" must be a string, number or bool");
		return;
	}

	device_c *dev;
	parameter_c *param;
	{
		std::lock_guard<std::mutex> lock(device_c::mydevices_mutex);
		dev = find_device(devname);
		param = dev ? dev->param_by_name(paramname) : nullptr;
	}
	if (dev == nullptr) {
		send_error(conn, 404, "unknown device \"" + devname + "\"");
		return;
	}
	if (param == nullptr) {
		send_error(conn, 404, "unknown parameter \"" + paramname + "\"");
		return;
	}

	{
		// same serialization as one command in the devices menu
		std::lock_guard<std::mutex> ops_lock(device_configuration_c::operations_mutex);
		try {
			if (param == &dev->enabled) {
				// "enabled" is a readonly parameter; the menu switches it
				// with the en/dis commands via set(), and so does the web
				bool on;
				if (value == "1" || !strcasecmp(value.c_str(), "true"))
					on = true;
				else if (value == "0" || !strcasecmp(value.c_str(), "false"))
					on = false;
				else {
					send_error(conn, 400, "\"enabled\" must be 1/0 or true/false");
					return;
				}
				dev->enabled.set(on);
				// disabling a controller disables the drives it contains:
				// a drive on a removed controller cannot function
				storagecontroller_c *ctrl = dynamic_cast<storagecontroller_c *>(dev);
				if (!on && ctrl != nullptr)
					for (storagedrive_c *drv : ctrl->storagedrives)
						if (drv != nullptr && drv->enabled.value) {
							drv->enabled.set(false);
							WEB_INFO("%s disabled with controller %s",
									drv->name.value.c_str(), dev->name.value.c_str());
						}
			} else if (is_bus_placement_param(param->name)
					&& dynamic_cast<qunibusdevice_c *>(dev) != nullptr
					&& dev->enabled.value) {
				// The bus placement of an installed device is locked; moving it
				// re-registers the device on the bus, which is only safe with the
				// CPU halted. Re-jumper by unplugging (unregister, unlock the
				// placement fields), setting the new value, and re-plugging.
				qunibusdevice_c *qd = dynamic_cast<qunibusdevice_c *>(dev);
				if (!webevents_is_halted()) {
					send_error(conn, 409,
							"halt the CPU before changing a device's bus address or interrupt");
					return;
				}
				// a colliding address would abort the emulator at registration, so
				// refuse it here instead of re-plugging into an occupied window
				if (param->name == "base_addr") {
					uint32_t newaddr = (uint32_t) strtoul(value.c_str(), nullptr, 8);
					std::string owner = address_range_owner(qd, newaddr, qd->register_count);
					if (!owner.empty()) {
						send_error(conn, 409, "address " + value
								+ " overlaps device \"" + owner + "\"");
						return;
					}
				}
				dev->enabled.set(false); // unplug: unregister, unlock placement
				param->parse(value);     // re-jumper
				dev->enabled.set(true);  // re-plug: lock, register at the new placement
				if (qd->handle == 0) {
					send_error(conn, 409,
							"device could not be re-registered at the new placement");
					return;
				}
			} else {
				if (param->name == "image") {
					// the web interface keeps images in one directory, so a
					// bare name attaches the file it manages by that name
					value = webstorage_image_path(value);
					// both drives would write it
					std::string other = webstorage_image_held_by(value, dev->name.value);
					if (!other.empty()) {
						send_error(conn, 409, "that image is mounted on " + other);
						return;
					}
					// Attaching a medium is the operator saying they want this
					// drive in the machine, so it is switched on with the same
					// call their own switch makes — a configuration saved
					// afterwards records the drive as enabled. A drive on a
					// disabled controller cannot come up at all, so that is
					// reported rather than leaving the drive silently off.
					storagedrive_c *drv = dynamic_cast<storagedrive_c *>(dev);
					if (drv != nullptr && !value.empty() && !dev->enabled.value) {
						if (drv->controller != nullptr
								&& !drv->controller->enabled.value) {
							send_error(conn, 409, "enable controller \""
									+ drv->controller->name.value
									+ "\" before putting a medium in "
									+ dev->name.value);
							return;
						}
						param->parse(value);
						dev->enabled.set(true);
						WEB_INFO("%s enabled with its medium", dev->name.value.c_str());
						webstorage_refresh_readonly(webevents_is_powered()
								&& !webevents_is_halted());
						send_json(conn, 200, param_to_json(param));
						return;
					}
				} else if (param->name == "romfile") {
					// a ROM image is a file of the same tree, named by its
					// subpath. Several boards may be programmed from one file,
					// so nothing holds it.
					value = webstorage_image_path(value);
				}
				param->parse(value);
			}
			// keep the terminal user informed, like an echoed command
			WEB_INFO("%s.%s = %s", dev->name.value.c_str(),
					param->name.c_str(), value.c_str());
			// attaching/detaching an image changes which files the shares must
			// hold read-only while the machine runs
			if (param->name == "image")
				webstorage_refresh_readonly(webevents_is_powered()
						&& !webevents_is_halted());
		} catch (bad_parameter &e) {
			WEB_INFO("%s.%s = %s rejected: %s", dev->name.value.c_str(),
					param->name.c_str(), value.c_str(), e.what());
			send_error(conn, 422, e.what());
			return;
		}
	}
	send_json(conn, 200, param_to_json(param));
}

// /api/devices and /api/devices/<device>/params/<param>
static int api_devices_handler(struct mg_connection *conn, void * /*cbdata*/) {
	const struct mg_request_info *ri = mg_get_request_info(conn);
	std::string uri = ri->local_uri ? ri->local_uri : "";
	std::string rest = uri.substr(strlen("/api/devices"));

	if (rest.empty() || rest == "/") {
		if (strcmp(ri->request_method, "GET") != 0) {
			send_error(conn, 405, "GET required");
			return 405;
		}
		devices_list(conn);
		return 200;
	}

	// expect /<device>/params/<param>
	std::vector<std::string> seg;
	size_t pos = 1;
	while (pos <= rest.size()) {
		size_t next = rest.find('/', pos);
		if (next == std::string::npos)
			next = rest.size();
		if (next > pos)
			seg.push_back(rest.substr(pos, next - pos));
		pos = next + 1;
	}
	if (seg.size() != 3 || seg[1] != "params") {
		send_error(conn, 404, "unknown path");
		return 404;
	}
	if (strcmp(ri->request_method, "PUT") != 0 && strcmp(ri->request_method, "POST") != 0) {
		send_error(conn, 405, "PUT required");
		return 405;
	}
	device_param_set(conn, seg[0], seg[2]);
	return 200;
}

// The run controls, applied to an emulated CPU. Its console switches are the
// machine's front panel, so HALT, CONTINUE and RESTART are those switches
// rather than the bus signals a physical CPU watches — on UNIBUS there is no
// HALT line to pull at all. Returns false when no emulated CPU is running the
// machine, leaving the caller to drive the bus.
//
// RESTART is the front-panel sequence a KA11 has: LOAD ADDR, then START. The
// address is the one the M9312 resolved for its boot PROM when a machine has
// one; without it the CPU starts from the address already in its PC, which is
// what an operator setting it by hand would expect.
static bool control_apply_to_emulated_cpu(const std::string &action) {
#if defined(UNIBUS)
	// Only the run controls. init and powercycle are bus operations whichever
	// CPU is present, and dc_on/dc_off carry power bookkeeping that belongs
	// with them, so those keep the path below.
	if (action != "halt" && action != "continue" && action != "restart")
		return false;
	if (device_configuration == nullptr)
		return false;
	unibuscpu_c *cpu = device_configuration->emulated_cpu();
	if (cpu == nullptr)
		return false;
	std::lock_guard<std::mutex> ops_lock(device_configuration_c::operations_mutex);

	if (action == "halt") {
		cpu->panel_halt_switch()->set(true);
		webevents_note_halt(true);
		WEB_INFO("control %s: CPU halted", action.c_str());
		return true;
	}

	// A restart re-enters at the boot address, so a running CPU is stopped
	// first: the worker acts on the switch within a pass, and the wait is
	// bounded so a CPU that will not stop cannot hold the request open.
	if (action == "restart" && cpu->panel_run_led()->value) {
		cpu->panel_halt_switch()->set(true);
		timeout_c timeout;
		for (unsigned i = 0; i < 50 && cpu->panel_run_led()->value; i++)
			timeout.wait_ms(2);
	}
	cpu->panel_halt_switch()->set(false);
	webevents_note_halt(false);

	if (action == "restart") {
		// On a PDP-11, RESTART is LOAD ADDR of the M9312's boot address, then
		// START; the VAX's START rebuilds the machine and boots it by itself.
		cpu_base_c *pdp11 = device_configuration->emulated_pdp11();
		if (pdp11 != nullptr) {
			m9312_c *m9312 = device_configuration->m9312;
			if (m9312 != nullptr && m9312->enabled.value
					&& m9312->bootaddress != MEMORY_ADDRESS_INVALID)
				pdp11->pc.set(m9312->bootaddress & 0177777);
			cpu->panel_start_switch()->set(true);
			WEB_INFO("control restart: CPU started at %06o",
					(unsigned) pdp11->pc.value);
		} else {
			cpu->panel_start_switch()->set(true);
			WEB_INFO("control restart: CPU started");
		}
	} else {
		cpu->panel_continue_switch()->set(true);
		WEB_INFO("control %s: CPU continued", action.c_str());
	}
	return true;
#else
	(void) action;
	return false;
#endif
}

// POST /api/control
// {"action": "init"|"powercycle"|"restart"|"halt"|"continue"|"dc_on"|"dc_off"}
static int api_control_handler(struct mg_connection *conn, void * /*cbdata*/) {
	const struct mg_request_info *ri = mg_get_request_info(conn);
	if (strcmp(ri->request_method, "POST") != 0) {
		send_error(conn, 405, "POST required");
		return 405;
	}
	picojson::value req;
	if (!read_json_body(conn, &req) || !req.get("action").is<std::string>()) {
		send_error(conn, 400, "body must be a JSON object with an \"action\" string");
		return 400;
	}
	std::string action = req.get("action").get<std::string>();

	control_decision_c dec = control_decide(action, webevents_is_powered());
	if (!dec.known) {
		send_error(conn, 400, "unknown action \"" + action + "\"");
		return 400;
	}
	if (!dec.allowed) {
		send_error(conn, 409, "machine is powered off");
		return 409;
	}

	// An emulated CPU has no HALT line to pull: its switches are the front
	// panel, and the run controls belong on them. A board serving a physical
	// PDP-11 keeps driving the bus signals below.
	// A machine that has just been switched on has printed nothing, so the
	// consoles forget what the last one printed rather than handing a terminal
	// that reconnects a screen the machine did not put there.
	if (action == "dc_on" || action == "powercycle" || action == "restart") {
		webconsole_clear();
		webconsole_ext_clear();
	}

	if (control_apply_to_emulated_cpu(action)) {
		picojson::object res;
		res["ok"] = picojson::value(true);
		send_json(conn, 200, picojson::value(res));
		return 200;
	}

	{
		std::lock_guard<std::mutex> ops_lock(device_configuration_c::operations_mutex);
#if defined(QBUS)
		// Release the HALT line before any power-up so a machine brought up by
		// dc_on or restart comes up executing from the power-up vector. dc_off
		// leaves HALT asserted on the bus; powering up with it still asserted
		// would land the CPU in micro-ODT instead of running.
		if (dec.do_resume) {
			qunibus->set_halt(0);
			webevents_note_halt(false);
		}
#endif
#if defined(UNIBUS)
		// An emulated processor has no HALT line on the bus to release: it is
		// stopped and started through its own console switches. Power carries the
		// run state with it, so switching the machine on starts the processor and
		// switching it off stops it — otherwise AUX ON leaves a machine powered
		// and standing still, with nothing on the panel to say why.
		unibuscpu_c *ecpu = (device_configuration == nullptr)
				? nullptr : device_configuration->emulated_cpu();
		if (ecpu != nullptr && dec.set_powered == 0) {
			ecpu->panel_halt_switch()->set(true);
			webevents_note_halt(true);
		}
#endif
		if (dec.do_init)
			qunibus->init();
		if (dec.do_powercycle)
			qunibus->powercycle();
#if defined(QBUS)
		if (dec.do_halt) {
			qunibus->set_halt(1);
			webevents_note_halt(true);
		}
#endif
#if defined(UNIBUS)
		if (ecpu != nullptr && dec.set_powered == 1) {
			// the START switch rebuilds the machine and runs it, which is what
			// the power switch means on a processor that is emulated
			ecpu->panel_halt_switch()->set(false);
			ecpu->panel_start_switch()->set(true);
			webevents_note_halt(false);
			WEB_INFO("control %s: emulated CPU started", action.c_str());
		}
#endif
		if (dec.set_powered >= 0)
			webevents_note_powered(dec.set_powered != 0);
		WEB_INFO("control %s", action.c_str());
	}
	picojson::object res;
	res["ok"] = picojson::value(true);
	send_json(conn, 200, picojson::value(res));
	return 200;
}

// GET  /api/memory?address=<octal>&count=<n>  reads n words
// POST /api/memory {"address": <octal-or-number>, "words": [w, ...]}  writes
//
// The board is bus master, so this DMAs to and from the machine's memory -
// its own card or QBone's emulated range - without the CPU. Addresses and
// word values are octal, matching the console. Loading a program this way and
// starting it from the console is far faster than depositing it by hand.
static bool parse_octal(const std::string &s, unsigned *out) {
	if (s.empty())
		return false;
	char *end = nullptr;
	unsigned long v = strtoul(s.c_str(), &end, 8);
	if (*end != '\0')
		return false;
	*out = (unsigned) v;
	return true;
}

// mem_read/mem_write index their buffer by absolute bus address - the word for
// address A is at buffer[A/2] - and a DMA read copies into it from the adapter
// worker thread after DMA() returns. A stale PRU completion (the interrupt
// traffic of an enabled device makes these routine) can run that copy after the
// request is thought done, so the buffer must outlive any single request. One
// persistent, address-space-sized buffer, guarded by its own lock so only one
// bus-master transfer is in flight, is what the demo menu uses and what keeps
// the copy landing in valid memory.
static std::mutex memory_mutex;
static std::vector<uint16_t> &memory_buffer() {
	static std::vector<uint16_t> buf(QUNIBUS_MAX_WORDCOUNT, 0);
	return buf;
}

// What the last probe found, so the map can report the machine's own memory
// without putting a DMA sweep behind every poll of it.
static std::mutex probe_mutex;
static bool probe_valid = false;
static uint32_t probe_first_invalid = 0;
static time_t probe_when = 0;

// GET /api/memory/map — the address space as the board sees it: where the I/O
// page starts, what the board answers out of DDR, and where the machine's own
// memory ends as the last probe found it.
static void memory_map(struct mg_connection *conn) {
	picojson::object res;
	res["addr_width"] = picojson::value((double) qunibus->addr_width);
	res["iopage_start"] = picojson::value((double) qunibus->iopage_start_addr);
	res["addr_space_bytes"] = picojson::value((double) qunibus->addr_space_byte_count);

	// the emulated ranges, by slot: "memory" is the memory card, "device" a
	// window a device serves out of DDR (the VCB01 framebuffer)
	static const char *slot_names[DDRMEM_RANGE_COUNT] = { "memory", "device" };
	picojson::array ranges;
	for (unsigned slot = 0; slot < DDRMEM_RANGE_COUNT; slot++) {
		if (!ddrmem->range_enabled(slot))
			continue;
		picojson::object r;
		r["slot"] = picojson::value(std::string(slot_names[slot]));
		r["start"] = picojson::value((double) ddrmem->range_start(slot));
		r["end"] = picojson::value((double) ddrmem->range_end(slot));
		ranges.push_back(picojson::value(r));
	}
	res["emulated"] = picojson::value(ranges);

	{
		std::lock_guard<std::mutex> lock(probe_mutex);
		if (probe_valid) {
			// the last address the machine's own memory answered; the probe
			// returns the first that did not, and 0 means none did
			res["physical_end"] = probe_first_invalid >= 2 ?
					picojson::value((double) (probe_first_invalid - 2)) :
					picojson::value();
			res["probed_at"] = picojson::value((double) probe_when);
		} else {
			res["physical_end"] = picojson::value();
			res["probed_at"] = picojson::value();
		}
	}
	send_json(conn, 200, picojson::value(res));
}

// POST /api/memory/probe — size the machine's own memory: DATI ascending from 0
// until the bus times out. A sweep of the whole address space, so it is an
// operator action and not something a page poll triggers, and it wants the CPU
// halted: it takes the bus for the length of the sweep.
static void memory_probe(struct mg_connection *conn) {
	uint32_t first_invalid;
	{
		std::lock_guard<std::mutex> ops_lock(device_configuration_c::operations_mutex);
		std::lock_guard<std::mutex> mlock(memory_mutex);
		first_invalid = qunibus->test_sizer();
	}
	{
		std::lock_guard<std::mutex> lock(probe_mutex);
		probe_first_invalid = first_invalid;
		probe_when = time(nullptr);
		probe_valid = true;
	}
	WEB_INFO("memory: probe found memory up to %06o", first_invalid ? first_invalid - 2 : 0);
	picojson::object res;
	res["ok"] = picojson::value(true);
	res["first_invalid"] = picojson::value((double) first_invalid);
	res["physical_end"] = first_invalid >= 2 ?
			picojson::value((double) (first_invalid - 2)) : picojson::value();
	send_json(conn, 200, picojson::value(res));
}

// POST /api/memory/fill {"address":…, "count":…, "value":…} — set a run of
// words in an emulated range. Written into DDR directly: filling megabytes over
// the bus a DMA block at a time would take the machine's bus for the duration,
// and only the board's own memory can be filled this way anyway.
static void memory_fill(struct mg_connection *conn, const picojson::value &req) {
	unsigned address = 0;
	const picojson::value &av = req.get("address");
	if (av.is<double>())
		address = (unsigned) av.get<double>();
	else if (!av.is<std::string>() || !parse_octal(av.get<std::string>(), &address)) {
		send_error(conn, 400, "\"address\" must be a number or octal string");
		return;
	}
	if (address & 1) {
		send_error(conn, 400, "address must be even");
		return;
	}
	if (!req.get("count").is<double>() || req.get("count").get<double>() < 1) {
		send_error(conn, 400, "\"count\" must be a word count of 1 or more");
		return;
	}
	uint64_t count = (uint64_t) req.get("count").get<double>();
	uint16_t value = 0;
	const picojson::value &vv = req.get("value");
	if (vv.is<double>())
		value = (uint16_t) vv.get<double>();
	else if (vv.is<std::string>()) {
		unsigned parsed;
		if (!parse_octal(vv.get<std::string>(), &parsed)) {
			send_error(conn, 400, "\"value\" must be a number or octal string");
			return;
		}
		value = (uint16_t) parsed;
	} else if (!vv.is<picojson::null>()) {
		send_error(conn, 400, "\"value\" must be a number or octal string");
		return;
	}

	uint64_t bytes = count * 2;
	if (address + bytes > 2 * (uint64_t) QUNIBUS_MAX_WORDCOUNT
			|| !ddrmem->contains(address, (unsigned) bytes)) {
		send_error(conn, 409, "the range is not served out of the board's memory");
		return;
	}
	{
		std::lock_guard<std::mutex> mlock(memory_mutex);
		ddrmem->fill_range(address, address + (uint32_t) bytes - 2, value);
	}
	WEB_INFO("memory: filled %u words at %06o with %06o", (unsigned) count, address, value);
	picojson::object res;
	res["ok"] = picojson::value(true);
	res["address"] = picojson::value((double) address);
	res["count"] = picojson::value((double) count);
	send_json(conn, 200, picojson::value(res));
}

static int api_memory_handler(struct mg_connection *conn, void * /*cbdata*/) {
	const struct mg_request_info *ri = mg_get_request_info(conn);
	std::string uri = ri->local_uri ? ri->local_uri : "";
	std::string rest = uri.substr(strlen("/api/memory"));

	if (rest == "/map") {
		if (strcmp(ri->request_method, "GET") != 0) {
			send_error(conn, 405, "GET required");
			return 405;
		}
		memory_map(conn);
		return 200;
	}
	if (rest == "/probe" || rest == "/fill") {
		if (strcmp(ri->request_method, "POST") != 0) {
			send_error(conn, 405, "POST required");
			return 405;
		}
		if (rest == "/probe") {
			memory_probe(conn);
			return 200;
		}
		picojson::value req;
		if (!read_json_body(conn, &req) || !req.is<picojson::object>()) {
			send_error(conn, 400, "body must be a JSON object");
			return 400;
		}
		memory_fill(conn, req);
		return 200;
	}
	if (!rest.empty() && rest != "/") {
		send_error(conn, 404, "unknown path");
		return 404;
	}

	if (strcmp(ri->request_method, "GET") == 0) {
		char buf[64];
		unsigned address = 0, count = 1;
		if (mg_get_var(ri->query_string, ri->query_string ? strlen(ri->query_string) : 0,
				"address", buf, sizeof(buf)) <= 0 || !parse_octal(buf, &address)) {
			send_error(conn, 400, "address=<octal> required");
			return 400;
		}
		if (mg_get_var(ri->query_string, ri->query_string ? strlen(ri->query_string) : 0,
				"count", buf, sizeof(buf)) > 0)
			parse_octal(buf, &count);
		if (count < 1 || count > 4096
				|| (uint64_t) address + 2 * count > 2 * (uint64_t) QUNIBUS_MAX_WORDCOUNT) {
			send_error(conn, 400, "address/count out of range");
			return 400;
		}
		bool timeout = false;
		std::vector<uint16_t> &mem = memory_buffer();
		std::lock_guard<std::mutex> mlock(memory_mutex);
		qunibus->mem_read(mem.data(), address, address + 2 * (count - 1), &timeout);
		if (timeout) {
			send_error(conn, 502, "bus timeout reading memory");
			return 502;
		}
		picojson::array arr;
		for (unsigned i = 0; i < count; i++)
			arr.push_back(picojson::value((double) mem[address / 2 + i]));
		picojson::object res;
		res["address"] = picojson::value((double) address);
		res["words"] = picojson::value(arr);
		send_json(conn, 200, picojson::value(res));
		return 200;
	}

	if (strcmp(ri->request_method, "POST") != 0) {
		send_error(conn, 405, "GET or POST required");
		return 405;
	}

	picojson::value req;
	if (!read_json_body(conn, &req) || !req.is<picojson::object>()) {
		send_error(conn, 400, "body must be a JSON object");
		return 400;
	}
	unsigned address = 0;
	const picojson::value &av = req.get("address");
	if (av.is<double>())
		address = (unsigned) av.get<double>();
	else if (!av.is<std::string>() || !parse_octal(av.get<std::string>(), &address)) {
		send_error(conn, 400, "\"address\" must be a number or octal string");
		return 400;
	}
	if (!req.get("words").is<picojson::array>()) {
		send_error(conn, 400, "\"words\" must be an array");
		return 400;
	}
	const picojson::array &warr = req.get("words").get<picojson::array>();
	unsigned n = (unsigned) warr.size();
	if (n < 1 || n > 4096
			|| (uint64_t) address + 2 * n > 2 * (uint64_t) QUNIBUS_MAX_WORDCOUNT) {
		send_error(conn, 400, "address/word count out of range");
		return 400;
	}
	if (address & 1) {
		send_error(conn, 400, "address must be even");
		return 400;
	}

	bool timeout = false;
	{
		std::vector<uint16_t> &mem = memory_buffer();
		std::lock_guard<std::mutex> mlock(memory_mutex);
		for (unsigned i = 0; i < n; i++) {
			if (!warr[i].is<double>()) {
				send_error(conn, 400, "each word must be a number");
				return 400;
			}
			mem[address / 2 + i] = (uint16_t) warr[i].get<double>();
		}
		qunibus->mem_write(mem.data(), address, address + 2 * (n - 1), &timeout);
	}
	if (timeout) {
		send_error(conn, 502, "bus timeout writing memory");
		return 502;
	}
	WEB_INFO("memory: wrote %u words at %06o", n, address);
	picojson::object res;
	res["ok"] = picojson::value(true);
	res["address"] = picojson::value((double) address);
	res["count"] = picojson::value((double) n);
	send_json(conn, 200, picojson::value(res));
	return 200;
}

// called by webserver_c::start(); the host test build registers fixtures instead
// GET /api/log?before=<id>&limit=<n> — a page of the log journal, newest first
static int api_log_handler(struct mg_connection *conn, void * /*cbdata*/) {
	const struct mg_request_info *ri = mg_get_request_info(conn);
	if (strcmp(ri->request_method, "GET") != 0) {
		send_error(conn, 405, "GET required");
		return 405;
	}
	uint64_t before = 0;
	unsigned limit = 200;
	const char *q = ri->query_string;
	if (q != nullptr) {
		char buf[32];
		if (mg_get_var(q, strlen(q), "before", buf, sizeof buf) > 0)
			before = strtoull(buf, nullptr, 10);
		if (mg_get_var(q, strlen(q), "limit", buf, sizeof buf) > 0)
			limit = (unsigned) strtoul(buf, nullptr, 10);
	}
	std::string body = webevents_log_page_json(before, limit);
	mg_printf(conn,
			"HTTP/1.1 200 OK\r\n"
			"Content-Type: application/json\r\n"
			"Cache-Control: no-store\r\n"
			"Content-Length: %u\r\n\r\n",
			(unsigned) body.size());
	mg_write(conn, body.c_str(), body.size());
	return 200;
}

void webapi_register(struct mg_context *ctx) {
	// first, so the version this instance runs is on record and in
	// /run/qunilator/version before anything else can be asked of it
	webversion_register(ctx);
	mg_set_request_handler(ctx, "/api/devices", api_devices_handler, nullptr);
	mg_set_request_handler(ctx, "/api/control", api_control_handler, nullptr);
	mg_set_request_handler(ctx, "/api/memory", api_memory_handler, nullptr);
	mg_set_request_handler(ctx, "/api/log", api_log_handler, nullptr);
	webstorage_register(ctx);
	webconfigs_register(ctx);
	websettings_register(ctx);
	// after websettings_register: the persisted log levels are loaded, so
	// weblogging_register can apply them to the logger
	weblogging_register(ctx);
	// after websettings_register too: the updater's state directory sits inside
	// the state directory that call resolves, and the dismissed version is read
	// from the same settings file
	webupdate_register(ctx);
	webevents_register(ctx);
	webconsole_register(ctx);
	webconsole_ext_register(ctx);
	webserial_register(ctx);
	webvcb01_register(ctx);
	// apply the persisted external-console setting (loaded by
	// websettings_register) now that the bridge is up
	external_console_c ec = websettings_external_console();
	webconsole_ext_configure(ec.source, ec.port, ec.baud);
}

// called by webserver_c::stop() before the connections close
void webapi_shutdown(void) {
	webvcb01_shutdown();
	webserial_shutdown();
	webconsole_ext_shutdown();
	webconsole_shutdown();
	webevents_shutdown();
}
