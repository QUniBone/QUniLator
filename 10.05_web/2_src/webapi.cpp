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
#include <chrono>
#include <mutex>
#include <thread>

#include "civetweb.h"
#include "picojson.h"

#include "webserver.hpp"

#include "logger.hpp"
#include "device.hpp"
#include "storagedrive.hpp"
#include "storagecontroller.hpp"
#include "timeout.hpp"     // timeout_c, used by rl0102.hpp
#include "rl0102.hpp"
#include "memory.hpp"
#include "parameter.hpp"
#include "qunibus.h"
#include "ddrmem.h"
#include "iopageregister.h"
#include "qunibusadapter.hpp"
#include "panel.hpp"
#include "mscp_server.hpp"
#include "device_configuration.hpp"
#include "device_label.hpp"
#include "device_status.hpp"
#include "webcontrol.hpp"
#include "webpower.hpp"
#include "webbus.hpp"
#include "webdebug.hpp"

#include "weblog.hpp"
#include "webevents.hpp"
#include "webconsole.hpp"
#include "webserial.hpp"
#include "webconsole_ext.hpp"
#include "webrecordings.hpp"
#include "webvcb01.hpp"
#include "webstorage.hpp"
#include "webconfigs.hpp"
#include "websettings.hpp"
#include "weblogging.hpp"
#include "webversion.hpp"
#include "webupdate.hpp"
#include "webselftest.hpp"
#include "webserialports.hpp"
#include "websystem.hpp"

void web_send_json(struct mg_connection *conn, int status, const picojson::value &val) {
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
	web_send_json(conn, status, picojson::value(err));
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
static picojson::value param_to_json(device_c *dev, parameter_c *p) {
	picojson::object o;
	o["name"] = picojson::value(p->name);
	o["shortname"] = picojson::value(p->shortname);
	o["readonly"] = picojson::value(p->readonly);
	if (!p->info.empty())
		o["info"] = picojson::value(p->info);
	if (!p->unit.empty())
		o["unit"] = picojson::value(p->unit);
	// What the value names, when it is not an ordinary one: a file of the image
	// tree, and which kind belongs in it. The interface offers the file browser
	// for these rather than a text box, and does so because the device said so
	// — not because of the name the parameter happens to carry.
	if (p->content == parameter_c::CONTENT_IMAGE)
		o["content"] = picojson::value("image");
	else if (p->content == parameter_c::CONTENT_ROM)
		o["content"] = picojson::value("rom");

	if (parameter_string_c *ps = dynamic_cast<parameter_string_c *>(p)) {
		o["type"] = picojson::value("string");
		// the medium a switched-off drive holds is the one it comes back with
		o["value"] = picojson::value(webpower_param_value(dev, ps));
	} else if (parameter_bool_c *pb = dynamic_cast<parameter_bool_c *>(p)) {
		o["type"] = picojson::value("bool");
		// a card the power-down took out is still in the machine
		o["value"] = picojson::value(dev != nullptr && p == &dev->enabled
				? webpower_is_in_machine(dev) : pb->value);
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

// Take the medium out of a drive that is leaving the machine. A drive switched
// off answers nothing on the bus, so the file it named is held by nobody: the
// media manager stops counting it as in use, and it can be deleted and renamed
// again. Returns whether a medium was actually released.
static bool release_medium(storagedrive_c *drv) {
	if (drv == nullptr || drv->image_filepath.value.empty())
		return false;
	WEB_INFO("%s released %s", drv->name.value.c_str(),
			drv->image_filepath.value.c_str());
	drv->image_filepath.set("");
	return true;
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
				// computed verbal status, shared with the dashboard and MCP. It
				// reads the live device, so a drive with no power reads "off"
				// while the card it sits behind is still in the machine.
				o["status"] = picojson::value(device_status_for(d));
			}
			// A card the power-down took out is still in the machine: losing the
			// supply does not unplug it, and power-up puts it back.
			o["enabled"] = picojson::value(webpower_is_in_machine(d));
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
					statusparams.push_back(param_to_json(d, p));
				else
					params.push_back(param_to_json(d, p));
			}
			o["params"] = picojson::value(params);
			o["statusparams"] = picojson::value(statusparams);
			devices.push_back(picojson::value(o));
		}
	}
	web_send_json(conn, 200, picojson::value(devices));
}

