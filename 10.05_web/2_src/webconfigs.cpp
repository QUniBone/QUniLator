/* webconfigs.cpp: /api/configs — named device-setup snapshots

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.

   A configuration is a JSON snapshot of the emulated device setup, taken from
   and applied to the live parameter system — the same calls the devices menu
   and the REST parameter endpoint make.

   It describes the whole machine while naming as little as possible: the
   devices that are switched on, and of those only the parameters that differ
   from the values the devices were constructed with. Applying one therefore
   switches off every device it does not name and returns every parameter it
   does not name to that construction default.

   The running machine always represents one named configuration, the
   *current* one: a runtime pointer set at startup and updated whenever a
   configuration is applied or the live setup is saved under a name. The
   machine is *modified* when the live device set differs from the saved form
   of the current configuration; this is computed by comparison, not tracked at
   write time.

   The board's 4 DIP switches choose the configuration once, when the backend
   starts: the one whose stored *dip_value* (0..15) matches the switches is
   applied. When no configuration claims that value the bundled empty
   configuration is applied, leaving the machine passive on the bus. A power
   cycle keeps the configuration that is loaded, so switching machines means
   changing the switches and restarting the backend.

     GET    /api/configs               {current, modified, configs[]}
     GET    /api/configs?current=1     the live setup in snapshot form, for
                                       comparison against the saved ones;
                                       503 while the machine is busy
     GET    /api/configs/<name>        full snapshot content
     PUT    /api/configs/<name>        write a config document {"devices":[…]}
                                       to the file, validated against the known
                                       devices/params. ?from=live marks the body
                                       the live setup being saved: <name> becomes
                                       the current configuration and the modified
                                       state clears. Without the flag it is an
                                       offline edit of a stored file: the file is
                                       written, the current pointer and the live
                                       machine untouched.
     POST   /api/configs/<name>/apply  restore a snapshot (best effort,
                                       returns the rejections); sets current
     POST   /api/configs/<name>/rename {"name":"<new>"} rename the file; the
                                       current/default pointers follow
     PUT    /api/configs/<name>/default   designate <name> the startup default
     DELETE /api/configs/<name>        remove a snapshot; refused (409) for the
                                       current or the default configuration
     PUT    /api/configs/<name>/devices/<device>/image   {"value": "<image>"}
                                       the medium that drive starts with

   Files live in $QUNILATOR_DIR/configs/<name>.json. Besides the devices, a file
   may carry an operator "title" and a "dip_value" (the DIP setting that selects
   it at power-on); both are optional metadata, preserved across a live-save:

     {"title":"RT-11 bench","dip_value":3,
      "devices":[{"name":"RL11","enabled":true,
                  "params":{"address":"160010", ...}}, ...]}
*/

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <fstream>
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>

#include "civetweb.h"
#include "picojson.h"

#include "logger.hpp"
#include "device.hpp"
#include "parameter.hpp"
#include "qunibusadapter.hpp"
#include "qunibusdevice.hpp"
#include "panel.hpp"
#include "mscp_server.hpp"
#include "device_configuration.hpp"

#include "weblog.hpp"
#include "webconfigs.hpp"
#include "webpower.hpp"
#include "webstorage.hpp"
#include "websettings.hpp"
#include "weblogging.hpp"
#include "webevents.hpp"

static std::string configs_dir;

// The running machine represents this saved configuration. A runtime pointer,
// re-established from the default at every startup; never persisted here (the
// default lives in settings.json). Guarded, with the cached modified flag, by
// a small dedicated mutex so a status poll never contends with an apply.
static std::mutex current_mutex;
static std::string current_config_name;
static bool cached_modified = false;

// the bundled empty configuration, adopted as the default on first run
static const char *fallback_config_name = "default";

static bool valid_config_name(const std::string &name) {
	if (name.empty() || name.size() > 64)
		return false;
	for (char c : name)
		if (!isalnum(c) && c != '-' && c != '_' && c != '.' && c != ' ')
			return false;
	return name[0] != '.';
}

static std::string config_path(const std::string &name) {
	return configs_dir + "/" + name + ".json";
}

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

// infrastructure: part of the bridge or of a controller's implementation, not
// of the emulated configuration. mscp_server_base covers both protocol
// engines, the MSCP disk server and the TMSCP tape server; each is a device_c
// only so the logging macros work, and each is permanently enabled.
static bool device_is_infrastructure(device_c *d) {
	return dynamic_cast<qunibusadapter_c *>(d) != nullptr
			|| dynamic_cast<paneldriver_c *>(d) != nullptr
			|| dynamic_cast<mscp_server_base *>(d) != nullptr;
}

// Parameter values as the devices were constructed. Captured once at
// registration, which application.cpp reaches directly after devices_startup()
// and before anything can have changed them, so this is what "default" means.
// A parameter absent from the map is always written.
//
// Writability is captured with it. A drive locks its image parameters while a
// pack spins, and that lock would otherwise drop the mounted image out of the
// configuration that needs it. What the device was built with says whether an
// operator may set it; what it reads now only says whether this moment suits.
struct param_default_t {
	std::string value;
	bool writable;
};
static std::map<parameter_c *, param_default_t> parameter_defaults;

static void capture_parameter_defaults(void) {
	std::lock_guard<std::mutex> lock(device_c::mydevices_mutex);
	for (device_c *dev : device_c::mydevices)
		for (parameter_c *p : dev->parameter) {
			param_default_t d;
			d.value = *p->render();
			d.writable = !p->readonly;
			parameter_defaults[p] = d;
		}
}

static bool is_default(parameter_c *p) {
	std::map<parameter_c *, param_default_t>::iterator it = parameter_defaults.find(p);
	return it != parameter_defaults.end() && it->second.value == *p->render();
}

// the same question about a value the caller already has in hand
static bool is_default_value(parameter_c *p, const std::string &value) {
	std::map<parameter_c *, param_default_t>::iterator it = parameter_defaults.find(p);
	return it != parameter_defaults.end() && it->second.value == value;
}

// The bus-placement parameters an operator sets in a configuration: where the
// device sits in the I/O page, its interrupt vector and level, and its bus slot.
// They read back read-only on an installed device because a placement change
// takes hold only when the device is unplugged and re-registered - which apply
// does - so their live readonly flag means "not while running", not "never". A
// stored configuration may carry them, so they count as settable here.
static bool is_bus_placement(const std::string &name) {
	return strcasecmp(name.c_str(), "base_addr") == 0
			|| strcasecmp(name.c_str(), "intr_vector") == 0
			|| strcasecmp(name.c_str(), "intr_level") == 0
			|| strcasecmp(name.c_str(), "slot") == 0;
}

// an operator may set this, whatever a transient lock says right now
static bool is_settable(parameter_c *p) {
	if (is_bus_placement(p->name))
		return true;
	std::map<parameter_c *, param_default_t>::iterator it = parameter_defaults.find(p);
	return it == parameter_defaults.end() ? !p->readonly : it->second.writable;
}

// The kind a live device assigns a parameter, looked up by device and parameter
// name. A configuration saved by older logic may still carry a running-state
// parameter (an activity LED, a mirrored register); comparing against the live
// setup, which no longer reports it, needs the live device's own classification
// rather than any guess from the name. A name the registry no longer knows is
// treated as configuration and compared literally.
static parameter_c::parameter_kind_e registry_param_kind(const std::string &devname,
		const std::string &paramname) {
	std::lock_guard<std::mutex> lock(device_c::mydevices_mutex);
	for (device_c *dev : device_c::mydevices) {
		if (strcasecmp(dev->name.value.c_str(), devname.c_str()) != 0)
			continue;
		parameter_c *p = dev->param_by_name(paramname);
		return p != nullptr ? p->kind : parameter_c::PARAM_CONFIG;
	}
	return parameter_c::PARAM_CONFIG;
}

// Whether an operator may set a device's parameter, by name. An unknown device
// or parameter is treated as settable, so it is compared literally.
static bool param_settable(const std::string &devname, const std::string &paramname) {
	std::lock_guard<std::mutex> lock(device_c::mydevices_mutex);
	for (device_c *dev : device_c::mydevices) {
		if (strcasecmp(dev->name.value.c_str(), devname.c_str()) != 0)
			continue;
		parameter_c *p = dev->param_by_name(paramname);
		return p == nullptr ? true : is_settable(p);
	}
	return true;
}

