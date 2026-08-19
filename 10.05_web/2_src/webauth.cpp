/* webauth.cpp: admin credentials for the web interface

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.

   The interface is open until a QUniLator is set up, which is what makes a
   fresh one reachable at all. The frontend asks for a user name and a password
   on first contact and PUTs them here; from then on every request needs them,
   static files and WebSocket handshakes included (see begin_request_handler in
   webserver.cpp).

     GET /api/auth   {configured, user, min_length}
     PUT /api/auth   {user?, password?, current?, hostname?, ssh_key?}

   The first-run dialog settles the host name and the operator's ssh key at the
   same time as the credentials, so those two travel with this request and are
   applied once the account exists; websystem.cpp serves each on its own for an
   installation already set up.

   A QUniLator carries one operator, and the name is half of what identifies
   them: it reaches an OS account through webshares.cpp, so the same pair opens
   the web interface, the file shares and ssh. Credentials are therefore a name
   and a password together, set up when the SD card is prepared or on first
   contact with the web interface. A stored record carrying only a password
   comes from before that and names no account, so it is dropped and the
   first-run dialog replaces it.

   The password is stored as a PBKDF2-HMAC-SHA256 digest over a random salt.
   The build links no crypto library - it is static and civetweb is compiled
   -DNO_SSL - so SHA-256 and PBKDF2 are here in full.

   Basic auth resends the password on every request, and running PBKDF2 that
   often would cost more than serving the page. A password that verified once
   is therefore remembered as a single SHA-256 over a salt generated afresh
   each time the process starts, and later requests are checked against that.

   Basic auth is also all a browser is given to hold, and browsers hold it
   badly: Chrome forgets the credentials after a while and asks again, which is
   a sign-in dialog in the middle of somebody's work. So an answer from
   GET /api/auth carries a cookie holding a signed session, good for five days
   and pushed out again by every answer after it. The key that signs it is
   derived from a secret in settings.json - so a restart of the service, or of
   the board, keeps every session open - and from the stored password digest -
   so changing the credentials closes every one of them at once.
*/

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <mutex>
#include <string>

#include "civetweb.h"
#include "picojson.h"

#include "logger.hpp"
#include "weblog.hpp"
#include "webauth.hpp"
#include "websettings.hpp"
#include "webshares.hpp"
#include "websystem.hpp"

/*** SHA-256 (FIPS 180-4) ***/

typedef struct {
	uint32_t state[8];
	uint64_t bitlen;
	uint8_t buf[64];
	unsigned buflen;
} sha256_ctx;

