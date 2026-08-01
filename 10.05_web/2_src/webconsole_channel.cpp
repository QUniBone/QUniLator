/* webconsole_channel.cpp: retained-history console channel

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.

   See webconsole_channel.hpp for the design. This translation unit is free of
   civetweb so the channel links into the host test harness unchanged.
*/

#include <vector>

#include "webconsole_channel.hpp"
#include "webrecording.hpp"

console_channel_c::console_channel_c(send_fn_t send, send_text_fn_t send_text,
		bool designate_answerer, size_t cap)
	: cap_(cap), send_(send), send_text_(send_text),
	  designate_answerer_(designate_answerer) {
}

// caller holds mutex_: make client the answerer, but only once it has actually
// received the control frame (an unwritable client is not designated, so the
// role never lands on a client that will not hear about it).
void console_channel_c::set_answerer_locked(void *client) {
	static const char msg[] = "{\"answerer\":true}";
	if (!designate_answerer_ || send_text_ == nullptr)
		return;
	if (send_text_(client, msg, sizeof(msg) - 1) == 1)
		answerer_ = client;
}

// Remove terminal-query escapes from a copy of the history: DECID (ESC Z),
// Device Attributes (CSI ... c) and Device Status Report (CSI ... n). These make
// a terminal answer back; replaying them would make a connecting terminal answer
// a query the guest already handled, and the stray answer would land as input at
// the guest's current prompt. They render nothing, so dropping them leaves the
// reconstructed screen unchanged. Only the replay is stripped — the live stream
// carries the guest's real-time queries through to the answerer untouched.
static std::string strip_query_escapes(const std::string &in) {
	std::string out;
	out.reserve(in.size());
	for (size_t i = 0; i < in.size();) {
		unsigned char c = (unsigned char) in[i];
		if (c == 0x1b && i + 1 < in.size()) {
			unsigned char c1 = (unsigned char) in[i + 1];
			if (c1 == 'Z') {           // DECID
				i += 2;
				continue;
			}
			if (c1 == '[') {           // CSI: scan to the final byte
				size_t j = i + 2;
				while (j < in.size()) {
					unsigned char cj = (unsigned char) in[j];
					if (cj >= 0x40 && cj <= 0x7e)
						break;
					j++;
				}
				if (j < in.size() && (in[j] == 'c' || in[j] == 'n')) {
					i = j + 1; // drop the whole DA / DSR query
					continue;
				}
			}
		}
		out.push_back((char) c);
		i++;
	}
	return out;
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
	if (recorder_ != nullptr)
		recorder_->output(data, len);
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
	// Name the answerer before the ring replay, so its small control frame hits
	// an empty send buffer rather than queueing behind up to 256 KB of history.
	if (answerer_ == nullptr)
		set_answerer_locked(client);
	// Replay the retained history with query escapes stripped, then join the live
	// set — both under the one lock so no byte slips between the snapshot and the
	// insert. A client that reports dead on the snapshot is not inserted; one
	// merely behind (skipped) still joins and picks up the live stream.
	if (!ring_.empty()) {
		std::string replay = strip_query_escapes(ring_);
		if (!replay.empty() && send_(client, replay.data(), replay.size()) < 0) {
			if (answerer_ == client)
				answerer_ = nullptr;
			return;
		}
	}
	// Mark the end of the replay: everything after this frame is live. Sent
	// under the same lock, so no live byte can precede it.
	if (send_text_ != nullptr) {
		static const char live[] = "{\"live\":true}";
		if (send_text_(client, live, sizeof(live) - 1) < 0) {
			if (answerer_ == client)
				answerer_ = nullptr;
			return;
		}
	}
	clients_.insert(client);
}

void console_channel_c::remove_client(void *client) {
	std::lock_guard<std::mutex> lock(mutex_);
	clients_.erase(client);
	// The answerer left: promote the first remaining client that can take it.
	if (client == answerer_) {
		answerer_ = nullptr;
		for (void *c : clients_) {
			set_answerer_locked(c);
			if (answerer_ != nullptr)
				break;
		}
	}
}

void console_channel_c::clear_ring(void) {
	std::lock_guard<std::mutex> lock(mutex_);
	ring_.clear();
}

void console_channel_c::clear_clients(void) {
	std::lock_guard<std::mutex> lock(mutex_);
	clients_.clear();
}

void console_channel_c::set_recorder(console_recorder_c *rec) {
	std::lock_guard<std::mutex> lock(mutex_);
	recorder_ = rec;
}

console_recorder_c *console_channel_c::recorder(void) {
	std::lock_guard<std::mutex> lock(mutex_);
	return recorder_;
}

size_t console_channel_c::ring_size(void) {
	std::lock_guard<std::mutex> lock(mutex_);
	return ring_.size();
}

size_t console_channel_c::client_count(void) {
	std::lock_guard<std::mutex> lock(mutex_);
	return clients_.size();
}
