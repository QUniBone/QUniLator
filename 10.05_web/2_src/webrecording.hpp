/* webrecording.hpp: recording a console session on the board

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.

   A console channel's output reaches every client, but the input does not:
   each client's bytes go straight to the line, so no client can see what
   another typed. A recording of a session someone drives by hand therefore
   has to be made here, where both directions pass — a host-side tool can only
   ever record its own typing.

   The file is asciicast v3 (https://docs.asciinema.org/manual/asciicast/v3/):
   a header line, then one JSON array per event holding the seconds since the
   previous event, the direction ("o" output, "i" input) and the bytes. That is
   what `qcon render` and the asciinema player read.

   Recording is off until asked for, and stops itself at a size cap: input
   events carry whatever was typed, passwords included, so a recording is a
   thing an operator starts deliberately rather than a file the board always
   keeps.
*/
#ifndef _WEBRECORDING_HPP_
#define _WEBRECORDING_HPP_

#include <cstdio>
#include <mutex>
#include <string>

class console_recorder_c {
public:
	// 16 MB of console traffic is hours of a session; past it the recording
	// closes itself rather than filling the board's storage.
	static const size_t default_cap = 16 * 1024 * 1024;

	console_recorder_c() = default;
	~console_recorder_c();

	// Begin writing to path. Returns "" on success, else the reason. A
	// recording already running is stopped first.
	std::string start(const std::string &path, const std::string &title,
			size_t cap = default_cap);

	// Finish the file. Harmless when nothing is running.
	void stop(void);

	bool recording(void);
	// Path of the running recording, or "".
	std::string path(void);
	size_t bytes_written(void);

	// The two directions, called from the channel's producer thread and from
	// the WebSocket data handlers. Both no-op when not recording.
	void output(const char *data, size_t len);
	void input(const char *data, size_t len);

private:
	void write_event_locked(const char *code, const char *data, size_t len);
	void stop_locked(void);

	std::mutex mutex_;
	FILE *file_ = nullptr;
	std::string path_;
	size_t cap_ = default_cap;
	size_t written_ = 0;
	double last_ms_ = 0;
};

#endif // _WEBRECORDING_HPP_
