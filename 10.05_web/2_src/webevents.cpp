/* webevents.cpp: /ws/events — live state stream of the web interface

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.

   One multiplexed JSON WebSocket keeps every open page live:

     {"t":"param","dev":…,"param":…,"value":…}   committed parameter change
     {"t":"status","dev":…,"status":…}           drive's verbal state, on change
     {"t":"log","level":n,"label":…,"text":…}    log message
     {"t":"state","halt":…,"leds":[…],"switches":[…]}   hardware, on change
     {"t":"metrics","devs":[…]}                  what each device is doing, 1 Hz

   Producers (parameter_c::change_hook on device threads, the logger sink
   under its fifo mutex) only append to a bounded event_queue; a broadcast thread
   serializes the WebSocket writes, so a slow client never blocks emulation.
*/

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include <thread>

#include "civetweb.h"
#include "webws.hpp"
#include "picojson.h"

#include "logger.hpp"
#include "gpios.hpp"
#include "device.hpp"
#include "parameter.hpp"
#include "qunibus.h"
#include "mailbox.h"
#include "qunibusadapter.hpp"
#include "device_configuration.hpp"

#include "device_status.hpp"
#include "device_metrics.hpp"
#include "metric.hpp"

#include "webevents.hpp"
#include "webbuspower.hpp"
#include "webconfigs.hpp"
#include "webpower.hpp"
#include "webstorage.hpp"
#include "webupdate.hpp"
#include "webselftest.hpp"

// clients, guarded by clients_mutex; writes only from the broadcast thread
static std::mutex clients_mutex;
static std::set<struct mg_connection *> clients;

// bounded event event_queue, oldest dropped on overflow
static std::mutex queue_mutex;
static std::condition_variable queue_cv;
static std::deque<std::string> event_queue;
static const size_t queue_max = 1000;

static std::atomic<bool> running(false);
static std::thread broadcaster;

// current hardware state, guarded by state_mutex; a new client gets this
// as its first event, later events arrive on change only
static std::mutex state_mutex;
static bool last_halt = false;
static int cur_leds = 0, cur_switches = 0;
static bool cur_init = false;
// The bus power signals as last read. They start unknown, which is what they
// are before the first poll has established that anything is reading the bus.
static bus_power_reading_c cur_power = { BUS_SIGNAL_UNKNOWN, BUS_SIGNAL_UNKNOWN };
// Logical power flag: runtime only. The board comes up dark - it loads the
// configuration the switches name without putting any of it on the bus - so
// this starts false and the panel switch, or an autostart, raises it.
static bool cur_powered = false;
// What holds the board, "" when nothing does. Part of the state frame, so a
// page that connects while the board is held is locked as soon as it arrives
// rather than only on the next change.
static std::string cur_held_by;
// The standing notice, "" when there is none. See webevents.hpp.
static std::string cur_notice;

// a tri-state bus signal as JSON: true, false, or null for one nothing read
static picojson::value signal_json(bus_signal_e s) {
	if (s == BUS_SIGNAL_UNKNOWN)
		return picojson::value();
	return picojson::value(s == BUS_SIGNAL_ASSERTED);
}

// serialized {"t":"state",...} of the current values; caller holds state_mutex
static std::string state_json(void) {
	picojson::object event;
	event["t"] = picojson::value("state");
	event["halt"] = picojson::value(last_halt);
	event["powered"] = picojson::value(cur_powered);
	picojson::array led_arr, switch_arr;
	for (unsigned i = 0; i < 4; i++) {
		led_arr.push_back(picojson::value((bool) (cur_leds & (1 << i))));
		switch_arr.push_back(picojson::value((bool) (cur_switches & (1 << i))));
	}
	event["leds"] = picojson::value(led_arr);
	event["switches"] = picojson::value(switch_arr);
	event["init"] = picojson::value(cur_init);
	// null is "not being read", which is neither true nor false: see
	// webbuspower.hpp. A client showing it as either would be inventing a
	// measurement nobody made.
	event["dcok"] = signal_json(cur_power.dcok);
	event["pok"] = signal_json(cur_power.pok);
	event["held_by"] = cur_held_by.empty() ?
			picojson::value() : picojson::value(cur_held_by);
	event["notice"] = cur_notice.empty() ?
			picojson::value() : picojson::value(cur_notice);
	return picojson::value(event).serialize();
}

static void enqueue_str(const std::string &msg) {
	{
		std::lock_guard<std::mutex> lock(queue_mutex);
		if (event_queue.size() >= queue_max)
			event_queue.pop_front();
		event_queue.push_back(msg);
	}
	queue_cv.notify_one();
}

