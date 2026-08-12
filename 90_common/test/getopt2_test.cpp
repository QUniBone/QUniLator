/* getopt2_test.cpp: tests for the commandline parser of 90_common/src/getopt2.cpp

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


 getopt2 is target independent, so these tests are built and run by the host
 compiler on the build machine, like the CPU core tests of 10.05_cputest.

 Two phases:

 1. Parse cases. A table of commandlines, each parsed with an option set and
	compared against the trace it must produce. Covers the ordinary option
	forms and the ones which arise when "demo" is used as a "#!" script
	interpreter, where the kernel puts the script name in front of the user's
	options and passes the whole tail of the "#!" line as one single argument.

 2. Script cases. The same, but for real: this binary writes a "#!" script
	naming itself as interpreter, executes it, and compares what the child
	reports. Only this proves what the kernel actually hands over; phase 1
	merely simulates it. Environmental failures report SKIP, not FAIL - they
	say nothing about the parser. Reasons: the build directory not writable or
	not executable, the "#!" line longer than the kernel's limit (256 bytes on
	Linux), or a space in the path to this binary, which cannot be a "#!"
	interpreter at all since the kernel splits that line at the first
	whitespace.

 Exit code 0 = all passed, 1 = a case failed, 2 = usage error.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#include <string>
#include <vector>
#include <sstream>

#include "getopt2.hpp"

/*** option sets under test ***/

// One option of an option set, in the terms of getopt_c::define(). An entry with
// empty names describes the non-option ("positional") arguments, as there too.
struct optdef_t {
	const char *short_name;
	const char *long_name;
	const char *fix_args;	// comma separated list of argument names
	const char *var_args;	// comma separated list of optional argument names
};

// A synthetic set which isolates the parser rules from any application: a flag,
// an option with a fixed argument count, one with an optional argument on top,
// and one or two non-option arguments.
static const optdef_t generic_opts[] = { //
		{ "f", "flag", "", "" }, //
				{ "a", "alpha", "a1", "" }, //
				{ "b", "beta", "b1", "b2" }, //
				{ "", "", "n1", "n2" }, //
				{ NULL, NULL, NULL, NULL } };

// The option set of the "demo" application, mirroring the define() calls in
// 10.03_app_demo/2_src/application.cpp parse_commandline().
// It is a copy, not the original: application.cpp cannot be compiled on the
// host, it pulls in the PRU, GPIO and logger stack. Keep the two in sync - what
// is tested here is the option *syntax*, and only these declared argument
// counts decide how a commandline is split up.
// The QBUS-only --addresswidth is left out, --cmdfile and --leds already cover
// an option with one fixed argument.
static const optdef_t demo_opts[] = { //
		{ "?", "help", "", "" }, //
				{ "v", "verbose", "", "" }, //
				{ "dbg", "debug", "", "" }, //
				{ "cf", "cmdfile", "cmdfilename", "" }, //
				{ "leds", "leds", "ledcode", "" }, //
				{ "", "", "cmdfile", "" },	// the command file, as bare argument
				{ NULL, NULL, NULL, NULL } };

// demo_opts plus the flag with which this binary recognizes itself as the
// interpreter of a "#!" script, for phase 2.
static const optdef_t child_opts[] = { //
		{ "child", "child", "", "" }, //
				{ "?", "help", "", "" }, //
				{ "v", "verbose", "", "" }, //
				{ "dbg", "debug", "", "" }, //
				{ "cf", "cmdfile", "cmdfilename", "" }, //
				{ "leds", "leds", "ledcode", "" }, //
				{ "", "", "cmdfile", "" }, //
				{ NULL, NULL, NULL, NULL } };

/*** the parse driver ***/

static const char *status_name(int status)
{
	switch (status) {
	case GETOPT_STATUS_ILLEGALOPTION:
		return "ILLEGALOPTION";
	case GETOPT_STATUS_MINARGCOUNT:
		return "MINARGCOUNT";
	case GETOPT_STATUS_MAXARGCOUNT:
		return "MAXARGCOUNT";
	case GETOPT_STATUS_ILLEGALARG:
		return "ILLEGALARG";
	case GETOPT_STATUS_ARGNOTSET:
		return "ARGNOTSET";
	case GETOPT_STATUS_ARGFORMATINT:
		return "ARGFORMATINT";
	case GETOPT_STATUS_ARGFORMATHEX:
		return "ARGFORMATHEX";
	}
	return "UNKNOWN";
}