// The device's bus placement — where it sits in the I/O page and how it
// interrupts. Locked while the device is on the bus (readonly), so changing one
// re-registers the device, which is only safe with the CPU halted.
static bool is_bus_placement_param(const std::string &n) {
	return n == "base_addr" || n == "intr_vector" || n == "intr_level" || n == "slot";
}

// The device in the machine whose register window overlaps [addr, addr +
// 2*count), or "" when the range is free. register_device() aborts the emulator
// on an I/O page collision, so a re-address is checked against this first — the
// cards of a machine with its power off among them, since those are the cards
// the power-up will install.
// Caller holds device_c::mydevices_mutex.
static std::string address_range_owner(qunibusdevice_c *self, uint32_t addr,
		unsigned count) {
	uint32_t end = addr + 2 * count;
	for (device_c *d : device_c::mydevices) {
		if (d == self || device_is_infrastructure(d))
			continue;
		qunibusdevice_c *qd = dynamic_cast<qunibusdevice_c *>(d);
		if (qd == nullptr || !webpower_is_in_machine(qd))
			continue;
		uint32_t o = qd->base_addr.value, oend = o + 2 * qd->register_count;
		if (addr < oend && o < end)
			return qd->name.value;
	}
	return "";
}

// Publish what a device now carries: whether it is in the machine and the
// medium it holds. A controller takes its drives with it, so they are published
// with it. Caller holds operations_mutex.
static void notify_carried(device_c *dev) {
	std::vector<device_c *> devs;
	devs.push_back(dev);
	{
		std::lock_guard<std::mutex> lock(device_c::mydevices_mutex);
		for (device_c *d : device_c::mydevices)
			if (d->parent == dev)
				devs.push_back(d);
	}
	for (device_c *d : devs) {
		webevents_note_param(d->name.value, d->enabled.name,
				picojson::value(webpower_is_in_machine(d)));
		for (parameter_c *img : d->parameter)
			if (img->content == parameter_c::CONTENT_IMAGE)
				webevents_note_param(d->name.value, img->name,
						picojson::value(webpower_param_value(d, img)));
	}
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
			// A machine with its power off is edited as the machine it is:
			// which cards it carries and what is in its drives are held for the
			// power-up that configures it from them, and nothing an operator
			// changes reaches the emulation until then. The parameters beneath
			// travel with the card and are set where they stand — a device out
			// of the machine is on no bus, so a value set on it is inert until
			// the card goes in.
			if (webpower_devices_are_off()
					&& (param == &dev->enabled
							|| param->content == parameter_c::CONTENT_IMAGE)) {
				std::string err;
				bool ok;
				if (param == &dev->enabled) {
					bool on;
					if (value == "1" || !strcasecmp(value.c_str(), "true"))
						on = true;
					else if (value == "0" || !strcasecmp(value.c_str(), "false"))
						on = false;
					else {
						send_error(conn, 400, "\"enabled\" must be 1/0 or true/false");
						return;
					}
					ok = webpower_set_in_machine(dev, on, &err);
				} else {
					// the web interface keeps images in one directory, so a bare
					// name names the file it manages by that name
					value = webstorage_image_path(value);
					std::string other = webpower_image_held_by(value, dev->name.value);
					if (other.empty())
						other = webstorage_image_held_by(value, dev->name.value);
					if (!other.empty()) {
						send_error(conn, 409, "that image is mounted on " + other);
						return;
					}
					ok = webpower_set_image(dev, value, &err);
				}
				if (!ok) {
					send_error(conn, 409, err);
					return;
				}
				WEB_INFO("%s.%s = %s, held until the machine is switched on",
						dev->name.value.c_str(), param->name.c_str(), value.c_str());
				// The device parameters publish themselves as they are set; an
				// edit to a dark machine sets none, so it is published here —
				// for the card, and for the drives a controller takes with it.
				notify_carried(dev);
				web_send_json(conn, 200, param_to_json(dev, param));
				return;
			}
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
				// A device leaving the machine takes the media with it, the way
				// pulling a drive takes its pack out. A drive switched off gives
				// up what it held, and so does every drive of a controller
				// switched off — those go with their controller, since a drive
				// on a removed controller cannot function.
				storagecontroller_c *ctrl = dynamic_cast<storagecontroller_c *>(dev);
				bool released = false;
				if (!on) {
					released = release_medium(dynamic_cast<storagedrive_c *>(dev));
					if (ctrl != nullptr)
						for (storagedrive_c *drv : ctrl->storagedrives)
							if (drv != nullptr)
								released |= release_medium(drv);
				}
				// A device that cannot take the switch leaves it where it was:
				// a card whose range the machine already answers is not
				// installed. Report that with the reason it logged, so the
				// operator is not told the device is in the machine when the
				// machine refused it.
				dev->last_error.clear();
				dev->enabled.set(on);
				if (dev->enabled.value != on) {
					std::string why = dev->last_error;
					send_error(conn, 409, "device \"" + devname + "\" could not be "
							+ (on ? "installed" : "removed")
							+ (why.empty() ? std::string() : ": " + why));
					return;
				}
				if (!on && ctrl != nullptr)
					for (storagedrive_c *drv : ctrl->storagedrives)
						if (drv != nullptr && drv->enabled.value) {
							drv->enabled.set(false);
							WEB_INFO("%s disabled with controller %s",
									drv->name.value.c_str(), dev->name.value.c_str());
						}
				// the shares hold an attached image read-only while the machine
				// runs, and what was released is attached no longer
				if (released)
					webstorage_refresh_readonly(webevents_is_powered()
							&& !webevents_is_halted());
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
				// A card of a machine with its power off takes a new placement
				// where it stands, being on no bus to be unplugged from. Two
				// cards at one address would still meet at power-up, so the
				// window is checked as it is on a running machine.
				if (param->name == "base_addr" && webpower_is_in_machine(dev)) {
					qunibusdevice_c *qd = dynamic_cast<qunibusdevice_c *>(dev);
					uint32_t newaddr = (uint32_t) strtoul(value.c_str(), nullptr, 8);
					std::string owner = qd == nullptr ? std::string()
							: address_range_owner(qd, newaddr, qd->register_count);
					if (!owner.empty()) {
						send_error(conn, 409, "address " + value
								+ " overlaps device \"" + owner + "\"");
						return;
					}
				}
				if (param->content == parameter_c::CONTENT_IMAGE) {
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
						web_send_json(conn, 200, param_to_json(dev, param));
						return;
					}
				} else if (param->content == parameter_c::CONTENT_ROM) {
					// a ROM is a file of the same tree, named by its subpath.
					// Several sockets may be programmed from one file, so
					// nothing holds it. An absolute path outside the tree is
					// left as it stands, which is what keeps a configuration
					// naming a packaged listing working.
					value = webstorage_image_path(value);
				}
				// A device may refuse a value — a memory card whose new range
				// the machine already answers keeps the range it had. The set
				// leaves the old value in place, so the answer is the reason
				// the device logged rather than a 200 quoting a value that
				// never took. A device that normalizes what it stores writes
				// the value it settled on and logs nothing, and that stands.
				dev->last_error.clear();
				param->parse(value);
				if (*param->render() != value && !dev->last_error.empty()) {
					send_error(conn, 409, dev->name.value + "." + param->name
							+ " could not be set to \"" + value + "\": "
							+ dev->last_error);
					return;
				}
			}
			// keep the terminal user informed, like an echoed command
			WEB_INFO("%s.%s = %s", dev->name.value.c_str(),
					param->name.c_str(), value.c_str());
			// attaching/detaching an image changes which files the shares must
			// hold read-only while the machine runs
			if (param->content == parameter_c::CONTENT_IMAGE)
				webstorage_refresh_readonly(webevents_is_powered()
						&& !webevents_is_halted());
		} catch (bad_parameter &e) {
			WEB_INFO("%s.%s = %s rejected: %s", dev->name.value.c_str(),
					param->name.c_str(), value.c_str(), e.what());
			send_error(conn, 422, e.what());
			return;
		}
	}
	web_send_json(conn, 200, param_to_json(dev, param));
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

