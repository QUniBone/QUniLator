/* webevents.cpp: /ws/events — live state stream of the web interface

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.

   One multiplexed JSON WebSocket keeps every open page live:

     {"t":"param","dev":…,"param":…,"value":…}   committed parameter change
     {"t":"log","level":n,"label":…,"text":…}    log message
     {"t":"state","halt":…,"leds":[…],"switches":[…]}   hardware, on change

   Producers (parameter_c::change_hook on device threads, the logger sink
   under its fifo mutex) only append to a bounded queue; a broadcast thread
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
#include "qunibusadapter.hpp"

#include "webevents.hpp"
#include "webconfigs.hpp"
#include "webstorage.hpp"

// clients, guarded by clients_mutex; writes only from the broadcast thread
static std::mutex clients_mutex;
static std::set<struct mg_connection *> clients;

// bounded event queue, oldest dropped on overflow
static std::mutex queue_mutex;
static std::condition_variable queue_cv;
static std::deque<std::string> queue;
static const size_t queue_max = 1000;

static std::atomic<bool> running(false);
static std::thread broadcaster;

// current hardware state, guarded by state_mutex; a new client gets this
// as its first event, later events arrive on change only
static std::mutex state_mutex;
static bool last_halt = false;
static int cur_leds = 0, cur_switches = 0;
static bool cur_init = false, cur_dcok = false, cur_pok = false;
// logical power flag: runtime only, comes up powered on
static bool cur_powered = true;

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
	event["dcok"] = picojson::value(cur_dcok);
	event["pok"] = picojson::value(cur_pok);
	return picojson::value(event).serialize();
}

static void enqueue_str(const std::string &msg) {
	{
		std::lock_guard<std::mutex> lock(queue_mutex);
		if (queue.size() >= queue_max)
			queue.pop_front();
		queue.push_back(msg);
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

// Recompute the configuration state and, when it differs from what was last
// published (or force), enqueue a config event. A busy machine leaves the
// modified flag at its last value rather than flapping it.
static void publish_config(bool force) {
	std::string current;
	bool modified = false, busy = false;
	webconfigs_status(&current, &modified, &busy);
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

static void on_param_changed(parameter_c *param) {
	device_c *dev = dynamic_cast<device_c *>(param->parameterized);
	if (dev == nullptr)
		return;
	picojson::object event;
	event["t"] = picojson::value("param");
	event["dev"] = picojson::value(dev->name.value);
	event["param"] = picojson::value(param->name);
	event["value"] = param_value_json(param);
	enqueue(event);
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
		if (journal_file.is_open()) {
			journal_file << log_entry_json(e).serialize() << "\n";
			journal_file.flush();
		}
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
	bool running;
	{
		std::lock_guard<std::mutex> lock(state_mutex);
		last_halt = halted;
		running = cur_powered && !last_halt;
		picojson::object event;
		event["t"] = picojson::value("state");
		event["halt"] = picojson::value(halted);
		enqueue(event);
	}
	// an image attached to a running machine is read-only over the shares
	webstorage_refresh_readonly(running);
}

bool webevents_is_halted(void) {
	std::lock_guard<std::mutex> lock(state_mutex);
	return last_halt;
}

void webevents_note_powered(bool powered) {
	bool running;
	{
		std::lock_guard<std::mutex> lock(state_mutex);
		cur_powered = powered;
		running = cur_powered && !last_halt;
		picojson::object event;
		event["t"] = picojson::value("state");
		event["powered"] = picojson::value(powered);
		enqueue(event);
	}
	webstorage_refresh_readonly(running);
}

bool webevents_is_powered(void) {
	std::lock_guard<std::mutex> lock(state_mutex);
	return cur_powered;
}

// Lamp parameters (RL02 panel etc.) are updated by direct value assignment
// on the device threads, which bypasses the change hook — poll them.
static void poll_lamps(void) {
	static std::map<parameter_c *, bool> last;
	std::lock_guard<std::mutex> lock(device_c::mydevices_mutex);
	for (device_c *dev : device_c::mydevices) {
		// activity lamps are lit for a fixed span, expired here
		dev->refresh_activity();
		for (parameter_c *p : dev->parameter) {
			if (!p->readonly || p->name.length() < 4
					|| p->name.compare(p->name.length() - 4, 4, "lamp") != 0)
				continue;
			parameter_bool_c *pb = dynamic_cast<parameter_bool_c *>(p);
			if (pb == nullptr)
				continue;
			std::map<parameter_c *, bool>::iterator it = last.find(p);
			if (it != last.end() && it->second == pb->value)
				continue;
			last[p] = pb->value;
			picojson::object event;
			event["t"] = picojson::value("param");
			event["dev"] = picojson::value(dev->name.value);
			event["param"] = picojson::value(p->name);
			event["value"] = picojson::value(pb->value);
			enqueue(event);
		}
	}
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
	bool line_init = false, dcok = false, pok = false;
	if (qunibusadapter != nullptr) {
		line_init = qunibusadapter->line_INIT;
		dcok = !qunibusadapter->line_DCLO; // DCLO asserted = DC power bad
		pok = !qunibusadapter->line_ACLO;  // ACLO asserted = AC power failing
	}

	std::lock_guard<std::mutex> lock(state_mutex);
	if (!first && leds == cur_leds && switches == cur_switches
			&& line_init == cur_init && dcok == cur_dcok && pok == cur_pok)
		return;
	first = false;
	cur_leds = leds;
	cur_switches = switches;
	cur_init = line_init;
	cur_dcok = dcok;
	cur_pok = pok;
	std::string msg = state_json();
	{
		std::lock_guard<std::mutex> qlock(queue_mutex);
		if (queue.size() >= queue_max)
			queue.pop_front();
		queue.push_back(msg);
	}
	queue_cv.notify_one();
}

static void broadcast_loop(void) {
	while (running) {
		std::deque<std::string> batch;
		{
			std::unique_lock<std::mutex> lock(queue_mutex);
			queue_cv.wait_for(lock, std::chrono::milliseconds(100));
			batch.swap(queue);
		}
		if (!running)
			break;
		poll_hardware();
		poll_lamps();
		publish_config(false); // emit a config event when the modified flag flips
		if (batch.empty())
			continue;
		std::lock_guard<std::mutex> lock(clients_mutex);
		std::vector<struct mg_connection *> dead;
		for (struct mg_connection *conn : clients)
			for (const std::string &msg : batch) {
				int r = web_ws_try_send(conn, MG_WEBSOCKET_OPCODE_TEXT,
						msg.c_str(), msg.size());
				if (r < 0)
					dead.push_back(conn);
				if (r <= 0)
					break; // dead or behind: nothing more this round
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
	std::lock_guard<std::mutex> lock(clients_mutex);
	mg_websocket_write(conn, MG_WEBSOCKET_OPCODE_TEXT, snapshot.c_str(),
			snapshot.size());
	if (!config.empty())
		mg_websocket_write(conn, MG_WEBSOCKET_OPCODE_TEXT, config.c_str(),
				config.size());
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
