/* scriptpath_test.cpp: tests for 90_common/src/scriptpath.cpp

 Copyright (c) 2026, Joerg Hoppe
 j_hoppe@t-online.de, www.retrocmp.com

 Permission is hereby granted, free of charge, to any person obtaining a
 copy of this software and associated documentation files (the "Software"),
 to deal in the Software without restriction, including without limitation
 the rights to use, copy, modify, merge, publish, distribute, sublicense,
 and/or sell copies of the Software, and to permit persons to whom the
 Software is furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 JOERG HOPPE BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.


 The unit decides where a file named by a command script is opened, so the
 tests work on a real directory tree, built below this binary:

	<bindir>/scriptpath_test.tmp/script/	the "script" directory, with a file,
						a subdirectory and a file in that
	<bindir>/scriptpath_test.tmp/work/	stands for the directory the user
						started the script from

 What matters most is the *negative* half: a name which does not exist next to
 the script must come back unchanged, because that is what makes a file the
 application creates appear in the current working directory instead of next to
 the script.

 A symbolic link to the script is covered too: the example scripts are started
 through shortcuts in the operator's home directory, and the files they name sit
 next to the script itself, not next to the link.

 Exit code 0 = all passed, 1 = a case failed, 2 = usage error.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#include <string>

#include "scriptpath.hpp"

static unsigned cases_run = 0, cases_failed = 0, cases_skipped = 0;
static bool verbose = false;

static void check(const char *what, const std::string& expect, const std::string& actual)
{
	cases_run++;
	if (expect == actual) {
		if (verbose)
			printf("ok    %s\n           -> \"%s\"\n", what, actual.c_str());
		return;
	}
	cases_failed++;
	printf("FAIL  %s\n", what);
	printf("        expected: \"%s\"\n", expect.c_str());
	printf("        actual:   \"%s\"\n", actual.c_str());
}

static void skip(const char *what, const char *why)
{
	cases_skipped++;
	printf("SKIP  %s (%s)\n", what, why);
}

// absolute path of this binary, "" if it cannot be determined
static std::string own_path(const char *argv0)
{
	char buff[PATH_MAX];
	ssize_t n = readlink("/proc/self/exe", buff, sizeof(buff) - 1);
	if (n > 0) {
		buff[n] = 0;
		return std::string(buff);
	}
	if (realpath(argv0, buff))
		return std::string(buff);
	return std::string();
}

static bool make_file(const std::string& path)
{
	FILE *f = fopen(path.c_str(), "w");
	if (!f)
		return false;
	fprintf(f, "test data\n");
	fclose(f);
	return true;
}

static bool exists(const std::string& path)
{
	struct stat statbuff;
	return stat(path.c_str(), &statbuff) == 0;
}

int main(int argc, char *argv[])
{
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose"))
			verbose = true;
		else {
			fprintf(stderr, "usage: %s [-v]\n", argv[0]);
			fprintf(stderr, "  Tests scriptpath.cpp, see the file header.\n");
			fprintf(stderr, "  -v  list every case, not just the failing ones\n");
			return 2;
		}
	}

	/*** cases which need no files at all ***/

	// A path is only ever rewritten while a script is running.
	scriptpath_set("");
	check("no script running: name unchanged", "image.rl02", scriptpath_resolve("image.rl02"));
	check("no script running: directory is empty", "", scriptpath_dir());

	// The directory part of the script name is what gets remembered.
	scriptpath_set("/opt/pdp11/tests/rl.cmd");
	check("directory of the script", "/opt/pdp11/tests", scriptpath_dir());
	scriptpath_set("tests/rl.cmd");
	check("relative script name", "tests", scriptpath_dir());
	scriptpath_set("tests/");
	check("script name with trailing slash", "tests", scriptpath_dir());
	scriptpath_set("rl.cmd");
	check("script in the current directory: no directory to prepend", "", scriptpath_dir());
	check("script in the current directory: name unchanged", "image.rl02",
			scriptpath_resolve("image.rl02"));
	scriptpath_set("/rl.cmd");
	check("script in the root directory", "/", scriptpath_dir());
	// no "//tmp": the one directory which already ends in a slash
	if (exists("/tmp"))
		check("no doubled slash below the root directory", "/tmp", scriptpath_resolve("tmp"));
	else
		skip("no doubled slash below the root directory", "no /tmp on this machine");

	/*** cases on a real directory tree ***/

	std::string bindir = own_path(argv[0]);
	size_t pos = bindir.find_last_of('/');
	bindir = (pos == std::string::npos) ? "" : bindir.substr(0, pos);

	std::string tmpdir = bindir + "/scriptpath_test.tmp";
	std::string scriptdir = tmpdir + "/script";
	std::string workdir = tmpdir + "/work";
	bool tree_ok = !bindir.empty() && mkdir(tmpdir.c_str(), 0755) == 0 //
			&& mkdir(scriptdir.c_str(), 0755) == 0 //
			&& mkdir(workdir.c_str(), 0755) == 0 //
			&& mkdir((scriptdir + "/sub").c_str(), 0755) == 0 //
			&& make_file(scriptdir + "/rl.cmd") //
			&& make_file(scriptdir + "/image.rl02") //
			&& make_file(scriptdir + "/sub/rom.lst") //
			&& make_file(workdir + "/own.rl02");

	if (!tree_ok) {
		skip("everything below a real script directory", "cannot create the test tree");
		printf("scriptpath test: %u cases, %u skipped, %s\n", cases_run, cases_skipped,
				cases_failed ? "FAILED" : "all passed");
		return cases_failed ? 1 : 0;
	}

	scriptpath_set(scriptdir + "/rl.cmd");

	// The point of the whole unit: a file which travels with the script is
	// found next to it, wherever the user started the script from.
	check("existing file next to the script", scriptdir + "/image.rl02",
			scriptpath_resolve("image.rl02"));
	check("existing file in a subdirectory of the script", scriptdir + "/sub/rom.lst",
			scriptpath_resolve("sub/rom.lst"));
	// a shared host directory is named the same way
	check("existing directory next to the script", scriptdir + "/sub", scriptpath_resolve("sub"));

	// The other half: not next to the script means the current directory, which
	// is where a file the application creates has to appear.
	check("file which exists nowhere: unchanged, so it is created in the current directory",
			"new.rl02", scriptpath_resolve("new.rl02"));
	check("file which exists only in the current directory: unchanged", "own.rl02",
			scriptpath_resolve("own.rl02"));
	check("subdirectory which does not exist next to the script: unchanged", "out/dump.bin",
			scriptpath_resolve("out/dump.bin"));

	// An absolute path means itself, always.
	check("absolute path: unchanged", workdir + "/own.rl02",
			scriptpath_resolve(workdir + "/own.rl02"));
	check("absolute path of a file next to the script: unchanged", scriptdir + "/image.rl02",
			scriptpath_resolve(scriptdir + "/image.rl02"));
	check("absolute path which exists nowhere: unchanged", "/nonexistent/image.rl02",
			scriptpath_resolve("/nonexistent/image.rl02"));

	check("empty path", "", scriptpath_resolve(""));

	// Resolving is idempotent: an already resolved (absolute) name stays put.
	check("resolving twice changes nothing", scriptdir + "/image.rl02",
			scriptpath_resolve(scriptpath_resolve("image.rl02")));

	// A script which sits in the directory the user stands in needs no rewriting
	// at all, even for a file which exists there.
	scriptpath_set("rl.cmd");
	check("script without directory part: existing name still unchanged", "image.rl02",
			scriptpath_resolve("image.rl02"));

	// The example scripts are reached through shortcuts in the operator's home
	// directory - symbolic links into 10.03_app_demo/5_applications - and what
	// their images and listings sit next to is the script, not the link. So the
	// link is followed before the directory is taken.
	std::string shortcut = tmpdir + "/shortcut.sh";
	if (symlink((scriptdir + "/rl.cmd").c_str(), shortcut.c_str()) == 0) {
		scriptpath_set(shortcut);
		check("script reached through a symbolic link: directory of the script itself",
				scriptdir, scriptpath_dir());
		check("file next to the script, reached through a symbolic link",
				scriptdir + "/image.rl02", scriptpath_resolve("image.rl02"));
		// and the negative half still holds through the link
		check("file which exists nowhere, reached through a symbolic link: unchanged",
				"new.rl02", scriptpath_resolve("new.rl02"));
		unlink(shortcut.c_str());
	} else
		skip("script reached through a symbolic link", "cannot create the link");

	// clean up the tree
	unlink((scriptdir + "/rl.cmd").c_str());
	unlink((scriptdir + "/image.rl02").c_str());
	unlink((scriptdir + "/sub/rom.lst").c_str());
	unlink((workdir + "/own.rl02").c_str());
	rmdir((scriptdir + "/sub").c_str());
	rmdir(scriptdir.c_str());
	rmdir(workdir.c_str());
	rmdir(tmpdir.c_str());

	printf("scriptpath test: %u cases", cases_run);
	if (cases_skipped)
		printf(", %u skipped", cases_skipped);
	if (cases_failed)
		printf(", %u FAILED\n", cases_failed);
	else
		printf(", all passed\n");
	return cases_failed ? 1 : 0;
}