static void enqueue(const picojson::object &event) {
	enqueue_str(picojson::value(event).serialize());
}

// last published configuration state, guarded by config_mutex; the poll and
// the explicit transitions both diff against it so an event is sent only on a
// real change
static std::mutex config_mutex;
static bool config_known = false;
static std::string config_current;
static bool config_modified = false;

// caller holds config_mutex
static std::string config_json_locked(void) {
	picojson::object event;
	event["t"] = picojson::value("config");
	event["current"] = picojson::value(config_current);
	event["modified"] = picojson::value(config_modified);
	return picojson::value(event).serialize();
}

// Whether the configuration state is worth recomputing. Answering it costs a
// snapshot of every enabled device, a read and parse of the saved file, and a
// canonical comparison of the two — far too much to run on a timer for a flag
// that moves only when an operator edits something. So it is computed when
// something that could have moved it has happened: a committed parameter change,
// or a save/apply/rename announcing itself through webevents_note_config().
static std::atomic<bool> config_dirty(true);

void webevents_note_config_dirty(void) {
	config_dirty = true;
}

// Recompute the configuration state and, when it differs from what was last
// published (or force), enqueue a config event. A busy machine leaves the
// modified flag at its last value rather than flapping it, and stays dirty so
// the next pass asks again.
static void publish_config(bool force) {
	if (!force && !config_dirty.exchange(false))
		return;
	std::string current;
	bool modified = false, busy = false;
	webconfigs_status(&current, &modified, &busy);
	if (busy)
		config_dirty = true;
	else
		// Something moved the machine, which is exactly when the mirror of it
		// is worth rewriting. It writes only on a real change, so the common
		// case of this poll finding nothing new costs a comparison.
		webconfigs_mirror_current();
	std::string msg;
	{
		std::lock_guard<std::mutex> lock(config_mutex);
		bool changed = force || !config_known
				|| current != config_current
				|| (!busy && modified != config_modified);
		config_current = current;
		if (!busy)
			config_modified = modified;
		config_known = true;
		if (!changed)
			return;
		msg = config_json_locked();
	}
	enqueue_str(msg);
}

void webevents_note_config(void) {
	publish_config(true);
}

// A machine setting changed. The frame carries no payload: a page rereads
// /api/settings, which is the one description of what the settings now are.
// This is how a page follows a change it did not make itself — another browser,
// a curl, a second operator — and in particular how a console that has moved to
// a different port re-points itself instead of going quietly dead.
void webevents_note_settings(void) {
	enqueue_str("{\"t\":\"settings\"}");
}

// The DIP switches read as one 0..15 value, SW0 the least significant bit.
// Read from the pins directly (not the cached poll sample) so a power-on
// selection sees the switches as they stand now, even before the first poll.
int webevents_dip_value(void) {
	if (gpios == nullptr)
		return -1;
	int value = 0;
	for (unsigned i = 0; i < 4; i++)
		if (gpios->swtch[i] && GPIO_GETVAL(gpios->swtch[i]))
			value |= 1 << i;
	return value;
}

// typed value serialization, same shape as the REST snapshot
static picojson::value param_value_json(parameter_c *p) {
	if (parameter_string_c *ps = dynamic_cast<parameter_string_c *>(p))
		return picojson::value(ps->value);
	if (parameter_bool_c *pb = dynamic_cast<parameter_bool_c *>(p))
		return picojson::value(pb->value);
	if (parameter_unsigned_c *pu = dynamic_cast<parameter_unsigned_c *>(p))
		return picojson::value((double) pu->value);
	if (parameter_unsigned64_c *pu64 = dynamic_cast<parameter_unsigned64_c *>(p))
		return picojson::value((double) pu64->value);
	if (parameter_double_c *pd = dynamic_cast<parameter_double_c *>(p))
		return picojson::value(pd->value);
	return picojson::value();
}

// A parameter as the machine carries it, which is what the REST device list and
// a configuration snapshot report. The two differ while the board's power is
// off: the emulation holds no card and no drive holds a pack, and the machine
// still carries both. A page reads the device set from these values, so they
// are the ones the stream publishes — the "enabled" flag and the medium of a
// dark machine describe the configuration it will come up with.
static picojson::value param_value_carried(device_c *dev, parameter_c *p) {
	if (p == &dev->enabled)
		return picojson::value(webpower_is_in_machine(dev));
	if (dynamic_cast<parameter_string_c *>(p) != nullptr)
		return picojson::value(webpower_param_value(dev, p));
	return param_value_json(p);
}

