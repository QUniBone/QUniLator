/* webconsole.cpp: /ws/console/<n> — emulated terminal lines over WebSocket

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.

   Byte-transparent binary WebSockets bridged to the emulated terminal lines
   through their rs232adapters: 0 is the console DL11 at 777560, 1 the second
   line at 776500, and vax the console of the emulated VAX-11/780, which is
   part of that processor rather than a device on the bus.

   - transmit: the adapter's stream_xmt_tap copies every byte the PDP-11
     sends into a buffer; a flush thread forwards it to all connected
     clients every 20 ms. The physical UART stays attached — the web
     terminal is a parallel tap.
   - receive: client bytes go into the adapter's rcv stream under its
     mutex, exactly as the devices menu's "dl11 rcv" command injects them.

   No echo, no line discipline, all 256 byte values pass through — the
   terminal emulation lives entirely in the browser.
*/

#include <string.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <ostream>
#include <sstream>
#include <streambuf>
#include <string>
#include <thread>

#include "civetweb.h"
#include "webws.hpp"
#include "webconsole_channel.hpp"

#include "logger.hpp"
#include "device_configuration.hpp"

#include "webconsole.hpp"

// per-SLU bridge state
struct console_c {
	// ostream sink for the rs232adapter's stream_xmt_tap
	class tap_streambuf : public std::streambuf {
	public:
		console_c *owner = nullptr;
	protected:
		int overflow(int c) override {
			if (c != EOF) {
				std::lock_guard<std::mutex> lock(owner->xmt_mutex);
				owner->xmt_buffer.push_back((char) c);
			}
			return c;
		}
	};

	// retained history + live clients for this line's /ws/console/<n>. The text
	// callback lets the channel name one client the terminal answerer, so the
	// guest's identification queries — RSX's SET /INQUIRE, VMS's SET
	// TERMINAL/INQUIRE — are answered exactly once however many consoles watch.
	console_channel_c channel{web_ws_console_send, web_ws_console_send_text};

	// xmt bytes from the PDP-11, buffered so the DL11 thread never blocks
	std::mutex xmt_mutex;
	std::string xmt_buffer;

	tap_streambuf tap_buf;
	std::ostream tap_stream;

	// the line this bridge carries, whatever device owns it
	rs232adapter_c *adapter = nullptr;  // set while registered
	std::stringstream *rcv_stream = nullptr;

	console_c() : tap_stream(&tap_buf) {
		tap_buf.owner = this;
	}
};

#if defined(UNIBUS)
#define CONSOLE_COUNT 3                     // two DL11 lines and the VAX console
#else
#define CONSOLE_COUNT 2
#endif

static console_c consoles[CONSOLE_COUNT];

static std::atomic<bool> running(false);
static std::thread flusher;

static void flush_loop(void) {
	while (running) {
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		for (console_c &console : consoles) {
			std::string batch;
			{
				std::lock_guard<std::mutex> lock(console.xmt_mutex);
				batch.swap(console.xmt_buffer);
			}
			if (batch.empty())
				continue;
			console.channel.append(batch.data(), batch.size());
		}
	}
}

static int ws_connect_handler(const struct mg_connection *, void *) {
	return device_configuration == nullptr; // 0 = accept
}

static void ws_ready_handler(struct mg_connection *conn, void *cbdata) {
	console_c *console = (console_c *) cbdata;
	console->channel.add_client(conn);
}

static int ws_data_handler(struct mg_connection *, int opcode, char *data,
		size_t len, void *cbdata) {
	console_c *console = (console_c *) cbdata;
	if ((opcode & 0x0f) == MG_WEBSOCKET_OPCODE_CONNECTION_CLOSE)
		return 0;
	if (len == 0 || device_configuration == nullptr)
		return 1;
	// inject like the menu's "dl11 rcv" command
	if (console->adapter == nullptr)
		return 1;
	pthread_mutex_lock(&console->adapter->mutex);
	console->rcv_stream->clear();
	console->rcv_stream->write(data, len);
	pthread_mutex_unlock(&console->adapter->mutex);
	return 1;
}

static void ws_close_handler(const struct mg_connection *conn, void *cbdata) {
	console_c *console = (console_c *) cbdata;
	console->channel.remove_client(const_cast<struct mg_connection *>(conn));
}

void webconsole_register(struct mg_context *ctx) {
	if (device_configuration != nullptr) {
		consoles[0].adapter = &device_configuration->DL11->rs232adapter;
		consoles[0].rcv_stream = &device_configuration->dl11_rcv_stream;
		consoles[1].adapter = &device_configuration->DL11b->rs232adapter;
		consoles[1].rcv_stream = &device_configuration->dl11b_rcv_stream;
#if defined(UNIBUS)
		if (device_configuration->CPUVAX != nullptr) {
			consoles[2].adapter = &device_configuration->CPUVAX->rs232adapter;
			consoles[2].rcv_stream = &device_configuration->cpuvax_rcv_stream;
		}
#endif
	}
	mg_set_websocket_handler(ctx, "/ws/console/0", ws_connect_handler,
			ws_ready_handler, ws_data_handler, ws_close_handler, &consoles[0]);
	mg_set_websocket_handler(ctx, "/ws/console/1", ws_connect_handler,
			ws_ready_handler, ws_data_handler, ws_close_handler, &consoles[1]);
#if defined(UNIBUS)
	mg_set_websocket_handler(ctx, "/ws/console/vax", ws_connect_handler,
			ws_ready_handler, ws_data_handler, ws_close_handler, &consoles[2]);
#endif
	running = true;
	flusher = std::thread(flush_loop);
	for (console_c &console : consoles)
		if (console.adapter != nullptr)
			console.adapter->stream_xmt_tap = &console.tap_stream;
}

void webconsole_shutdown(void) {
	if (!running)
		return;
	for (console_c &console : consoles)
		if (console.adapter != nullptr)
			console.adapter->stream_xmt_tap = nullptr;
	running = false;
	flusher.join();
	for (console_c &console : consoles)
		console.channel.clear_clients();
}
