/* webconsole_control.hpp: out-of-band control frames on a console channel

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.

   A console channel carries the terminal stream as binary frames. Anything
   that is not a byte on the line travels as a TEXT frame holding one small
   JSON object, so it can never be mistaken for data the guest typed:

     server -> client   {"live":true}      end of the replayed history
                        {"answerer":true}  you answer the guest's queries

     client -> server   {"break":true}     assert a line BREAK

   BREAK is a line condition rather than a character, which is why it cannot
   ride in the byte stream at all.
*/
#ifndef _WEBCONSOLE_CONTROL_HPP_
#define _WEBCONSOLE_CONTROL_HPP_

#include <cstddef>
#include <cstring>

// Is this TEXT frame one of the control messages at all?
//
// A console is byte-transparent, and a client is free to send what the operator
// typed as a TEXT frame -- the web interface does exactly that. So a TEXT frame
// is control only when it is shaped like one of the messages above: a JSON
// object naming a key this side knows. Everything else is what somebody typed
// and goes to the line, which is what a console did before any of these
// messages existed.
static inline bool web_console_is_control(const char *data, size_t len) {
	size_t b = 0, e = len;
	while (b < e && (data[b] == ' ' || data[b] == '\t' || data[b] == '\n'
			|| data[b] == '\r'))
		b++;
	while (e > b && (data[e - 1] == ' ' || data[e - 1] == '\t'
			|| data[e - 1] == '\n' || data[e - 1] == '\r'))
		e--;
	if (e - b < 2 || data[b] != '{' || data[e - 1] != '}')
		return false;
	for (size_t i = b; i + 7 <= e; i++)
		if (memcmp(data + i, "\"break\"", 7) == 0)
			return true;
	return false;
}

// Is this TEXT frame the {"break":true} control message? Parsed by hand: the
// frame is a fixed tiny object, and the alternative is dragging a JSON parser
// into the WebSocket data path for one key.
static inline bool web_console_is_break(const char *data, size_t len) {
	bool saw_break = false, saw_true = false;
	for (size_t i = 0; i < len; i++) {
		if (len - i >= 7 && memcmp(data + i, "\"break\"", 7) == 0)
			saw_break = true;
		if (len - i >= 4 && memcmp(data + i, "true", 4) == 0)
			saw_true = true;
	}
	return saw_break && saw_true;
}

#endif // _WEBCONSOLE_CONTROL_HPP_