/* The cadence of the control actions: when the last one finished, and what it
 * was. control_await_settle() reads it before a power sequence begins and
 * control_note_timing() writes it once one has ended.
 */
static std::mutex control_timing_mutex;
static std::chrono::steady_clock::time_point control_last_end;
static std::string control_last_action;
static bool control_have_last = false;

// How long the bus is given to finish the previous power sequence. The DCLO
// and ACLO edges drive real cards, whose power-up behaviour outlasts the
// handler that asked for it, so the interval belongs to the backplane rather
// than to the request.
static const long CONTROL_SETTLE_MS = 2000;

// The actions that drive the bus edges, as against reading state or moving the
// run/halt lines.
static bool control_is_disruptive(const std::string &action) {
	return action == "powercycle" || action == "dc_on"
			|| action == "dc_off" || action == "restart";
}

/* Hold a power sequence off until the previous one has settled.
 *
 * The handler already serializes these: it holds operations_mutex for the
 * whole sequence, so one cannot begin while another is mid-flight. What that
 * does not bound is how soon the next may begin once the previous returns, and
 * a machine still coming up when the next DCLO/ACLO arrives is driven through
 * a power sequence it never finished - which is what the KDJ11 failing its own
 * self-test in #49 looks like.
 *
 * So the interval is waited out rather than merely reported. A wait is logged
 * with what it cost, which is also the record of a crowded cadence that the
 * warning here used to carry: if the wedge recurs, the journal says whether
 * the cycles were close enough to matter.
 */
