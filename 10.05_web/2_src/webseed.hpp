/* webseed.hpp: the setup file an SD card carries

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.
*/

#ifndef _WEBSEED_HPP_
#define _WEBSEED_HPP_

#include <string>

// What a seed file says. The operator's name and one form of the password are
// required; everything else is optional, and an absent field is empty.
//
// The password comes either in the clear, which is what somebody typing the
// file by hand writes, or already derived - which is what a tool preparing a
// card writes, so the password itself never lands on the card. The three
// derived forms are all needed, because the one identity is checked in three
// places that each want it in their own shape.
struct webseed_c {
	std::string user;
	std::string hostname;
	std::string ssh_key;

	std::string password;    // in the clear, empty when the file carries digests

	std::string web_salt;    // hex, what the web interface checks against
	std::string web_hash;    // hex, PBKDF2-HMAC-SHA256 over the salt
	unsigned web_iterations;
	std::string unix_hash;   // crypt(3), the Linux account and ssh
	std::string nt_hash;     // hex, the file shares

	bool derived(void) const { return password.empty(); }

	webseed_c() : web_iterations(0) {}
};

// Read a seed file's text. True with the fields in *out; false with the reason
// in *error, naming the line it stopped at. A key or a section this version
// does not know is refused rather than passed over: a mistyped one would
// otherwise leave the installation asking to be set up with no clue why.
bool webseed_parse(const std::string &text, webseed_c *out, std::string *error);

// The path an SD card carries it at: the FAT partition that a workstation
// mounts when the card is inserted, so the file can be written from macOS,
// Windows or Linux with no ext4 driver in sight.
#define WEBSEED_PATH "/boot/firmware/qunilator.toml"

#endif // _WEBSEED_HPP_
