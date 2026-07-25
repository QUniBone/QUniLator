/* config_test.cpp: host test of the configuration model (webconfigs.cpp)

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see any source file header for the full text.

   The configuration model operates on the device_c / parameter_c abstraction,
   not on the PRU, so it is testable on the development host over a synthetic
   device set. This links the real webconfigs.cpp against:

     - the real parameter.cpp / bitcalc.cpp (typed parameters),
     - a handful of stub device_c instances with real parameter_c members,
     - lightweight stubs for the pieces webconfigs only names in a dynamic_cast
       (bus adapter, panel, MSCP server) or a persistence backend (websettings's
       default_config, the event hook, the image-path resolver).

   The model functions are driven directly, without civetweb: civetweb.o is
   linked only so the (unused) HTTP handlers resolve.

   Build & run: 10.05_web/tools/run_config_test.sh
*/

#include <algorithm>
#include <fstream>
#include <list>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "picojson.h"

#include "logger.hpp"
#include "logsource.hpp"
#include "device.hpp"
#include "parameter.hpp"

#include "device_configuration.hpp"
#include "device_label.hpp"
#include "webconfigs.hpp"
#include "websettings.hpp"
#include "weblogging.hpp"
#include "webevents.hpp"
#include "webstorage.hpp"

/*** logger / logsource stubs: quiet unless something logs an error ***/
logger_c *logger = nullptr;

logger_c::logger_c() {
	fifo = nullptr;
	fifo_capacity = fifo_readidx = fifo_writeidx = fifo_fill = 0;
	messagecount = 0;
	life_level = LL_ERROR;
}
logger_c::~logger_c() {}
void logger_c::vlog(logsource_c *logsource, unsigned msglevel, bool,
		const char *, unsigned, const char *fmt, va_list args) {
	fprintf(stderr, "[%s %u] ", logsource->log_label.c_str(), msglevel);
	vfprintf(stderr, fmt, args);
	fprintf(stderr, "\n");
}
// The real logger registers each source and seeds its level with the global
// default; the logging-control layer walks the registered set. The stub keeps
// the same behaviour over the (otherwise unused) logsources vector so the
// synthetic sources of the logging test appear in list_logsources().
void logger_c::add_source(logsource_c *logsource) {
	*(logsource->log_level_ptr) = default_level;
	logsources.push_back(logsource);
}
void logger_c::remove_source(logsource_c *logsource) {
	for (logsource_c *&s : logsources)
		if (s == logsource)
			s = nullptr;
}
void logger_c::reset_log_levels(void) {
	for (logsource_c *s : logsources)
		if (s != nullptr)
			*(s->log_level_ptr) = default_level;
}
std::vector<logger_c::logsource_ref_t> logger_c::list_logsources(void) {
	std::vector<logsource_ref_t> result;
	for (logsource_c *s : logsources)
		if (s != nullptr)
			result.push_back({ s, s->log_label, s->log_level_ptr });
	return result;
}

logsource_c::logsource_c() {
	log_level = LL_ERROR; // suppress INFO/WARNING chatter from the model
	log_level_ptr = &log_level;
	log_id = 0;
}
logsource_c::~logsource_c() {}
void logsource_c::connect() {}
void logsource_c::disconnect() {}

/*** device_c: a hardware-free stand-in that keeps the parameter machinery but
     starts no worker threads. Mirrors the real constructor's parameter
     registration so a snapshot sees the same built-in parameters. ***/
std::list<device_c *> device_c::mydevices;
std::mutex device_c::mydevices_mutex;

device_c::device_c() {
	{
		std::lock_guard<std::mutex> lock(mydevices_mutex);
		mydevices.push_back(this);
	}
	parent = NULL;
	workers_terminate = false;
	name.parameterized = this;
	type_name.parameterized = this;
	enabled.parameterized = this;
	verbosity.parameterized = this;
	enabled.value = false;
	param_add(&name);
	param_add(&type_name);
	param_add(&enabled);
	param_add(&emulation_speed);
	param_add(&verbosity);
	emulation_speed.value = 1;
	init_asserted = false;
	log_label = name.value;
	log_level_ptr = &(verbosity.value);
}