// The construction-default value of a device's parameter, by name. False when
// the device or parameter is unknown, or its default was never captured.
static bool param_default_value(const std::string &devname,
		const std::string &paramname, std::string *out) {
	std::lock_guard<std::mutex> lock(device_c::mydevices_mutex);
	for (device_c *dev : device_c::mydevices) {
		if (strcasecmp(dev->name.value.c_str(), devname.c_str()) != 0)
			continue;
		parameter_c *p = dev->param_by_name(paramname);
		if (p == nullptr)
			return false;
		std::map<parameter_c *, param_default_t>::iterator it = parameter_defaults.find(p);
		if (it == parameter_defaults.end())
			return false;
		*out = it->second.value;
		return true;
	}
	return false;
}

// Put a device back the way it was constructed. Parameters named in "keep" are
// left alone, the caller being about to set them.
static void reset_to_defaults(device_c *dev, const std::set<std::string> *keep,
		picojson::array *errors) {
	for (parameter_c *p : dev->parameter) {
		if (p->readonly || is_default(p))
			continue;
		if (keep != nullptr && keep->count(p->name))
			continue;
		std::map<parameter_c *, param_default_t>::iterator it = parameter_defaults.find(p);
		if (it == parameter_defaults.end())
			continue;
		try {
			p->parse(it->second.value);
		} catch (bad_parameter &e) {
			if (errors != nullptr)
				errors->push_back(picojson::value(
						dev->name.value + "." + p->name + ": " + e.what()));
		}
	}
}

// A parameter a configuration names, as text.
static bool config_param_text(const picojson::object &po, const char *key,
		std::string *out) {
	picojson::object::const_iterator it = po.find(key);
	if (it == po.end() || !it->second.is<std::string>())
		return false;
	*out = it->second.get<std::string>();
	return true;
}

// Place the memory card as a configuration names it. Its start address and its
// size describe one range, so they are applied together: taken one at a time
// they can pass through a placement that runs into the I/O page, which the card
// refuses. What the file does not name is the value the card was constructed
// with.
//
// The size may be given as an end address: a configuration written when the
// card was placed by its last address names an endaddr, and that address at the
// start address it names gives the size the card carries.
static void apply_memory_placement(memory_c *mem, const picojson::object &po,
		picojson::array *errors) {
	std::string text;
	uint32_t start = 0;
	if (config_param_text(po, "startaddr", &text)
			|| param_default_value(mem->name.value, "startaddr", &text))
		start = (uint32_t) strtoul(text.c_str(), nullptr, 8);

	mem->last_error.clear();
	bool placed;
	if (config_param_text(po, "size", &text))
		placed = mem->place_at(start, text);
	else {
		uint32_t end = 0;
		if (config_param_text(po, "endaddr", &text)
				|| param_default_value(mem->name.value, "endaddr", &text))
			end = (uint32_t) strtoul(text.c_str(), nullptr, 8);
		placed = end >= start && mem->place_at(start, end - start + 2);
	}
	if (!placed && errors != nullptr)
		errors->push_back(picojson::value(mem->name.value + ": the card is not placed"
				+ (mem->last_error.empty() ? "" : ": " + mem->last_error)));
}

// A configuration describes the whole machine: it carries the devices that are
// switched on and, of those, only the parameters that differ from the
// construction defaults. Everything it does not mention is off and default.
//
// What the machine carries is read through webpower, so a machine switched off
// at the panel still describes the configuration it holds: its cards are out of
// the emulation for the duration, and losing power neither unplugs a card nor
// ejects a pack.
//
// Caller holds operations_mutex and mydevices_mutex.
static picojson::value snapshot_devices_locked(void) {
	picojson::array devices;
	for (device_c *dev : device_c::mydevices) {
		if (device_is_infrastructure(dev) || !webpower_is_in_machine(dev))
			continue;
		picojson::object o;
		o["name"] = picojson::value(dev->name.value);
		o["enabled"] = picojson::value(true);
		picojson::object params;
		for (parameter_c *p : dev->parameter) {
			// verbosity is owned by settings.json (log_levels), not the
			// configuration, so it stays out of the snapshot; it remains the
			// live per-device knob
			if (p->name == "verbosity")
				continue;
			// running-state parameters (lamps, activity LEDs, the drive state
			// machine, mirrored registers) are not configuration by kind;
			// keeping them out of the snapshot keeps an untouched, running
			// machine from reading modified
			if (p->kind == parameter_c::PARAM_STATUS)
				continue;
			std::string value = webpower_param_value(dev, p);
			if (!is_settable(p) || is_default_value(p, value))
				continue;
			params[p->name] = picojson::value(value);
		}
		o["params"] = picojson::value(params);
		devices.push_back(picojson::value(o));
	}
	picojson::object root;
	root["devices"] = picojson::value(devices);
	return picojson::value(root);
}

// Saving is an explicit operator action and waits for the machine to be free.
static picojson::value snapshot_devices(void) {
	std::lock_guard<std::mutex> ops_lock(device_configuration_c::operations_mutex);
	std::lock_guard<std::mutex> lock(device_c::mydevices_mutex);
	return snapshot_devices_locked();
}

// The same snapshot for a status query, which must never block a worker thread
// waiting on the machine: both locks are polled to a deadline and released
// again if only one comes free, so a busy registry costs a 503 and not a wedged
// connection. Returns false if the machine stayed busy.
static bool snapshot_devices_now(picojson::value *out, unsigned timeout_ms) {
	std::unique_lock<std::mutex> ops_lock(device_configuration_c::operations_mutex,
			std::defer_lock);
	std::unique_lock<std::mutex> dev_lock(device_c::mydevices_mutex, std::defer_lock);
	for (unsigned waited = 0;; waited += 10) {
		if (ops_lock.try_lock()) {
			if (dev_lock.try_lock()) {
				*out = snapshot_devices_locked();
				return true;
			}
			ops_lock.unlock(); // never hold one while waiting for the other
		}
		if (waited >= timeout_ms)
			return false;
		usleep(10000);
	}
}

static bool read_config(const std::string &name, picojson::value *out,
		std::string *err) {
	std::ifstream in(config_path(name).c_str());
	if (!in.is_open()) {
		*err = "unknown configuration \"" + name + "\"";
		return false;
	}
	std::stringstream buffer;
	buffer << in.rdbuf();
	std::string parse_err = picojson::parse(*out, buffer.str());
	if (!parse_err.empty()) {
		*err = "unreadable configuration \"" + name + "\": " + parse_err;
		return false;
	}
	return true;
}

// the saved device set of a configuration; false when the file is missing or
// unreadable, or names no devices
static bool read_config_devices(const std::string &name, picojson::value *out) {
	std::string err;
	return read_config(name, out, &err)
			&& out->get("devices").is<picojson::array>();
}

// Carry file-level metadata (the operator title, the DIP selection value and the
// dashboard layout) forward across a save whose document describes only devices.
// A document that names a field keeps its own; otherwise the field already stored
// under <name> is preserved, so saving the live setup drops none of the metadata
// the operator gave the configuration.
static void preserve_metadata(const std::string &name, picojson::value *doc) {
	if (!doc->is<picojson::object>())
		return;
	picojson::object &o = doc->get<picojson::object>();
	bool want_title = o.find("title") == o.end();
	bool want_dip = o.find("dip_value") == o.end();
	bool want_layout = o.find("layout") == o.end();
	if (!want_title && !want_dip && !want_layout)
		return;
	picojson::value existing;
	std::string err;
	if (!read_config(name, &existing, &err))
		return;
	if (want_title && existing.get("title").is<std::string>())
		o["title"] = existing.get("title");
	if (want_dip && existing.get("dip_value").is<double>())
		o["dip_value"] = existing.get("dip_value");
	if (want_layout && existing.get("layout").is<picojson::object>())
		o["layout"] = existing.get("layout");
}

// The DIP value a configuration binds itself to, or -1 when it names none.
static int config_dip_value(const picojson::value &content) {
	if (content.get("dip_value").is<double>())
		return (int) content.get("dip_value").get<double>();
	return -1;
}