void webevents_note_param(const std::string &devname, const std::string &param,
		const picojson::value &value) {
	config_dirty = true;
	picojson::object event;
	event["t"] = picojson::value("param");
	event["dev"] = picojson::value(devname);
	event["param"] = picojson::value(param);
	event["value"] = value;
	enqueue(event);
}

static void on_param_changed(parameter_c *param) {
	device_c *dev = dynamic_cast<device_c *>(param->parameterized);
	if (dev == nullptr)
		return;
	// a committed edit is what makes the live setup differ from the saved one
	webevents_note_param(dev->name.value, param->name, param_value_carried(dev, param));
}

// ---- log journal: a persisted, paginated history of log lines ----
// The live stream also appends here, so the diagnostics page loads the tail when
// it opens and pages older entries in — the log survives a page reload and a
// service restart. The in-memory deque is the retained window; the file is
// trimmed to it at startup so it cannot grow without bound.
struct log_entry_t {
	uint64_t id;
	std::string time; // server clock, HH:MM:SS
	unsigned level;
	std::string label;
	std::string text;
};
static std::mutex journal_mutex;
static std::deque<log_entry_t> journal; // ascending id; oldest dropped
static uint64_t next_log_id = 1;
static std::string journal_path;
static std::ofstream journal_file;
static const size_t JOURNAL_MAX = 20000;

static picojson::value log_entry_json(const log_entry_t &e) {
	picojson::object o;
	o["id"] = picojson::value((double) e.id);
	o["time"] = picojson::value(e.time);
	o["level"] = picojson::value((double) e.level);
	o["label"] = picojson::value(e.label);
	o["text"] = picojson::value(e.text);
	return picojson::value(o);
}

void webevents_log_init(const std::string &dir) {
	journal_path = dir + "/log.jsonl";
	std::ifstream in(journal_path.c_str());
	std::string line;
	while (std::getline(in, line)) {
		if (line.empty())
			continue;
		picojson::value v;
		if (!picojson::parse(v, line).empty() || !v.is<picojson::object>()
				|| !v.get("id").is<double>())
			continue;
		log_entry_t e;
		e.id = (uint64_t) v.get("id").get<double>();
		e.time = v.get("time").is<std::string>() ? v.get("time").get<std::string>() : "";
		e.level = v.get("level").is<double>() ? (unsigned) v.get("level").get<double>() : 4;
		e.label = v.get("label").is<std::string>() ? v.get("label").get<std::string>() : "";
		e.text = v.get("text").is<std::string>() ? v.get("text").get<std::string>() : "";
		journal.push_back(e);
		if (journal.size() > JOURNAL_MAX)
			journal.pop_front();
	}
	in.close();
	if (!journal.empty())
		next_log_id = journal.back().id + 1;
	// rewrite the file to the retained tail, then reopen for append
	std::ofstream out(journal_path.c_str(), std::ios::trunc);
	for (const log_entry_t &e : journal)
		out << log_entry_json(e).serialize() << "\n";
	out.close();
	journal_file.open(journal_path.c_str(), std::ios::app);
}

// A page of the journal, newest first: up to `limit` entries with id < `before`
// (before == 0 → the latest). "more" is true when older entries remain.
std::string webevents_log_page_json(uint64_t before, unsigned limit) {
	if (limit == 0 || limit > 1000)
		limit = 200;
	picojson::array entries;
	bool more = false;
	{
		std::lock_guard<std::mutex> lock(journal_mutex);
		for (std::deque<log_entry_t>::const_reverse_iterator it = journal.rbegin();
				it != journal.rend(); ++it) {
			if (before != 0 && it->id >= before)
				continue;
			if (entries.size() >= limit) {
				more = true;
				break;
			}
			entries.push_back(log_entry_json(*it));
		}
	}
	picojson::object o;
	o["entries"] = picojson::value(entries);
	o["more"] = picojson::value(more);
	return picojson::value(o).serialize();
}

// Push the appended journal lines out to storage. The line itself is written on
// the thread that logged it, so the file keeps the order the log has; the write
// to storage is a syscall per line if it is forced there, so it is left to the
// stream's buffer and pushed from the broadcast thread instead. A log storm from
// a device thread then costs that thread a memcpy, not a write.
static void flush_journal(void) {
	std::lock_guard<std::mutex> lock(journal_mutex);
	if (journal_file.is_open())
		journal_file.flush();
}