static std::vector<std::string> split_csv(const char *csv)
{
	std::vector<std::string> res;
	std::istringstream ss(csv);
	std::string token;
	while (std::getline(ss, token, ','))
		if (!token.empty())
			res.push_back(token);
	return res;
}

// Parse args[] with the given option set and render what the parser returned as
// one canonical line: an option as "<longname>(<arg>=<value>,...)", the
// non-option arguments as "NONOPT(<arg>=<value>,...)", in the order returned,
// a parse failure as a final "ERROR:<statuscode>".
// This is the whole verdict of a parse case: it shows which options were seen,
// in which order, and which argument each of them took.
static std::string parse_trace(const optdef_t *opts, const std::vector<std::string>& args)
{
	getopt_c parser;
	std::ostringstream trace;

	parser.init(/*ignore_case*/true);
	for (const optdef_t *o = opts; o->short_name; o++)
		parser.define(o->short_name, o->long_name, o->fix_args, o->var_args, "", "info", "", "",
				"", "");

	// argv[0] is the program name and never parsed
	std::vector<char*> argv;
	std::string progname = "test";
	argv.push_back(&progname[0]);
	std::vector<std::string> argstore = args; // need writable char *
	for (unsigned i = 0; i < argstore.size(); i++)
		argv.push_back(&argstore[i][0]);

	int res = parser.first(argv.size(), &argv[0]);
	while (res > 0) {
		// which of the definitions did the parser return?
		const optdef_t *found = NULL;
		for (const optdef_t *o = opts; o->short_name && !found; o++) {
			std::string name = *o->long_name ? o->long_name : o->short_name;
			if (parser.isoption(name))
				found = o;
		}
		if (!found) {
			trace << (trace.tellp() > 0 ? " " : "") << "UNMATCHED";
			break;
		}
		trace << (trace.tellp() > 0 ? " " : "");
		trace << (*found->long_name ? found->long_name : "NONOPT") << "(";
		// argument values, in declaration order, those which are set
		std::vector<std::string> argnames = split_csv(found->fix_args);
		std::vector<std::string> optnames = split_csv(found->var_args);
		argnames.insert(argnames.end(), optnames.begin(), optnames.end());
		bool first_arg = true;
		for (unsigned i = 0; i < argnames.size(); i++) {
			std::string value;
			if (parser.arg_s(argnames[i], value) != GETOPT_STATUS_OK)
				continue; // not given on the commandline
			trace << (first_arg ? "" : ",") << argnames[i] << "=" << value;
			first_arg = false;
		}
		trace << ")";
		res = parser.next();
	}
	if (res < 0)
		trace << (trace.tellp() > 0 ? " " : "") << "ERROR:" << status_name(res);
	return trace.str();
}

/*** phase 1: parse cases ***/

struct parsecase_t {
	const char *what;		// what this case verifies
	const optdef_t *opts;	// option set to parse with
	const char *args[6];	// commandline behind the program name, NULL terminated
	const char *expect;		// the trace it must produce
};

