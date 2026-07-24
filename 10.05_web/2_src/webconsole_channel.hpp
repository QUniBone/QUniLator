/* webconsole_channel.hpp: retained-history console channel

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.

   A console channel owns a bounded ring of the raw bytes a console line has
   emitted and the set of live WebSocket clients tapping it. Both console
   backends use it: the external tty bridge (/ws/console/ext) and the emulated
   DL11 taps (/ws/console/0,/1).

   The reader/flush thread calls append(): under the channel lock it grows the
   ring, drops the oldest bytes past the 256 KB cap, and forwards the new bytes
   to every client. ws_ready_handler calls add_client(): under the SAME lock it
   sends the current ring snapshot to the newcomer and then inserts it, so the
   handoff is atomic — the client receives the full retained history followed by
   the live stream with no gap and no duplicated byte. A client whose send
   reports dead is dropped rather than stalling the producer.

   The ring holds raw tty/DL11 bytes for screen reconstruction; it is unrelated
   to the logger's journal. It is in memory only and does not survive a restart.

   The client sink is a plain callback so the channel is host-testable without
   civetweb: the production callback wraps web_ws_try_send (see webws.hpp), the
   test supplies a synthetic sink.
*/
#ifndef _WEBCONSOLE_CHANNEL_HPP_
#define _WEBCONSOLE_CHANNEL_HPP_

#include <cstddef>
#include <mutex>
#include <set>
#include <string>

class console_channel_c {
public:
	// Send len bytes to one client. Return follows web_ws_try_send:
	//   1 = sent, 0 = skipped (client behind), -1 = dead (drop the client).
	typedef int (*send_fn_t)(void *client, const char *data, size_t len);

	static const size_t default_cap = 256 * 1024; // per-channel ring, bytes

	explicit console_channel_c(send_fn_t send, size_t cap = default_cap);

	// Producer thread: append raw bytes to the ring and broadcast them live,
	// dropping any client that reports dead.
	void append(const char *data, size_t len);

	// ws_ready_handler: atomically replay the ring to the new client, then add
	// it to the live set. A client that reports dead on the snapshot is dropped.
	void add_client(void *client);

	// ws_close_handler: forget a client.
	void remove_client(void *client);

	// shutdown: forget every client (the ring is left intact).
	void clear_clients();

	// Introspection for tests.
	size_t ring_size();
	size_t client_count();

private:
	void trim_locked();

	std::mutex mutex_;
	std::string ring_;             // raw bytes, at most cap_
	size_t cap_;
	std::set<void *> clients_;
	send_fn_t send_;
};

#endif // _WEBCONSOLE_CHANNEL_HPP_