static void on_log_message(unsigned msglevel, const char *label, const char *text) {
	char ts[16] = "";
	time_t now = time(nullptr);
	struct tm tmv;
	localtime_r(&now, &tmv);
	strftime(ts, sizeof ts, "%H:%M:%S", &tmv);

	log_entry_t e;
	{
		std::lock_guard<std::mutex> lock(journal_mutex);
		e.id = next_log_id++;
		e.time = ts;
		e.level = msglevel;
		e.label = label ? label : "";
		e.text = text ? text : "";
		journal.push_back(e);
		if (journal.size() > JOURNAL_MAX)
			journal.pop_front();
		if (journal_file.is_open())
			journal_file << log_entry_json(e).serialize() << "\n";
	}
	picojson::object event;
	event["t"] = picojson::value("log");
	event["id"] = picojson::value((double) e.id);
	event["time"] = picojson::value(e.time);
	event["level"] = picojson::value((double) e.level);
	event["label"] = picojson::value(e.label);
	event["text"] = picojson::value(e.text);
	enqueue(event);
}

void webevents_note_halt(bool halted) {
	std::lock_guard<std::mutex> lock(state_mutex);
	last_halt = halted;
	picojson::object event;
	event["t"] = picojson::value("state");
	event["halt"] = picojson::value(halted);
	enqueue(event);
}

bool webevents_is_halted(void) {
	std::lock_guard<std::mutex> lock(state_mutex);
	return last_halt;
}

void webevents_note_powered(bool powered) {
	std::lock_guard<std::mutex> lock(state_mutex);
	cur_powered = powered;
	picojson::object event;
	event["t"] = picojson::value("state");
	event["powered"] = picojson::value(powered);
	enqueue(event);
}

bool webevents_is_powered(void) {
	std::lock_guard<std::mutex> lock(state_mutex);
	return cur_powered;
}

void webevents_note_notice(const std::string &text) {
	std::lock_guard<std::mutex> lock(state_mutex);
	if (cur_notice == text)
		return;
	cur_notice = text;
	picojson::object event;
	event["t"] = picojson::value("state");
	event["notice"] = text.empty() ? picojson::value() : picojson::value(text);
	enqueue(event);
}

std::string webevents_notice(void) {
	std::lock_guard<std::mutex> lock(state_mutex);
	return cur_notice;
}

// Taking the board for work the interfaces must not act during: the checks a
// power-up runs before the bus edges are driven, and the interactive menu
// holding the hardware. The reason travels in the state frame, so every page
// connected says the same thing about why it cannot be used, and a page that
// arrives mid-operation is locked by the snapshot it starts from.
void webevents_hold_board(const std::string &reason) {
	std::lock_guard<std::mutex> lock(state_mutex);
	cur_held_by = reason;
	picojson::object event;
	event["t"] = picojson::value("state");
	event["held_by"] = picojson::value(reason);
	enqueue(event);
}

void webevents_release_board(void) {
	std::lock_guard<std::mutex> lock(state_mutex);
	if (cur_held_by.empty())
		return;
	cur_held_by.clear();
	picojson::object event;
	event["t"] = picojson::value("state");
	event["held_by"] = picojson::value();
	enqueue(event);
}

std::string webevents_board_held_by(void) {
	std::lock_guard<std::mutex> lock(state_mutex);
	return cur_held_by;
}

// Status parameters carry what the machine itself drives - the panel lamps, a
// drive's state, the CPU's RUN lamp, program counter and opcode count. The
// device threads move them by direct value assignment, which bypasses the
// change hook every operator-set parameter reports through, so nothing
// announces them and a browser would show whatever it read when it loaded.
// Poll them here and publish what changed.
//
// PARAM_STATUS is the selector because it is what "the emulator drives this"
// already means: GET /api/devices reports exactly these as "statusparams", and
// the frontend replays exactly these over a refetched model. A CPU running an
// operating system moves its PC and opcode count constantly, so this poll's
// interval is what bounds their event rate.
//
// The registry holds every device the machine could carry, enabled or not, and
// four times as many status parameters as the running set has. A disabled device
// cannot move anything, so the scan covers the enabled ones and the parameters
// each of them actually publishes, held per device rather than filtered out of
// the whole parameter list on every pass. A device that has just been switched
// off is scanned once more, so the lamps it cleared on the way out are reported
// before it goes quiet.
struct status_cache_t {
	std::vector<parameter_c *> params; // the device's PARAM_STATUS parameters
	std::vector<std::string> rendered; // what was last published, by index
	std::string status;                // verbal drive status, as last published
	size_t parameter_count = 0;        // the list this was built from
	bool enabled = false;              // as of the last pass
	unsigned seen = 0;                 // the pass that last found the device
};
static std::map<device_c *, status_cache_t> status_cache;