static const parsecase_t parsecases[] = {

/*** the parser rules, on the synthetic option set ***/

{ "empty commandline", generic_opts, { NULL }, "" },

		{ "a flag", generic_opts, { "-f", NULL }, "flag()" },

		{ "option names are case insensitive", generic_opts, { "--FLAG", NULL }, "flag()" },

		{ "option with one fixed argument", generic_opts, { "-a", "x", NULL }, "alpha(a1=x)" },

		{ "optional argument given", generic_opts, { "-b", "x", "y", NULL }, "beta(b1=x,b2=y)" },

		{ "optional argument omitted, next option ends the argument list", generic_opts, { "-b",
				"x", "-f", NULL }, "beta(b1=x) flag()" },

		// A flag must not eat what follows it, whatever comes after that.
		// Regression: it used to scan ahead to the next "-option" and take
		// everything up to it, so this failed with MAXARGCOUNT.
		{ "flag followed by a non-option argument", generic_opts, { "-f", "n", NULL },
				"flag() NONOPT(n1=n)" },

		{ "flag between non-option argument and option", generic_opts,
				{ "-f", "n", "-a", "x", NULL }, "flag() NONOPT(n1=n) alpha(a1=x)" },

		// Regression: the non-option arguments used to be parsed to the end of
		// the commandline, swallowing any option behind them: MAXARGCOUNT.
		{ "option after the non-option argument", generic_opts, { "n", "-f", NULL },
				"NONOPT(n1=n) flag()" },

		{ "option with argument after the non-option argument", generic_opts, { "n", "-a", "x",
				NULL }, "NONOPT(n1=n) alpha(a1=x)" },

		{ "option with a fixed argument count takes exactly those", generic_opts, { "-a", "x", "n",
				NULL }, "alpha(a1=x) NONOPT(n1=n)" },

		{ "several non-option arguments form one group", generic_opts, { "n", "m", NULL },
				"NONOPT(n1=n,n2=m)" },

		// An option is allowed to appear between them, and then the parser
		// reports two separate groups. It is up to the application to reject
		// that if a second value makes no sense, as application.cpp does for a
		// second command file.
		{ "non-option arguments separated by an option are two groups", generic_opts, { "n", "-f",
				"m", NULL }, "NONOPT(n1=n) flag() NONOPT(n1=m)" },

		// Deliberate, and documented in getopt2.cpp: for an option with
		// optional arguments there is no way to tell its last argument from a
		// following non-option argument, so the declaration must be unambiguous.
		{ "optional arguments swallow a trailing non-option argument", generic_opts, { "-b", "x",
				"n", NULL }, "beta(b1=x,b2=n)" },

		{ "a lone dash is a non-option argument", generic_opts, { "-", NULL }, "NONOPT(n1=-)" },

		{ "too many non-option arguments", generic_opts, { "n", "m", "o", NULL },
				"ERROR:MAXARGCOUNT" },

		{ "missing option argument", generic_opts, { "-a", NULL }, "ERROR:MINARGCOUNT" },

		{ "undefined option", generic_opts, { "-nosuch", NULL }, "ERROR:ILLEGALOPTION" },

		/*** the option set of "demo": ordinary invocations ***/

		{ "demo without arguments", demo_opts, { NULL }, "" },

		{ "demo -v", demo_opts, { "-v", NULL }, "verbose()" },

		{ "demo --cmdfile testseq", demo_opts, { "--cmdfile", "testseq", NULL },
				"cmdfile(cmdfilename=testseq)" },

		{ "demo -cf testseq -v", demo_opts, { "-cf", "testseq", "-v", NULL },
				"cmdfile(cmdfilename=testseq) verbose()" },

		{ "demo -v -cf testseq", demo_opts, { "-v", "-cf", "testseq", NULL },
				"verbose() cmdfile(cmdfilename=testseq)" },

		{ "demo --leds 3 --verbose", demo_opts, { "--leds", "3", "--verbose", NULL },
				"leds(ledcode=3) verbose()" },

		{ "an option value containing a space stays one value", demo_opts, { "--leds", "3 4", NULL },
				"leds(ledcode=3 4)" },

		/*** the option set of "demo": script invocations ***/

		// "./testseq": the kernel calls the interpreter with the script name as
		// first argument, which is the bare-argument form of --cmdfile.
		{ "script name as bare argument", demo_opts, { "./testseq", NULL },
				"NONOPT(cmdfile=./testseq)" },

		// "./testseq -v": options the user adds land behind the script name.
		{ "option behind the script name", demo_opts, { "./testseq", "-v", NULL },
				"NONOPT(cmdfile=./testseq) verbose()" },

		{ "two options behind the script name", demo_opts, { "./testseq", "--verbose", "--leds",
				"3", NULL }, "NONOPT(cmdfile=./testseq) verbose() leds(ledcode=3)" },

		// "#!/path/demo -v" puts the option in front of the script name.
		{ "option from the \"#!\" line, in front of the script name", demo_opts, { "-v",
				"./testseq", NULL }, "verbose() NONOPT(cmdfile=./testseq)" },

		{ "options from the \"#!\" line and from the invocation", demo_opts, { "-v", "./testseq",
				"-dbg", NULL }, "verbose() NONOPT(cmdfile=./testseq) debug()" },

		// Linux hands the whole tail of the "#!" line over as ONE argument, so
		// "#!/path/demo -v -dbg" arrives as argv[1] = "-v -dbg".
		{ "two options in one \"#!\" argument", demo_opts, { "-v -dbg", "./testseq", NULL },
				"verbose() debug() NONOPT(cmdfile=./testseq)" },

		{ "option with value in one \"#!\" argument", demo_opts, { "--leds 3 -v", "./testseq", NULL },
				"leds(ledcode=3) verbose() NONOPT(cmdfile=./testseq)" },

		{ "\"#!\" argument separated by a tab", demo_opts, { "-v\t-dbg", "./testseq", NULL },
				"verbose() debug() NONOPT(cmdfile=./testseq)" },

		{ "\"#!\" options plus options at the invocation", demo_opts, { "-v -dbg", "./testseq",
				"--leds", "3", NULL },
				"verbose() debug() NONOPT(cmdfile=./testseq) leds(ledcode=3)" },

		// Only argv[1] can come from a "#!" line, so only that one is split.
		{ "no splitting behind argv[1]", demo_opts, { "-v", "-dbg --leds 3", NULL },
				"verbose() ERROR:ILLEGALOPTION" },

		// ... and only when it starts with a dash, so a file name survives.
		{ "a file name containing a space is not split", demo_opts, { "my testseq.cmd", NULL },
				"NONOPT(cmdfile=my testseq.cmd)" },

		/*** errors in script invocations ***/

		{ "two bare arguments", demo_opts, { "./testseq", "other", NULL }, "ERROR:MAXARGCOUNT" },

		// Two command files, which application.cpp rejects itself: for the
		// parser these are simply two non-option groups.
		{ "command file named twice", demo_opts, { "./testseq", "-v", "other", NULL },
				"NONOPT(cmdfile=./testseq) verbose() NONOPT(cmdfile=other)" },

		{ "--leds without its value", demo_opts, { "--leds", NULL }, "ERROR:MINARGCOUNT" },

		{ "undefined option behind the script name", demo_opts, { "./testseq", "-nosuch", NULL },
				"NONOPT(cmdfile=./testseq) ERROR:ILLEGALOPTION" },

};