// The saved configuration whose dip_value matches this DIP setting, or empty
// when none claims it. A negative dip (no switch hardware) matches nothing.
static std::string config_for_dip(int dip) {
	if (dip < 0)
		return "";
	DIR *dir = opendir(configs_dir.c_str());
	if (dir == nullptr)
		return "";
	std::string match;
	struct dirent *entry;
	while ((entry = readdir(dir)) != nullptr) {
		std::string fname = entry->d_name;
		if (fname.size() < 6 || fname.compare(fname.size() - 5, 5, ".json") != 0)
			continue;
		std::string name = fname.substr(0, fname.size() - 5);
		picojson::value content;
		std::string err;
		if (read_config(name, &content, &err) && config_dip_value(content) == dip) {
			match = name;
			break;
		}
	}
	closedir(dir);
	return match;
}

// A snapshot reduced to a name-keyed map of {enabled, params}, so two
// configurations compare equal whatever order their device arrays hold. Both
// the live snapshot and a saved file derive from registry order, but a
// hand-edited file need not, and the modified flag must not turn on that.
static picojson::value canonical(const picojson::value &snapshot) {
	picojson::object by_name;
	if (snapshot.get("devices").is<picojson::array>())
		for (const picojson::value &d : snapshot.get("devices").get<picojson::array>()) {
			if (!d.get("name").is<std::string>())
				continue;
			picojson::object e;
			e["enabled"] = d.get("enabled").is<bool>()
					? picojson::value(d.get("enabled").get<bool>())
					: picojson::value(true);
			// Drop running-state parameters so the two sides normalize the same
			// way the live snapshot does. A configuration saved before these were
			// filtered — or hand-edited to carry one — still compares equal to a
			// live setup that no longer reports it: the live device's own kind,
			// not the parameter name, decides.
			std::string devname = d.get("name").get<std::string>();
			picojson::object params;
			if (d.get("params").is<picojson::object>())
				for (const std::pair<const std::string, picojson::value> &kv :
						d.get("params").get<picojson::object>()) {
					// Drop what the live snapshot never emits, so a saved file that
					// still carries it (older logic, or hand-edited) does not read
					// modified: running-state parameters, read-only ones (e.g. an
					// interrupt vector now auto-assigned from the arbitration slot),
					// and any parameter written at its construction default.
					if (registry_param_kind(devname, kv.first) == parameter_c::PARAM_STATUS)
						continue;
					if (!param_settable(devname, kv.first))
						continue;
					std::string def;
					if (kv.second.is<std::string>()
							&& param_default_value(devname, kv.first, &def)
							&& kv.second.get<std::string>() == def)
						continue;
					params[kv.first] = kv.second;
				}
			e["params"] = picojson::value(params);
			by_name[d.get("name").get<std::string>()] = picojson::value(e);
		}
	return picojson::value(by_name);
}

std::string webconfigs_current(void) {
	std::lock_guard<std::mutex> lock(current_mutex);
	return current_config_name;
}

// true when the live machine differs from the saved form of the current
// configuration. *busy is set, and the result is not meaningful, when the
// machine could not be sampled within the deadline.
static bool compute_modified(bool *busy, unsigned timeout_ms) {
	if (busy != nullptr)
		*busy = false;
	std::string current = webconfigs_current();
	picojson::value live;
	if (!snapshot_devices_now(&live, timeout_ms)) {
		if (busy != nullptr)
			*busy = true;
		return false;
	}
	picojson::value saved;
	if (!read_config_devices(current, &saved))
		return true; // no saved form to match: the live setup counts as edited
	return !(canonical(live) == canonical(saved));
}

void webconfigs_status(std::string *current, bool *modified, bool *busy) {
	if (current != nullptr)
		*current = webconfigs_current();
	bool b = false;
	// The event poll runs this at 10 Hz, so it gives up quickly rather than
	// stalling the broadcast thread behind an apply; the apply publishes the
	// cleared flag itself once it releases the lock.
	bool m = compute_modified(&b, 20);
	if (busy != nullptr)
		*busy = b;
	if (modified != nullptr)
		*modified = b ? cached_modified : m;
	if (!b) {
		std::lock_guard<std::mutex> lock(current_mutex);
		cached_modified = m;
	}
}

// Point the current configuration at <name> and tell the event stream. A
// no-op publish is harmless: the event carries the recomputed modified flag,
// which an apply or save has just cleared.
static void set_current(const std::string &name) {
	{
		std::lock_guard<std::mutex> lock(current_mutex);
		current_config_name = name;
	}
	webevents_note_config();
}

// read and parse a JSON object request body
static bool read_json_body(struct mg_connection *conn, picojson::value *out) {
	char body[4096];
	int body_len = mg_read(conn, body, sizeof(body) - 1);
	if (body_len <= 0)
		return false;
	body[body_len] = 0;
	std::string parse_err = picojson::parse(*out, body);
	return parse_err.empty() && out->is<picojson::object>();
}

// A config document can carry the whole device set, so read the body to its end
// rather than to a fixed buffer.
static bool read_json_body_full(struct mg_connection *conn, picojson::value *out) {
	std::string body;
	char buf[4096];
	int n;
	while ((n = mg_read(conn, buf, sizeof(buf))) > 0)
		body.append(buf, (size_t) n);
	if (body.empty())
		return false;
	std::string parse_err = picojson::parse(*out, body);
	return parse_err.empty() && out->is<picojson::object>();
}

// Write a configuration file atomically: a reader either sees the previous file
// or the new one, never a half-written document. The temporary is created
// alongside the target so the rename stays within one filesystem.
static bool write_config_file(const std::string &name, const std::string &body,
		std::string *error) {
	std::string path = config_path(name);
	std::string tmp = path + ".tmp";
	{
		std::ofstream out(tmp.c_str(), std::ios::trunc);
		if (!out.is_open()) {
			if (error != nullptr)
				*error = "cannot write configuration \"" + name + "\"";
			return false;
		}
		out << body;
		out.close();
		if (out.fail()) {
			::unlink(tmp.c_str());
			if (error != nullptr)
				*error = "cannot write configuration \"" + name + "\"";
			return false;
		}
	}
	if (::rename(tmp.c_str(), path.c_str()) != 0) {
		::unlink(tmp.c_str());
		if (error != nullptr)
			*error = "cannot write configuration \"" + name + "\"";
		return false;
	}
	return true;
}

// The backplane slots a device occupies, as offsets from its "slot" parameter.
// A device whose receive and transmit interrupts arbitrate separately sits in
// two adjacent slots, so the second one is taken as much as the first. The
// offsets are read from the device's own requests rather than tabulated here.
static std::set<int> device_slot_offsets(qunibusdevice_c *dev) {
	std::set<int> offsets;
	int base = (int) dev->priority_slot.value;
	// Slot 0 is reserved and means "no backplane position": the emulated CPU
	// arbitrates rather than requesting, and holds no slot to clash over.
	if (base == 0)
		return offsets;
	for (dma_request_c *req : dev->dma_requests)
		if (req->get_priority_slot() < PRIORITY_SLOT_COUNT)
			offsets.insert((int) req->get_priority_slot() - base);
	for (intr_request_c *req : dev->intr_requests)
		if (req->get_priority_slot() < PRIORITY_SLOT_COUNT)
			offsets.insert((int) req->get_priority_slot() - base);
	if (offsets.empty())
		offsets.insert(0); // a device with neither DMA nor interrupts still has a place
	return offsets;
}

// true unless the entry says otherwise: a saved document lists the devices that
// are on the bus, and carries "enabled" for each of them
static bool device_entry_enabled(const picojson::value &d) {
	const picojson::value &en = d.get("enabled");
	return !en.is<bool>() || en.get<bool>();
}

// The slot a device entry places its device in: what the document sets, or the
// device's DEC default when it sets nothing.
static unsigned device_entry_slot(const picojson::value &d, qunibusdevice_c *dev) {
	if (d.get("params").is<picojson::object>()) {
		const picojson::object &params = d.get("params").get<picojson::object>();
		picojson::object::const_iterator it = params.find("slot");
		if (it != params.end() && it->second.is<std::string>())
			return (unsigned) strtoul(it->second.get<std::string>().c_str(), nullptr, 10);
	}
	return dev->default_priority_slot;
}

