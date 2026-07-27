/* webserial.cpp: /ws/serial/<dev>/<line> — mux and SLU serial lines over WebSocket

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.

   A serial line configured with tcp_role "websocket" opens no socket
   (serial_tcp_line_c ROLE_WEBSOCKET); its byte stream is carried here instead.
   The path names the device and the line, e.g. /ws/serial/dzv11/0 for the first
   line of the dzv11, /ws/serial/DL11/0 for the DL11.

   - transmit: the line's data_tap copies every byte the PDP-11 sends into a
     buffer; a flush thread forwards it to all connected clients every 20 ms, so
     the mux worker never blocks on a socket write.
   - receive: client bytes are injected with inject_rx (always accepted, since a
     websocket line never has a competing TCP client).

   Byte-transparent binary WebSockets, like /ws/console; the terminal emulation
   lives in the browser. A line not in websocket mode is refused, so a telnet or
   serial-port line is never quietly tapped.
*/

#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "civetweb.h"
#include "webws.hpp"
#include "webconsole_channel.hpp"

#include "logger.hpp"
#include "device_configuration.hpp"
#include "dzv11.hpp"
#include "dhv11.hpp"
#include "dl11w.hpp"
#include "serial_tcp_line.hpp"

#include "webserial.hpp"

// One line's bridge: the line, its clients (with retained history) and the
// transmit buffer the flush thread drains. Created on first use and never freed.
struct serial_bridge_c {
	serial_tcp_line_c *line = nullptr;
	console_channel_c channel{web_ws_console_send};
	std::mutex xmt_mutex;
	std::string xmt_buffer;
};

static std::mutex bridges_mutex;
static std::map<std::string, serial_bridge_c *> bridges; // key "dev/line"
static std::atomic<bool> running(false);
static std::thread flusher;

// dev + line -> the serial line, or nullptr. device_configuration and the mux
// instances are created once at startup, so no lock is needed to read them.
static serial_tcp_line_c *find_line(const std::string &dev, unsigned line) {
	device_configuration_c *dc = device_configuration;
	if (dc == nullptr)
		return nullptr;
	if (dc->DL11 && dev == dc->DL11->name.value && line == 0)
		return &dc->DL11->tcp_line;
	if (dc->DL11b && dev == dc->DL11b->name.value && line == 0)
		return &dc->DL11b->tcp_line;
	for (dzv11_c *d : dc->DZV11)
		if (d && dev == d->name.value && line < DZ_LINE_COUNT)
			return &d->tcp_line[line];
	for (dhv11_c *d : dc->DHV11)
		if (d && dev == d->name.value && line < DHV_LINE_COUNT)
			return &d->tcp_line[line];
	return nullptr;
}

// parse "/ws/serial/<dev>/<line>" into dev + line; false on a malformed path
static bool parse_path(const struct mg_connection *conn, std::string *dev, unsigned *line) {
	const struct mg_request_info *ri = mg_get_request_info(conn);
	std::string uri = ri && ri->local_uri ? ri->local_uri : "";
	const std::string pfx = "/ws/serial/";
	if (uri.compare(0, pfx.size(), pfx) != 0)
		return false;
	std::string tail = uri.substr(pfx.size());
	size_t slash = tail.rfind('/');
	if (slash == std::string::npos || slash == 0 || slash + 1 >= tail.size())
		return false;
	*dev = tail.substr(0, slash);
	*line = (unsigned) strtoul(tail.substr(slash + 1).c_str(), nullptr, 10);
	return true;
}

static std::string bridge_key(const std::string &dev, unsigned line) {
	return dev + "/" + std::to_string(line);
}

