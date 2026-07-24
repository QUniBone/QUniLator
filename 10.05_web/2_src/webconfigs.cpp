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
   *current* one: a runtime pointer set to the default at startup and updated
   whenever a configuration is applied or the live setup is saved under a name.
   The machine is *modified* when the live device set differs from the saved
   form of the current configuration; this is computed by comparison, not
   tracked at write time. The *default* configuration, applied at startup,
   lives in settings.json (websettings.cpp), separate from the configurations.

     GET    /api/configs               {current, default, modified, configs[]}
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

   Files live in $QUNIBONE_DIR/configs/<name>.json:

     {"devices":[{"name":"RL11","enabled":true,
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
#include "panel.hpp"
#include "mscp_server.hpp"
#include "device_configuration.hpp"

#include "weblog.hpp"
#include "webconfigs.hpp"
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

static bool device_is_infrastructure(device_c *d) {
	return dynamic_cast<qunibusadapter_c *>(d) != nullptr
			|| dynamic_cast<paneldriver_c *>(d) != nullptr
			|| dynamic_cast<mscp_server *>(d) != nullptr;
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

// an operator may set this, whatever a transient lock says right now
static bool is_settable(parameter_c *p) {
	std::map<parameter_c *, param_default_t>::iterator it = parameter_defaults.find(p);
	return it == parameter_defaults.end() ? !p->readonly : it->second.writable;
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

// A configuration describes the whole machine: it carries the devices that are
// switched on and, of those, only the parameters that differ from the
// construction defaults. Everything it does not mention is off and default.
//
// Caller holds operations_mutex and mydevices_mutex.
static picojson::value snapshot_devices_locked(void) {
	picojson::array devices;
	for (device_c *dev : device_c::mydevices) {
		if (device_is_infrastructure(dev) || !dev->enabled.value)
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
			if (!is_settable(p) || is_default(p))
				continue;
			params[p->name] = picojson::value(*p->render());
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
			e["params"] = d.get("params").is<picojson::object>()
					? d.get("params") : picojson::value(picojson::object());
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

void webconfigs_status(std::string *current, std::string *def, bool *modified,
		bool *busy) {
	if (current != nullptr)
		*current = webconfigs_current();
	if (def != nullptr)
		*def = websettings_default_config();
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
	return true;
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

static std::string basename_of(const std::string &path) {
	size_t base = path.rfind('/');
	return base == std::string::npos ? path : path.substr(base + 1);
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
			if (path.empty() || basename_of(path) != image_name)
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
	std::string def = websettings_default_config();
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
			o["default"] = picojson::value(name == def);
			// the enabled devices, as the card summary
			picojson::value content;
			std::string err;
			picojson::array enabled;
			if (read_config(name, &content, &err) && content.get("devices").is<picojson::array>())
				for (picojson::value &d : content.get("devices").get<picojson::array>())
					if (d.get("enabled").is<bool>() && d.get("enabled").get<bool>())
						enabled.push_back(d.get("name"));
			o["enabled"] = picojson::value(enabled);
			configs.push_back(picojson::value(o));
		}
		closedir(dir);
	}
	picojson::object root;
	root["current"] = picojson::value(webconfigs_current());
	root["default"] = picojson::value(def);
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
	if (!write_config_file(name, snapshot_devices().serialize(), error))
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
	if (!write_config_file(name, document.serialize(), error)) {
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
			if (device_image(d) != path)
				continue;
			std::string other = d.get("name").is<std::string>()
					? d.get("name").get<std::string>() : "another drive";
			send_error(conn, 409, "\"" + basename_of(path) + "\" is already the image of "
					+ other + " in this configuration");
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

// POST /api/configs/<name>/apply — restore a snapshot. Devices are stored
// in registry order (controllers before their drives), so applying in
// order enables controllers first. Rejections are collected, not fatal.
// Apply a saved configuration to the device set: the work behind both
// POST /api/configs/<name>/apply and the --config option of the service.
// Returns false when the configuration cannot be read; parameters the devices
// reject are collected in "errors" and do not fail the call.
static bool apply_config(const std::string &name, picojson::array *errors,
		std::string *error) {
	picojson::value content;
	if (!read_config(name, &content, error))
		return false;
	if (!content.get("devices").is<picojson::array>()) {
		*error = "configuration \"" + name + "\" has no devices";
		return false;
	}
	{
		std::lock_guard<std::mutex> ops_lock(device_configuration_c::operations_mutex);

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
			reset_to_defaults(dev, &listed, errors);

			if (d.get("params").is<picojson::object>())
				for (const std::pair<const std::string, picojson::value> &kv :
						d.get("params").get<picojson::object>()) {
					if (!kv.second.is<std::string>())
						continue;
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
			if (d.get("enabled").is<bool>())
				dev->enabled.set(d.get("enabled").get<bool>());
		}
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
	if (!apply_config(name, &errors, &error)) {
		send_error(conn, 404, error);
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
	if (!apply_config(name, &errors, error))
		return false;
	set_current(name);
	if (rejections != nullptr)
		for (picojson::value &e : errors)
			rejections->push_back(e.is<std::string>() ? e.get<std::string>() : "?");
	return true;
}

// Rename the file, and let the current/default pointers follow it. It is a
// file/metadata operation only: the live device set is untouched, so a machine
// modified against <from> stays modified against <to>.
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
	if (websettings_default_config() == from)
		websettings_set_default_config(to);
	WEB_INFO("configuration \"%s\" renamed to \"%s\"", from.c_str(), to.c_str());
	webevents_note_config(); // current and/or default may now name <to>
	return true;
}

// Remove a configuration. The current and the default are protected: a valid
// default must always exist, and the current names the running machine. On
// refusal *status is 409, on an unknown name 404.
bool webconfigs_delete(const std::string &name, std::string *error, int *status) {
	if (name == webconfigs_current()) {
		if (error != nullptr)
			*error = "configuration \"" + name
					+ "\" is the current one; switch to another first";
		if (status != nullptr)
			*status = 409;
		return false;
	}
	if (name == websettings_default_config()) {
		if (error != nullptr)
			*error = "configuration \"" + name
					+ "\" is the default; designate another first";
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

// Designate <name> the startup default, persisting settings.json.
bool webconfigs_set_default(const std::string &name, std::string *error) {
	struct stat st;
	if (!valid_config_name(name) || stat(config_path(name).c_str(), &st) != 0) {
		if (error != nullptr)
			*error = "unknown configuration \"" + name + "\"";
		return false;
	}
	websettings_set_default_config(name);
	WEB_INFO("configuration \"%s\" is now the default", name.c_str());
	webevents_note_config();
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

void webconfigs_startup(const std::string &override_config) {
	// --config is an explicit override for bring-up and testing; otherwise the
	// machine comes up as the designated default.
	std::string name = override_config;
	if (name.empty()) {
		name = websettings_default_config();
		struct stat st;
		// unset, or naming a file that is gone: adopt the bundled empty config
		// and record it, so a valid default always exists thereafter.
		if (name.empty() || !valid_config_name(name)
				|| stat(config_path(name).c_str(), &st) != 0) {
			ensure_fallback_config();
			name = fallback_config_name;
			websettings_set_default_config(name);
		}
	}
	std::vector<std::string> rejections;
	std::string error;
	if (!webconfigs_apply(name, &rejections, &error)) {
		WEB_ERROR("Startup configuration \"%s\" not applied: %s",
				name.c_str(), error.c_str());
		// keep the current pointer pointed at what was asked for, so the UI
		// reports the intended configuration even when it failed to read
		set_current(name);
		return;
	}
	for (const std::string &r : rejections)
		WEB_WARNING("Startup configuration \"%s\": %s", name.c_str(), r.c_str());
	WEB_INFO("Startup configuration \"%s\" applied, %u rejections.",
			name.c_str(), (unsigned) rejections.size());
}

// /api/configs, /api/configs/<name>, /api/configs/<name>/apply
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
	std::string action; // "apply", "rename", "default", or empty for the config itself
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
			if (tail == "apply" || tail == "rename" || tail == "default") {
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
	} else if (action == "default" && method == "PUT") {
		std::string error;
		if (!webconfigs_set_default(name, &error)) {
			send_error(conn, 404, error);
			return 404;
		}
		picojson::object res;
		res["ok"] = picojson::value(true);
		send_json(conn, 200, picojson::value(res));
	} else if (action.empty() && method == "PUT") {
		const char *query = ri->query_string;
		bool from_live = query != nullptr && strstr(query, "from=live") != nullptr;
		config_put(conn, name, from_live);
	} else if (action.empty() && method == "GET") {
		picojson::value content;
		std::string err;
		if (!read_config(name, &content, &err))
			send_error(conn, 404, err);
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
	const char *base = getenv("QUNIBONE_DIR");
	if (base == nullptr)
		base = getenv("HOME");
	webconfigs_init(std::string(base ? base : ".") + "/configs");
	mg_set_request_handler(ctx, "/api/configs", api_configs_handler, nullptr);
}