// One backplane, one card per slot: the BR/NPR grant chain runs through the
// slots in order, so two devices sharing one have no defined priority between
// them. Reject a configuration that places them there, whichever bus this is.
static bool validate_config_slots(const picojson::value &doc, std::string *error) {
	std::map<unsigned, std::string> occupied;
	for (const picojson::value &d : doc.get("devices").get<picojson::array>()) {
		if (!device_entry_enabled(d))
			continue;
		std::string devname = d.get("name").get<std::string>();
		qunibusdevice_c *dev = nullptr;
		for (device_c *cand : device_c::mydevices)
			if (strcasecmp(cand->name.value.c_str(), devname.c_str()) == 0) {
				dev = dynamic_cast<qunibusdevice_c *>(cand);
				break;
			}
		if (dev == nullptr)
			continue; // drives and other devices off the bus hold no slot
		unsigned slot = device_entry_slot(d, dev);
		for (int offset : device_slot_offsets(dev)) {
			unsigned s = (unsigned) ((int) slot + offset);
			if (s >= PRIORITY_SLOT_COUNT) {
				*error = devname + ": backplane slot " + std::to_string(s)
						+ " is beyond the last slot "
						+ std::to_string(PRIORITY_SLOT_COUNT - 1);
				return false;
			}
			std::map<unsigned, std::string>::iterator held = occupied.find(s);
			if (held != occupied.end()) {
				*error = devname + " and " + held->second
						+ " both use backplane slot " + std::to_string(s);
				return false;
			}
			occupied[s] = devname;
		}
	}
	return true;
}

// Check a config document against the live device registry before it is stored,
// so an edited file can never name a device that does not exist or a parameter
// a device does not have or an operator may not set. The registry is only read,
// never changed — a stored edit must not disturb the running machine. On
// rejection *error names the offending device/param.
static bool validate_config_document(const picojson::value &doc, std::string *error) {
	std::lock_guard<std::mutex> lock(device_c::mydevices_mutex);
	for (const picojson::value &d : doc.get("devices").get<picojson::array>()) {
		if (!d.get("name").is<std::string>()) {
			*error = "every device entry needs a string \"name\"";
			return false;
		}
		std::string devname = d.get("name").get<std::string>();
		device_c *dev = nullptr;
		for (device_c *cand : device_c::mydevices)
			if (!device_is_infrastructure(cand)
					&& strcasecmp(cand->name.value.c_str(), devname.c_str()) == 0) {
				dev = cand;
				break;
			}
		if (dev == nullptr) {
			*error = "unknown device \"" + devname + "\"";
			return false;
		}
		if (!d.get("params").is<picojson::object>())
			continue;
		for (const std::pair<const std::string, picojson::value> &kv :
				d.get("params").get<picojson::object>()) {
			parameter_c *p = dev->param_by_name(kv.first);
			if (p == nullptr) {
				*error = devname + "." + kv.first + ": unknown parameter";
				return false;
			}
			if (!is_settable(p)) {
				*error = devname + "." + kv.first + ": not settable";
				return false;
			}
			if (!kv.second.is<std::string>()) {
				*error = devname + "." + kv.first + ": value must be a string";
				return false;
			}
		}
	}
	return validate_config_slots(doc, error);
}

// the image a device entry names, empty when it names none
static std::string device_image(const picojson::value &d) {
	if (!d.get("params").is<picojson::object>())
		return "";
	const picojson::object &params = d.get("params").get<picojson::object>();
	picojson::object::const_iterator it = params.find("image");
	if (it == params.end() || !it->second.is<std::string>())
		return "";
	return it->second.get<std::string>();
}

// every drive, in every saved configuration, that names this image file
std::vector<config_image_use_t> webconfigs_image_uses(const std::string &image_name) {
	std::vector<config_image_use_t> uses;
	DIR *dir = opendir(configs_dir.c_str());
	if (dir == nullptr)
		return uses;
	struct dirent *entry;
	while ((entry = readdir(dir)) != nullptr) {
		std::string fname = entry->d_name;
		if (fname.size() < 6 || fname.compare(fname.size() - 5, 5, ".json") != 0)
			continue;
		std::string name = fname.substr(0, fname.size() - 5);
		picojson::value content;
		std::string err;
		if (!read_config(name, &content, &err)
				|| !content.get("devices").is<picojson::array>())
			continue;
		for (picojson::value &d : content.get("devices").get<picojson::array>()) {
			std::string path = device_image(d);
			// image_name is the canonical images-root-relative subpath; compare
			// on that so two same-named files in different folders don't collide
			if (path.empty() || webstorage_image_subpath(path) != image_name)
				continue;
			config_image_use_t use;
			use.config = name;
			use.device = d.get("name").is<std::string>()
					? d.get("name").get<std::string>() : "";
			uses.push_back(use);
		}
	}
	closedir(dir);
	return uses;
}

// deleting an image must not break a saved configuration
std::string webconfigs_image_referenced(const std::string &image_name) {
	std::vector<config_image_use_t> uses = webconfigs_image_uses(image_name);
	return uses.empty() ? "" : uses[0].config;
}

// The object form of GET /api/configs: the current/default pointers, the live
// modified flag (omitted when the busy machine blocks its comparison), and the
// saved configurations with their enabled-device summary.
static picojson::value configs_list_value(void) {
	picojson::array configs;
	DIR *dir = opendir(configs_dir.c_str());
	if (dir != nullptr) {
		struct dirent *entry;
		while ((entry = readdir(dir)) != nullptr) {
			std::string fname = entry->d_name;
			if (fname.size() < 6 || fname.compare(fname.size() - 5, 5, ".json") != 0)
				continue;
			std::string name = fname.substr(0, fname.size() - 5);
			struct stat st;
			if (stat(config_path(name).c_str(), &st) != 0)
				continue;
			picojson::object o;
			o["name"] = picojson::value(name);
			char mtime[32];
			strftime(mtime, sizeof(mtime), "%Y-%m-%d %H:%M",
					localtime(&st.st_mtime));
			o["mtime"] = picojson::value(mtime);
			// the enabled devices, as the card summary
			picojson::value content;
			std::string err;
			picojson::array enabled;
			bool read = read_config(name, &content, &err);
			if (read && content.get("devices").is<picojson::array>())
				for (picojson::value &d : content.get("devices").get<picojson::array>())
					if (d.get("enabled").is<bool>() && d.get("enabled").get<bool>())
						enabled.push_back(d.get("name"));
			// the operator title, falling back to the name
			o["title"] = picojson::value(
					read && content.get("title").is<std::string>()
							? content.get("title").get<std::string>() : name);
			// the DIP value that selects this configuration at power-on, or -1
			o["dip_value"] = picojson::value(
					(double) (read ? config_dip_value(content) : -1));
			o["enabled"] = picojson::value(enabled);
			configs.push_back(picojson::value(o));
		}
		closedir(dir);
	}
	picojson::object root;
	root["current"] = picojson::value(webconfigs_current());
	bool busy = false;
	bool modified = compute_modified(&busy, 500);
	if (!busy) {
		root["modified"] = picojson::value(modified);
		std::lock_guard<std::mutex> lock(current_mutex);
		cached_modified = modified;
	}
	root["configs"] = picojson::value(configs);
	return picojson::value(root);
}

std::string webconfigs_list_json(void) {
	return configs_list_value().serialize();
}

// GET /api/configs
static void configs_list(struct mg_connection *conn) {
	send_json(conn, 200, configs_list_value());
}

// Save the live setup under <name> and make it the current configuration,
// which clears the modified state. Save and Save As are the same operation.
bool webconfigs_save(const std::string &name, std::string *error) {
	picojson::value snap = snapshot_devices();
	preserve_metadata(name, &snap);
	if (!write_config_file(name, snap.serialize(), error))
		return false;
	WEB_INFO("configuration \"%s\" saved", name.c_str());
	set_current(name);
	return true;
}

