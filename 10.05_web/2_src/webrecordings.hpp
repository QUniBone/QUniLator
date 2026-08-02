/* webrecordings.hpp: the recordings API

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.

   POST   /api/console/<channel>/recording   {"action":"start"|"stop","name"?}
   GET    /api/recordings                    list
   GET    /api/recordings/<name>             download the cast
   DELETE /api/recordings/<name>             remove it

   Files live in $QUNILATOR_DIR/recordings. Which channels can be recorded, and
   how a channel's recorder is reached, is the console backends' business; this
   module only routes.
*/
#ifndef _WEBRECORDINGS_HPP_
#define _WEBRECORDINGS_HPP_

#include <string>

struct mg_context;

// The recorder for a channel name ("ext", "0", "1", "vax"), or null when the
// channel does not exist. Defined by the console backends.
class console_recorder_c;
console_recorder_c *webconsole_recorder(const std::string &channel);

void webrecordings_register(struct mg_context *ctx);

// Directory the casts live in; also used by the host test.
void webrecordings_init(const std::string &dir);
std::string webrecordings_dir(void);

#endif // _WEBRECORDINGS_HPP_
