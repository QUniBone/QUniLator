/* weblogging.hpp: /api/logging — runtime log-level control

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.
*/
#ifndef _WEBLOGGING_HPP_
#define _WEBLOGGING_HPP_

#include <string>
#include <vector>

#include "picojson.h"

struct mg_context;

// register /api/logging and apply the loaded levels to the logger
void weblogging_register(struct mg_context *ctx);

// Persistence, called from websettings.cpp: the "log_levels" member of
// settings.json holds the global default and the per-source overrides.
void weblogging_load(const picojson::value &v);
picojson::value weblogging_json(void);

// Set logger->default_level from the stored default and walk every registered
// source: a source named in the overrides gets its override, the rest fall to
// the default. Run at startup and again after every configuration apply (which
// resets device verbosity to construction defaults).
void weblogging_apply(void);

// one source as /api/logging reports it
struct weblogging_source_t {
	std::string label;
	unsigned level;    // one of LL_*
	std::string kind;  // "device" | "subsystem"
};
// the registered sources with their current level and kind
std::vector<weblogging_source_t> weblogging_sources(void);
// the global default as an LL_* constant
unsigned weblogging_default_level(void);

// Set the global default (a lowercase level name), re-level every un-overridden
// source, persist. False with *error on an unknown level name.
bool weblogging_set_default(const std::string &level, std::string *error);

// Set one source's override. "level" is a lowercase level name; a JSON null
// clears the override back to the default. Persists. False with *error on an
// unknown level name.
bool weblogging_set_source(const std::string &label, const picojson::value &level,
		std::string *error);

#endif // _WEBLOGGING_HPP_