// Write a config document to <name>, validated against the device registry.
// One endpoint stores every configuration: the body is always the document to
// save. With from_live true the body is the live setup being saved under
// <name>, so <name> becomes the current configuration and the modified state
// clears; with it false the document is an offline edit, written to the file
// only, leaving the current pointer and the running machine untouched — even
// when <name> is the current configuration, editing its stored file being
// distinct from the live dirty state. An unknown device or unsettable parameter
// is refused (*status 422) and nothing is written.
bool webconfigs_write(const std::string &name, const picojson::value &document,
		bool from_live, std::string *error, int *status) {
	if (!document.is<picojson::object>()
			|| !document.get("devices").is<picojson::array>()) {
		if (error != nullptr)
			*error = "configuration must be a JSON object with a \"devices\" array";
		if (status != nullptr)
			*status = 422;
		return false;
	}
	std::string verr;
	if (!validate_config_document(document, &verr)) {
		if (error != nullptr)
			*error = verr;
		if (status != nullptr)
			*status = 422;
		return false;
	}
	picojson::value doc = document;
	preserve_metadata(name, &doc);
	if (!write_config_file(name, doc.serialize(), error)) {
		if (status != nullptr)
			*status = 500;
		return false;
	}
	if (from_live) {
		WEB_INFO("configuration \"%s\" saved from the live setup", name.c_str());
		set_current(name);
	} else
		WEB_INFO("configuration \"%s\" edited (%u devices)", name.c_str(),
				(unsigned) document.get("devices").get<picojson::array>().size());
	if (status != nullptr)
		*status = 200;
	return true;
}

// PUT /api/configs/<name> — write the config document in the body. ?from=live
// marks it the live setup being saved under <name>.
static void config_put(struct mg_connection *conn, const std::string &name,
		bool from_live) {
	picojson::value doc;
	if (!read_json_body_full(conn, &doc)) {
		send_error(conn, 400,
				"body must be a JSON configuration document {\"devices\":[…]}");
		return;
	}
	std::string error;
	int status = 200;
	if (!webconfigs_write(name, doc, from_live, &error, &status)) {
		send_error(conn, status, error);
		return;
	}
	picojson::object res;
	res["ok"] = picojson::value(true);
	send_json(conn, 200, picojson::value(res));
}

// The first file two of a configuration's devices both name, as the message
// that says so; "" when every medium is named once. Both the stored edit and
// the apply refuse on this, so a document that would put one image in two
// drives can be neither written nor restored.
static std::string duplicate_image_in(const picojson::value &content) {
	if (!content.get("devices").is<picojson::array>())
		return "";
	// image subpath -> the device that named it first
	std::map<std::string, std::string> seen;
	const picojson::array &devices = content.get("devices").get<picojson::array>();
	for (const picojson::value &d : devices) {
		std::string sub = webstorage_image_subpath(device_image(d));
		if (sub.empty())
			continue;
		std::string devname = d.get("name").is<std::string>()
				? d.get("name").get<std::string>() : "another drive";
		std::map<std::string, std::string>::const_iterator it = seen.find(sub);
		if (it != seen.end())
			return "\"" + sub + "\" is the image of both " + it->second
					+ " and " + devname;
		seen[sub] = devname;
	}
	return "";
}

// PUT /api/configs/<name>/devices/<device>/image  {"value": "<image name>"}
//
// The medium a drive starts with belongs to the configuration, so it is
// editable there without disturbing the machine. An empty value leaves the
// drive with no image, which is the value it is constructed with, so the key
// is dropped rather than stored: a snapshot names only what differs.
//
// Two drives in one configuration must not name the same file — applying it
// would open the image twice, and both drives would write it.
static void config_set_image(struct mg_connection *conn, const std::string &name,
		const std::string &devname) {
	picojson::value req;
	if (!read_json_body(conn, &req) || !req.get("value").is<std::string>()) {
		send_error(conn, 400, "body must be a JSON object with a string \"value\"");
		return;
	}
	std::string value = req.get("value").get<std::string>();
	// a bare name is one of the images this interface manages
	std::string path = value.empty() ? "" : webstorage_image_path(value);

	picojson::value content;
	std::string err;
	if (!read_config(name, &content, &err)) {
		send_error(conn, 404, err);
		return;
	}
	if (!content.get("devices").is<picojson::array>()) {
		send_error(conn, 422, "configuration \"" + name + "\" names no devices");
		return;
	}
	picojson::array &devices = content.get<picojson::object>()["devices"]
			.get<picojson::array>();

	picojson::value *target = nullptr;
	for (picojson::value &d : devices) {
		if (!d.get("name").is<std::string>() || d.get("name").get<std::string>() != devname)
			continue;
		target = &d;
		break;
	}
	if (target == nullptr) {
		send_error(conn, 404, "configuration \"" + name + "\" does not name device \""
				+ devname + "\"");
		return;
	}
	if (!path.empty())
		for (picojson::value &d : devices) {
			if (&d == target)
				continue;
			if (webstorage_image_subpath(device_image(d)) != webstorage_image_subpath(path))
				continue;
			std::string other = d.get("name").is<std::string>()
					? d.get("name").get<std::string>() : "another drive";
			send_error(conn, 409, "\"" + webstorage_image_subpath(path)
					+ "\" is already the image of " + other + " in this configuration");
			return;
		}

	if (!target->get("params").is<picojson::object>())
		target->get<picojson::object>()["params"] = picojson::value(picojson::object());
	picojson::object &params = target->get<picojson::object>()["params"]
			.get<picojson::object>();
	if (path.empty())
		params.erase("image");
	else
		params["image"] = picojson::value(path);

	std::ofstream out(config_path(name).c_str());
	if (!out.is_open()) {
		send_error(conn, 500, "cannot write configuration \"" + name + "\"");
		return;
	}
	out << content.serialize();
	out.close();
	WEB_INFO("configuration \"%s\": %s image = %s", name.c_str(),
			devname.c_str(), path.empty() ? "none" : path.c_str());
	picojson::object res;
	res["ok"] = picojson::value(true);
	res["image"] = picojson::value(path);
	send_json(conn, 200, picojson::value(res));
}

// PUT /api/configs/<name>/title  {"value": "<title>"}
//
// The operator's human-friendly title is file metadata, not a device setting,
// so it is edited in the file directly and disturbs neither the current pointer
// nor the running machine. An empty value clears the title back to the name, so
// nothing is stored rather than an empty string.
static void config_set_title(struct mg_connection *conn, const std::string &name) {
	picojson::value req;
	if (!read_json_body(conn, &req) || !req.get("value").is<std::string>()) {
		send_error(conn, 400, "body must be a JSON object with a string \"value\"");
		return;
	}
	std::string value = req.get("value").get<std::string>();
	if (value.size() > 128) {
		send_error(conn, 422, "title too long (max 128 characters)");
		return;
	}
	picojson::value content;
	std::string err;
	if (!read_config(name, &content, &err)) {
		send_error(conn, 404, err);
		return;
	}
	if (!content.is<picojson::object>()) {
		send_error(conn, 422, "configuration \"" + name + "\" is not a JSON object");
		return;
	}
	picojson::object &root = content.get<picojson::object>();
	if (value.empty())
		root.erase("title");
	else
		root["title"] = picojson::value(value);
	std::string werr;
	if (!write_config_file(name, content.serialize(), &werr)) {
		send_error(conn, 500, werr);
		return;
	}
	WEB_INFO("configuration \"%s\" title = %s", name.c_str(),
			value.empty() ? "(name)" : value.c_str());
	webevents_note_config();
	picojson::object res;
	res["ok"] = picojson::value(true);
	res["title"] = picojson::value(value.empty() ? name : value);
	send_json(conn, 200, picojson::value(res));
}

