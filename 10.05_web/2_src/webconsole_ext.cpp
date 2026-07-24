/* webconsole_ext.cpp: /ws/console/ext — external tty console over WebSocket

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.

   Byte-transparent binary WebSocket bridged to a raw BeagleBone serial
   port (e.g. /dev/ttyS2), the real PDP-11's console line. Unlike
   /ws/console/<n>, no emulated device sits behind it:

   - a reader thread polls the tty and forwards bytes to all clients;
   - client bytes are written straight to the tty (bidirectional).

   The port is opened/closed from the external-console setting
   (/api/settings). It is refused while an enabled DL11 already owns the
   same tty, so the emulated SLU and the external console never fight over
   one device node.
*/

#include <stdio.h>
#include <string.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>

#include "civetweb.h"
#include "webws.hpp"
#include "webconsole_channel.hpp"

#include "rs232.hpp"
#include "dl11w.hpp"
#include "device_configuration.hpp"

#include "weblog.hpp"
#include "webconsole_ext.hpp"

static std::mutex port_mutex; // guards port_io + cur_* + port_open
static rs232_c port_io;
static bool port_open = false;
static std::string cur_source = "webserial";
static std::string cur_port = "ttyS2";
static unsigned cur_baud = 38400;

// retained history + live clients for /ws/console/ext
static console_channel_c channel(web_ws_console_send);

static std::atomic<bool> running(false);
static std::thread reader;

// rs232_c prepends /dev/, so store bare tty names like the SLU serialport
static std::string strip_dev(const std::string &p) {
	if (p.compare(0, 5, "/dev/") == 0)
		return p.substr(5);
	return p;
}

// caller holds port_mutex: is the tty claimed by an enabled DL11?
static std::string console_conflict(const std::string &port) {
	if (device_configuration == nullptr)
		return "";
	slu_c *slus[] = { device_configuration->DL11, device_configuration->DL11b };
	for (slu_c *slu : slus)
		if (slu != nullptr && slu->enabled.value && slu->serialport.value == port)
			return "port /dev/" + port + " is in use by the enabled " + slu->name.value;
	return "";
}

static void reader_loop(void) {
	unsigned char buf[512];
	while (running) {
		int n = 0;
		{
			std::lock_guard<std::mutex> lock(port_mutex);
			if (port_open)
				n = port_io.PollComport(buf, sizeof(buf));
		}
		if (n > 0) {
			channel.append((const char *) buf, (size_t) n);
		} else {
			std::this_thread::sleep_for(std::chrono::milliseconds(n == 0 ? 5 : 20));
		}
	}
}

std::string webconsole_ext_configure(const std::string &source,
		const std::string &port, unsigned baud) {
	std::lock_guard<std::mutex> lock(port_mutex);
	if (port_open) {
		port_io.CloseComport();
		port_open = false;
	}
	cur_source = source;
	cur_port = strip_dev(port);
	cur_baud = baud;
	if (source != "ttys2")
		return "";
	std::string conflict = console_conflict(cur_port);
	if (!conflict.empty()) {
		WEB_INFO("external console: %s", conflict.c_str());
		return conflict;
	}
	if (port_io.OpenComport(cur_port.c_str(), (int) cur_baud, "8N1", false)) {
		std::string reason = "cannot open /dev/" + cur_port;
		WEB_INFO("external console: %s", reason.c_str());
		return reason;
	}
	WEB_INFO("external console on /dev/%s at %u baud", cur_port.c_str(), cur_baud);
	port_open = true;
	return "";
}

static int ws_connect_handler(const struct mg_connection *, void *) {
	return 0; // accept even when the port is closed
}

static void ws_ready_handler(struct mg_connection *conn, void *) {
	channel.add_client(conn);
}

static int ws_data_handler(struct mg_connection *, int opcode, char *data,
		size_t len, void *) {
	if ((opcode & 0x0f) == MG_WEBSOCKET_OPCODE_CONNECTION_CLOSE)
		return 0;
	if (len == 0)
		return 1;
	std::lock_guard<std::mutex> lock(port_mutex);
	if (port_open)
		port_io.SendBuf((unsigned char *) data, (int) len);
	return 1;
}

static void ws_close_handler(const struct mg_connection *conn, void *) {
	channel.remove_client(const_cast<struct mg_connection *>(conn));
}

void webconsole_ext_register(struct mg_context *ctx) {
	mg_set_websocket_handler(ctx, "/ws/console/ext", ws_connect_handler,
			ws_ready_handler, ws_data_handler, ws_close_handler, nullptr);
	running = true;
	reader = std::thread(reader_loop);
}

void webconsole_ext_shutdown(void) {
	if (!running)
		return;
	running = false;
	reader.join();
	std::lock_guard<std::mutex> lock(port_mutex);
	if (port_open) {
		port_io.CloseComport();
		port_open = false;
	}
	channel.clear_clients();
}
