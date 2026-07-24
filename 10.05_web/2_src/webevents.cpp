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

#include <string.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
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
static std::string config_current, config_default;
static bool config_modified = false;

// caller holds config_mutex
static std::string config_json_locked(void) {
	picojson::object event;
	event["t"] = picojson::value("config");
	event["current"] = picojson::value(config_current);
	event["default"] = picojson::value(config_default);
	event["modified"] = picojson::value(config_modified);
	return picojson::value(event).serialize();
}

// Recompute the configuration state and, when it differs from what was last
// published (or force), enqueue a config event. A busy machine leaves the
// modified flag at its last value rather than flapping it.
static void publish_config(bool force) {
	std::string current, def;
	bool modified = false, busy = false;
	webconfigs_status(&current, &def, &modified, &busy);
	std::string msg;
	{
		std::lock_guard<std::mutex> lock(config_mutex);
		bool changed = force || !config_known
				|| current != config_current || def != config_default
				|| (!busy && modified != config_modified);
		config_current = current;
		config_default = def;
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

static void on_log_message(unsigned msglevel, const char *label, const char *text) {
	picojson::object event;
	event["t"] = picojson::value("log");
	event["level"] = picojson::value((double) msglevel);
	event["label"] = picojson::value(label);
	event["text"] = picojson::value(text);
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
