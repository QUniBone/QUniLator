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

// The browser's session. Basic auth is what a script uses and what settles a
// browser's first contact, but a browser keeps those credentials only as long
// as it cares to - Chrome drops them after a while, and each time it does the
// sign-in dialog is back. So a request that has authenticated leaves with a
// signed cookie, and a request carrying that cookie needs no password.

// How long a session lasts, and how long each answer from GET /api/auth pushes
// it out again: a browser that is used stays signed in, one that is not asks
// again after five idle days.
#define WEBAUTH_SESSION_SECONDS (5 * 24 * 60 * 60)

// The Set-Cookie header line, "\r\n" included, for a request that has just
// authenticated. Empty on an installation with no operator - there is no
// session to hand out - and empty when no randomness was available for the
// signing secret.
std::string webauth_session_cookie(void);

// True when the Cookie header carries a session this QUniLator signed, for the
// operator in force and not yet expired. cookies may be nullptr.
bool webauth_verify_session(const char *cookies);

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