// PUT /api/configs/<name>/dip  {"value": <0..15> | null}
//
// Binds the configuration to a DIP-switch setting, so the board loads it at
// power-on when the switches read that value. It is file metadata, disturbing
// neither the current pointer nor the running machine. At most one
// configuration may claim a value: one another configuration already holds is
// refused with 409. A null value clears the binding.
static void config_set_dip(struct mg_connection *conn, const std::string &name) {
	picojson::value req;
	if (!read_json_body(conn, &req)) {
		send_error(conn, 400, "body must be a JSON object with a \"value\"");
		return;
	}
	int dip = -1; // null (or absent) clears the binding
	const picojson::value &v = req.get("value");
	if (v.is<double>()) {
		dip = (int) v.get<double>();
		if (dip < 0 || dip > 15) {
			send_error(conn, 422, "dip value must be 0..15 or null");
			return;
		}
	} else if (!v.is<picojson::null>()) {
		send_error(conn, 400, "value must be a number 0..15 or null");
		return;
	}
	// no two configurations may claim the same DIP value
	if (dip >= 0) {
		std::string other = config_for_dip(dip);
		if (!other.empty() && other != name) {
			send_error(conn, 409, "DIP value " + std::to_string(dip)
					+ " already selects configuration \"" + other + "\"");
			return;
		}
	}
	picojson::value content;
	std::string err;
	if (!read_config(name, &content, &err)) {
		send_error(conn, 404, err);
		return;
	}
	if (!content.is<picojson::object>()) {
		send_error(conn, 422, "configuration \"" + name + "\" is not a JSON object");
		return;
	}
	picojson::object &root = content.get<picojson::object>();
	if (dip < 0)
		root.erase("dip_value");
	else
		root["dip_value"] = picojson::value((double) dip);
	std::string werr;
	if (!write_config_file(name, content.serialize(), &werr)) {
		send_error(conn, 500, werr);
		return;
	}
	WEB_INFO("configuration \"%s\" dip_value = %d", name.c_str(), dip);
	webevents_note_config();
	picojson::object res;
	res["ok"] = picojson::value(true);
	res["dip_value"] = picojson::value((double) dip);
	send_json(conn, 200, picojson::value(res));
}

// PUT /api/configs/<name>/layout  {"value": { <widget-key>: {x,y,hidden}, … }}
//
// The dashboard arrangement is file metadata, stored per configuration so
// switching machines switches layout. It is opaque here — the dashboard owns
// its shape; this only persists it, without disturbing the current pointer or
// the running machine. A null (or non-object) value clears the layout.
static void config_set_layout(struct mg_connection *conn, const std::string &name) {
	picojson::value req;
	if (!read_json_body_full(conn, &req)) {
		send_error(conn, 400, "body must be a JSON object with a \"value\"");
		return;
	}
	const picojson::value &v = req.get("value");
	picojson::value content;
	std::string err;
	if (!read_config(name, &content, &err)) {
		send_error(conn, 404, err);
		return;
	}
	if (!content.is<picojson::object>()) {
		send_error(conn, 422, "configuration \"" + name + "\" is not a JSON object");
		return;
	}
	picojson::object &root = content.get<picojson::object>();
	if (v.is<picojson::object>())
		root["layout"] = v;
	else
		root.erase("layout");
	std::string werr;
	if (!write_config_file(name, content.serialize(), &werr)) {
		send_error(conn, 500, werr);
		return;
	}
	WEB_INFO("configuration \"%s\" dashboard layout updated", name.c_str());
	webevents_note_config();
	picojson::object res;
	res["ok"] = picojson::value(true);
	send_json(conn, 200, picojson::value(res));
}

// POST /api/configs/<name>/apply — restore a snapshot. Devices are stored
// in registry order (controllers before their drives), so applying in
// order enables controllers first. Rejections are collected, not fatal.
// Apply a saved configuration to the device set: the work behind both
// POST /api/configs/<name>/apply and the --config option of the service.
// Returns false when the configuration cannot be read; parameters the devices
// reject are collected in "errors" and do not fail the call.
static bool apply_config(const std::string &name, picojson::array *errors,
		std::string *error, int *status) {
	picojson::value content;
	if (!read_config(name, &content, error)) {
		if (status != nullptr)
			*status = 404;
		return false;
	}
	if (!content.get("devices").is<picojson::array>()) {
		*error = "configuration \"" + name + "\" has no devices";
		if (status != nullptr)
			*status = 404;
		return false;
	}
	// Two drives naming one file would open the image twice and both write it.
	// The stored path refuses that as it is written; refusing it here too means a
	// machine cannot be restored into a state that path prevents being saved.
	{
		std::string conflict = duplicate_image_in(content);
		if (!conflict.empty()) {
			*error = conflict;
			if (status != nullptr)
				*status = 409;
			return false;
		}
	}
	{
		std::lock_guard<std::mutex> ops_lock(device_configuration_c::operations_mutex);

		// Skip modeled mechanics for the span of the apply, so a timed device
		// (a disk drive's spin-up/-down) settles at once and does not stretch the
		// reconfiguration over physical delays. Restored at the end.
		{
			std::lock_guard<std::mutex> lock(device_c::mydevices_mutex);
			for (device_c *dev : device_c::mydevices)
				dev->config_apply_immediate = true;
		}

		// The configuration is the whole machine, so anything it leaves out is
		// switched off and back at its defaults. Work backwards through the
		// registry so drives go before the controllers they hang off.
		std::set<std::string> mentioned;
		for (picojson::value &d : content.get("devices").get<picojson::array>())
			if (d.get("name").is<std::string>())
				mentioned.insert(d.get("name").get<std::string>());
		{
			std::lock_guard<std::mutex> lock(device_c::mydevices_mutex);
			for (std::list<device_c *>::reverse_iterator it = device_c::mydevices.rbegin();
					it != device_c::mydevices.rend(); ++it) {
				device_c *dev = *it;
				if (device_is_infrastructure(dev))
					continue;
				bool named = false;
				for (const std::string &n : mentioned)
					if (strcasecmp(n.c_str(), dev->name.value.c_str()) == 0) {
						named = true;
						break;
					}
				if (named)
					continue;
				if (dev->enabled.value)
					dev->enabled.set(false);
				reset_to_defaults(dev, nullptr, errors);
			}
		}

		for (picojson::value &d : content.get("devices").get<picojson::array>()) {
			if (!d.get("name").is<std::string>())
				continue;
			std::string devname = d.get("name").get<std::string>();
			device_c *dev = nullptr;
			{
				std::lock_guard<std::mutex> lock(device_c::mydevices_mutex);
				for (device_c *cand : device_c::mydevices)
					if (!device_is_infrastructure(cand)
							&& strcasecmp(cand->name.value.c_str(), devname.c_str()) == 0) {
						dev = cand;
						break;
					}
			}
			if (dev == nullptr) {
				errors->push_back(picojson::value(devname + ": unknown device"));
				continue;
			}
			// parameters the file omits are at their defaults too
			std::set<std::string> listed;
			if (d.get("params").is<picojson::object>())
				for (const std::pair<const std::string, picojson::value> &kv :
						d.get("params").get<picojson::object>())
					listed.insert(kv.first);

			// A device's bus placement (address, vector, level, slot) is locked
			// while it is installed. When the configuration moves an enabled
			// device, unplug it first so the fields below apply; the enable at the
			// end re-registers it at the new placement.
			if (dev->enabled.value && d.get("params").is<picojson::object>()) {
				const picojson::object &po = d.get("params").get<picojson::object>();
				static const char *placement[] = {"base_addr", "intr_vector",
						"intr_level", "slot"};
				for (const char *k : placement) {
					picojson::object::const_iterator it = po.find(k);
					if (it == po.end() || !it->second.is<std::string>())
						continue;
					parameter_c *p = dev->param_by_name(k);
					if (p != nullptr && *p->render() != it->second.get<std::string>()) {
						dev->enabled.set(false);
						break;
					}
				}
			}

			// The memory card is placed in one step, from parameters that
			// describe one range together; reset_to_defaults() would apply
			// them one at a time.
			memory_c *mem = dynamic_cast<memory_c *>(dev);
			if (mem != nullptr) {
				listed.insert("startaddr");
				listed.insert("size");
			}

			reset_to_defaults(dev, &listed, errors);

			if (d.get("params").is<picojson::object>())
				for (const std::pair<const std::string, picojson::value> &kv :
						d.get("params").get<picojson::object>()) {
					if (!kv.second.is<std::string>())
						continue;
					if (mem != nullptr
							&& (kv.first == "startaddr" || kv.first == "size"))
						continue; // placed below, as one range
					parameter_c *param = dev->param_by_name(kv.first);
					if (param == nullptr || param->readonly)
						continue;
					if (*param->render() == kv.second.get<std::string>())
						continue; // unchanged — don't disturb the device
					try {
						param->parse(kv.second.get<std::string>());
					} catch (bad_parameter &e) {
						errors->push_back(picojson::value(
								devname + "." + kv.first + ": " + e.what()));
					}
				}

			// The card takes its range once the parameters that qualify the
			// claim are in place: the probe reads the file's setting, not the
			// default it was reset to.
			if (mem != nullptr)
				apply_memory_placement(mem, d.get("params").is<picojson::object>()
						? d.get("params").get<picojson::object>() : picojson::object(),
						errors);

			if (d.get("enabled").is<bool>())
				dev->enabled.set(d.get("enabled").get<bool>());
		}

		// The machine has settled; restore the normal timed simulation.
		{
			std::lock_guard<std::mutex> lock(device_c::mydevices_mutex);
			for (device_c *dev : device_c::mydevices)
				dev->config_apply_immediate = false;
		}

		// A configuration loaded into a switched-off machine leaves it switched
		// off: the devices it names are what the panel switch will bring up.
		webpower_recapture_if_off();
	}
	// An apply resets every device's verbosity to its construction default, so
	// re-assert the persisted log levels: the stored overrides win, the rest
	// fall to the global default.
	weblogging_apply();
	WEB_INFO("configuration \"%s\" applied, %u rejections",
			name.c_str(), (unsigned) errors->size());
	return true;
}