static const uint32_t sha256_k[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
	0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
	0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
	0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
	0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
	0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
	0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
	0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
	0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static inline uint32_t ror32(uint32_t x, unsigned n) {
	return (x >> n) | (x << (32 - n));
}

static void sha256_compress(sha256_ctx *c, const uint8_t *p) {
	uint32_t w[64], a, b, cc, d, e, f, g, h;
	unsigned i;
	for (i = 0; i < 16; i++)
		w[i] = ((uint32_t) p[i * 4] << 24) | ((uint32_t) p[i * 4 + 1] << 16)
				| ((uint32_t) p[i * 4 + 2] << 8) | (uint32_t) p[i * 4 + 3];
	for (; i < 64; i++) {
		uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	a = c->state[0]; b = c->state[1]; cc = c->state[2]; d = c->state[3];
	e = c->state[4]; f = c->state[5]; g = c->state[6]; h = c->state[7];
	for (i = 0; i < 64; i++) {
		uint32_t s1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
		uint32_t ch = (e & f) ^ ((~e) & g);
		uint32_t t1 = h + s1 + ch + sha256_k[i] + w[i];
		uint32_t s0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
		uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
		uint32_t t2 = s0 + maj;
		h = g; g = f; f = e; e = d + t1;
		d = cc; cc = b; b = a; a = t1 + t2;
	}
	c->state[0] += a; c->state[1] += b; c->state[2] += cc; c->state[3] += d;
	c->state[4] += e; c->state[5] += f; c->state[6] += g; c->state[7] += h;
}

static void sha256_init(sha256_ctx *c) {
	c->state[0] = 0x6a09e667; c->state[1] = 0xbb67ae85;
	c->state[2] = 0x3c6ef372; c->state[3] = 0xa54ff53a;
	c->state[4] = 0x510e527f; c->state[5] = 0x9b05688c;
	c->state[6] = 0x1f83d9ab; c->state[7] = 0x5be0cd19;
	c->bitlen = 0;
	c->buflen = 0;
}

static void sha256_update(sha256_ctx *c, const void *data, size_t len) {
	const uint8_t *p = (const uint8_t *) data;
	c->bitlen += (uint64_t) len * 8;
	while (len > 0) {
		unsigned n = 64 - c->buflen;
		if (n > len)
			n = (unsigned) len;
		memcpy(c->buf + c->buflen, p, n);
		c->buflen += n;
		p += n;
		len -= n;
		if (c->buflen == 64) {
			sha256_compress(c, c->buf);
			c->buflen = 0;
		}
	}
}

#define SHA256_LEN 32

static void sha256_final(sha256_ctx *c, uint8_t out[SHA256_LEN]) {
	uint64_t bits = c->bitlen;
	uint8_t pad = 0x80;
	uint8_t lenbe[8];
	unsigned i;
	sha256_update(c, &pad, 1);
	pad = 0;
	while (c->buflen != 56)
		sha256_update(c, &pad, 1);
	for (i = 0; i < 8; i++)
		lenbe[i] = (uint8_t) (bits >> (56 - i * 8));
	sha256_update(c, lenbe, 8);
	for (i = 0; i < 8; i++) {
		out[i * 4] = (uint8_t) (c->state[i] >> 24);
		out[i * 4 + 1] = (uint8_t) (c->state[i] >> 16);
		out[i * 4 + 2] = (uint8_t) (c->state[i] >> 8);
		out[i * 4 + 3] = (uint8_t) c->state[i];
	}
}

static void sha256(const void *data, size_t len, uint8_t out[SHA256_LEN]) {
	sha256_ctx c;
	sha256_init(&c);
	sha256_update(&c, data, len);
	sha256_final(&c, out);
}

/*** HMAC-SHA256 and PBKDF2 (RFC 2104, RFC 8018) ***/

static void hmac_sha256(const uint8_t *key, size_t keylen, const uint8_t *msg,
		size_t msglen, uint8_t out[SHA256_LEN]) {
	uint8_t k[64], ipad[64], opad[64], inner[SHA256_LEN];
	sha256_ctx c;
	unsigned i;
	memset(k, 0, sizeof(k));
	if (keylen > 64)
		sha256(key, keylen, k);
	else
		memcpy(k, key, keylen);
	for (i = 0; i < 64; i++) {
		ipad[i] = k[i] ^ 0x36;
		opad[i] = k[i] ^ 0x5c;
	}
	sha256_init(&c);
	sha256_update(&c, ipad, 64);
	sha256_update(&c, msg, msglen);
	sha256_final(&c, inner);
	sha256_init(&c);
	sha256_update(&c, opad, 64);
	sha256_update(&c, inner, SHA256_LEN);
	sha256_final(&c, out);
}

// Single-block PBKDF2, which is all a 32-byte derived key needs.
static void pbkdf2_sha256(const std::string &password, const uint8_t *salt,
		size_t saltlen, unsigned iterations, uint8_t out[SHA256_LEN]) {
	uint8_t block[128], u[SHA256_LEN];
	size_t n = saltlen;
	unsigned i, j;
	if (n > sizeof(block) - 4)
		n = sizeof(block) - 4;
	memcpy(block, salt, n);
	block[n] = 0; block[n + 1] = 0; block[n + 2] = 0; block[n + 3] = 1; // INT(1)
	hmac_sha256((const uint8_t *) password.data(), password.size(), block, n + 4, u);
	memcpy(out, u, SHA256_LEN);
	for (i = 1; i < iterations; i++) {
		hmac_sha256((const uint8_t *) password.data(), password.size(), u, SHA256_LEN, u);
		for (j = 0; j < SHA256_LEN; j++)
			out[j] ^= u[j];
	}
}

// Comparison whose duration does not depend on where the first difference is.
static bool equal_constant_time(const uint8_t *a, const uint8_t *b, size_t len) {
	uint8_t diff = 0;
	for (size_t i = 0; i < len; i++)
		diff |= (uint8_t) (a[i] ^ b[i]);
	return diff == 0;
}

static std::string to_hex(const uint8_t *p, size_t len) {
	static const char *digits = "0123456789abcdef";
	std::string s;
	s.reserve(len * 2);
	for (size_t i = 0; i < len; i++) {
		s.push_back(digits[p[i] >> 4]);
		s.push_back(digits[p[i] & 15]);
	}
	return s;
}

// Result is the number of bytes decoded, 0 on anything malformed.
static size_t from_hex(const std::string &s, uint8_t *out, size_t maxlen) {
	if (s.size() % 2 != 0 || s.size() / 2 > maxlen)
		return 0;
	for (size_t i = 0; i < s.size(); i += 2) {
		unsigned hi, lo;
		if (sscanf(s.c_str() + i, "%1x%1x", &hi, &lo) != 2)
			return 0;
		out[i / 2] = (uint8_t) ((hi << 4) | lo);
	}
	return s.size() / 2;
}

static bool random_bytes(uint8_t *out, size_t len) {
	FILE *f = fopen("/dev/urandom", "rb");
	if (f == nullptr)
		return false;
	size_t got = fread(out, 1, len, f);
	fclose(f);
	return got == len;
}

/*** the password in force ***/

// PBKDF2 cost: ~110 ms on a desktop, and something above a second on the
// BeagleBone's 1 GHz Cortex-A8. Exactly one request per run of the process
// pays it - the first one that authenticates - and the cache below carries
// every request after that, so the wait lands once after a restart.
#define PBKDF2_ITERATIONS 120000
#define SALT_LEN 16
#define MIN_PASSWORD_LEN 8

static std::mutex auth_mutex; // guards everything below
static bool configured = false;      // a name and a password are in force
static std::string stored_user;      // the operator's name
static uint8_t stored_salt[SALT_LEN];
static uint8_t stored_hash[SHA256_LEN];
static unsigned stored_iterations = PBKDF2_ITERATIONS;

// The secret that signs session cookies. It lives in settings.json beside the
// digest, so the sessions a board handed out still open it after a restart,
// and it is made the first time one is asked for.
#define SESSION_SECRET_LEN 32
#define SESSION_COOKIE "qunilator_session"
static uint8_t session_secret[SESSION_SECRET_LEN];
static bool session_secret_valid = false;

// A password that has verified once, kept as a hash over a salt that exists
// only for this run of the process.
static uint8_t cache_salt[SALT_LEN];
static uint8_t cache_digest[SHA256_LEN];
static bool cache_valid = false;

// caller holds auth_mutex
static void cache_store(const std::string &password) {
	sha256_ctx c;
	sha256_init(&c);
	sha256_update(&c, cache_salt, sizeof(cache_salt));
	sha256_update(&c, password.data(), password.size());
	sha256_final(&c, cache_digest);
	cache_valid = true;
}

// caller holds auth_mutex
static bool cache_matches(const std::string &password) {
	uint8_t digest[SHA256_LEN];
	sha256_ctx c;
	if (!cache_valid)
		return false;
	sha256_init(&c);
	sha256_update(&c, cache_salt, sizeof(cache_salt));
	sha256_update(&c, password.data(), password.size());
	sha256_final(&c, digest);
	return equal_constant_time(digest, cache_digest, SHA256_LEN);
}

void webauth_init(void) {
	std::lock_guard<std::mutex> lock(auth_mutex);
	if (!random_bytes(cache_salt, sizeof(cache_salt))) {
		// Without a salt the cache would be a plain hash of the password, so
		// go without it and pay PBKDF2 on every request instead.
		memset(cache_salt, 0, sizeof(cache_salt));
	}
}

bool webauth_configured(void) {
	std::lock_guard<std::mutex> lock(auth_mutex);
	return configured;
}

std::string webauth_user(void) {
	std::lock_guard<std::mutex> lock(auth_mutex);
	return configured ? stored_user : std::string();
}

bool webauth_verify(const std::string &user, const std::string &password) {
	{
		std::lock_guard<std::mutex> lock(auth_mutex);
		// The name is not a secret, so it is compared plainly; the password
		// below is what the constant-time comparison protects.
		if (configured && user != stored_user)
			return false;
	}
	return webauth_verify_password(password);
}

bool webauth_verify_password(const std::string &password) {
	std::lock_guard<std::mutex> lock(auth_mutex);
	if (!configured)
		return true; // an installation nobody has set up yet answers anything
	if (cache_matches(password))
		return true;
	uint8_t derived[SHA256_LEN];
	pbkdf2_sha256(password, stored_salt, sizeof(stored_salt), stored_iterations, derived);
	if (!equal_constant_time(derived, stored_hash, SHA256_LEN))
		return false;
	cache_store(password);
	return true;
}

/*** the browser's session ***/

// The cookie carries "1.<user>.<expiry>.<mac>": the operator's name, the unix
// second the session ends at, and an HMAC-SHA256 over the three fields before
// it. Nothing in it is secret - it is a claim this QUniLator signed, and the
// mac is what makes it unforgeable.
//
// The signing key is derived from the stored secret and the stored password
// digest together, so a password or name change ends every session that was
// open, while a restart - which reloads both from settings.json - ends none.

// caller holds auth_mutex; result false when there is nothing to sign with.
// *created is set when this made the secret, which the caller then persists.
static bool session_key_locked(uint8_t out[SHA256_LEN], bool *created) {
	*created = false;
	if (!configured)
		return false;
	if (!session_secret_valid) {
		if (!random_bytes(session_secret, sizeof(session_secret)))
			return false;
		session_secret_valid = true;
		*created = true;
	}
	hmac_sha256(session_secret, sizeof(session_secret), stored_hash, SHA256_LEN, out);
	return true;
}

std::string webauth_session_cookie(void) {
	std::string token;
	bool created = false;
	{
		std::lock_guard<std::mutex> lock(auth_mutex);
		uint8_t key[SHA256_LEN], mac[SHA256_LEN];
		if (!session_key_locked(key, &created))
			return std::string();
		char expiry[32];
		snprintf(expiry, sizeof(expiry), "%lld",
				(long long) time(nullptr) + WEBAUTH_SESSION_SECONDS);
		std::string message = "1." + stored_user + "." + expiry;
		hmac_sha256(key, sizeof(key), (const uint8_t *) message.data(), message.size(),
				mac);
		token = message + "." + to_hex(mac, sizeof(mac));
	}
	if (created)
		websettings_save(); // outside the lock: the save reads back through us
	// No Secure: a QUniLator serves plain HTTP on a workshop network. SameSite
	// is Lax so a link into the interface arrives signed in, and HttpOnly
	// keeps the token out of reach of anything running on the page.
	char header[512];
	snprintf(header, sizeof(header),
			"Set-Cookie: " SESSION_COOKIE "=%s; Path=/; Max-Age=%d; HttpOnly; "
			"SameSite=Lax\r\n", token.c_str(), WEBAUTH_SESSION_SECONDS);
	return header;
}

static bool session_token_valid(const std::string &token) {
	std::lock_guard<std::mutex> lock(auth_mutex);
	// A board that has signed nothing cannot have signed this; asked here so
	// verification never makes a secret of its own.
	if (!configured || !session_secret_valid)
		return false;
	size_t mac_sep = token.rfind('.');
	if (mac_sep == std::string::npos)
		return false;
	std::string message = token.substr(0, mac_sep);
	uint8_t given[SHA256_LEN], mac[SHA256_LEN], key[SHA256_LEN];
	bool created;
	if (from_hex(token.substr(mac_sep + 1), given, sizeof(given)) != sizeof(given))
		return false;
	if (!session_key_locked(key, &created))
		return false;
	hmac_sha256(key, sizeof(key), (const uint8_t *) message.data(), message.size(), mac);
	if (!equal_constant_time(mac, given, SHA256_LEN))
		return false;
	// The fields, now that they are known to be this QUniLator's own.
	if (message.compare(0, 2, "1.") != 0)
		return false;
	size_t expiry_sep = message.rfind('.');
	if (expiry_sep < 2)
		return false;
	if (message.substr(2, expiry_sep - 2) != stored_user)
		return false; // signed for an operator this board no longer carries
	return strtoll(message.c_str() + expiry_sep + 1, nullptr, 10) > (long long) time(nullptr);
}

bool webauth_verify_session(const char *cookies) {
	static const size_t name_len = sizeof(SESSION_COOKIE) - 1;
	if (cookies == nullptr)
		return false;
	// "a=1; qunilator_session=…; b=2", and any of the three may be ours
	for (const char *p = cookies; *p != 0; ) {
		while (*p == ' ' || *p == ';')
			p++;
		const char *eq = strchr(p, '=');
		if (eq == nullptr)
			break;
		const char *end = strchr(eq, ';');
		if (end == nullptr)
			end = eq + strlen(eq);
		if ((size_t) (eq - p) == name_len && strncmp(p, SESSION_COOKIE, name_len) == 0)
			return session_token_valid(std::string(eq + 1, end - eq - 1));
		p = *end == ';' ? end + 1 : end;
	}
	return false;
}

bool webauth_set_credentials(const std::string &user, const std::string &password,
		std::string *error) {
	std::string previous_user;
	{
		std::lock_guard<std::mutex> lock(auth_mutex);
		if (user.empty()) {
			*error = "a user name is required";
			return false;
		}
		if (password.size() < MIN_PASSWORD_LEN) {
			char msg[80];
			snprintf(msg, sizeof(msg), "password must be at least %d characters",
					MIN_PASSWORD_LEN);
			*error = msg;
			return false;
		}
		if (!webshares_name_acceptable(user, error))
			return false;
		if (!random_bytes(stored_salt, sizeof(stored_salt))) {
			*error = "no randomness available for a salt";
			return false;
		}
		stored_iterations = PBKDF2_ITERATIONS;
		pbkdf2_sha256(password, stored_salt, sizeof(stored_salt), stored_iterations,
				stored_hash);
		cache_store(password);
		previous_user = stored_user;
		stored_user = user;
		configured = true;
	}
	websettings_save();
	// The shares authenticate against the OS, so the password reaches the
	// accounts from here - the one place that has it in plain form.
	webshares_apply(previous_user, user, password);
	return true;
}

bool webauth_set_digest(const std::string &user, const std::string &salt_hex,
		const std::string &hash_hex, unsigned iterations, std::string *error) {
	{
		std::lock_guard<std::mutex> lock(auth_mutex);
		if (user.empty()) {
			*error = "a user name is required";
			return false;
		}
		if (!webshares_name_acceptable(user, error))
			return false;
		if (from_hex(salt_hex, stored_salt, sizeof(stored_salt)) != sizeof(stored_salt)) {
			*error = "the salt is not 16 bytes of hex";
			return false;
		}
		if (from_hex(hash_hex, stored_hash, sizeof(stored_hash)) != sizeof(stored_hash)) {
			*error = "the hash is not 32 bytes of hex";
			return false;
		}
		stored_iterations = iterations != 0 ? iterations : PBKDF2_ITERATIONS;
		// Nothing here has seen the password, so there is nothing to remember;
		// the first request that verifies pays the derivation and fills it.
		cache_valid = false;
		stored_user = user;
		configured = true;
	}
	websettings_save();
	return true;
}

void webauth_load(const picojson::value &admin) {
	if (!admin.is<picojson::object>())
		return;
	if (!admin.get("salt").is<std::string>() || !admin.get("hash").is<std::string>())
		return;
	uint8_t salt[SALT_LEN], hash[SHA256_LEN];
	if (from_hex(admin.get("salt").get<std::string>(), salt, sizeof(salt)) != sizeof(salt))
		return;
	if (from_hex(admin.get("hash").get<std::string>(), hash, sizeof(hash)) != sizeof(hash))
		return;
	// A password with no name beside it is a record from before an operator
	// account existed. There is no account it could name, so it is dropped and
	// the first-run dialog asks for both halves - which is the one moment the
	// interface can say so.
	if (!admin.get("user").is<std::string>()
			|| admin.get("user").get<std::string>().empty()) {
		WEB_WARNING("the stored password carries no user name, so it is not in force: "
				"open the web interface to set this QUniLator up");
		return;
	}
	std::lock_guard<std::mutex> lock(auth_mutex);
	// The secret that signs sessions, so the ones this board handed out before
	// a restart still open it. A record from before sessions existed carries
	// none, and the first cookie asked for makes one.
	session_secret_valid = admin.get("session_secret").is<std::string>()
			&& from_hex(admin.get("session_secret").get<std::string>(), session_secret,
					sizeof(session_secret)) == sizeof(session_secret);
	memcpy(stored_salt, salt, sizeof(stored_salt));
	memcpy(stored_hash, hash, sizeof(stored_hash));
	stored_user = admin.get("user").get<std::string>();
	stored_iterations = admin.get("iterations").is<double>()
			? (unsigned) admin.get("iterations").get<double>() : PBKDF2_ITERATIONS;
	if (stored_iterations == 0)
		stored_iterations = PBKDF2_ITERATIONS;
	configured = true;
	cache_valid = false;
}

picojson::value webauth_json(void) {
	std::lock_guard<std::mutex> lock(auth_mutex);
	if (!configured)
		return picojson::value(); // null: nothing of ours to persist
	picojson::object o;
	o["algorithm"] = picojson::value("pbkdf2-sha256");
	o["user"] = picojson::value(stored_user);
	o["iterations"] = picojson::value((double) stored_iterations);
	o["salt"] = picojson::value(to_hex(stored_salt, sizeof(stored_salt)));
	o["hash"] = picojson::value(to_hex(stored_hash, sizeof(stored_hash)));
	if (session_secret_valid)
		o["session_secret"] = picojson::value(
				to_hex(session_secret, sizeof(session_secret)));
	return picojson::value(o);
}

/*** /api/auth ***/

// extra is whole header lines, "\r\n" and all, or empty: this is where the
// session cookie rides out.
static void send_json(struct mg_connection *conn, int status, const picojson::value &val,
		const std::string &extra = std::string()) {
	std::string body = val.serialize();
	mg_printf(conn,
			"HTTP/1.1 %d %s\r\n"
			"Content-Type: application/json\r\n"
			"Cache-Control: no-store\r\n"
			"%s"
			"Content-Length: %u\r\n\r\n",
			status, status == 200 ? "OK" : "Error", extra.c_str(),
			(unsigned) body.size());
	mg_write(conn, body.c_str(), body.size());
}

static void send_error(struct mg_connection *conn, int status, const std::string &message) {
	picojson::object err;
	err["error"] = picojson::value(message);
	send_json(conn, status, picojson::value(err));
}

static bool read_json_body(struct mg_connection *conn, picojson::value *out) {
	char body[4096];
	int body_len = mg_read(conn, body, sizeof(body) - 1);
	if (body_len <= 0)
		return false;
	body[body_len] = 0;
	std::string parse_err = picojson::parse(*out, body);
	return parse_err.empty() && out->is<picojson::object>();
}

static void auth_get(struct mg_connection *conn) {
	picojson::object o;
	o["configured"] = picojson::value(webauth_configured());
	o["user"] = picojson::value(webauth_user());
	o["min_length"] = picojson::value((double) MIN_PASSWORD_LEN);
	// Nothing reaches this on a QUniLator with an operator without having
	// authenticated, so the answer renews the session: a page opened today is
	// good for another five days, and the sign-in dialog is asked for once
	// rather than whenever the browser has forgotten the password.
	send_json(conn, 200, picojson::value(o), webauth_session_cookie());
}

static void auth_put(struct mg_connection *conn) {
	picojson::value body;
	if (!read_json_body(conn, &body)) {
		send_error(conn, 400, "expected a JSON object");
		return;
	}
	bool have_user = body.get("user").is<std::string>();
	bool have_password = body.get("password").is<std::string>();
	if (!have_user && !have_password) {
		send_error(conn, 400, "a user name or a password is required");
		return;
	}
	// Once credentials exist, changing either takes the current password.
	// Basic auth has already been satisfied to get here; this is what stops a
	// left-open browser from being enough.
	std::string current;
	if (webauth_configured()) {
		if (!body.get("current").is<std::string>()
				|| !webauth_verify_password(body.get("current").get<std::string>())) {
			send_error(conn, 403, "the current password does not match");
			return;
		}
		current = body.get("current").get<std::string>();
	} else if (!have_user || !have_password) {
		// A first setup settles both halves at once: the name is an account,
		// and an account is made with a password.
		send_error(conn, 400, "a user name and a password are required");
		return;
	}
	// A request that changes only the name keeps the password in force, and
	// the shares are given that one.
	std::string password = have_password
			? body.get("password").get<std::string>() : current;
	std::string user = have_user ? body.get("user").get<std::string>()
			: webauth_user();
	std::string error;
	if (!webauth_set_credentials(user, password, &error)) {
		send_error(conn, 422, error);
		return;
	}
	// The first-run dialog asks for the host name and an ssh public key
	// beside the credentials and sends all of it in this one request: the key
	// belongs to the operator account, which the credentials are what create,
	// and this is the last request the browser makes before it has to
	// authenticate. Neither is required, and a refusal of either is reported
	// without taking the credentials back - those are set, and the rest can be
	// tried again from the System page.
	picojson::array warnings;
	if (body.get("hostname").is<std::string>()) {
		std::string name = body.get("hostname").get<std::string>();
		std::string why;
		if (!name.empty() && !websystem_set_hostname(name, &why))
			warnings.push_back(picojson::value("the host name was not changed: " + why));
	}
	if (body.get("ssh_key").is<std::string>()) {
		std::string key = body.get("ssh_key").get<std::string>();
		std::string why;
		if (!key.empty() && !webshares_set_ssh_key(webauth_user(), key, &why))
			warnings.push_back(picojson::value("the ssh key was not installed: " + why));
	}
	picojson::object o;
	o["ok"] = picojson::value(true);
	o["user"] = picojson::value(webauth_user());
	o["warnings"] = picojson::value(warnings);
	// The change ended every session, this browser's included - the signing key
	// follows the digest. It leaves with one for the credentials it just set.
	send_json(conn, 200, picojson::value(o), webauth_session_cookie());
}

static int api_auth_handler(struct mg_connection *conn, void * /*cbdata*/) {
	const struct mg_request_info *ri = mg_get_request_info(conn);
	if (!strcmp(ri->request_method, "GET")) {
		auth_get(conn);
		return 200;
	}
	if (!strcmp(ri->request_method, "PUT") || !strcmp(ri->request_method, "POST")) {
		auth_put(conn);
		return 200;
	}
	send_error(conn, 405, "method not allowed");
	return 405;
}

void webauth_register(struct mg_context *ctx) {
	mg_set_request_handler(ctx, "/api/auth", api_auth_handler, nullptr);
}