/*** phase 2: real "#!" script invocations ***/

// What goes behind the interpreter on the "#!" line, what the user adds when
// calling the script, and the trace the child must report. "%s" in the trace is
// the script path, which the kernel passes as it was typed.
struct scriptcase_t {
	const char *what;
	const char *shebang_options;	// behind "#!<this binary>"
	const char *invocation_args;	// behind the script name
	const char *expect;
};

static const scriptcase_t scriptcases[] = {

{ "no option on the \"#!\" line", "--child", "", "child() NONOPT(cmdfile=%s)" },

		{ "option at the invocation", "--child", "-v", "child() NONOPT(cmdfile=%s) verbose()" },

		{ "one option on the \"#!\" line", "--child -v", "",
				"child() verbose() NONOPT(cmdfile=%s)" },

		{ "two options on the \"#!\" line", "--child -v -dbg", "",
				"child() verbose() debug() NONOPT(cmdfile=%s)" },

		{ "options on the \"#!\" line and at the invocation", "--child -v -dbg", "--leds 3",
				"child() verbose() debug() NONOPT(cmdfile=%s) leds(ledcode=3)" },

};

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

/*** test runner ***/

static unsigned cases_run = 0, cases_failed = 0, cases_skipped = 0;

static void check(const char *phase, const char *what, const std::string& expect,
		const std::string& actual, bool verbose)
{
	cases_run++;
	if (expect == actual) {
		if (verbose)
			printf("ok    %s: %s\n           -> %s\n", phase, what,
					actual.empty() ? "<nothing>" : actual.c_str());
		return;
	}
	cases_failed++;
	printf("FAIL  %s: %s\n", phase, what);
	printf("        expected: %s\n", expect.empty() ? "<nothing>" : expect.c_str());
	printf("        actual:   %s\n", actual.empty() ? "<nothing>" : actual.c_str());
}

static void skip(const char *phase, const char *what, const std::string& why)
{
	cases_skipped++;
	printf("SKIP  %s: %s (%s)\n", phase, what, why.c_str());
}