// POST /api/configs/<name>/apply — sets the current configuration. A Revert is
// this call with the current name: it re-initialises the live machine to the
// saved device set, dropping any device enabled since the last save.
static void config_apply(struct mg_connection *conn, const std::string &name) {
	picojson::array errors;
	std::string error;
	int status = 404;
	if (!apply_config(name, &errors, &error, &status)) {
		send_error(conn, status, error);
		return;
	}
	set_current(name);
	picojson::object res;
	res["ok"] = picojson::value(errors.empty());
	res["errors"] = picojson::value(errors);
	send_json(conn, 200, picojson::value(res));
}

bool webconfigs_apply(const std::string &name, std::vector<std::string> *rejections,
		std::string *error) {
	picojson::array errors;
	if (!apply_config(name, &errors, error, nullptr))
		return false;
	set_current(name);
	if (rejections != nullptr)
		for (picojson::value &e : errors)
			rejections->push_back(e.is<std::string>() ? e.get<std::string>() : "?");
	return true;
}

// Rename the file, and let the current pointer follow it. It is a
// file/metadata operation only: the live device set is untouched, so a machine
// modified against <from> stays modified against <to>. The DIP binding travels
// with the file, so the renamed configuration keeps its power-on selection.
bool webconfigs_rename(const std::string &from, const std::string &to,
		std::string *error) {
	if (!valid_config_name(from) || !valid_config_name(to)) {
		if (error != nullptr)
			*error = "invalid configuration name";
		return false;
	}
	struct stat st;
	if (stat(config_path(from).c_str(), &st) != 0) {
		if (error != nullptr)
			*error = "unknown configuration \"" + from + "\"";
		return false;
	}
	if (stat(config_path(to).c_str(), &st) == 0) {
		if (error != nullptr)
			*error = "configuration \"" + to + "\" already exists";
		return false;
	}
	if (::rename(config_path(from).c_str(), config_path(to).c_str()) != 0) {
		if (error != nullptr)
			*error = "cannot rename configuration \"" + from + "\"";
		return false;
	}
	{
		std::lock_guard<std::mutex> lock(current_mutex);
		if (current_config_name == from)
			current_config_name = to;
	}
	WEB_INFO("configuration \"%s\" renamed to \"%s\"", from.c_str(), to.c_str());
	webevents_note_config(); // current may now name <to>
	return true;
}

// Remove a configuration. The current one is protected: it names the running
// machine. On refusal *status is 409, on an unknown name 404.
bool webconfigs_delete(const std::string &name, std::string *error, int *status) {
	if (name == webconfigs_current()) {
		if (error != nullptr)
			*error = "configuration \"" + name
					+ "\" is the current one; switch to another first";
		if (status != nullptr)
			*status = 409;
		return false;
	}
	if (unlink(config_path(name).c_str()) != 0) {
		if (error != nullptr)
			*error = "unknown configuration \"" + name + "\"";
		if (status != nullptr)
			*status = 404;
		return false;
	}
	WEB_INFO("configuration \"%s\" deleted", name.c_str());
	return true;
}

// Ensure the bundled empty configuration exists, writing it if absent. Never
// overwrites an operator's file.
static void ensure_fallback_config(void) {
	struct stat st;
	if (stat(config_path(fallback_config_name).c_str(), &st) == 0)
		return;
	std::ofstream out(config_path(fallback_config_name).c_str());
	if (out.is_open())
		out << "{\"devices\":[]}";
}

// Apply the configuration the given name selects (empty → the bundled empty
// configuration), set the current pointer, and report the applied name.
static std::string apply_named_or_fallback(const std::string &selected,
		int dip) {
	std::string name = selected;
	if (name.empty() || !valid_config_name(name)) {
		ensure_fallback_config();
		name = fallback_config_name;
	}
	std::vector<std::string> rejections;
	std::string error;
	if (!webconfigs_apply(name, &rejections, &error)) {
		WEB_ERROR("Configuration \"%s\" not applied: %s", name.c_str(), error.c_str());
		// keep the current pointer pointed at what was asked for, so the UI
		// reports the intended configuration even when it failed to read
		set_current(name);
		return name;
	}
	for (const std::string &r : rejections)
		WEB_WARNING("Configuration \"%s\": %s", name.c_str(), r.c_str());
	WEB_INFO("Configuration \"%s\" applied (DIP %d), %u rejections.",
			name.c_str(), dip, (unsigned) rejections.size());
	return name;
}

void webconfigs_startup(const std::string &override_config) {
	// --config is an explicit override for bring-up and testing; otherwise the
	// machine comes up as the configuration the DIP switches select.
	if (!override_config.empty()) {
		apply_named_or_fallback(override_config, -1);
		return;
	}
	int dip = webevents_dip_value();
	apply_named_or_fallback(config_for_dip(dip), dip);
}

// /api/configs, /api/configs/<name>, /api/configs/<name>/apply

// ---- export and import ---------------------------------------------------
//
// A configuration is a machine setup, and a setup an operator has arrived at is
// worth keeping off the board it was built on. Two forms travel:
//
//  - the JSON document, which is what this module already stores and what an
//    import reads back; and
//  - a command script in the interactive menu's own format, for a board driven
//    from the menu rather than through the API.
//
// The document carries the title, the DIP binding and the dashboard layout
// alongside the device set, so an export is the whole configuration. What an
// import does with the first two differs, because they say something about the
// board rather than about the machine: the layout is restored as it stands, and
// the DIP binding only when no configuration on this board already claims that
// value -- two configurations answering one switch setting is exactly the
// ambiguity the binding exists to prevent.

// Render the device set as menu commands: select a device, set its parameters,
// enable it. Order matters -- a parameter is set while the device is out of the
// machine, then the enable installs it, which is the order the API takes too.
static std::string config_as_script(const std::string &name,
		const picojson::value &content) {
	std::string out;
	out += "# " + name;
	if (content.get("title").is<std::string>())
		out += " - " + content.get("title").get<std::string>();
	out += "\n";
	out += "# QUniLator configuration, as commands for the interactive menu.\n";
	out += "# Paste at the device menu prompt, or feed with the menu's script\n";
	out += "# facility. Devices are set up first and enabled afterwards, which\n";
	out += "# is the order a parameter change requires.\n\n";
	if (!content.get("devices").is<picojson::array>())
		return out;
	const picojson::array &devs = content.get("devices").get<picojson::array>();
	std::string enables;
	for (const picojson::value &dv : devs) {
		if (!dv.is<picojson::object>())
			continue;
		const picojson::object &d = dv.get<picojson::object>();
		if (d.find("name") == d.end() || !d.at("name").is<std::string>())
			continue;
		std::string dname = d.at("name").get<std::string>();
		out += "sd " + dname + "\n";
		if (d.find("params") != d.end() && d.at("params").is<picojson::object>()) {
			const picojson::object &ps = d.at("params").get<picojson::object>();
			for (picojson::object::const_iterator it = ps.begin(); it != ps.end(); ++it) {
				std::string v = it->second.is<std::string>()
						? it->second.get<std::string>() : it->second.to_str();
				out += "p " + it->first + " " + v + "\n";
			}
		}
		out += "\n";
		if (d.find("enabled") != d.end() && d.at("enabled").is<bool>()
				&& d.at("enabled").get<bool>())
			enables += "en " + dname + "\n";
	}
	if (!enables.empty())
		out += "# the cards go into the machine\n" + enables;
	return out;
}

