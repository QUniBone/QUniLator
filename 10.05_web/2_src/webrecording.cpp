/* webrecording.cpp: recording a console session on the board

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.

   See webrecording.hpp for what this is for. Free of civetweb so it links
   into the host test harness unchanged.
*/

#include <sys/time.h>

#include "webrecording.hpp"

static double now_ms(void) {
	struct timeval tv;
	gettimeofday(&tv, nullptr);
	return (double) tv.tv_sec * 1000.0 + (double) tv.tv_usec / 1000.0;
}

// JSON string escaping for a byte stream: the guest emits control characters
// and bytes above 0x7f, and a cast is read as UTF-8, so anything outside plain
// printable ASCII goes out as \uXXXX of its latin1 code point. That round-trips
// every byte value, which is what the reader assumes.
static void append_json_string(std::string &out, const char *data, size_t len) {
	static const char hex[] = "0123456789abcdef";
	out.push_back('"');
	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char) data[i];
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			if (c < 0x20 || c >= 0x7f) {
				out += "\\u00";
				out.push_back(hex[(c >> 4) & 0xf]);
				out.push_back(hex[c & 0xf]);
			} else {
				out.push_back((char) c);
			}
		}
	}
	out.push_back('"');
}

console_recorder_c::~console_recorder_c() {
	stop();
}

std::string console_recorder_c::start(const std::string &path,
		const std::string &title, size_t cap) {
	std::lock_guard<std::mutex> lock(mutex_);
	stop_locked();
	FILE *f = fopen(path.c_str(), "w");
	if (f == nullptr)
		return "cannot write " + path;
	std::string header = "{\"version\":3,\"term\":{\"cols\":80,\"rows\":24,"
			"\"type\":\"vt100\"},\"timestamp\":";
	header += std::to_string((long long) (now_ms() / 1000.0));
	header += ",\"title\":";
	append_json_string(header, title.data(), title.size());
	header += "}\n";
	fwrite(header.data(), 1, header.size(), f);
	fflush(f);
	file_ = f;
	path_ = path;
	cap_ = cap;
	written_ = 0;
	last_ms_ = now_ms();
	return "";
}

// caller holds mutex_
void console_recorder_c::stop_locked(void) {
	if (file_ == nullptr)
		return;
	// Close the cast with an exit event, so a reader can tell a finished
	// recording from one whose writer died mid-session.
	std::string ev = "[0,\"x\",\"0\"]\n";
	fwrite(ev.data(), 1, ev.size(), file_);
	fclose(file_);
	file_ = nullptr;
	path_.clear();
}

void console_recorder_c::stop(void) {
	std::lock_guard<std::mutex> lock(mutex_);
	stop_locked();
}

// caller holds mutex_
void console_recorder_c::write_event_locked(const char *code, const char *data,
		size_t len) {
	if (file_ == nullptr)
		return;
	double t = now_ms();
	double interval = (t - last_ms_) / 1000.0;
	if (interval < 0)
		interval = 0;
	last_ms_ = t;
	char head[64];
	snprintf(head, sizeof head, "[%.3f,\"%s\",", interval, code);
	std::string line(head);
	append_json_string(line, data, len);
	line += "]\n";
	fwrite(line.data(), 1, line.size(), file_);
	fflush(file_); // a recording of a machine that hangs is still wanted
	written_ += line.size();
	if (written_ >= cap_) {
		std::string note = "[0,\"m\",\"recording stopped at the size cap\"]\n";
		fwrite(note.data(), 1, note.size(), file_);
		stop_locked();
	}
}

void console_recorder_c::output(const char *data, size_t len) {
	if (len == 0)
		return;
	std::lock_guard<std::mutex> lock(mutex_);
	write_event_locked("o", data, len);
}

void console_recorder_c::input(const char *data, size_t len) {
	if (len == 0)
		return;
	std::lock_guard<std::mutex> lock(mutex_);
	write_event_locked("i", data, len);
}

bool console_recorder_c::recording(void) {
	std::lock_guard<std::mutex> lock(mutex_);
	return file_ != nullptr;
}

std::string console_recorder_c::path(void) {
	std::lock_guard<std::mutex> lock(mutex_);
	return path_;
}

size_t console_recorder_c::bytes_written(void) {
	std::lock_guard<std::mutex> lock(mutex_);
	return written_;
}
