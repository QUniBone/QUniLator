/* webseed.cpp: the setup file an SD card carries

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.

   An installation is set up either through the first-run dialog or before the
   card has ever been booted, by writing this file onto the card's FAT
   partition. It names the operator - one identity for the web interface, the
   file shares and ssh - and optionally the host name and an ssh public key.
   The emulator applies it with "--seed", creates the account, and removes the
   file.

     config_version = 1

     [system]
     hostname = "shed-11"

     [user]
     name = "hans"
     password = "a long enough one"

     [ssh]
     authorized_keys = "ssh-ed25519 AAAA… you@workstation"

   The password stands in the clear because one identity opens three doors that
   take it in three different shapes: a PBKDF2 digest for the web interface, an
   NT hash for the file shares, and a crypt(3) hash for the Linux account. A
   hash of any one of them cannot produce the other two, so a file carrying one
   would set up a third of an installation. The card is readable by whoever
   holds it either way - the root filesystem is on the same card - and the file
   is gone after the first boot that reads it.

   The format is a small part of TOML: comments, sections, and a key with a
   quoted string or a bare number. That is all this file needs, and a reader in
   50 lines is worth more here than a dependency the appliance would carry
   into every build.
*/

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

#include <string>

#include "webseed.hpp"

// what the current version of the file may say
static const char *KNOWN_SECTIONS[] = { "system", "user", "ssh", nullptr };

static std::string trimmed(const std::string &s) {
	size_t b = 0, e = s.size();
	while (b < e && isspace((unsigned char) s[b]))
		b++;
	while (e > b && isspace((unsigned char) s[e - 1]))
		e--;
	return s.substr(b, e - b);
}

// A TOML basic string: quoted, with the escapes this file can need. An
// authorized_keys line carries none of them, but a password may carry a
// backslash or a quote.
static bool unquote(const std::string &in, std::string *out, std::string *error) {
	if (in.size() < 2 || in[0] != '"' || in[in.size() - 1] != '"') {
		*error = "a value is a quoted string";
		return false;
	}
	out->clear();
	for (size_t i = 1; i + 1 < in.size(); i++) {
		if (in[i] != '\\') {
			out->push_back(in[i]);
			continue;
		}
		if (i + 2 >= in.size()) {
			*error = "a string ends in a backslash";
			return false;
		}
		switch (in[++i]) {
		case '"': out->push_back('"'); break;
		case '\\': out->push_back('\\'); break;
		case 'n': out->push_back('\n'); break;
		case 't': out->push_back('\t'); break;
		default:
			*error = std::string("\\") + in[i] + " is not an escape this reads";
			return false;
		}
	}
	return true;
}

static bool known(const char *const *list, const std::string &name) {
	for (int i = 0; list[i] != nullptr; i++)
		if (name == list[i])
			return true;
	return false;
}

bool webseed_parse(const std::string &text, webseed_c *out, std::string *error) {
	*out = webseed_c();
	std::string section;
	size_t pos = 0;
	unsigned lineno = 0;
	while (pos <= text.size()) {
		size_t eol = text.find('\n', pos);
		std::string line = trimmed(text.substr(pos,
				eol == std::string::npos ? std::string::npos : eol - pos));
		lineno++;
		if (eol == std::string::npos)
			pos = text.size() + 1;
		else
			pos = eol + 1;

		size_t hash = line.find('#');
		if (hash != std::string::npos)
			line = trimmed(line.substr(0, hash));
		if (line.empty())
			continue;

		char where[48];
		snprintf(where, sizeof(where), " (line %u)", lineno);

		if (line[0] == '[') {
			if (line[line.size() - 1] != ']') {
				*error = "a section name ends in \"]\"" + std::string(where);
				return false;
			}
			section = trimmed(line.substr(1, line.size() - 2));
			if (!known(KNOWN_SECTIONS, section)) {
				*error = "\"" + section + "\" is not a section this reads"
						+ std::string(where);
				return false;
			}
			continue;
		}

		size_t eq = line.find('=');
		if (eq == std::string::npos) {
			*error = "expected \"key = value\"" + std::string(where);
			return false;
		}
		std::string key = trimmed(line.substr(0, eq));
		std::string raw = trimmed(line.substr(eq + 1));
		std::string value;

		// config_version stands outside every section and is a bare number
		if (section.empty() && key == "config_version") {
			if (raw != "1") {
				*error = "config_version " + raw + " is not one this reads"
						+ std::string(where);
				return false;
			}
			continue;
		}
		if (section.empty()) {
			*error = "\"" + key + "\" stands before any section" + std::string(where);
			return false;
		}
		if (!unquote(raw, &value, error)) {
			*error += std::string(where);
			return false;
		}

		if (section == "system" && key == "hostname")
			out->hostname = value;
		else if (section == "user" && key == "name")
			out->user = value;
		else if (section == "user" && key == "password")
			out->password = value;
		else if (section == "ssh" && key == "authorized_keys")
			out->ssh_key = value;
		else {
			*error = "\"" + key + "\" is not a key of [" + section + "]"
					+ std::string(where);
			return false;
		}
	}

	if (out->user.empty() || out->password.empty()) {
		*error = "a seed file names the operator: [user] name and password";
		return false;
	}
	return true;
}