// Send a document as a file the browser saves rather than renders.
static void send_attachment(struct mg_connection *conn, const std::string &body,
		const std::string &filename, const char *type) {
	mg_printf(conn,
			"HTTP/1.1 200 OK\r\nContent-Type: %s\r\n"
			"Content-Disposition: attachment; filename=\"%s\"\r\n"
			"Content-Length: %u\r\nConnection: close\r\n\r\n",
			type, filename.c_str(), (unsigned) body.size());
	mg_write(conn, body.data(), body.size());
}

// POST /api/configs/<name>/import  — the body is a configuration document.
//
// The name is the operator's choice and must be free: an import brings in a
// machine that was not here, and writing over one that was is what PUT is for.
static void config_import(struct mg_connection *conn, const std::string &name) {
	picojson::value doc;
	if (!read_json_body_full(conn, &doc) || !doc.is<picojson::object>()) {
		send_error(conn, 400, "body must be a configuration document");
		return;
	}
	picojson::value existing;
	std::string err;
	if (read_config(name, &existing, &err)) {
		send_error(conn, 409, "a configuration named \"" + name
				+ "\" is already here; import under another name");
		return;
	}
	picojson::object &o = doc.get<picojson::object>();
	// The DIP binding belongs to the board, not to the machine: keep it only
	// when this board has the switch value free, and say so when it does not.
	std::string dip_note;
	int dip = config_dip_value(doc);
	if (dip >= 0) {
		std::string holder = config_for_dip(dip);
		if (!holder.empty() && holder != name) {
			o.erase("dip_value");
			dip_note = "the DIP value " + std::to_string(dip)
					+ " is claimed by \"" + holder + "\", so the import is unbound";
		}
	}
	std::string error;
	int status = 422;
	if (!webconfigs_write(name, doc, /*from_live*/false, &error, &status)) {
		send_error(conn, status, error);
		return;
	}
	picojson::object res;
	res["ok"] = picojson::value(true);
	res["name"] = picojson::value(name);
	if (!dip_note.empty())
		res["note"] = picojson::value(dip_note);
	send_json(conn, 200, picojson::value(res));
}

static int api_configs_handler(struct mg_connection *conn, void * /*cbdata*/) {
	const struct mg_request_info *ri = mg_get_request_info(conn);
	std::string uri = ri->local_uri ? ri->local_uri : "";
	std::string rest = uri.substr(strlen("/api/configs"));
	std::string method = ri->request_method;

	if (rest.empty() || rest == "/") {
		if (method != "GET") {
			send_error(conn, 405, "GET required");
			return 405;
		}
		// ?current=1 renders the live setup in snapshot form, so a caller can
		// tell which saved configuration — if any — is the one loaded
		const char *query = ri->query_string;
		if (query != nullptr && strstr(query, "current") != nullptr) {
			picojson::value current;
			if (!snapshot_devices_now(&current, 500)) {
				send_error(conn, 503, "machine busy, current setup unavailable");
				return 503;
			}
			send_json(conn, 200, current);
		} else
			configs_list(conn);
		return 200;
	}

	std::string name = rest.substr(1);
	std::string action; // "apply", "rename", "title", "dip", "layout", or empty for the config itself
	// /<name>/devices/<device>/image
	std::string image_device;
	size_t devsep = name.find("/devices/");
	if (devsep != std::string::npos) {
		std::string tail = name.substr(devsep + strlen("/devices/"));
		name = name.substr(0, devsep);
		if (tail.size() < 7 || tail.compare(tail.size() - 6, 6, "/image") != 0) {
			send_error(conn, 404, "only the image of a device is editable");
			return 404;
		}
		image_device = tail.substr(0, tail.size() - 6);
		if (image_device.empty() || image_device.find('/') != std::string::npos) {
			send_error(conn, 404, "unknown device");
			return 404;
		}
	} else {
		size_t sep = name.rfind('/');
		if (sep != std::string::npos) {
			std::string tail = name.substr(sep + 1);
			if (tail == "apply" || tail == "rename" || tail == "title"
					|| tail == "dip" || tail == "layout" || tail == "import") {
				action = tail;
				name = name.substr(0, sep);
			}
		}
	}
	if (!valid_config_name(name)) {
		send_error(conn, 404, "unknown configuration");
		return 404;
	}

	if (!image_device.empty()) {
		if (method != "PUT") {
			send_error(conn, 405, "PUT required");
			return 405;
		}
		config_set_image(conn, name, image_device);
	} else if (action == "apply" && method == "POST")
		config_apply(conn, name);
	else if (action == "rename" && method == "POST") {
		picojson::value req;
		if (!read_json_body(conn, &req) || !req.get("name").is<std::string>()) {
			send_error(conn, 400, "body must be a JSON object with a string \"name\"");
			return 400;
		}
		std::string error;
		if (!webconfigs_rename(name, req.get("name").get<std::string>(), &error)) {
			// an existing target or invalid name is a conflict, not a missing config
			send_error(conn, 409, error);
			return 409;
		}
		picojson::object res;
		res["ok"] = picojson::value(true);
		send_json(conn, 200, picojson::value(res));
	} else if (action == "title" && method == "PUT") {
		config_set_title(conn, name);
	} else if (action == "dip" && method == "PUT") {
		config_set_dip(conn, name);
	} else if (action == "layout" && method == "PUT") {
		config_set_layout(conn, name);
	} else if (action == "import" && method == "POST") {
		config_import(conn, name);
	} else if (action.empty() && method == "PUT") {
		const char *query = ri->query_string;
		bool from_live = query != nullptr && strstr(query, "from=live") != nullptr;
		config_put(conn, name, from_live);
	} else if (action.empty() && method == "GET") {
		picojson::value content;
		std::string err;
		if (!read_config(name, &content, &err)) {
			send_error(conn, 404, err);
			return 404;
		}
		// ?export=json hands the same document back as a file to save;
		// ?export=script renders it as menu commands.
		const char *query = ri->query_string;
		std::string ex;
		if (query != nullptr) {
			const char *p = strstr(query, "export=");
			if (p != nullptr) {
				p += 7;
				while (*p != '\0' && *p != '&')
					ex.push_back(*p++);
			}
		}
		if (ex == "script")
			send_attachment(conn, config_as_script(name, content), name + ".cmd",
					"text/plain; charset=utf-8");
		else if (ex == "json")
			send_attachment(conn, content.serialize(true), name + ".qcfg.json",
					"application/json");
		else
			send_json(conn, 200, content);
	} else if (action.empty() && method == "DELETE") {
		std::string error;
		int status = 404;
		if (!webconfigs_delete(name, &error, &status)) {
			send_error(conn, status, error);
			return status;
		}
		picojson::object res;
		res["ok"] = picojson::value(true);
		send_json(conn, 200, picojson::value(res));
	} else {
		send_error(conn, 405, "unsupported method");
		return 405;
	}
	return 200;
}

// Locate the configuration directory and capture the parameter defaults an
// apply resets to. Separated from the HTTP registration so the host test can
// drive the model against a temporary directory without civetweb.
void webconfigs_init(const std::string &dir) {
	configs_dir = dir;
	mkdir(configs_dir.c_str(), 0755); // may already exist
	capture_parameter_defaults();
}

void webconfigs_register(struct mg_context *ctx) {
	const char *base = getenv("QUNILATOR_DIR");
	if (base == nullptr)
		base = getenv("HOME");
	webconfigs_init(std::string(base ? base : ".") + "/configs");
	mg_set_request_handler(ctx, "/api/configs", api_configs_handler, nullptr);
}
