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
#include <condition_variable>
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

#include "webconsole_control.hpp"
#include "webrecording.hpp"
#include "webconsole.hpp"

// Wakes the flush thread; defined with the flusher below, declared here because
// the tap that calls it is part of the bridge state.
static void flush_wake(void);

// per-SLU bridge state
struct console_c {
	// ostream sink for the rs232adapter's stream_xmt_tap
	class tap_streambuf : public std::streambuf {
	public:
		console_c *owner = nullptr;
	protected:
		int overflow(int c) override {
			if (c != EOF) {
				bool was_empty;
				{
					std::lock_guard<std::mutex> lock(owner->xmt_mutex);
					was_empty = owner->xmt_buffer.empty();
					owner->xmt_buffer.push_back((char) c);
				}
				// Outside the buffer lock: this runs on the device thread, which
				// must not be held up, and nothing may take the two in one hand.
				if (was_empty)
					flush_wake();
			}
			return c;
		}
	};

	// retained history + live clients for this line's /ws/console/<n>. The text
	// callback carries the channel's control frames; this channel names one
	// client the terminal answerer, so the guest's identification queries —
	// RSX's SET /INQUIRE, VMS's SET TERMINAL/INQUIRE — are answered exactly once
	// however many consoles watch.
	console_channel_c channel{web_ws_console_send, web_ws_console_send_text,
			/*designate_answerer*/ true};

	// xmt bytes from the PDP-11, buffered so the DL11 thread never blocks
	std::mutex xmt_mutex;
	std::string xmt_buffer;

	tap_streambuf tap_buf;
	std::ostream tap_stream;

	// the line this bridge carries, whatever device owns it
	rs232adapter_c *adapter = nullptr;  // set while registered
	std::stringstream *rcv_stream = nullptr;
	// the SLU behind this line, where there is one: a BREAK is a line
	// condition the model presents through its registers, not a byte the
	// receive stream can carry. Null for the VAX console.
	slu_c *slu = nullptr;

	// This line's session recorder: output through the channel, input in the
	// data handler below (the one place every client's bytes pass).
	console_recorder_c recorder;

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

// How long a first byte waits while the rest of its burst arrives. The tap
// hands over one character at a time and a guest at 38400 baud sends four per
// millisecond, so this is what keeps a WebSocket frame per character off the
// wire — the reason the flush is batched at all.
static const unsigned FLUSH_MS = 20;
// The flusher used to run that batch on a fixed cadence, which cost 50 wakeups
// a second whether or not any line had spoken. It now sleeps until a tap says
// there is something, and this bounded wait is only a safety net: a
// notification lost to a race is recovered within the second rather than never,
// and shutdown never waits longer than this for the thread to notice.
static const unsigned FLUSH_IDLE_MS = 1000;

static std::mutex flush_mutex;
static std::condition_variable flush_cv;
static bool flush_pending = false;

// Called by a tap when its buffer goes from empty to holding something. Only
// that transition signals: every further byte of the same burst lands in a
// buffer the flusher has not drained yet, and goes out in the same batch.
static void flush_wake(void) {
	{
		std::lock_guard<std::mutex> lock(flush_mutex);
		flush_pending = true;
	}
	flush_cv.notify_one();
}

static void flush_loop(void) {
	while (running) {
		{
			std::unique_lock<std::mutex> lock(flush_mutex);
			flush_cv.wait_for(lock, std::chrono::milliseconds(FLUSH_IDLE_MS),
					[] { return flush_pending || !running; });
			flush_pending = false;
		}
		if (!running)
			break;
		std::this_thread::sleep_for(std::chrono::milliseconds(FLUSH_MS));
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
	// A TEXT frame carrying one of the control messages is out-of-band (see
	// webconsole_control.hpp); any other TEXT frame is what the operator typed
	// and is injected like every other byte.
	if ((opcode & 0x0f) == MG_WEBSOCKET_OPCODE_TEXT
			&& web_console_is_control(data, len)) {
		if (web_console_is_break(data, len) && console->slu != nullptr)
			console->slu->receive_break();
		return 1;
	}
	// inject like the menu's "dl11 rcv" command
	if (console->adapter == nullptr)
		return 1;
	console->recorder.input(data, len);
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
		consoles[0].slu = device_configuration->DL11;
		consoles[1].adapter = &device_configuration->DL11b->rs232adapter;
		consoles[1].rcv_stream = &device_configuration->dl11b_rcv_stream;
		consoles[1].slu = device_configuration->DL11b;
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
	for (console_c &console : consoles)
		console.channel.set_recorder(&console.recorder);
	running = true;
	flusher = std::thread(flush_loop);
	for (console_c &console : consoles)
		if (console.adapter != nullptr)
			console.adapter->stream_xmt_tap = &console.tap_stream;
}

// The recorder for an emulated-console channel, for the recordings API.
console_recorder_c *webconsole_channel_recorder(unsigned index) {
	return index < CONSOLE_COUNT ? &consoles[index].recorder : nullptr;
}

void webconsole_clear(void) {
	for (console_c &console : consoles)
		console.channel.clear_ring();
}

void webconsole_shutdown(void) {
	if (!running)
		return;
	for (console_c &console : consoles)
		if (console.adapter != nullptr)
			console.adapter->stream_xmt_tap = nullptr;
	running = false;
	flush_cv.notify_all(); // do not wait out the idle timeout
	flusher.join();
	for (console_c &console : consoles)
		console.channel.clear_clients();
	// The taps point into the device set, and the caller may be taking it down:
	// a channel left holding them would hand freed memory to the next frame that
	// arrives. Registering again re-reads them from the machine that is there.
	for (console_c &console : consoles) {
		console.adapter = nullptr;
		console.rcv_stream = nullptr;
		console.slu = nullptr;
	}
}
