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
#include "qunibusdevice.hpp"

#include "device_configuration.hpp"
#include "device_label.hpp"
#include "webconfigs.hpp"
#include "websettings.hpp"
#include "weblogging.hpp"
#include "webevents.hpp"
#include "webpower.hpp"
#include "webstorage.hpp"

/*** logger / logsource stubs: quiet unless something logs an error ***/
logger_c *logger = nullptr;

logger_c::logger_c() {
	fifo = nullptr;
	fifo_capacity = fifo_readidx = fifo_writeidx = fifo_fill = 0;
	messagecount = 0;
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

// weblogging.cpp persists on every write; the test keeps no settings file
void websettings_save(void) {}

void webevents_note_config(void) {} // the event stream is not part of this test

// the board's DIP switches, which pick the power-on configuration; the test
// drives the value directly to stand in for the switch hardware
static int g_dip_value = -1;
int webevents_dip_value(void) { return g_dip_value; }

std::string webstorage_image_path(const std::string &name) { return name; }
std::string webstorage_image_subpath(const std::string &name) { return name; }

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

/*** a synthetic bus device: a backplane slot, and the arbitration requests
     whose own slots give the device its footprint. A device whose receive and
     transmit interrupts arbitrate separately occupies two adjacent slots. ***/
struct test_busdevice_c : public qunibusdevice_c {
	intr_request_c rcv_intr;
	intr_request_c xmt_intr;

	test_busdevice_c(const char *nm, const char *ty, unsigned slot, bool two_slots) {
		name.value = nm;
		type_name.value = ty;
		default_priority_slot = slot;
		priority_slot.value = slot;
		rcv_intr.priority_slot = slot;
		intr_requests.push_back(&rcv_intr);
		if (two_slots) {
			xmt_intr.priority_slot = slot + 1;
			intr_requests.push_back(&xmt_intr);
		}
	}
	void on_power_changed(signal_edge_enum, signal_edge_enum) override {}
	void on_init_changed(void) override {}
};

/*** a synthetic controller: switching it off takes its drives with it, the way
     a storage controller unplugged from the bus takes the drives that hang off
     it (storagecontroller_c::on_after_uninstall). A power-down that read a
     drive after its controller had gone would never see the drive, and would
     leave it out of the machine when the power came back. ***/
struct test_controller_c : public test_device_c {
	std::vector<test_device_c *> drives;

	test_controller_c(const char *nm, const char *ty) : test_device_c(nm, ty) {}

	bool on_param_changed(parameter_c *param) override {
		if (param == &enabled && !enabled.new_value)
			for (test_device_c *d : drives)
				d->enabled.set(false);
		return device_c::on_param_changed(param);
	}
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
	webconfigs_status(nullptr, &m, &busy);
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

	/* 1b. the operator title: stored in the file, surviving a save that omits
	       it, and carried in the list; a configuration with none falls back to
	       its name. Title edits are file metadata, so they leave current alone. */
	{
		std::string err;
		int status = 0;
		picojson::value doc = make_doc({ dev_entry("rl", true, {}) });
		doc.get<picojson::object>()["title"] = picojson::value("Bench 11/73");
		// a dashboard layout rides along as file metadata too
		picojson::object lay_rl;
		lay_rl["x"] = picojson::value(2.0);
		lay_rl["y"] = picojson::value(1.0);
		picojson::object layout;
		layout["rl"] = picojson::value(lay_rl);
		doc.get<picojson::object>()["layout"] = picojson::value(layout);
		check(webconfigs_write("titled", doc, false, &err, &status),
				"write titled with a title and layout");
		check(webconfigs_current() == "cfgA", "an offline write leaves current alone");

		picojson::value snap;
		check(read_json_file(cfg_path("titled"), &snap), "titled file written");
		check(snap.get("title").is<std::string>()
				&& snap.get("title").get<std::string>() == "Bench 11/73",
				"title stored in the file");
		check(snap.get("layout").is<picojson::object>()
				&& snap.get("layout").get("rl").get("x").get<double>() == 2.0,
				"layout stored in the file");

		// a later write that omits the title/layout preserves the stored ones
		picojson::value doc2 = make_doc({ dev_entry("rl", true, {}),
				dev_entry("rl0", true, {}) });
		check(webconfigs_write("titled", doc2, false, &err, &status),
				"rewrite titled without a title or layout");
		check(read_json_file(cfg_path("titled"), &snap), "titled reread");
		check(snap.get("title").is<std::string>()
				&& snap.get("title").get<std::string>() == "Bench 11/73",
				"title preserved across a save that omits it");
		check(snap.get("layout").is<picojson::object>()
				&& snap.get("layout").get("rl").get("x").get<double>() == 2.0,
				"layout preserved across a save that omits it");

		// the list carries the title, and falls back to the name when untitled
		picojson::value list;
		check(picojson::parse(list, webconfigs_list_json()).empty(), "list parses");
		std::string titled_title, cfgA_title;
		for (const picojson::value &c : list.get("configs").get<picojson::array>()) {
			if (c.get("name").get<std::string>() == "titled")
				titled_title = c.get("title").get<std::string>();
			if (c.get("name").get<std::string>() == "cfgA")
				cfgA_title = c.get("title").get<std::string>();
		}
		check(titled_title == "Bench 11/73", "list carries the stored title");
		check(cfgA_title == "cfgA", "list falls back to the name when untitled");

		unlink(cfg_path("titled").c_str());
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

	/* 4. rename moves the file and carries the current pointer and the DIP
	      binding; the live dirty state is unaffected */
	{
		std::string err;
		int status = 0;
		// bind cfgA to a DIP value, so the rename can be shown to carry it
		picojson::value doc = make_doc({ dev_entry("rl", true, {}),
				dev_entry("rl0", true, {}) });
		doc.get<picojson::object>()["dip_value"] = picojson::value((double) 5);
		check(webconfigs_write("cfgA", doc, false, &err, &status), "bind cfgA to DIP 5");
		std::vector<std::string> rej;
		webconfigs_apply("cfgA", &rej, &err); // clean the machine against cfgA
		check(!modified_now(), "clean cfgA before rename");
	}
	rl0->image.set("dirty.rl02"); // make the machine modified against cfgA
	check(modified_now(), "modified before rename");
	{
		std::string err;
		check(webconfigs_rename("cfgA", "cfgB", &err), "rename cfgA to cfgB");
		check(!file_exists(cfg_path("cfgA")), "cfgA file gone after rename");
		check(file_exists(cfg_path("cfgB")), "cfgB file present after rename");
		check(webconfigs_current() == "cfgB", "current follows rename");
		picojson::value snap;
		check(read_json_file(cfg_path("cfgB"), &snap)
				&& snap.get("dip_value").is<double>()
				&& (int) snap.get("dip_value").get<double>() == 5,
				"DIP binding travels with the rename");
		check(modified_now(), "still modified against the renamed config");
	}
	{
		std::vector<std::string> rej;
		std::string err;
		webconfigs_apply("cfgB", &rej, &err); // clean the machine again
	}

	/* 5. delete refuses the current one; a non-current config deletes */
	{
		std::string err;
		int status = 0;
		check(!webconfigs_delete("cfgB", &err, &status), "delete refuses current");
		check(status == 409, "delete of current answers 409");

		// make cfgC the current, leaving cfgB non-current
		rl0->image.set("c.rl02");
		check(webconfigs_save("cfgC", &err), "save cfgC");
		check(webconfigs_current() == "cfgC", "cfgC current");
		status = 0;
		check(webconfigs_delete("cfgB", &err, &status), "delete cfgB once not current");
		check(!file_exists(cfg_path("cfgB")), "cfgB removed");
	}

	/* 6. the DIP switches pick the power-on configuration: the file whose
	      dip_value matches is applied; a value no file claims brings up the
	      empty configuration, passive on the bus. */
	{
		std::string err;
		int status = 0;
		// dipA at DIP 3 (rl on); dipB at DIP 7 (rl + rl0 on)
		picojson::value docA = make_doc({ dev_entry("rl", true, {}) });
		docA.get<picojson::object>()["dip_value"] = picojson::value((double) 3);
		check(webconfigs_write("dipA", docA, false, &err, &status), "write dipA at DIP 3");
		picojson::value docB = make_doc({ dev_entry("rl", true, {}),
				dev_entry("rl0", true, {}) });
		docB.get<picojson::object>()["dip_value"] = picojson::value((double) 7);
		check(webconfigs_write("dipB", docB, false, &err, &status), "write dipB at DIP 7");

		// the list reports each file's dip_value
		picojson::value list;
		check(picojson::parse(list, webconfigs_list_json()).empty(), "list parses");
		int dipA_v = -2, dipB_v = -2;
		for (const picojson::value &c : list.get("configs").get<picojson::array>()) {
			if (c.get("name").get<std::string>() == "dipA")
				dipA_v = (int) c.get("dip_value").get<double>();
			if (c.get("name").get<std::string>() == "dipB")
				dipB_v = (int) c.get("dip_value").get<double>();
		}
		check(dipA_v == 3, "list reports dipA at DIP 3");
		check(dipB_v == 7, "list reports dipB at DIP 7");

		// DIP 3 → dipA (rl on, rl0 off)
		g_dip_value = 3;
		webconfigs_startup("");
		check(webconfigs_current() == "dipA", "DIP 3 selects dipA at startup");
		check(rl->enabled.value && !rl0->enabled.value, "dipA enables rl only");

		// changing the switches and restarting the backend switches machines;
		// the switches are read at startup and nowhere else, so a power cycle
		// keeps whatever configuration is loaded
		g_dip_value = 7;
		webconfigs_startup("");
		check(webconfigs_current() == "dipB", "DIP 7 selects dipB at the next startup");
		check(rl->enabled.value && rl0->enabled.value, "dipB enables rl and rl0");

		// a value no file claims → the empty fallback, passive on the bus
		g_dip_value = 12;
		webconfigs_startup("");
		check(webconfigs_current() == "default",
				"an unclaimed DIP value brings up the empty config");
		check(file_exists(cfg_path("default")), "fallback default.json written");
		check(!rl->enabled.value && !rl0->enabled.value,
				"empty fallback switches everything off");
	}

	/* 7. --config overrides the DIP selection for bring-up and testing */
	g_dip_value = 3; // would otherwise pick dipA
	webconfigs_startup("dipB");
	check(webconfigs_current() == "dipB", "--config overrides the DIP selection");

	/* 8. device_label: the static per-type table renders each shape correctly.
	      Fed the (type_name, name, unit, instance) tuple the API extracts. Every
	      label leads with the DEC board type and appends the instance only where
	      a machine can carry more than one. */
	{
		// a machine carries one of these, so the label is board type and role
		check(device_label("UDA50", "uda", "", 0) == "UDA50 disk controller",
				"label: controller is board type and role");
		check(device_label("RLV12", "rl", "", 0) == "RLV12 disk controller",
				"label: RL controller board type and role");
		// a drive appends its unit number
		check(device_label("RA81", "uda0", "0", 0) == "RA81 disk 0",
				"label: instanced drive appends its unit");
		check(device_label("RL02", "rl1", "1", 0) == "RL02 disk 1",
				"label: RL02 drive appends its unit");
		check(device_label("TK50", "tqk501", "1", 0) == "TK50 tape 1",
				"label: TK50 drive appends its unit");
		// several boards of one type: numbered by their ordinal, not their address
		check(device_label("slu_c", "DL11", "", 0) == "DL11 serial line 0",
				"label: the first DL11 is numbered 0");
		check(device_label("slu_c", "DL11b", "", 1) == "DL11 serial line 1",
				"label: the second DL11 is numbered 1");
		check(device_label("dzv11_c", "dzv11c", "", 2) == "DZV11 serial mux 2",
				"label: the third DZV11 is numbered 2");
		check(device_label("dhv11_c", "dhv11", "", 0) == "DHV11 serial mux 0",
				"label: the first DHV11 is numbered 0");
		// internal device with no DEC code: the role opens the label
		check(device_label("blinkenbone_c", "panel", "", 0) == "Front panel",
				"label: internal device leads with its role");
		check(device_label("RX0102uCPU", "rxcpu", "", 0) == "Floppy microcontroller",
				"label: floppy microcontroller names an internal device");
		check(device_label("DELQA", "delqa", "", 0) == "DELQA Ethernet controller",
				"label: DELQA interface");
		// the UNIBUS controllers, whose types differ from their Q-bus counterparts
		check(device_label("RL11", "rl", "", 0) == "RL11 disk controller",
				"label: UNIBUS RL controller");
		check(device_label("RK11", "rk", "", 0) == "RK11 disk controller",
				"label: UNIBUS RK controller");
		check(device_label("RX11", "rx", "", 0) == "RX11 floppy controller",
				"label: UNIBUS RX01 controller");
		check(device_label("RY211", "ry", "", 0) == "RX211 floppy controller",
				"label: UNIBUS RX02 controller");
		check(device_label("DEUNA", "deuna", "", 0) == "DEUNA Ethernet controller",
				"label: DEUNA controller");
		check(device_label("KE11", "ke", "", 0) == "KE11-A arithmetic element",
				"label: KE11-A");
		check(device_label("m9312_c", "M9312", "", 0) == "M9312 bootstrap ROM",
				"label: M9312 bootstrap");
		check(device_label("PDP-11/20", "CPU20", "", 0) == "KA11 processor",
				"label: the emulated PDP-11/20");
		check(device_label("VAX-11/780", "cpuvax", "", 0) == "KA780 processor",
				"label: the emulated VAX-11/780");
		check(device_label("MS11", "MEM", "", 0) == "MS11 memory",
				"label: the UNIBUS memory card");
		check(device_label("MSV11", "MEM", "", 0) == "MSV11 memory",
				"label: the Q-bus memory card");
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

	/* 10. backplane slots: one card per slot, so a configuration that puts two
	       devices on the bus in one slot is refused before it is written, as is
	       one that runs a device past the last slot. A device's footprint comes
	       from its own requests, not from a table. */
	{
		// a mux at slot 10 with separate receive and transmit interrupts, so it
		// holds 10 and 11; a single-slot controller to place against it
		test_busdevice_c *mux = new test_busdevice_c("mux", "dzv11_c", 10, true);
		test_busdevice_c *disk = new test_busdevice_c("disk", "RK11", 20, false);
		reset_devices();
		webconfigs_init(configs_dir); // capture the new devices' defaults

		std::string err;
		int status = 0;

		// their defaults do not overlap, so the pair writes
		std::vector<picojson::value> ok;
		ok.push_back(dev_entry("mux", true, { }));
		ok.push_back(dev_entry("disk", true, { }));
		check(webconfigs_write("slots", make_doc(ok), false, &err, &status),
				"non-overlapping slots write");

		// the controller moved onto the mux's transmit slot is refused, and the
		// error names both devices and the slot
		std::vector<picojson::value> clash;
		clash.push_back(dev_entry("mux", true, { }));
		clash.push_back(dev_entry("disk", true, { { "slot", "11" } }));
		err.clear();
		status = 0;
		check(!webconfigs_write("slots", make_doc(clash), false, &err, &status),
				"second slot of a two-slot device is taken");
		check(status == 422, "slot conflict answers 422");
		check(err.find("disk") != std::string::npos && err.find("mux") != std::string::npos
				&& err.find("11") != std::string::npos,
				"slot conflict names both devices and the slot");

		// the same slot outright
		std::vector<picojson::value> same;
		same.push_back(dev_entry("mux", true, { }));
		same.push_back(dev_entry("disk", true, { { "slot", "10" } }));
		err.clear();
		status = 0;
		check(!webconfigs_write("slots", make_doc(same), false, &err, &status),
				"two devices in one slot refused");

		// a device that is off the bus holds no slot, so the overlap is allowed
		std::vector<picojson::value> off;
		off.push_back(dev_entry("mux", true, { }));
		off.push_back(dev_entry("disk", false, { { "slot", "10" } }));
		err.clear();
		status = 0;
		check(webconfigs_write("slots", make_doc(off), false, &err, &status),
				"a disabled device may share a slot");

		// the last slot is 31, and a two-slot device placed there runs past it
		std::vector<picojson::value> over;
		over.push_back(dev_entry("mux", true, { { "slot", "31" } }));
		err.clear();
		status = 0;
		check(!webconfigs_write("slots", make_doc(over), false, &err, &status),
				"a device running past the last slot is refused");
		check(status == 422, "slot overflow answers 422");

		// the emulated CPU arbitrates rather than requesting, and sits in no
		// slot: it neither takes one nor collides with a device that has one
		test_busdevice_c *cpu = new test_busdevice_c("cpu", "PDP-11/20", 0, false);
		cpu->priority_slot.value = 0;
		cpu->default_priority_slot = 0;
		cpu->rcv_intr.priority_slot = 0xff; // uninitialized: the request is unused
		std::vector<picojson::value> withcpu;
		withcpu.push_back(dev_entry("cpu", true, { }));
		withcpu.push_back(dev_entry("mux", true, { }));
		err.clear();
		status = 0;
		check(webconfigs_write("slots", make_doc(withcpu), false, &err, &status),
				"a device in no slot takes none");

		unlink(cfg_path("slots").c_str());
		delete cpu;
		delete mux;
		delete disk;
	}

	/* 11. the power cycle (webpower.cpp): the panel switch takes every card out
	       of the machine and puts it back, which is the complete teardown — the
	       reset a card gets when it loses its supply. What the machine carries
	       is unchanged by that: the cards are still in it and the drives still
	       hold their packs, so a switched-off machine still describes the
	       configuration it is loaded with. */
	{
		// a controller and the two drives that hang off it, so the cascade a
		// real controller runs when it leaves the bus is part of the test
		test_controller_c *uda = new test_controller_c("uda", "UDA50");
		test_device_c *uda0 = new test_device_c("uda0", "RA81");
		test_device_c *uda1 = new test_device_c("uda1", "RA81");
		uda->drives.push_back(uda0);
		uda->drives.push_back(uda1);

		reset_devices();
		webconfigs_init(configs_dir);
		rl->enabled.set(true);
		rl0->enabled.set(true);
		rl0->image.set("rt11.rl02");
		rl1->enabled.set(true);
		rl1->image.set("games.rl02");
		uda->enabled.set(true);
		uda0->enabled.set(true);
		uda0->image.set("2.11bsd.dsk");
		std::string err;
		check(webconfigs_save("powercfg", &err), "save powercfg");
		check(!modified_now(), "clean powercfg before the power cycle");

		/* a. power down takes the cards out and sets the packs aside */
		webpower_devices_off();
		check(webpower_devices_are_off(), "the machine reads as switched off");
		check(!rl->enabled.value && !rl0->enabled.value && !rl1->enabled.value,
				"every card is out of the emulation");
		check(!uda->enabled.value && !uda0->enabled.value,
				"a controller and its drives are out too");
		check(rl0->image.value == "" && rl1->image.value == ""
				&& uda0->image.value == "",
				"the image files are closed, so a written medium is read afresh");
		check(dl->enabled.value == false, "a card that was out stays out");

		/* b. and yet the machine still carries them: the pack is in the drive,
		      the card is in the machine, and nothing reads as an operator edit */
		check(webpower_is_in_machine(rl0), "the card is still in the machine");
		check(webpower_is_in_machine(uda0),
				"a drive its controller took with it is still in the machine");
		check(!webpower_is_in_machine(uda1), "an empty drive bay stays empty");
		check(!webpower_is_in_machine(dl), "a card that was never in stays out");
		check(webpower_param_value(rl0, &rl0->image) == "rt11.rl02",
				"the drive still holds its pack");
		check(webpower_param_value(uda0, &uda0->image) == "2.11bsd.dsk",
				"a drive behind a controller holds its pack too");
		check(!modified_now(), "a switched-off machine does not read as modified");
		{
			picojson::value live;
			check(webconfigs_save("offsave", &err), "save while switched off");
			check(read_json_file(cfg_path("offsave"), &live), "offsave written");
			check(snap_device(live, "rl0") != nullptr,
					"the snapshot of a dark machine still names its cards");
			check(dev_param(live, "rl0", "image") == "rt11.rl02",
					"the snapshot of a dark machine still names the pack");
			unlink(cfg_path("offsave").c_str());
			std::vector<std::string> rej;
			webconfigs_apply("powercfg", &rej, &err); // offsave took the pointer
		}

		/* c. switching an already dark machine off changes nothing */
		webpower_devices_off();
		check(webpower_is_in_machine(rl0), "a second power-down keeps the record");

		/* d. power up puts the cards back, each with the pack it held */
		webpower_devices_on();
		check(!webpower_devices_are_off(), "the machine reads as switched on");
		check(rl->enabled.value && rl0->enabled.value && rl1->enabled.value,
				"every card is back in the emulation");
		check(uda->enabled.value && uda0->enabled.value,
				"a drive its controller took with it comes back with the power");
		check(!uda1->enabled.value, "an empty drive bay stays empty on power-up");
		check(!dl->enabled.value, "a card that was out is not brought in by power-up");
		check(rl0->image.value == "rt11.rl02" && rl1->image.value == "games.rl02"
				&& uda0->image.value == "2.11bsd.dsk",
				"each drive comes back with the pack it held");
		check(!modified_now(), "the machine is unmodified across a power cycle");

		/* e. power up with nothing set aside changes nothing */
		webpower_devices_on();
		check(rl->enabled.value, "a second power-up leaves the machine alone");

		/* f. a configuration loaded into a dark machine leaves it dark, and it
		      is that configuration the panel switch brings up */
		webpower_devices_off();
		{
			picojson::value doc = make_doc({ dev_entry("rl", true, {}),
					dev_entry("dl", true, {}) });
			int status = 0;
			check(webconfigs_write("powercfg2", doc, false, &err, &status),
					"write powercfg2");
			std::vector<std::string> rej;
			check(webconfigs_apply("powercfg2", &rej, &err), "apply into a dark machine");
			check(webpower_devices_are_off(), "the machine stays dark across the apply");
			check(!rl->enabled.value && !dl->enabled.value,
					"the new configuration's cards are out too");
			check(webpower_is_in_machine(rl) && webpower_is_in_machine(dl),
					"the new configuration is what the machine now carries");
			check(!webpower_is_in_machine(rl0),
					"a card the new configuration drops is not carried");
			check(!modified_now(), "a dark machine is unmodified against what it loaded");
		}
		webpower_devices_on();
		check(rl->enabled.value && dl->enabled.value,
				"the panel switch brings up the configuration that was loaded");
		check(!rl0->enabled.value, "and only that one");
		check(!modified_now(), "clean after the power cycle");
		unlink(cfg_path("powercfg2").c_str());
		delete uda0;
		delete uda1;
		delete uda;
	}

	// tidy the temp tree
	for (const char *n : {"cfgA", "cfgB", "cfgC", "default", "legacy", "logcfg",
			"powercfg", "runcfg", "stored1", "live1", "live2"})
		unlink(cfg_path(n).c_str());
	rmdir(configs_dir.c_str());
	rmdir(dir);

	printf("%d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