device_c::~device_c() {
	parameter.clear();
	std::lock_guard<std::mutex> lock(mydevices_mutex);
	std::list<device_c *>::iterator p = std::find(mydevices.begin(), mydevices.end(), this);
	if (p != mydevices.end())
		mydevices.erase(p);
}

// The real device starts/stops workers here; the stub just accepts the value.
bool device_c::on_param_changed(parameter_c *) {
	return true;
}

/*** the pieces webconfigs.cpp links against, reduced to what it uses ***/
std::mutex device_configuration_c::operations_mutex;

static std::string g_default_config; // stands in for settings.json default_config
std::string websettings_default_config(void) { return g_default_config; }
void websettings_set_default_config(const std::string &name) { g_default_config = name; }

// weblogging.cpp persists on every write; the test keeps no settings file
void websettings_save(void) {}

void webevents_note_config(void) {} // the event stream is not part of this test

std::string webstorage_image_path(const std::string &name) { return name; }

/*** a synthetic device: the standard built-ins plus a disk image and an
     address, the writable parameters a configuration snapshot captures ***/
struct test_device_c : public device_c {
	parameter_string_c image = parameter_string_c(this, "image", "img", false,
			"disk image");
	parameter_unsigned_c address = parameter_unsigned_c(this, "address", "addr",
			false, "", "%o", "controller address", 18, 8);
	// running-state parameters: an activity LED the device keeps as a settable
	// LED number yet moves as I/O runs, and a read-only panel lamp. Neither is
	// configuration; the snapshot and the modified comparison must ignore both.
	parameter_unsigned_c activity_led = parameter_unsigned_c(this, "activityled",
			"al", false, "", "%d", "activity LED number", 8, 10);
	parameter_bool_c access_lamp = parameter_bool_c(this, "accesslamp", "acl",
			true, "state of the ACCESS lamp");

	test_device_c(const char *nm, const char *ty) {
		name.value = nm;
		type_name.value = ty;
		// running state the emulator drives, classified by kind — the settable
		// activity LED and the read-only access lamp are both PARAM_STATUS
		activity_led.kind = parameter_c::PARAM_STATUS;
		access_lamp.kind = parameter_c::PARAM_STATUS;
	}
	void on_power_changed(signal_edge_enum, signal_edge_enum) override {}
	void on_init_changed(void) override {}
};

static test_device_c *rl, *rl0, *rl1, *dl;

static void reset_devices(void) {
	for (device_c *d : device_c::mydevices) {
		d->enabled.value = false;
		test_device_c *t = dynamic_cast<test_device_c *>(d);
		if (t != nullptr) {
			t->image.set("");
			t->address.set(0);
		}
	}
}

/*** test scaffolding ***/
static int failures = 0;
static int checks = 0;

static void check(bool ok, const char *what) {
	checks++;
	if (!ok) {
		failures++;
		fprintf(stderr, "FAIL: %s\n", what);
	}
}

static std::string configs_dir;
static std::string cfg_path(const std::string &name) {
	return configs_dir + "/" + name + ".json";
}
static bool file_exists(const std::string &path) {
	struct stat st;
	return stat(path.c_str(), &st) == 0;
}
static bool read_json_file(const std::string &path, picojson::value *out) {
	std::ifstream in(path.c_str());
	if (!in.is_open())
		return false;
	std::stringstream ss;
	ss << in.rdbuf();
	return picojson::parse(*out, ss.str()).empty();
}

// the entry for device "name" in a saved snapshot, or nullptr
static const picojson::value *snap_device(const picojson::value &snap,
		const std::string &name) {
	if (!snap.get("devices").is<picojson::array>())
		return nullptr;
	const picojson::array &devs = snap.get("devices").get<picojson::array>();
	for (const picojson::value &d : devs)
		if (d.get("name").is<std::string>() && d.get("name").get<std::string>() == name)
			return &d;
	return nullptr;
}