static void poll_status_params(void) {
	static unsigned pass = 0;
	pass++;
	std::lock_guard<std::mutex> lock(device_c::mydevices_mutex);
	for (device_c *dev : device_c::mydevices) {
		status_cache_t &c = status_cache[dev];
		c.seen = pass;
		// (re)build the parameter list when the device is new or has grown one
		if (c.parameter_count != dev->parameter.size()) {
			c.parameter_count = dev->parameter.size();
			c.params.clear();
			for (parameter_c *p : dev->parameter)
				if (p->kind == parameter_c::PARAM_STATUS)
					c.params.push_back(p);
			c.rendered.assign(c.params.size(), std::string());
		}
		bool enabled = dev->enabled.value;
		bool just_disabled = c.enabled && !enabled;
		c.enabled = enabled;
		if (!enabled && !just_disabled)
			continue;
		// activity lamps are lit for a fixed span, expired here
		dev->refresh_activity();
		for (size_t i = 0; i < c.params.size(); i++) {
			parameter_c *p = c.params[i];
			// compare rendered values, so one path serves every parameter type
			picojson::value v = param_value_json(p);
			std::string rendered = v.serialize();
			if (c.rendered[i] == rendered)
				continue;
			c.rendered[i] = rendered;
			picojson::object event;
			event["t"] = picojson::value("param");
			event["dev"] = picojson::value(dev->name.value);
			event["param"] = picojson::value(p->name);
			event["value"] = v;
			enqueue(event);
		}
		// A disk drive's verbal status is derived from the very signals scanned
		// above, so it moves whenever they do — a pack spinning down, a medium
		// coming out, a transfer starting. Publishing it here carries a state
		// the machine reaches by itself to the page, the way the lamps beside
		// the chip already travel.
		std::string status = device_status_for(dev);
		if (!status.empty() && status != c.status) {
			c.status = status;
			picojson::object event;
			event["t"] = picojson::value("status");
			event["dev"] = picojson::value(dev->name.value);
			event["status"] = picojson::value(status);
			enqueue(event);
		}
	}
	// forget the devices that have gone; a reused allocation address would
	// otherwise inherit the entry and swallow the new device's first event
	for (std::map<device_c *, status_cache_t>::iterator it = status_cache.begin();
			it != status_cache.end(); ) {
		if (it->second.seen != pass)
			status_cache.erase(it++);
		else
			++it;
	}
}

// What each device is doing, once a second.
//
// The counters themselves are meaningless to an operator - a device's totals
// only say how long the board has been up - so what travels is the rate over the
// interval, computed by device_metrics.cpp, which is where the cases with no
// rate to report are decided.
//
// Once a second rather than at POLL_MS, for two reasons that point the same way:
// a rate over 100 ms of a disk that moves in bursts is mostly noise, and a
// number that changes ten times a second cannot be read. A second is also what
// makes the panel's history mean something - sixty samples is a minute.
//
// The frame is the whole set, not a difference: a device that has stopped
// reporting - switched off, taken out of the configuration - simply is not in
// the next one, and the panel drops its row without needing to be told. That is
// also what implements "only when enabled": a disabled device is skipped here,
// and its samplers are dropped so re-enabling it anchors afresh instead of
// dividing a year of standing still into the first interval.
//
// A machine that is switched off reports nothing at all. Its cards are off the
// bus and their counters cannot move, so every rate would be a zero that looks
// like a measurement of a running machine.
struct metric_cache_t {
	std::vector<metric_c *> metrics;          // the device's metrics
	std::vector<metric_sampler_c> samplers;   // one per metric, by index
	unsigned seen = 0;                        // the pass that last found the device
	bool was_enabled = false;
};
static std::map<device_c *, metric_cache_t> metric_cache;
// The last set published, so an idle board does not send the same empty frame
// once a second for the client to re-render — and so GET /api/metrics can
// answer with what was last measured. Written by the broadcast thread, read by
// request threads, so it has a mutex of its own rather than borrowing the
// queue's.
static std::mutex metrics_mutex;
static std::string last_metrics_devs;

