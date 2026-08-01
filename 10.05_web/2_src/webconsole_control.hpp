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