// Run one scriptcase: write a "#!" script naming this binary as its
// interpreter, execute it, return what it printed. ok = false on an
// environmental failure, which is a SKIP and not a FAIL.
static std::string run_script(const std::string& interpreter, const std::string& scriptpath,
		const scriptcase_t& c, bool& ok, std::string& why)
{
	ok = false;
	FILE *f = fopen(scriptpath.c_str(), "w");
	if (!f) {
		why = "cannot write " + scriptpath;
		return "";
	}
	// The "#!" line is the only line that matters here. The rest is what a
	// command file looks like; the parser never sees it.
	fprintf(f, "#!%s %s\n", interpreter.c_str(), c.shebang_options);
	fprintf(f, "# a command file, run by the interpreter named above\n");
	fclose(f);
	if (chmod(scriptpath.c_str(), 0755)) {
		why = "cannot make " + scriptpath + " executable";
		return "";
	}

	// popen() runs this through a shell, so quote the path: the build
	// directory may well sit below a directory name containing a space.
	std::string cmd = "'" + scriptpath + "'";
	if (*c.invocation_args)
		cmd += std::string(" ") + c.invocation_args;
	cmd += " 2>&1";
	FILE *p = popen(cmd.c_str(), "r");
	if (!p) {
		why = "popen failed";
		return "";
	}
	std::string out;
	char buff[512];
	while (fgets(buff, sizeof(buff), p))
		out += buff;
	int status = pclose(p);
	while (!out.empty() && (out[out.size() - 1] == '\n' || out[out.size() - 1] == '\r'))
		out.erase(out.size() - 1);

	// Did the child run at all? It reports its parse as "child(...)". Anything
	// else with a non-zero exit status is the environment refusing to execute
	// the script (no exec permission here, "#!" line too long, ...), which says
	// nothing about the parser.
	if (status != 0 && out.compare(0, 6, "child(") != 0) {
		why = out.empty() ? "script did not execute" : out;
		return "";
	}
	ok = true;
	return out;
}

static void usage(const char *argv0)
{
	fprintf(stderr, "usage: %s [-v]\n", argv0);
	fprintf(stderr, "  Tests the commandline parser getopt2.cpp, see the file header.\n");
	fprintf(stderr, "  -v  list every case, not just the failing ones\n");
	fprintf(stderr, "Exit code 0 = all passed, 1 = a case failed, 2 = usage error.\n");
}

int main(int argc, char *argv[])
{
	// Started as the interpreter of one of the "#!" scripts of phase 2? Then
	// just report how the commandline the kernel built up was parsed.
	// The flag may be the first word of the "#!" tail, which arrives as one
	// single argument, so this cannot use the parser itself to find it.
	if (argc > 1 && !strncmp(argv[1], "--child", 7)) {
		std::vector<std::string> args;
		for (int i = 1; i < argc; i++)
			args.push_back(argv[i]);
		printf("%s\n", parse_trace(child_opts, args).c_str());
		return 0;
	}

	bool verbose = false;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose"))
			verbose = true;
		else {
			usage(argv[0]);
			return 2;
		}
	}

	// phase 1: parse cases
	for (unsigned i = 0; i < sizeof(parsecases) / sizeof(parsecases[0]); i++) {
		const parsecase_t& c = parsecases[i];
		std::vector<std::string> args;
		for (unsigned j = 0; j < sizeof(c.args) / sizeof(c.args[0]) && c.args[j]; j++)
			args.push_back(c.args[j]);
		check("parse", c.what, c.expect, parse_trace(c.opts, args), verbose);
	}

	// phase 2: real "#!" script invocations
	std::string interpreter = own_path(argv[0]);
	std::string scriptpath = interpreter + ".script";
	for (unsigned i = 0; i < sizeof(scriptcases) / sizeof(scriptcases[0]); i++) {
		const scriptcase_t& c = scriptcases[i];
		if (interpreter.empty()) {
			skip("script", c.what, "own path unknown");
			continue;
		}
		bool ok;
		std::string why;
		std::string actual = run_script(interpreter, scriptpath, c, ok, why);
		if (!ok) {
			skip("script", c.what, why);
			continue;
		}
		// the kernel passes the script name as it was typed, here the full path
		char expect[2 * PATH_MAX];
		snprintf(expect, sizeof(expect), c.expect, scriptpath.c_str());
		check("script", c.what, expect, actual, verbose);
	}
	if (!interpreter.empty())
		unlink(scriptpath.c_str());

	printf("getopt2 test: %u cases", cases_run);
	if (cases_skipped)
		printf(", %u skipped", cases_skipped);
	if (cases_failed)
		printf(", %u FAILED\n", cases_failed);
	else
		printf(", all passed\n");
	return cases_failed ? 1 : 0;
}