static void control_await_settle(const std::string &action) {
	if (!control_is_disruptive(action))
		return;
	long wait_ms = 0;
	std::string after;
	{
		std::lock_guard<std::mutex> lock(control_timing_mutex);
		if (!control_have_last)
			return;
		long gap_ms = (long) std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - control_last_end).count();
		if (gap_ms >= CONTROL_SETTLE_MS)
			return;
		wait_ms = CONTROL_SETTLE_MS - gap_ms;
		after = control_last_action;
	}
	WEB_INFO("control %s waits %ld ms for the bus to settle after %s",
			action.c_str(), wait_ms, after.c_str());
	std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
}

// What a control action cost and how soon it followed the last one.
static void control_note_timing(const std::string &action) {
	std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
	std::lock_guard<std::mutex> lock(control_timing_mutex);
	if (control_have_last) {
		long gap_ms = (long) std::chrono::duration_cast<std::chrono::milliseconds>(
				now - control_last_end).count();
		WEB_INFO("control %s, %ld ms after %s", action.c_str(), gap_ms,
				control_last_action.c_str());
	} else {
		WEB_INFO("control %s", action.c_str());
	}
	control_last_end = std::chrono::steady_clock::now();
	control_last_action = action;
	control_have_last = true;
}

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

	// A card the machine will not take stops the power-up where it stands: the
	// bus edges are not driven, the machine is left dark and the answer names
	// the card and the reason it gave.
	bool powered_up = true;
	std::string power_up_error;

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
		web_send_json(conn, 200, picojson::value(res));
		return 200;
	}

	// The bus edges below drive real cards. Give the previous power sequence
	// time to finish before starting another, outside operations_mutex so the
	// wait blocks this request rather than every device operation.
	control_await_settle(action);

	{
		// Bringing the cards back checks each one against the machine before it
		// is put in - a memory card against the addresses the CPU answers
		// itself, a device against what already answers its registers - and the
		// probing that takes runs on the bus. The interfaces are held for the
		// length of it: a page must not act on a machine that is half assembled,
		// and every page connected says the same thing about why.
		board_hold_c hold("validating configuration for power on", dec.devices_on);

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
#if defined(QBUS)
		// Stop the CPU before the cards come out, so no guest transfer is in
		// flight while a device is torn down and the bus can still complete the
		// cycles already started.
		if (dec.do_halt) {
			qunibus->set_halt(1);
			webevents_note_halt(true);
		}
#endif
		// The cards are rebuilt before the CPU is given its power-up edge, so a
		// machine coming up finds the whole configuration present.
		if (dec.devices_off)
			webpower_devices_off();
		if (dec.devices_on && !webpower_devices_on(&power_up_error)) {
			// A card the machine will not take leaves it standing dark: the CPU
			// keeps the HALT line it was stopped with, the power flag reads off,
			// and the configuration is there to be changed before the operator
			// tries again.
#if defined(QBUS)
			qunibus->set_halt(1);
#endif
			webevents_note_halt(true);
			webevents_note_powered(false);
			powered_up = false;
		}
		if (powered_up && dec.do_init)
			qunibus->init();
		if (powered_up && dec.do_powercycle)
			qunibus->powercycle();
#if defined(UNIBUS)
		if (powered_up && ecpu != nullptr && dec.set_powered == 1) {
			// the START switch rebuilds the machine and runs it, which is what
			// the power switch means on a processor that is emulated
			ecpu->panel_halt_switch()->set(false);
			ecpu->panel_start_switch()->set(true);
			webevents_note_halt(false);
			WEB_INFO("control %s: emulated CPU started", action.c_str());
		}
#endif
		if (powered_up && dec.set_powered >= 0)
			webevents_note_powered(dec.set_powered != 0);
		control_note_timing(action);
	}
	// The shares hold an attached image read-only while the machine runs, and a
	// power cycle changes what is attached: a machine switched off holds no
	// medium, so the files it had open are the operator's again.
	if (dec.devices_off || dec.devices_on)
		webstorage_refresh_readonly(webevents_is_powered() && !webevents_is_halted());
	if (!powered_up) {
		send_error(conn, 409, power_up_error);
		return 409;
	}
	picojson::object res;
	res["ok"] = picojson::value(true);
	web_send_json(conn, 200, picojson::value(res));
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
// persistent, address-space-sized buffer, guarded by web_bus_mutex() so only
// one bus-master transfer is in flight, is what the demo menu uses and what
// keeps the copy landing in valid memory. That lock is shared with every other
// handler that reaches the bus, /api/debug/cpu among them - see webbus.hpp.
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
	// where the CPU module's own memory begins, and so the address a card has
	// to stay below; null when the whole space up to the I/O page is free
	res["cpu_reserved_start"] = qunibus->cpu_reserved_start ?
			picojson::value((double) qunibus->cpu_reserved_start) : picojson::value();
	res["memory_limit"] = picojson::value((double) qunibus->memory_limit_addr());

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
		// How many cycles the board has answered out of this range, reads and
		// writes apart. The PRU serves them without telling the ARM, so these
		// counts are the only evidence a range is being used at all - which is
		// what an operator looking at a card placed over the wrong addresses
		// needs to see.
		if (pru_iopage_registers != nullptr) {
			r["reads"] = picojson::value(
					(double) pru_iopage_registers->memory_read_count[slot]);
			r["writes"] = picojson::value(
					(double) pru_iopage_registers->memory_write_count[slot]);
		}
		ranges.push_back(picojson::value(r));
	}
	res["emulated"] = picojson::value(ranges);
	if (pru_iopage_registers != nullptr)
		res["rom_accesses"] = picojson::value(
				(double) pru_iopage_registers->rom_access_count);

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
	web_send_json(conn, 200, picojson::value(res));
}