// the bridge for one line, created with its data_tap installed on first use
static serial_bridge_c *get_bridge(const std::string &key, serial_tcp_line_c *line) {
	std::lock_guard<std::mutex> lock(bridges_mutex);
	std::map<std::string, serial_bridge_c *>::iterator it = bridges.find(key);
	if (it != bridges.end())
		return it->second;
	serial_bridge_c *b = new serial_bridge_c();
	b->line = line;
	// device output (from_pdp) buffered for the flush thread; client input is
	// echoed by the guest, so it is not mirrored back here
	line->data_tap = [b](const uint8_t *d, size_t n, bool from_pdp) {
		if (!from_pdp)
			return;
		std::lock_guard<std::mutex> lk(b->xmt_mutex);
		b->xmt_buffer.append((const char *) d, n);
	};
	bridges[key] = b;
	return b;
}

static serial_bridge_c *lookup_bridge(const std::string &key) {
	std::lock_guard<std::mutex> lock(bridges_mutex);
	std::map<std::string, serial_bridge_c *>::iterator it = bridges.find(key);
	return it == bridges.end() ? nullptr : it->second;
}

static void flush_loop(void) {
	while (running) {
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		std::vector<serial_bridge_c *> bs;
		{
			std::lock_guard<std::mutex> lock(bridges_mutex);
			for (std::map<std::string, serial_bridge_c *>::iterator it = bridges.begin();
					it != bridges.end(); ++it)
				bs.push_back(it->second);
		}
		for (serial_bridge_c *b : bs) {
			std::string batch;
			{
				std::lock_guard<std::mutex> lk(b->xmt_mutex);
				batch.swap(b->xmt_buffer);
			}
			if (!batch.empty())
				b->channel.append(batch.data(), batch.size());
		}
	}
}

// accept only an existing line that is in websocket mode
static int ws_connect_handler(const struct mg_connection *conn, void *) {
	std::string dev;
	unsigned line = 0;
	if (!parse_path(conn, &dev, &line))
		return 1;
	serial_tcp_line_c *ln = find_line(dev, line);
	if (ln == nullptr || ln->role != serial_tcp_line_c::ROLE_WEBSOCKET)
		return 1;
	return 0;
}

static void ws_ready_handler(struct mg_connection *conn, void *) {
	std::string dev;
	unsigned line = 0;
	if (!parse_path(conn, &dev, &line))
		return;
	serial_tcp_line_c *ln = find_line(dev, line);
	if (ln == nullptr)
		return;
	serial_bridge_c *b = get_bridge(bridge_key(dev, line), ln);
	b->channel.add_client(conn);
}

static int ws_data_handler(struct mg_connection *conn, int opcode, char *data,
		size_t len, void *) {
	if ((opcode & 0x0f) == MG_WEBSOCKET_OPCODE_CONNECTION_CLOSE)
		return 0;
	if (len == 0)
		return 1;
	std::string dev;
	unsigned line = 0;
	if (!parse_path(conn, &dev, &line))
		return 1;
	serial_bridge_c *b = lookup_bridge(bridge_key(dev, line));
	if (b != nullptr && b->line != nullptr)
		b->line->inject_rx((const uint8_t *) data, len);
	return 1;
}

static void ws_close_handler(const struct mg_connection *conn, void *) {
	std::string dev;
	unsigned line = 0;
	if (!parse_path(conn, &dev, &line))
		return;
	serial_bridge_c *b = lookup_bridge(bridge_key(dev, line));
	if (b != nullptr)
		b->channel.remove_client(const_cast<struct mg_connection *>(conn));
}

void webserial_register(struct mg_context *ctx) {
	// a single prefix handler serves every line; the path names device and line
	mg_set_websocket_handler(ctx, "/ws/serial/", ws_connect_handler,
			ws_ready_handler, ws_data_handler, ws_close_handler, nullptr);
	running = true;
	flusher = std::thread(flush_loop);
}

void webserial_shutdown(void) {
	if (!running)
		return;
	running = false;
	flusher.join();
	std::lock_guard<std::mutex> lock(bridges_mutex);
	for (std::map<std::string, serial_bridge_c *>::iterator it = bridges.begin();
			it != bridges.end(); ++it)
		it->second->channel.clear_clients();
}