// Build the "devs" array of a metrics frame, sampling every enabled device.
static picojson::array sample_metrics(bool powered) {
	static unsigned pass = 0;
	pass++;
	picojson::array devs;
	uint64_t now_ms = (uint64_t) std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();

	std::lock_guard<std::mutex> lock(device_c::mydevices_mutex);
	for (device_c *dev : device_c::mydevices) {
		if (dev->metrics.empty())
			continue;
		metric_cache_t &c = metric_cache[dev];
		c.seen = pass;
		if (c.samplers.size() != dev->metrics.size()) {
			c.metrics = dev->metrics;
			c.samplers.assign(c.metrics.size(), metric_sampler_c());
		}
		bool enabled = powered && dev->enabled.value;
		if (!enabled) {
			// forget where the counters stood, so the interval across the gap
			// is never divided into
			if (c.was_enabled)
				c.samplers.assign(c.metrics.size(), metric_sampler_c());
			c.was_enabled = false;
			continue;
		}
		c.was_enabled = true;

		picojson::array metrics;
		for (size_t i = 0; i < c.metrics.size(); i++) {
			metric_c *m = c.metrics[i];
			if (!c.samplers[i].sample(m->total(), now_ms)
					&& !c.samplers[i].known())
				continue; // nothing measured yet
			double rate = c.samplers[i].rate();
			picojson::object o;
			o["name"] = picojson::value(m->name);
			o["unit"] = picojson::value(
					m->unit == metric_c::UNIT_BYTE ? "byte"
					: m->unit == metric_c::UNIT_INSTRUCTION ? "instruction"
					: "count");
			o["label"] = picojson::value(m->label);
			o["rate"] = picojson::value(rate);
			double pct = metric_percent(rate, m->reference_per_second);
			if (pct >= 0) {
				o["pct"] = picojson::value(pct);
				o["reference"] = picojson::value(m->reference_per_second);
			}
			metrics.push_back(picojson::value(o));
		}
		if (metrics.empty())
			continue; // enabled, but nothing measured over an interval yet

		picojson::object d;
		d["dev"] = picojson::value(dev->name.value);
		d["type"] = picojson::value(dev->type_name.value);
		d["kind"] = picojson::value(std::string(dev->category()));
		d["metrics"] = picojson::value(metrics);
		devs.push_back(picojson::value(d));
	}
	// forget devices that have gone, so a reused allocation address does not
	// inherit another device's samples
	for (std::map<device_c *, metric_cache_t>::iterator it = metric_cache.begin();
			it != metric_cache.end(); ) {
		if (it->second.seen != pass)
			metric_cache.erase(it++);
		else
			++it;
	}
	return devs;
}

static void poll_metrics(void) {
	bool powered;
	{
		std::lock_guard<std::mutex> lock(state_mutex);
		powered = cur_powered;
	}
	picojson::array devs = sample_metrics(powered);
	std::string rendered = picojson::value(devs).serialize();
	{
		std::lock_guard<std::mutex> lock(metrics_mutex);
		// An empty set is published once - so a panel showing rows drops them
		// when the machine goes off - and then not again until there is
		// something to say.
		if (devs.empty() && last_metrics_devs == rendered)
			return;
		last_metrics_devs = rendered;
	}
	enqueue_str("{\"t\":\"metrics\",\"devs\":" + rendered + "}");
}

// The same set as a one-shot answer, for GET /api/metrics. It reports what the
// 1 Hz poll last measured rather than sampling again: two samples a millisecond
// apart measure nothing, and a caller polling this endpoint would otherwise
// destroy the very interval the poll is built on.
std::string webevents_metrics_json(void) {
	std::string devs;
	{
		std::lock_guard<std::mutex> lock(metrics_mutex);
		devs = last_metrics_devs;
	}
	if (devs.empty())
		devs = "[]";
	return "{\"devs\":" + devs + "}";
}

/* The bus power signals, read rather than assumed. See webbuspower.hpp for what
 * they are and are not.
 *
 * The value is the PRU's own sample: do_event_initializationsignals() reads the
 * initialization latch once per pass of the main loop and leaves what it read
 * in mailbox->events.power_signals_cur. The adapter's line_DCLO/line_ACLO are
 * not used here - those are an edge-tracked copy that starts life at "power
 * good" and only moves when an event arrives, so a board that has never seen a
 * power event reports good power it never measured.
 *
 * Whether the sample is current is decided by the same loop-pass counter
 * /api/debug/pru reports, compared against the previous poll. The counter turns
 * tens of thousands of times in the 100 ms between two of these, so any advance
 * at all settles it - and a firmware whose mailbox layout this build does not
 * recognise is not read from at all, the way that endpoint refuses to report
 * its counters.
 */
static bus_power_reading_c poll_bus_power(void) {
	static uint32_t last_passes = 0;
	static bool have_last = false;

	if (qunibusadapter == nullptr || mailbox == nullptr
			|| mailbox->diag.magic != MAILBOX_DIAG_MAGIC) {
		have_last = false;
		return bus_power_read(false, false, false);
	}
	uint32_t passes = mailbox->diag.loop_passes;
	bool sampling = have_last && passes != last_passes;
	last_passes = passes;
	have_last = true;

	// The wire encoding differs by bus and is decoded exactly as the adapter's
	// power event does it: UNIBUS carries the two failing-rail signals, QBUS
	// the two good-rail ones.
	uint8_t signals = mailbox->events.power_signals_cur;
#if defined(UNIBUS)
	bool dclo = (signals & INITIALIZATIONSIGNAL_DCLO) != 0;
	bool aclo = (signals & INITIALIZATIONSIGNAL_ACLO) != 0;
#elif defined(QBUS)
	bool dclo = (signals & INITIALIZATIONSIGNAL_DCOK) == 0;
	bool aclo = (signals & INITIALIZATIONSIGNAL_POK) == 0;
#endif
	return bus_power_read(sampling, dclo, aclo);
}