// POST /api/memory/probe — size the machine's own memory: DATI ascending from 0
// until the bus times out. A sweep of the whole address space, so it is an
// operator action and not something a page poll triggers, and it wants the CPU
// halted: it takes the bus for the length of the sweep.
static void memory_probe(struct mg_connection *conn) {
	uint32_t first_invalid;
	bool no_grant = false;
	{
		std::lock_guard<std::mutex> ops_lock(device_configuration_c::operations_mutex);
		std::lock_guard<std::mutex> mlock(web_bus_mutex());
		first_invalid = qunibus->test_sizer(&no_grant);
		// Nothing on the backplane granted the board the bus, so the sweep says
		// nothing about what the machine carries. Reported rather than recorded:
		// a machine that is switched off would otherwise be filed as one with no
		// memory, over the last probe that did reach the bus.
		if (no_grant) {
			send_error(conn, 504, "QUniLator asked for the bus and was not granted it: "
					"nothing on this backplane is arbitrating. A machine that is "
					"switched off grants nothing.");
			return;
		}

		// The board answers its own ranges, and a sweep cannot tell those from
		// memory the machine carries: with a card placed above the machine's own
		// memory the two run together and the probe reports the sum, which reads
		// as a machine that already fills the space and leaves nowhere to put a
		// card. What the board serves is known, so the answer stops below it and
		// reports the machine's own.
		for (unsigned slot = 0; slot < DDRMEM_RANGE_COUNT; slot++)
			if (ddrmem->range_enabled(slot) && ddrmem->range_start(slot) < first_invalid)
				first_invalid = ddrmem->range_start(slot);
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
	web_send_json(conn, 200, picojson::value(res));
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
		send_error(conn, 409, "the range is not served out of QUniLator's memory");
		return;
	}
	{
		std::lock_guard<std::mutex> mlock(web_bus_mutex());
		ddrmem->fill_range(address, address + (uint32_t) bytes - 2, value);
	}
	WEB_INFO("memory: filled %u words at %06o with %06o", (unsigned) count, address, value);
	picojson::object res;
	res["ok"] = picojson::value(true);
	res["address"] = picojson::value((double) address);
	res["count"] = picojson::value((double) count);
	web_send_json(conn, 200, picojson::value(res));
}

