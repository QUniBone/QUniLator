/* webauth.cpp: admin password for the web interface

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.

   The interface is open until a password is set, which is what makes a fresh
   board reachable at all. The frontend asks for one on first contact and PUTs
   it here; from then on every request needs it, static files and WebSocket
   handshakes included (see begin_request_handler in webserver.cpp).

     GET /api/auth   {configured, source, min_length}
     PUT /api/auth   {password, current?}

   The password is stored as a PBKDF2-HMAC-SHA256 digest over a random salt.
   The build links no crypto library - it is static and civetweb is compiled
   -DNO_SSL - so SHA-256 and PBKDF2 are here in full.

   Basic auth resends the password on every request, and running PBKDF2 that
   often would cost more than serving the page. A password that verified once
   is therefore remembered as a single SHA-256 over a salt generated afresh
   each time the process starts, and later requests are checked against that.
*/

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <mutex>
#include <string>

#include "civetweb.h"
#include "picojson.h"

#include "logger.hpp"
#include "weblog.hpp"
#include "webauth.hpp"
#include "websettings.hpp"

// Propagate a freshly set web password to the qunilator OS user so the SMB,
// FTP and SFTP shares accept the same credential. Best effort, root-only:
// chpasswd sets the PAM/Unix password (FTP + SFTP), smbpasswd the separate
// Samba database. The password is fed on stdin, never on a command line, and
// never logged. A missing user or tool (a dev host) is ignored.
static void feed_password(const char *cmd, const std::string &lines) {
	FILE *p = popen(cmd, "w");
	if (p == nullptr)
		return;
	fwrite(lines.data(), 1, lines.size(), p);
	pclose(p);
}

static void sync_share_password(const std::string &password) {
	if (getuid() != 0 || getpwnam("qunilator") == nullptr)
		return;
	feed_password("chpasswd 2>/dev/null", "qunilator:" + password + "\n");
	feed_password("smbpasswd -s -a qunilator 2>/dev/null",
			password + "\n" + password + "\n");
	WEB_INFO("share password for user qunilator updated from the web password");
}

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
static webauth_source_e source = webauth_source_none;
static std::string env_password;     // WEBUI_PASSWORD, verified as given
static uint8_t stored_salt[SALT_LEN];
static uint8_t stored_hash[SHA256_LEN];
static unsigned stored_iterations = PBKDF2_ITERATIONS;

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
	const char *env = getenv("WEBUI_PASSWORD");
	if (env != nullptr && *env != 0) {
		env_password = env;
		source = webauth_source_environment;
	}
}

webauth_source_e webauth_source(void) {
	std::lock_guard<std::mutex> lock(auth_mutex);
	return source;
}

bool webauth_configured(void) {
	std::lock_guard<std::mutex> lock(auth_mutex);
	return source != webauth_source_none;
}

bool webauth_verify(const std::string &password) {
	std::lock_guard<std::mutex> lock(auth_mutex);
	switch (source) {
	case webauth_source_none:
		return true;
	case webauth_source_environment:
		return password.size() == env_password.size()
				&& equal_constant_time((const uint8_t *) password.data(),
						(const uint8_t *) env_password.data(), password.size());
	case webauth_source_settings:
		break;
	}
	if (cache_matches(password))
		return true;
	uint8_t derived[SHA256_LEN];
	pbkdf2_sha256(password, stored_salt, sizeof(stored_salt), stored_iterations, derived);
	if (!equal_constant_time(derived, stored_hash, SHA256_LEN))
		return false;
	cache_store(password);
	return true;
}

bool webauth_set_password(const std::string &password, std::string *error) {
	{
		std::lock_guard<std::mutex> lock(auth_mutex);
		if (source == webauth_source_environment) {
			*error = "the password comes from WEBUI_PASSWORD and is set outside the interface";
			return false;
		}
		if (password.size() < MIN_PASSWORD_LEN) {
			char msg[80];
			snprintf(msg, sizeof(msg), "password must be at least %d characters",
					MIN_PASSWORD_LEN);
			*error = msg;
			return false;
		}
		if (!random_bytes(stored_salt, sizeof(stored_salt))) {
			*error = "no randomness available for a salt";
			return false;
		}
		stored_iterations = PBKDF2_ITERATIONS;
		pbkdf2_sha256(password, stored_salt, sizeof(stored_salt), stored_iterations,
				stored_hash);
		source = webauth_source_settings;
		cache_store(password);
	}
	websettings_save();
	sync_share_password(password);
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
	std::lock_guard<std::mutex> lock(auth_mutex);
	// WEBUI_PASSWORD is the machine's own setting and outranks the file
	if (source == webauth_source_environment)
		return;
	memcpy(stored_salt, salt, sizeof(stored_salt));
	memcpy(stored_hash, hash, sizeof(stored_hash));
	stored_iterations = admin.get("iterations").is<double>()
			? (unsigned) admin.get("iterations").get<double>() : PBKDF2_ITERATIONS;
	if (stored_iterations == 0)
		stored_iterations = PBKDF2_ITERATIONS;
	source = webauth_source_settings;
	cache_valid = false;
}

picojson::value webauth_json(void) {
	std::lock_guard<std::mutex> lock(auth_mutex);
	if (source != webauth_source_settings)
		return picojson::value(); // null: nothing of ours to persist
	picojson::object o;
	o["algorithm"] = picojson::value("pbkdf2-sha256");
	o["iterations"] = picojson::value((double) stored_iterations);
	o["salt"] = picojson::value(to_hex(stored_salt, sizeof(stored_salt)));
	o["hash"] = picojson::value(to_hex(stored_hash, sizeof(stored_hash)));
	return picojson::value(o);
}

/*** /api/auth ***/

static void send_json(struct mg_connection *conn, int status, const picojson::value &val) {
	std::string body = val.serialize();
	mg_printf(conn,
			"HTTP/1.1 %d %s\r\n"
			"Content-Type: application/json\r\n"
			"Cache-Control: no-store\r\n"
			"Content-Length: %u\r\n\r\n",
			status, status == 200 ? "OK" : "Error", (unsigned) body.size());
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

static const char *source_name(webauth_source_e s) {
	switch (s) {
	case webauth_source_settings:
		return "settings";
	case webauth_source_environment:
		return "environment";
	default:
		return "none";
	}
}

static void auth_get(struct mg_connection *conn) {
	webauth_source_e s = webauth_source();
	picojson::object o;
	o["configured"] = picojson::value(s != webauth_source_none);
	o["source"] = picojson::value(source_name(s));
	o["min_length"] = picojson::value((double) MIN_PASSWORD_LEN);
	send_json(conn, 200, picojson::value(o));
}

static void auth_put(struct mg_connection *conn) {
	picojson::value body;
	if (!read_json_body(conn, &body)) {
		send_error(conn, 400, "expected a JSON object");
		return;
	}
	if (!body.get("password").is<std::string>()) {
		send_error(conn, 400, "password is required");
		return;
	}
	// Once a password exists, changing it takes the current one. Basic auth has
	// already been satisfied to get here; this is what stops a left-open browser
	// from being enough.
	if (webauth_configured()) {
		if (!body.get("current").is<std::string>()
				|| !webauth_verify(body.get("current").get<std::string>())) {
			send_error(conn, 403, "the current password does not match");
			return;
		}
	}
	std::string error;
	if (!webauth_set_password(body.get("password").get<std::string>(), &error)) {
		send_error(conn, 422, error);
		return;
	}
	picojson::object o;
	o["ok"] = picojson::value(true);
	send_json(conn, 200, picojson::value(o));
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
