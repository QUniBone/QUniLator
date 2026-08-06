/* webseed.hpp: the setup file an SD card carries

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.
*/

#ifndef _WEBSEED_HPP_
#define _WEBSEED_HPP_

#include <string>

// What a seed file says. Everything but the operator's name and password is
// optional, and an absent field is empty.
struct webseed_c {
	std::string user;
	std::string password;
	std::string hostname;
	std::string ssh_key;
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
