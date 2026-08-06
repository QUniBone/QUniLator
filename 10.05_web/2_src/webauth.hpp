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

// Seeds the per-process cache salt. Call once before the settings file is
// loaded.
void webauth_init(void);

// True once this QUniLator carries an operator: a name and a password. Until
// then the interface is open, which is how the first-run dialog reaches it.
bool webauth_configured(void);

// The operator's name, empty on an installation that is not set up yet.
std::string webauth_user(void);

// True when password is the one in force. Constant-time against the stored
// digest, and cheap enough to run on every request: a verified password is
// remembered as a single hash for the life of the process.
bool webauth_verify_password(const std::string &password);

// True when user and password are the credentials in force. The name is half
// of the credential: the right password under another name does not pass.
bool webauth_verify(const std::string &user, const std::string &password);

// Stores new credentials and returns true. The password is handed to the file
// shares as well, so a request that changes only the name passes the password
// that stays in force. On refusal, false with the reason in *error - a missing
// name, a password too short, or a name this QUniLator will not take.
bool webauth_set_credentials(const std::string &user, const std::string &password,
		std::string *error);

// The same, from a digest somebody else derived: a card prepared by a tool
// carries the password already hashed, so the password itself never reaches the
// card. The salt and hash are hex of the lengths this file works in. The file
// shares are not touched here - they take their own hashes, through
// webshares_apply_hashed - so this is the web half alone.
bool webauth_set_digest(const std::string &user, const std::string &salt_hex,
		const std::string &hash_hex, unsigned iterations, std::string *error);

// Persistence, called from websettings.cpp: the "admin" member of settings.json.
void webauth_load(const picojson::value &admin);
picojson::value webauth_json(void);

void webauth_register(struct mg_context *ctx);

#endif
