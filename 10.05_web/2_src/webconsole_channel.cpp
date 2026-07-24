/* webconsole_channel.cpp: retained-history console channel

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.

   See webconsole_channel.hpp for the design. This translation unit is free of
   civetweb so the channel links into the host test harness unchanged.
*/

#include <vector>

#include "webconsole_channel.hpp"

console_channel_c::console_channel_c(send_fn_t send, size_t cap)
	: cap_(cap), send_(send) {
}

// caller holds mutex_
void console_channel_c::trim_locked(void) {
	if (ring_.size() > cap_)
		ring_.erase(0, ring_.size() - cap_);
}

void console_channel_c::append(const char *data, size_t len) {
	if (len == 0)
		return;
	std::lock_guard<std::mutex> lock(mutex_);
	ring_.append(data, len);
	trim_locked();
	std::vector<void *> dead;
	for (void *client : clients_)
		if (send_(client, data, len) < 0)
			dead.push_back(client);
	for (void *client : dead)
		clients_.erase(client);
}

void console_channel_c::add_client(void *client) {
	std::lock_guard<std::mutex> lock(mutex_);
	// Replay the retained history first, then join the live set — both under the
	// one lock so no byte slips between the snapshot and the insert. A client
	// that reports dead on the snapshot is not inserted; one merely behind
	// (skipped) still joins and picks up the live stream.
	if (!ring_.empty() && send_(client, ring_.data(), ring_.size()) < 0)
		return;
	clients_.insert(client);
}

void console_channel_c::remove_client(void *client) {
	std::lock_guard<std::mutex> lock(mutex_);
	clients_.erase(client);
}

void console_channel_c::clear_clients(void) {
	std::lock_guard<std::mutex> lock(mutex_);
	clients_.clear();
}

size_t console_channel_c::ring_size(void) {
	std::lock_guard<std::mutex> lock(mutex_);
	return ring_.size();
}

size_t console_channel_c::client_count(void) {
	std::lock_guard<std::mutex> lock(mutex_);
	return clients_.size();
}