// 10 Hz hardware poll: publish LED/DIP/bus-line state on change
static void poll_hardware(void) {
	static bool first = true;
	if (gpios == nullptr)
		return;
	int leds = 0, switches = 0;
	for (unsigned i = 0; i < 4; i++) {
		// Activity pulses are shorter than the interval between polls, so ask
		// the LED bank whether one occurred since the last sample rather than
		// reading the pin and missing it. The pin still counts, for LEDs held
		// on by something other than the monoflop.
		// The LEDs are wired active low: the pin is pulled down to light them.
		bool pulsed = gpios->activity_leds.take_activity(i);
		if (pulsed || (gpios->led[i] && !GPIO_GETVAL(gpios->led[i])))
			leds |= 1 << i;
		if (gpios->swtch[i] && GPIO_GETVAL(gpios->swtch[i]))
			switches |= 1 << i;
	}
	// A machine whose CPU is emulated reports whether it is running, instead of
	// leaving the panel to infer it from a HALT line an operator pulled: the RUN
	// lamp then follows the CPU through the halts it takes on its own — a HALT
	// opcode, a breakpoint, a double bus error. A physical CPU shows only its
	// HALT line, which is what webevents_note_halt() tracks.
	bool cpu_halt = false, have_cpu = false;
#if defined(UNIBUS)
	unibuscpu_c *cpu = (device_configuration == nullptr)
			? nullptr : device_configuration->emulated_cpu();
	if (cpu != nullptr) {
		have_cpu = true;
		cpu_halt = !cpu->panel_run_led()->value;
	}
#endif

	bool line_init = false;
	if (qunibusadapter != nullptr)
		line_init = qunibusadapter->line_INIT;
	bus_power_reading_c power = poll_bus_power();

	{
		std::lock_guard<std::mutex> lock(state_mutex);
		bool halt = have_cpu ? cpu_halt : last_halt;
		if (!first && leds == cur_leds && switches == cur_switches
				&& line_init == cur_init && power.dcok == cur_power.dcok
				&& power.pok == cur_power.pok && halt == last_halt)
			return;
		first = false;
		last_halt = halt;
		cur_leds = leds;
		cur_switches = switches;
		cur_init = line_init;
		cur_power = power;
		std::string msg = state_json();
		{
			std::lock_guard<std::mutex> qlock(queue_mutex);
			if (event_queue.size() >= queue_max)
				event_queue.pop_front();
			event_queue.push_back(msg);
		}
	}
	queue_cv.notify_one();
}

// The poll cadence, in ms. It bounds the event rate of everything the machine
// drives without announcing it, and the cost of the three polls themselves.
static const unsigned POLL_MS = 100;

// The performance counters are sampled on a slower cadence of their own: a rate
// over a tenth of a second is noise, and a number that moves ten times a second
// cannot be read. A whole multiple of POLL_MS, counted off in the same loop.
static const unsigned METRICS_POLL_MS = 1000;
static unsigned metrics_divider = 0;

// One frame carrying the whole cycle's events, rather than one frame each.
// Server side that is one send per cycle instead of one per event; client side
// it is one macrotask, so the store's microtask coalescing merges the whole
// cycle into a single re-render instead of one per event.
static std::string batch_frame(const std::deque<std::string> &batch) {
	std::string out = "{\"t\":\"batch\",\"events\":[";
	bool first = true;
	for (const std::string &msg : batch) {
		if (!first)
			out += ',';
		first = false;
		out += msg;
	}
	out += "]}";
	return out;
}

