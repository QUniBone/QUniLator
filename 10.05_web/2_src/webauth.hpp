/* webauth.hpp: admin credentials for the web interface

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.
*/

#ifndef _WEBAUTH_HPP_
#define _WEBAUTH_HPP_

#include <string>

#include "civetweb.h"
#include "picojson.h"

// Where the password in force came from.
enum webauth_source_e {
	webauth_source_none,        // no password set: the interface is open
	webauth_source_settings,    // set through the web interface, in settings.json
	webauth_source_environment  // WEBUI_PASSWORD, which the interface cannot change
};

// Reads WEBUI_PASSWORD, and seeds the per-process cache salt. Call once before
// the settings file is loaded.
void webauth_init(void);

webauth_source_e webauth_source(void);
bool webauth_configured(void);

// The user name in force, empty when none is set.
std::string webauth_user(void);

// True when password is the one in force. Constant-time against the stored
// digest, and cheap enough to run on every request: a verified password is
// remembered as a single hash for the life of the process.
bool webauth_verify_password(const std::string &password);

// True when user and password are the credentials in force. Any user name
// passes while no name is set.
bool webauth_verify(const std::string &user, const std::string &password);

// Stores new credentials and returns true. user is the operator's name, empty
// to accept any; password is stored and handed to the file shares, so a name
// change passes the password that stays in force. On refusal, false with the
// reason in *error - a password too short, a name this board will not take, or
// credentials the interface does not own.
bool webauth_set_credentials(const std::string &user, const std::string &password,
		std::string *error);

// Persistence, called from websettings.cpp: the "admin" member of settings.json.
void webauth_load(const picojson::value &admin);
picojson::value webauth_json(void);

void webauth_register(struct mg_context *ctx);

#endif
