/* webrecording.cpp: recording a console session on the board

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.

   See webrecording.hpp for what this is for. Free of civetweb so it links
   into the host test harness unchanged.
*/

#include <stdlib.h>
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

// The bytes of the JSON string starting at s[*i] (the opening quote), with
// *i left past the closing quote. A \u escape below 0x100 is the byte itself,
// which is how the writer above encodes them; one above is written as UTF-8.
static bool read_json_string(const std::string &s, size_t *i, std::string *out) {
	if (*i >= s.size() || s[*i] != '"')
		return false;
	size_t p = *i + 1;
	out->clear();
	while (p < s.size()) {
		char c = s[p++];
		if (c == '"') {
			*i = p;
			return true;
		}
		if (c != '\\') {
			out->push_back(c);
			continue;
		}
		if (p >= s.size())
			return false;
		char e = s[p++];
		switch (e) {
		case 'n': out->push_back('\n'); break;
		case 'r': out->push_back('\r'); break;
		case 't': out->push_back('\t'); break;
		case 'b': out->push_back('\b'); break;
		case 'f': out->push_back('\f'); break;
		case 'u': {
			if (p + 4 > s.size())
				return false;
			unsigned cp = (unsigned) strtoul(s.substr(p, 4).c_str(), nullptr, 16);
			p += 4;
			if (cp < 0x100) {
				out->push_back((char) cp);
			} else if (cp < 0x800) {
				out->push_back((char) (0xc0 | (cp >> 6)));
				out->push_back((char) (0x80 | (cp & 0x3f)));
			} else {
				out->push_back((char) (0xe0 | (cp >> 12)));
				out->push_back((char) (0x80 | ((cp >> 6) & 0x3f)));
				out->push_back((char) (0x80 | (cp & 0x3f)));
			}
			break;
		}
		default: out->push_back(e); break; // \" \\ \/
		}
	}
	return false;
}

// A byte above 0x7f is a latin1 character on the guest's line; the transcript
// is UTF-8.
static void append_latin1(std::string &out, unsigned char c) {
	if (c < 0x80) {
		out.push_back((char) c);
	} else {
		out.push_back((char) (0xc0 | (c >> 6)));
		out.push_back((char) (0x80 | (c & 0x3f)));
	}
}

namespace {

// One line of paper under a print head: carriage return moves the head back
// to the margin and what follows overprints, backspace moves it one left.
struct transcript_writer {
	std::string text;
	std::string line;   // one byte per column, latin1
	size_t col = 0;
	enum { NORMAL, ESC, CSI, STRING } state = NORMAL;
	bool string_esc = false;
	std::string csi_params;

	void put(unsigned char c) {
		if (col < line.size())
			line[col] = (char) c;
		else {
			line.append(col - line.size(), ' ');
			line.push_back((char) c);
		}
		col++;
	}

	void newline(void) {
		size_t end = line.find_last_not_of(' ');
		std::string kept = end == std::string::npos ? "" : line.substr(0, end + 1);
		for (unsigned char c : kept)
			append_latin1(text, c);
		text.push_back('\n');
		line.clear();
		col = 0;
	}

	void feed(unsigned char c) {
		switch (state) {
		case ESC:
			if (c == '[')
				state = CSI, csi_params.clear();
			else if (c == ']' || c == 'P' || c == '_' || c == '^')
				state = STRING, string_esc = false;
			else if (c < 0x20 || c > 0x2f)  // intermediates continue the sequence
				state = NORMAL;
			return;
		case CSI:
			if (c >= 0x40 && c <= 0x7e) {
				state = NORMAL;
				// erase to the end of the line, the one erase that changes
				// what a line reads as
				if (c == 'K' && (csi_params.empty() || csi_params == "0") && col < line.size())
					line.resize(col);
			} else {
				csi_params.push_back((char) c);
			}
			return;
		case STRING:    // OSC/DCS run to BEL or ESC backslash
			if (c == 0x07 || (string_esc && c == '\\'))
				state = NORMAL;
			string_esc = (c == 0x1b);
			return;
		case NORMAL:
			break;
		}
		switch (c) {
		case 0x1b: state = ESC; break;
		case '\n': case '\f': case '\v': newline(); break;
		case '\r': col = 0; break;
		case '\b': if (col > 0) col--; break;
		case '\t': do put(' '); while (col % 8 != 0); break;
		default:
			if (c >= 0x20 && c != 0x7f && !(c >= 0x80 && c < 0xa0))
				put(c);
		}
	}
};

} // namespace

std::string recording_transcript(const std::string &cast) {
	transcript_writer w;
	size_t start = 0;
	std::string code, data;
	while (start < cast.size()) {
		size_t nl = cast.find('\n', start);
		if (nl == std::string::npos)
			nl = cast.size();
		std::string ev = cast.substr(start, nl - start);
		start = nl + 1;
		if (ev.empty() || ev[0] != '[')
			continue;           // the header, or anything else that is not an event
		size_t i = ev.find('"');
		if (i == std::string::npos || !read_json_string(ev, &i, &code))
			continue;
		i = ev.find('"', i);
		if (i == std::string::npos || !read_json_string(ev, &i, &data))
			continue;
		if (code == "o") {
			for (unsigned char c : data)
				w.feed(c);
		} else if (code == "m") {
			if (!w.line.empty())
				w.newline();
			for (unsigned char c : "[" + data + "]")
				w.put(c);
			w.newline();
		}
	}
	if (!w.line.empty())
		w.newline();
	return w.text;
}
