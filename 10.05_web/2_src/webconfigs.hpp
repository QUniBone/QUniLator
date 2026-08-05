/* webconfigs.hpp: /api/configs — named device-setup snapshots

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.
*/
#ifndef _WEBCONFIGS_HPP_
#define _WEBCONFIGS_HPP_

#include <string>
#include <vector>

#include "picojson.h"

struct mg_context;

// register /api/configs; configurations live in $QUNILATOR_DIR/configs
void webconfigs_register(struct mg_context *ctx);

// Locate the configuration directory and capture the parameter defaults, the
// work webconfigs_register does before installing the HTTP handler. Exposed so
// the host test can drive the model without a civetweb server.
void webconfigs_init(const std::string &dir);

// Bring the machine up as a configuration: the --config override if given,
// otherwise the one whose DIP value matches the board's switches, adopting the
// bundled empty configuration (passive on the bus) when no configuration claims
// that value. Sets the current pointer. Call after the web server is registered.
void webconfigs_startup(const std::string &override_config);

// Mirror the live machine to disk, so a board can come back up as it stands
// rather than as the last thing anyone saved. Cheap and idempotent: it writes
// only when the machine has changed, and skips a busy registry.
void webconfigs_mirror_current(void);

// Re-select and apply the DIP-matched configuration, as a power-on does. The
// the configuration the running machine currently represents
std::string webconfigs_current(void);

// The current configuration and the live modified flag, for the config event
// and the list endpoint. *busy is set (and *modified left at the last
// confidently-computed value) when the busy machine blocks the comparison.
void webconfigs_status(std::string *current, bool *modified, bool *busy);

// Save the live setup under <name>, which becomes the current configuration.
bool webconfigs_save(const std::string &name, std::string *error);

// Write a config document to <name>, validated against the device registry
// first. An unknown device or unsettable parameter is refused (*status 422,
// nothing written). With from_live true the body is the live setup being saved
// under <name>, so <name> becomes the current configuration and the modified
// state clears; with it false the document is an offline edit written to the
// file only, leaving the current pointer and the running machine untouched.
bool webconfigs_write(const std::string &name, const picojson::value &document,
		bool from_live, std::string *error, int *status);

// Rename a configuration file; the current/default pointers follow it. The
// live device set — and so the modified state — is left untouched.
bool webconfigs_rename(const std::string &from, const std::string &to,
		std::string *error);

// Delete a configuration. Refused (*status 409) for the current one; *status
// 404 for an unknown name.
bool webconfigs_delete(const std::string &name, std::string *error, int *status);

// the object form of GET /api/configs, serialized (used by the host test)
std::string webconfigs_list_json(void);

// name of a saved configuration whose image parameters reference the
// image file name; empty if none
std::string webconfigs_image_referenced(const std::string &image_name);

// one place a saved configuration puts an image: the drive it names it for
struct config_image_use_t {
	std::string config;
	std::string device;
};

// Apply a saved configuration to the device set, as the apply endpoint does.
// False when the configuration cannot be read, with the reason in "error";
// parameters the devices reject are collected in "rejections" and do not fail
// the call. The web server must be started first: registering it is what finds
// the configuration directory and captures the parameter defaults.
//
// "warnings" is what the apply did that succeeded but is worth saying out loud
// - a configuration that puts a processor on the bus, for one. Rejections are
// the configuration failing to take; warnings are it taking, and the operator
// needing to know what it took.
bool webconfigs_apply(const std::string &name, std::vector<std::string> *rejections,
		std::string *error, std::vector<std::string> *warnings = nullptr);

// every configuration/drive pair that names this image file
std::vector<config_image_use_t> webconfigs_image_uses(const std::string &image_name);

#endif // _WEBCONFIGS_HPP_
