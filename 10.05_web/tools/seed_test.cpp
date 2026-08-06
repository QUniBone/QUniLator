/* seed_test.cpp: host test of the setup file an SD card carries

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see any source file header for the full text.

   The seed file is written by a workstation tool and read once, on a first boot
   nobody is watching. A file it misreads sets an installation up wrongly, and a
   file it refuses leaves one asking to be set up with a reason in the journal -
   so what it accepts and what it refuses are both worth pinning down.

   Built and run by run_config_test.sh. Exit status is the test result.
*/

#include <stdio.h>
#include <string>

#include "webseed.hpp"

static int failures = 0;

static void check(bool ok, const char *what) {
	printf("%-68s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok)
		failures++;
}

static bool takes(const std::string &text, webseed_c *out) {
	std::string error;
	bool ok = webseed_parse(text, out, &error);
	if (!ok)
		printf("    refused: %s\n", error.c_str());
	return ok;
}

static bool refuses(const std::string &text, const char *because) {
	webseed_c seed;
	std::string error;
	if (webseed_parse(text, &seed, &error))
		return false;
	// the reason has to name the thing that is wrong, or an operator with a
	// card in one hand cannot act on it
	return error.find(because) != std::string::npos;
}

static const char *A_WHOLE_FILE =
		"# written by the QUniLator installer\n"
		"config_version = 1\n"
		"\n"
		"[system]\n"
		"hostname = \"shed-11\"\n"
		"\n"
		"[user]\n"
		"name = \"hans\"\n"
		"password = \"a long enough one\"   # keeps its spaces\n"
		"\n"
		"[ssh]\n"
		"authorized_keys = \"ssh-ed25519 AAAAC3Nz you@workstation\"\n";

int main(void) {
	webseed_c seed;

	printf("--- a whole file\n");
	check(takes(A_WHOLE_FILE, &seed), "a file with every field is read");
	check(seed.user == "hans", "the operator's name");
	check(seed.password == "a long enough one", "the password, spaces and all");
	check(seed.hostname == "shed-11", "the host name");
	check(seed.ssh_key == "ssh-ed25519 AAAAC3Nz you@workstation", "the ssh key");

	printf("--- the least a file may say\n");
	check(takes("[user]\nname = \"op\"\npassword = \"longenough\"\n", &seed),
			"a name and a password are enough");
	check(seed.hostname.empty() && seed.ssh_key.empty(),
			"and leave the rest empty");

	printf("--- what it refuses\n");
	check(refuses("[user]\nname = \"op\"\n", "password"),
			"a file with no password names what is missing");
	check(refuses("[user]\npassword = \"longenough\"\n", "name"),
			"a file with no name says so");
	check(refuses("[user]\nnmae = \"op\"\npassword = \"x\"\n", "nmae"),
			"a mistyped key is refused by name");
	check(refuses("[users]\nname = \"op\"\n", "users"),
			"a mistyped section is refused by name");
	check(refuses("config_version = 2\n[user]\nname = \"op\"\n", "config_version"),
			"a version this does not read is refused");
	check(refuses("[user]\nname = op\npassword = \"x\"\n", "quoted"),
			"an unquoted value is refused");
	check(refuses("name = \"op\"\n", "before any section"),
			"a key outside every section is refused");
	check(refuses("[user\nname = \"op\"\n", "]"),
			"an unclosed section is refused");

	printf("--- the shapes a line comes in\n");
	check(takes("  [user]  \n  name   =   \"op\"  \npassword=\"longenough\"\n", &seed)
			&& seed.user == "op" && seed.password == "longenough",
			"space around names, values and the equals sign");
	check(takes("[user] # who\nname = \"op\" # the operator\npassword = \"x y\"\n", &seed)
			&& seed.password == "x y",
			"comments after a section and after a value");
	check(takes("[user]\r\nname = \"op\"\r\npassword = \"pw\"\r\n", &seed)
			&& seed.user == "op",
			"a file written on Windows, with CRLF line ends");
	check(takes("[user]\nname = \"op\"\npassword = \"quote\\\" and \\\\ back\"\n", &seed)
			&& seed.password == "quote\" and \\ back",
			"a password carrying a quote and a backslash");
	check(refuses("[user]\nname = \"op\"\npassword = \"bell \\a\"\n", "escape"),
			"an escape this does not read is refused");

	printf("\n%s\n", failures == 0 ? "seed_test: all checks passed"
			: "seed_test: FAILURES");
	return failures == 0 ? 0 : 1;
}