// build a config document {"devices":[…]} for the bulk-PUT tests
static picojson::value dev_entry(const std::string &name, bool enabled,
		const std::vector<std::pair<std::string, std::string> > &params) {
	picojson::object o;
	o["name"] = picojson::value(name);
	o["enabled"] = picojson::value(enabled);
	picojson::object p;
	for (const std::pair<std::string, std::string> &kv : params)
		p[kv.first] = picojson::value(kv.second);
	o["params"] = picojson::value(p);
	return picojson::value(o);
}
static picojson::value make_doc(const std::vector<picojson::value> &devs) {
	picojson::array a(devs.begin(), devs.end());
	picojson::object root;
	root["devices"] = picojson::value(a);
	return picojson::value(root);
}
static std::string dev_param(const picojson::value &snap, const std::string &dev,
		const std::string &param) {
	const picojson::value *d = snap_device(snap, dev);
	if (d == nullptr || !d->get("params").is<picojson::object>())
		return "";
	const picojson::object &p = d->get("params").get<picojson::object>();
	picojson::object::const_iterator it = p.find(param);
	return it == p.end() || !it->second.is<std::string>() ? "" : it->second.get<std::string>();
}

static bool modified_now(void) {
	bool m = false, busy = false;
	webconfigs_status(nullptr, nullptr, &m, &busy);
	check(!busy, "machine not busy for modified check");
	return m;
}