// The machine's memory card, or nullptr on a build that carries none.
// Caller holds device_c::mydevices_mutex.
static memory_c *memory_card_locked(void) {
	for (device_c *d : device_c::mydevices) {
		memory_c *mem = dynamic_cast<memory_c *>(d);
		if (mem != nullptr)
			return mem;
	}
	return nullptr;
}

// What the card answers, as the interfaces read it back.
static picojson::value memory_placement_json(memory_c *mem) {
	picojson::object res;
	res["ok"] = picojson::value(true);
	res["startaddr"] = picojson::value((double) mem->startaddr.value);
	res["endaddr"] = picojson::value((double) mem->endaddr.value);
	res["size"] = picojson::value(mem->size.value);
	res["enabled"] = picojson::value(mem->enabled.value);
	return picojson::value(res);
}

// POST /api/memory/place {"startaddr":…, "size":…} — place the card.
//
// A start address and a size describe one range, and a card set one parameter
// at a time passes through the placements between the old range and the new
// one: moving the start of a card that fills the machine runs it into the I/O
// page, which the card refuses. So the two arrive together and the card is
// placed once, which is also how a configuration file applies them.
//
// A card in the machine gives up its range and takes the new one, so this is
// the operator re-strapping it where it stands; a range the machine already
// answers is refused and the card stays where it was.
static void memory_place(struct mg_connection *conn, const picojson::value &req) {
	unsigned start = 0;
	const picojson::value &sv = req.get("startaddr");
	if (sv.is<double>())
		start = (unsigned) sv.get<double>();
	else if (!sv.is<std::string>() || !parse_octal(sv.get<std::string>(), &start)) {
		send_error(conn, 400, "\"startaddr\" must be a number or octal string");
		return;
	}
	if (!req.get("size").is<std::string>()) {
		send_error(conn, 400,
				"\"size\" must be text: a count of bytes, or one followed by KB or MB");
		return;
	}
	std::string sizespec = req.get("size").get<std::string>();

	std::lock_guard<std::mutex> ops_lock(device_configuration_c::operations_mutex);
	memory_c *mem;
	{
		std::lock_guard<std::mutex> lock(device_c::mydevices_mutex);
		mem = memory_card_locked();
	}
	if (mem == nullptr) {
		send_error(conn, 404, "this machine carries no memory card");
		return;
	}
	mem->last_error.clear();
	if (!mem->place_at(start, sizespec)) {
		std::string why = mem->last_error;
		send_error(conn, 409, mem->name.value + ": " + sizespec + " at "
				+ qunibus->addr2text(start)
				+ (why.empty() ? " is not a placement the machine takes" : ": " + why));
		return;
	}
	WEB_INFO("memory: %s at %s..%s", mem->size.value.c_str(),
			qunibus->addr2text(mem->startaddr.value),
			qunibus->addr2text(mem->endaddr.value));
	web_send_json(conn, 200, memory_placement_json(mem));
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
	if (rest == "/probe" || rest == "/fill" || rest == "/place") {
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
		if (rest == "/place")
			memory_place(conn, req);
		else
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
		// The machine's own space, not the largest one the board could drive.
		// A transfer running past the end of it is a programming error to the
		// bus adapter, which asserts and takes the whole emulator down with it:
		// 64 words from 777776 is past the top of an 18-bit machine, and that
		// is one keystroke away in the debug panel's dump.
		uint32_t space = qunibus->addr_space_byte_count;
		if (count < 1 || count > 4096 || address >= space) {
			char msg[128];
			snprintf(msg, sizeof msg,
					"address/count out of range: the machine's address space is %u bit, "
					"ending at %06o", qunibus->addr_width, space - 2);
			send_error(conn, 400, msg);
			return 400;
		}
		// A reader asking for a screenful at the top of the space gets the
		// words that are there rather than an error: what it wanted to know is
		// what those addresses hold.
		unsigned words_left = (space - address) / 2;
		if (count > words_left)
			count = words_left;
		std::vector<uint16_t> &mem = memory_buffer();
		uint16_t *words = mem.data() + address / 2;
		std::vector<bool> answered(count, true);
		std::lock_guard<std::mutex> mlock(web_bus_mutex());

		// One transfer for the whole run, which is what the PRU is good at and
		// what a range backed by memory costs. A timeout is not an error here:
		// the range may cross the end of what a card answers, or be the I/O
		// page where most addresses belong to nobody. So the adapter is told
		// the timeout is expected - it neither logs it nor counts it against
		// the machine - and the read falls back to one cycle per word to find
		// out exactly which addresses answered.
		qunibus->dma_request->timeout_expected = true;
		timeout_c waited;
		waited.start_ns(0);
		bool whole_run = qunibus->dma(true, QUNIBUS_CYCLE_DATI, address, words, count,
				/*share_bus*/true, web_bus_timeout_ms);
		qunibus->dma_request->timeout_expected = false;

		if (!whole_run) {
			// A cycle that was never granted takes the whole wait; a slave that
			// did not answer takes microseconds. Only the first is worth
			// refusing over - and walking a hundred words that each wait out
			// the bus would take a minute of them.
			if (waited.elapsed_ms() >= web_bus_timeout_ms) {
				send_error(conn, 504, "QUniLator asked for the bus and was not granted it: "
						"nothing on this backplane is arbitrating. A machine that is "
						"switched off grants nothing.");
				return 504;
			}
			for (unsigned i = 0; i < count; i++) {
				timeout_c cycle;
				cycle.start_ns(0);
				answered[i] = qunibus->probe_word(address + 2 * i, &words[i],
						/*share_bus*/true, web_bus_probe_timeout_ms);
				if (!answered[i] && cycle.elapsed_ms() >= web_bus_probe_timeout_ms) {
					send_error(conn, 504, "QUniLator asked for the bus and was not granted "
							"it: nothing on this backplane is arbitrating. A machine that "
							"is switched off grants nothing.");
					return 504;
				}
			}
		}
		picojson::array arr;
		for (unsigned i = 0; i < count; i++)
			arr.push_back(answered[i] ? picojson::value((double) words[i]) : picojson::value());
		picojson::object res;
		res["address"] = picojson::value((double) address);
		res["words"] = picojson::value(arr);
		web_send_json(conn, 200, picojson::value(res));
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
	// against the machine's own space, as on the read - past its end the bus
	// adapter asserts and the emulator dies. A write is refused rather than
	// shortened: half of what the caller sent is not what it asked for.
	if (n < 1 || n > 4096
			|| (uint64_t) address + 2 * n > (uint64_t) qunibus->addr_space_byte_count) {
		char msg[128];
		snprintf(msg, sizeof msg,
				"address/word count out of range: the machine's address space is %u bit, "
				"ending at %06o", qunibus->addr_width, qunibus->addr_space_byte_count - 2);
		send_error(conn, 400, msg);
		return 400;
	}
	if (address & 1) {
		send_error(conn, 400, "address must be even");
		return 400;
	}

	bool timeout = false;
	timeout_c waited;
	{
		std::vector<uint16_t> &mem = memory_buffer();
		std::lock_guard<std::mutex> mlock(web_bus_mutex());
		for (unsigned i = 0; i < n; i++) {
			if (!warr[i].is<double>()) {
				send_error(conn, 400, "each word must be a number");
				return 400;
			}
			mem[address / 2 + i] = (uint16_t) warr[i].get<double>();
		}
		waited.start_ns(0);
		qunibus->mem_write(mem.data(), address, address + 2 * (n - 1), &timeout,
				web_bus_timeout_ms);
	}
	if (timeout) {
		// as on the read: a wait run out to the end is a bus never granted
		if (waited.elapsed_ms() >= web_bus_timeout_ms) {
			send_error(conn, 504, "QUniLator asked for the bus and was not granted it: "
					"nothing on this backplane is arbitrating. A machine that is "
					"switched off grants nothing.");
			return 504;
		}
		send_error(conn, 502, "bus timeout writing memory");
		return 502;
	}
	WEB_INFO("memory: wrote %u words at %06o", n, address);
	picojson::object res;
	res["ok"] = picojson::value(true);
	res["address"] = picojson::value((double) address);
	res["count"] = picojson::value((double) n);
	web_send_json(conn, 200, picojson::value(res));
	return 200;
}

// called by webserver_c::start(); the host test build registers fixtures instead
// GET /api/log?before=<id>&limit=<n> — a page of the log journal, newest first
// GET  /api/notice           the standing notice, or null
// POST /api/notice/dismiss   clear it
//
// The notice is what the board did on its own and no request of the operator's
// would show them. Dismissing it is the acknowledgement that somebody read it,
// which is the only record that the warning reached a person.
static int api_notice_handler(struct mg_connection *conn, void * /*cbdata*/) {
	const struct mg_request_info *ri = mg_get_request_info(conn);
	std::string uri = ri->local_uri ? ri->local_uri : "";
	std::string rest = uri.substr(strlen("/api/notice"));
	std::string method = ri->request_method;

	if (rest == "/dismiss" && method == "POST") {
		std::string had = webevents_notice();
		webevents_note_notice("");
		if (!had.empty())
			WEB_INFO("notice dismissed: %s", had.c_str());
	} else if (!(rest.empty() || rest == "/") || method != "GET") {
		send_error(conn, 405, "GET /api/notice or POST /api/notice/dismiss");
		return 405;
	}
	picojson::object o;
	std::string text = webevents_notice();
	o["notice"] = text.empty() ? picojson::value() : picojson::value(text);
	web_send_json(conn, 200, picojson::value(o));
	return 200;
}

// GET /api/metrics — what each device is doing, as rates over the last second.
//
// The same set the `metrics` event carries on /ws/events, for a caller that has
// no socket open. It reports what the 1 Hz poll last measured rather than
// sampling on the spot: a rate exists only between two samples, and a request
// arriving a millisecond after the last one has no interval to measure.
static int api_metrics_handler(struct mg_connection *conn, void * /*cbdata*/) {
	const struct mg_request_info *ri = mg_get_request_info(conn);
	if (strcmp(ri->request_method, "GET") != 0) {
		send_error(conn, 405, "GET required");
		return 405;
	}
	std::string body = webevents_metrics_json();
	mg_printf(conn,
			"HTTP/1.1 200 OK\r\n"
			"Content-Type: application/json\r\n"
			"Cache-Control: no-store\r\n"
			"Content-Length: %u\r\n\r\n",
			(unsigned) body.size());
	mg_write(conn, body.c_str(), body.size());
	return 200;
}

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
	// what the processor holds, for the debug panel
	webdebug_register(ctx);
	mg_set_request_handler(ctx, "/api/log", api_log_handler, nullptr);
	mg_set_request_handler(ctx, "/api/notice", api_notice_handler, nullptr);
	mg_set_request_handler(ctx, "/api/metrics", api_metrics_handler, nullptr);
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
	// the board's own name and the operator's ssh key
	websystem_register(ctx);
	// which UART carries the Linux login, and which is left for the emulator
	webserialports_register(ctx);
	// the hardware self-tests, run in the cli as a child of the service
	webselftest_register(ctx);
	webevents_register(ctx);
	webconsole_register(ctx);
	webconsole_ext_register(ctx);
	// after both console backends: the recordings API reaches their recorders
	webrecordings_register(ctx);
	webserial_register(ctx);
	webvcb01_register(ctx);
	// apply the persisted external-console setting (loaded by
	// websettings_register) now that the bridge is up
	external_console_c ec = websettings_external_console();
	webconsole_ext_configure(ec.source, ec.port, ec.baud);
}

// called by webserver_c::stop() before the connections close
void webapi_shutdown(void) {
	// first: a running test child holds the board claim, and its exit has to be
	// seen before the claim socket goes down with the server
	webselftest_shutdown();
	webvcb01_shutdown();
	webserial_shutdown();
	webconsole_ext_shutdown();
	webconsole_shutdown();
	webevents_shutdown();
}