static void broadcast_loop(void) {
	// The queue wakes this thread the moment any producer appends, which is what
	// keeps a log line or a committed edit from waiting out the cadence. The
	// polls are not driven by that: they run on their own timer, so a device
	// thread logging in a loop does not drag the whole poll cycle along at the
	// log rate — which is exactly when emulation timing is most fragile.
	std::chrono::steady_clock::time_point next_poll = std::chrono::steady_clock::now();
	while (running) {
		std::deque<std::string> batch;
		{
			std::unique_lock<std::mutex> lock(queue_mutex);
			queue_cv.wait_for(lock, std::chrono::milliseconds(POLL_MS));
			batch.swap(event_queue);
		}
		if (!running)
			break;
		std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
		if (now >= next_poll) {
			next_poll = now + std::chrono::milliseconds(POLL_MS);
			poll_hardware();
			poll_status_params();
			// The performance counters are sampled a tenth as often; see
			// poll_metrics() for why a rate over 100 ms is not worth having.
			if (++metrics_divider >= METRICS_POLL_MS / POLL_MS) {
				metrics_divider = 0;
				poll_metrics();
			}
			publish_config(false); // emit a config event when the modified flag flips
			flush_journal();
			// The updater writes its progress to a status file from its own unit,
			// so the only way the service learns of it is by watching that file.
			// One stat a second, which webupdate_poll() paces itself to.
			if (webupdate_poll()) {
				std::string msg = webupdate_event_json();
				if (!msg.empty())
					enqueue_str(msg); // goes out with the next batch, like the others
			}
			// a self-test run started or ended since the last cycle
			if (webselftest_poll())
				enqueue_str(webselftest_event_json());
		}
		if (batch.empty())
			continue;
		std::string frame = batch_frame(batch);
		std::lock_guard<std::mutex> lock(clients_mutex);
		std::vector<struct mg_connection *> dead;
		for (struct mg_connection *conn : clients) {
			int r = web_ws_try_send(conn, MG_WEBSOCKET_OPCODE_TEXT,
					frame.c_str(), frame.size());
			if (r < 0)
				dead.push_back(conn);
		}
		for (struct mg_connection *conn : dead)
			clients.erase(conn);
	}
}

static int ws_connect_handler(const struct mg_connection *, void *) {
	return 0; // accept
}

static void ws_ready_handler(struct mg_connection *conn, void *) {
	// the current hardware state opens the stream; holding clients_mutex
	// serializes this write against the broadcast thread
	std::string snapshot;
	{
		std::lock_guard<std::mutex> lock(state_mutex);
		snapshot = state_json();
	}
	// the current configuration opens the stream beside the hardware state, so
	// a fresh page shows which configuration is loaded and whether it is edited
	std::string config;
	{
		std::lock_guard<std::mutex> lock(config_mutex);
		if (config_known)
			config = config_json_locked();
	}
	// The update status opens the stream too, so a tab opened during an install -
	// or one reconnecting after the service was replaced by it - knows at once
	// what is going on and how it went.
	std::string update = webupdate_event_json();
	// the self-test state too, so a page opened mid-run knows one is running
	std::string selftest = webselftest_event_json();
	std::lock_guard<std::mutex> lock(clients_mutex);
	mg_websocket_write(conn, MG_WEBSOCKET_OPCODE_TEXT, snapshot.c_str(),
			snapshot.size());
	if (!config.empty())
		mg_websocket_write(conn, MG_WEBSOCKET_OPCODE_TEXT, config.c_str(),
				config.size());
	if (!update.empty())
		mg_websocket_write(conn, MG_WEBSOCKET_OPCODE_TEXT, update.c_str(),
				update.size());
	if (!selftest.empty())
		mg_websocket_write(conn, MG_WEBSOCKET_OPCODE_TEXT, selftest.c_str(),
				selftest.size());
	clients.insert(conn);
}

static int ws_data_handler(struct mg_connection *, int, char *, size_t, void *) {
	return 1; // client input (pings) ignored, keep open
}

static void ws_close_handler(const struct mg_connection *conn, void *) {
	std::lock_guard<std::mutex> lock(clients_mutex);
	clients.erase(const_cast<struct mg_connection *>(conn));
}

void webevents_register(struct mg_context *ctx) {
	// load the persisted log tail before the sink is installed, so live lines
	// continue the stored id sequence
	const char *base = getenv("QUNILATOR_DIR");
	if (base == nullptr)
		base = getenv("HOME");
	webevents_log_init(std::string(base ? base : "."));
	mg_set_websocket_handler(ctx, "/ws/events", ws_connect_handler,
			ws_ready_handler, ws_data_handler, ws_close_handler, nullptr);
	running = true;
	broadcaster = std::thread(broadcast_loop);
	parameter_c::change_hook = on_param_changed;
	logger->message_sink = on_log_message;
}

void webevents_shutdown(void) {
	if (!running)
		return;
	parameter_c::change_hook = nullptr;
	logger->message_sink = nullptr;
	running = false;
	queue_cv.notify_one();
	broadcaster.join();
	std::lock_guard<std::mutex> lock(clients_mutex);
	clients.clear();
}