int main(void) {
	logger = new logger_c();

	char tmpl[] = "/tmp/qbone_cfgtest_XXXXXX";
	char *dir = mkdtemp(tmpl);
	if (dir == nullptr) {
		perror("mkdtemp");
		return 2;
	}
	configs_dir = std::string(dir) + "/configs";

	// Devices exist before init so their construction-time values are captured
	// as the defaults an apply resets to.
	rl = new test_device_c("rl", "RL11");
	rl0 = new test_device_c("rl0", "RL02");
	rl1 = new test_device_c("rl1", "RL02");
	dl = new test_device_c("dl", "DL11");

	webconfigs_init(configs_dir);

	/* 1. save captures the enabled devices and only their non-default params */
	reset_devices();
	rl->enabled.value = true;
	rl0->enabled.value = true;
	rl0->image.set("rt11.rl02");
	{
		std::string err;
		check(webconfigs_save("cfgA", &err), "save cfgA");
		check(webconfigs_current() == "cfgA", "save makes cfgA current");

		picojson::value snap;
		check(read_json_file(cfg_path("cfgA"), &snap), "cfgA file written");
		check(snap_device(snap, "rl") != nullptr, "cfgA has enabled rl");
		check(snap_device(snap, "rl0") != nullptr, "cfgA has enabled rl0");
		check(snap_device(snap, "rl1") == nullptr, "cfgA omits disabled rl1");
		check(snap_device(snap, "dl") == nullptr, "cfgA omits disabled dl");
		const picojson::value *d0 = snap_device(snap, "rl0");
		if (d0 != nullptr) {
			const picojson::object &p = d0->get("params").get<picojson::object>();
			check(p.count("image") == 1 && p.at("image").get<std::string>() == "rt11.rl02",
					"cfgA rl0 keeps the non-default image");
		}
		const picojson::value *dctrl = snap_device(snap, "rl");
		if (dctrl != nullptr)
			check(dctrl->get("params").get<picojson::object>().count("address") == 0,
					"cfgA rl omits the default address");
	}

	/* 2. apply switches off unnamed devices, resets to defaults, sets current */
	rl1->enabled.value = true;
	rl1->image.set("games.rl02");
	rl0->image.set("scratch.rl02"); // diverge before restoring
	{
		std::vector<std::string> rej;
		std::string err;
		check(webconfigs_apply("cfgA", &rej, &err), "apply cfgA");
		check(webconfigs_current() == "cfgA", "apply sets cfgA current");
		check(rl->enabled.value, "apply keeps rl enabled");
		check(rl0->enabled.value, "apply keeps rl0 enabled");
		check(!rl1->enabled.value, "apply switches rl1 off (unnamed)");
		check(rl1->image.value == "", "apply resets rl1 image to default");
		check(rl0->image.value == "rt11.rl02", "apply restores rl0 image");
	}

	/* 3. modified is false after apply/save, true after a parameter change */
	check(!modified_now(), "not modified right after apply");
	rl0->image.set("edited.rl02");
	check(modified_now(), "modified after a parameter change");
	{
		std::vector<std::string> rej;
		std::string err;
		check(webconfigs_apply("cfgA", &rej, &err), "revert via apply(current)");
		check(!modified_now(), "not modified after revert");
	}

	/* 3-runtime. running-state parameters — an activity LED (settable) and a
	   panel lamp (read-only) — are PARAM_STATUS, not configuration. On a clean
	   machine they move as the emulation runs; neither may read as an operator
	   edit, and neither enters a saved snapshot. The settable activity LED is
	   excluded by kind, not by its name — a settable status value would
	   otherwise flip modified with no edit, the false positive the dashboard
	   showed. */
	{
		check(!modified_now(), "clean before a running-state change");
		rl0->activity_led.set(7);      // a settable running value diverges
		rl0->access_lamp.value = true; // a read-only lamp lights, as the device does it
		check(!modified_now(), "running-state change does not read as modified");

		std::string err;
		check(webconfigs_save("runcfg", &err), "save with running state active");
		picojson::value snap;
		check(read_json_file(cfg_path("runcfg"), &snap), "runcfg written");
		const picojson::value *d0 = snap_device(snap, "rl0");
		if (d0 != nullptr) {
			const picojson::object &p = d0->get("params").get<picojson::object>();
			check(p.count("activityled") == 0, "snapshot omits the activity LED by kind");
			check(p.count("accesslamp") == 0, "snapshot omits the access lamp by kind");
		}
		// saving made runcfg current; restore the clean cfgA the next sections
		// expect and confirm the reverted machine reads unmodified even with the
		// lamp still lit.
		std::vector<std::string> rej;
		check(webconfigs_apply("cfgA", &rej, &err), "restore cfgA current");
		check(webconfigs_current() == "cfgA", "cfgA current again");
		check(!modified_now(), "clean cfgA after the running-state test");
	}

	/* 3-legacy. a configuration written by the old logic — carrying a settable
	   running-state parameter (activityled) the current snapshot no longer
	   emits — must still compare unmodified against the live setup. The
	   comparison drops it by the live device's kind, not its name, so an
	   untouched machine loaded from such a file reads modified:false. */
	{
		// hand-write cfgA's device set plus a stray activityled on rl0
		picojson::value base;
		check(read_json_file(cfg_path("cfgA"), &base), "read cfgA to seed a legacy file");
		picojson::array &devs = base.get<picojson::object>()["devices"].get<picojson::array>();
		for (picojson::value &d : devs)
			if (d.get("name").is<std::string>() && d.get("name").get<std::string>() == "rl0") {
				if (!d.get("params").is<picojson::object>())
					d.get<picojson::object>()["params"] = picojson::value(picojson::object());
				d.get<picojson::object>()["params"].get<picojson::object>()["activityled"] =
						picojson::value(std::string("7"));
			}
		{
			std::ofstream out(cfg_path("legacy").c_str());
			out << base.serialize();
		}
		std::vector<std::string> rej;
		std::string err;
		check(webconfigs_apply("legacy", &rej, &err), "apply legacy cfg carrying activityled");
		check(webconfigs_current() == "legacy", "legacy is current");
		// the live rl0 activity LED may sit anywhere; the saved status param must
		// not count as an operator edit
		rl0->activity_led.set(2);
		check(!modified_now(), "legacy status param does not read as modified");

		check(webconfigs_apply("cfgA", &rej, &err), "restore cfgA current after legacy test");
		check(webconfigs_current() == "cfgA", "cfgA current after legacy test");
		check(!modified_now(), "clean cfgA after the legacy test");
	}

	/* 3b. stored-config editing via webconfigs_write: a bulk write of the whole
	       document, validated, that does not move the current pointer or touch
	       the machine unless it is the live setup being saved (from_live). The
	       machine is clean on cfgA here (section 3 reverted it). */
	{
		std::vector<picojson::value> devs;
		devs.push_back(dev_entry("rl", true, { { "address", "160010" } }));
		devs.push_back(dev_entry("rl0", true, { { "image", "stored.rl02" } }));
		picojson::value doc = make_doc(devs);

		// a. a bulk edit of a non-current stored config validates, writes the
		//    document, and leaves the current pointer and the live machine alone
		std::string err;
		int status = 0;
		check(webconfigs_write("stored1", doc, false, &err, &status), "bulk PUT writes stored1");
		check(status == 200, "bulk PUT reports 200");
		check(webconfigs_current() == "cfgA", "stored edit leaves current at cfgA");
		check(rl0->image.value == "rt11.rl02", "stored edit does not touch the live machine");
		picojson::value snap;
		check(read_json_file(cfg_path("stored1"), &snap), "stored1 file written");
		check(dev_param(snap, "rl", "address") == "160010", "stored1 carries the edited address");
		check(dev_param(snap, "rl0", "image") == "stored.rl02", "stored1 carries the edited image");

		// b. an unknown device is refused (422) and the file is unchanged
		std::vector<picojson::value> bad;
		bad.push_back(dev_entry("nosuch", true, { }));
		err.clear();
		status = 0;
		check(!webconfigs_write("stored1", make_doc(bad), false, &err, &status),
				"unknown device refused");
		check(status == 422, "unknown device answers 422");
		picojson::value after;
		check(read_json_file(cfg_path("stored1"), &after), "stored1 still present after rejection");
		check(snap_device(after, "rl") != nullptr && snap_device(after, "nosuch") == nullptr,
				"stored1 unchanged after the rejected write");

		// c. an unknown parameter is refused (422)
		std::vector<picojson::value> badp;
		badp.push_back(dev_entry("rl", true, { { "frobnicate", "1" } }));
		err.clear();
		status = 0;
		check(!webconfigs_write("stored1", make_doc(badp), false, &err, &status),
				"unknown parameter refused");
		check(status == 422, "unknown parameter answers 422");

		// d. from=live moves the current pointer and clears modified; the body it
		//    stores is the live snapshot, so saved == live afterwards
		check(webconfigs_save("live1", &err), "capture the live setup into live1");
		picojson::value livedoc;
		check(read_json_file(cfg_path("live1"), &livedoc), "read back the live snapshot");
		err.clear();
		status = 0;
		check(webconfigs_write("live2", livedoc, true, &err, &status), "from=live writes live2");
		check(webconfigs_current() == "live2", "from=live moves current to live2");
		check(!modified_now(), "from=live clears the modified state");

		// e. a stored edit of a non-current config does not move the current pointer
		err.clear();
		status = 0;
		check(webconfigs_write("stored1", doc, false, &err, &status),
				"stored edit of a non-current config");
		check(webconfigs_current() == "live2", "stored edit leaves current at live2");

		// f. a stored edit of the CURRENT config writes the file without clearing
		//    the live modified state or touching the machine
		rl0->image.set("dirty.rl02"); // diverge the live setup from live2's saved form
		check(modified_now(), "machine modified against live2");
		std::vector<picojson::value> cur;
		cur.push_back(dev_entry("rl", true, { }));
		cur.push_back(dev_entry("rl0", true, { { "image", "other.rl02" } }));
		err.clear();
		status = 0;
		check(webconfigs_write("live2", make_doc(cur), false, &err, &status),
				"stored edit of the current config");
		check(webconfigs_current() == "live2", "stored edit of current keeps the pointer");
		check(rl0->image.value == "dirty.rl02", "stored edit of current does not touch the machine");
		check(modified_now(), "stored edit of current leaves modified set");
		picojson::value cursnap;
		check(read_json_file(cfg_path("live2"), &cursnap), "live2 file present");
		check(dev_param(cursnap, "rl0", "image") == "other.rl02", "live2 carries the offline edit");

		// restore a clean current cfgA for the sections that follow
		std::vector<std::string> rej;
		webconfigs_apply("cfgA", &rej, &err);
		check(webconfigs_current() == "cfgA", "restored current cfgA");
		check(!modified_now(), "clean cfgA for the following sections");
	}

	/* 4. rename moves the file and carries the current/default pointers; the
	      live dirty state is unaffected */
	{
		std::string err;
		check(webconfigs_set_default("cfgA", &err), "designate cfgA default");
		check(websettings_default_config() == "cfgA", "cfgA is default");
	}
	rl0->image.set("dirty.rl02"); // make the machine modified against cfgA
	check(modified_now(), "modified before rename");
	{
		std::string err;
		check(webconfigs_rename("cfgA", "cfgB", &err), "rename cfgA to cfgB");
		check(!file_exists(cfg_path("cfgA")), "cfgA file gone after rename");
		check(file_exists(cfg_path("cfgB")), "cfgB file present after rename");
		check(webconfigs_current() == "cfgB", "current follows rename");
		check(websettings_default_config() == "cfgB", "default follows rename");
		check(modified_now(), "still modified against the renamed config");
	}
	{
		std::vector<std::string> rej;
		std::string err;
		webconfigs_apply("cfgB", &rej, &err); // clean the machine again
	}

	/* 5. delete refuses the current and the default */
	{
		std::string err;
		int status = 0;
		// cfgB is both current and default
		check(!webconfigs_delete("cfgB", &err, &status), "delete refuses current");
		check(status == 409, "delete of current answers 409");

		// make cfgC the current, leaving cfgB the default
		rl0->image.set("c.rl02");
		check(webconfigs_save("cfgC", &err), "save cfgC");
		check(webconfigs_current() == "cfgC", "cfgC current");
		status = 0;
		check(!webconfigs_delete("cfgB", &err, &status), "delete refuses default");
		check(status == 409, "delete of default answers 409");

		/* 6. default protection: reassign, then the old default can go */
		check(webconfigs_set_default("cfgC", &err), "designate cfgC default");
		status = 0;
		check(webconfigs_delete("cfgB", &err, &status), "delete cfgB once neither current nor default");
		check(!file_exists(cfg_path("cfgB")), "cfgB removed");
	}

	/* 7. startup applies the default; a missing default adopts the fallback */
	websettings_set_default_config(""); // pretend a board that never set one
	webconfigs_startup("");
	check(websettings_default_config() == "default", "missing default adopts the fallback name");
	check(webconfigs_current() == "default", "startup makes the fallback current");
	check(file_exists(cfg_path("default")), "fallback default.json written");
	check(!rl->enabled.value && !rl0->enabled.value, "empty fallback switches everything off");

	// a set default is applied and made current
	{
		std::string err;
		check(webconfigs_set_default("cfgC", &err), "designate cfgC default for restart");
	}
	webconfigs_startup("");
	check(webconfigs_current() == "cfgC", "startup applies the designated default");
	check(websettings_default_config() == "cfgC", "default unchanged by startup");

	// --config overrides the default without changing it
	webconfigs_startup("default");
	check(webconfigs_current() == "default", "--config overrides the default");
	check(websettings_default_config() == "cfgC", "override leaves the default alone");

	/* 8. device_label: the static per-type table renders each shape correctly.
	      Fed the (type_name, name, unit, base_addr) triple the API extracts. */
	{
		// controller / standalone: bare role, no unit, no address
		check(device_label("UDA50", "uda", "", 0760334) == "MSCP disk controller (UDA50)",
				"label: controller shows the bare role");
		check(device_label("RLV12", "rl", "", 0774400) == "RL disk controller (RLV12)",
				"label: RL controller bare role");
		// instanced drive: unit appended before the code
		check(device_label("RA81", "uda0", "0", 0) == "MSCP disk 0 (RA81)",
				"label: instanced drive appends its unit");
		check(device_label("RL02", "rl1", "1", 0) == "RL02 cartridge disk 1 (RL02)",
				"label: RL02 drive appends its unit");
		// the two serial lines: disambiguated by CSR address
		check(device_label("slu_c", "DL11", "", 0777560) == "Serial line unit @777560 (DL11)",
				"label: DL11 carries its CSR address");
		check(device_label("slu_c", "DL11b", "", 0776500) == "Serial line unit @776500 (DL11)",
				"label: DL11b carries its CSR address");
		// internal device with no DEC code: bare role, no parentheses
		check(device_label("blinkenbone_c", "panel", "", 0) == "Front panel",
				"label: internal device has no code parentheses");
		check(device_label("RX0102uCPU", "rxcpu", "", 0) == "Floppy microcontroller",
				"label: floppy microcontroller names an internal device");
		// DELQA is instanced; a lone interface omits the unit
		check(device_label("DELQA", "delqa", "", 017774440) == "Ethernet controller (DELQA)",
				"label: lone DELQA omits its unit");
		// unknown type: raw handle fallback
		check(device_label("mystery_c", "widget0", "", 0) == "widget0",
				"label: unknown type falls back to the raw handle");
	}

	/* 9. logging control (weblogging.cpp): the global default reaches every
	      un-overridden source, an override sets one source and clears back to
	      the default, a configuration apply is followed by re-assertion so the
	      stored override wins, and an override for a not-yet-registered source
	      is applied when it appears. */
	{
		// a clean model: default warning, no overrides
		picojson::value seed;
		picojson::parse(seed, std::string("{\"default\":\"warning\",\"sources\":{}}"));
		weblogging_load(seed);

		// two subsystem sources and one device source
		logsource_c pru;
		pru.log_label = "PRU";
		logsource_c websrc;
		websrc.log_label = "web";
		test_device_c *ndev = new test_device_c("delqa", "DELQA");
		ndev->log_label = "delqa";
		logger->add_source(&pru);
		logger->add_source(&websrc);
		logger->add_source(ndev);

		reset_devices();
		webconfigs_init(configs_dir); // capture delqa's defaults now it exists

		// the global default reaches every source, device and subsystem alike
		{
			std::string err;
			check(weblogging_set_default("info", &err), "set default info");
		}
		check(*pru.log_level_ptr == LL_INFO, "default reaches subsystem PRU");
		check(*websrc.log_level_ptr == LL_INFO, "default reaches subsystem web");
		check(ndev->verbosity.value == LL_INFO, "default reaches device delqa");

		// an override sets one source and leaves the rest at the default
		{
			std::string err;
			check(weblogging_set_source("PRU", picojson::value(std::string("debug")), &err),
					"override PRU to debug");
		}
		check(*pru.log_level_ptr == LL_DEBUG, "override sets PRU to debug");
		check(*websrc.log_level_ptr == LL_INFO, "override leaves web at default");
		check(ndev->verbosity.value == LL_INFO, "override leaves delqa at default");

		// clearing the override returns the source to the default
		{
			std::string err;
			check(weblogging_set_source("PRU", picojson::value(), &err), "clear PRU override");
		}
		check(*pru.log_level_ptr == LL_INFO, "clear returns PRU to default");

		// device vs subsystem is reported from where the level lives
		{
			bool pru_sub = false, delqa_dev = false;
			for (const weblogging_source_t &s : weblogging_sources()) {
				if (s.label == "PRU" && s.kind == "subsystem") pru_sub = true;
				if (s.label == "delqa" && s.kind == "device") delqa_dev = true;
			}
			check(pru_sub, "PRU reported as subsystem");
			check(delqa_dev, "delqa reported as device");
		}

		// a configuration apply resets device verbosity to its construction
		// default; the re-assert at the end of the apply restores the stored
		// override. verbosity stays out of the saved snapshot.
		{
			std::string err;
			check(weblogging_set_source("delqa", picojson::value(std::string("debug")), &err),
					"override delqa to debug");
		}
		check(ndev->verbosity.value == LL_DEBUG, "delqa override takes effect");
		reset_devices();
		ndev->enabled.value = true;
		{
			std::string err;
			check(webconfigs_save("logcfg", &err), "save logcfg");
			picojson::value snap;
			check(read_json_file(cfg_path("logcfg"), &snap), "logcfg written");
			const picojson::value *d = snap_device(snap, "delqa");
			check(d != nullptr, "logcfg names delqa");
			if (d != nullptr)
				check(d->get("params").get<picojson::object>().count("verbosity") == 0,
						"logcfg omits verbosity");
		}
		ndev->verbosity.value = LL_FATAL; // a value neither default nor the override
		{
			std::vector<std::string> rej;
			std::string err;
			check(webconfigs_apply("logcfg", &rej, &err), "apply logcfg");
		}
		check(ndev->verbosity.value == LL_DEBUG,
				"apply re-asserts the stored delqa override");

		// an override for a not-yet-registered label is retained and applied
		// when that source appears
		{
			std::string err;
			check(weblogging_set_source("late", picojson::value(std::string("error")), &err),
					"store override for an absent source");
		}
		logsource_c late;
		late.log_label = "late";
		logger->add_source(&late);
		weblogging_apply();
		check(*late.log_level_ptr == LL_ERROR,
				"retained override applied when the source appears");

		logger->remove_source(&pru);
		logger->remove_source(&websrc);
		logger->remove_source(ndev);
		logger->remove_source(&late);
	}

	// tidy the temp tree
	for (const char *n : {"cfgA", "cfgB", "cfgC", "default", "legacy", "logcfg",
			"runcfg", "stored1", "live1", "live2"})
		unlink(cfg_path(n).c_str());
	rmdir(configs_dir.c_str());
	rmdir(dir);

	printf("%d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
