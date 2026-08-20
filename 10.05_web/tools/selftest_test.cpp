/* selftest_test.cpp: host test of the self-test child supervisor

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see any source file header for the full text.

   selftest_runner_c forks a program and owns it to the exit code; nothing in
   it needs a board, so the child here is a shell script that plays the cli's
   parts: print and pass, fail, refuse, stop on SIGINT, ignore SIGINT until
   the SIGKILL escalation. What is checked:

     - output arrives through the sink, in order;
     - the exit-code contract maps to the verdicts (0 passed, 1 failed,
       other "error", signal "aborted");
     - a second start while one runs is refused;
     - stop() ends a cooperating child as "passed" and a stuck one by SIGKILL
       as "aborted";
     - a cli path that cannot be exec'd ends as "error" with the reason in the
       output;
     - the changed callback fires on start and on end.

   Build & run: 10.05_web/tools/run_config_test.sh.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include <chrono>
#include <mutex>
#include <string>
#include <thread>

#include "selftest_runner.hpp"

static int failures = 0;
#define CHECK(cond, name) do { \
	if (cond) \
		printf("ok    %s\n", name); \
	else { \
		printf("FAIL  %s\n", name); \
		failures++; \
	} \
} while (0)

// what the runner's callbacks delivered; the sink runs on the supervisor thread
static std::mutex sink_mutex;
static std::string sink_output;
static int sink_changes = 0;

static void reset_sink(void) {
	std::lock_guard<std::mutex> lock(sink_mutex);
	sink_output.clear();
	sink_changes = 0;
}

static std::string output(void) {
	std::lock_guard<std::mutex> lock(sink_mutex);
	return sink_output;
}

static int changes(void) {
	std::lock_guard<std::mutex> lock(sink_mutex);
	return sink_changes;
}

// wait until no test runs, or the deadline; true = it ended
static bool wait_done(selftest_runner_c &r, int timeout_ms) {
	for (int waited = 0; waited < timeout_ms; waited += 50) {
		if (!r.status().running)
			return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	return !r.status().running;
}

// The stub child. argv is the cli's: --selftest <test> <seconds>, so $2 names
// the part to play.
static const char *stub_script =
		"#!/bin/sh\n"
		"case \"$2\" in\n"
		"pass) echo out1; echo out2; exit 0;;\n"
		"fail) echo bad; exit 1;;\n"
		"refuse) echo no; exit 2;;\n"
		// the background sleep gets /dev/null, so its inherited pipe end does
		// not hold the runner's EOF open after the script exits on SIGINT
		"wait-int) trap 'exit 0' INT; echo ready; sleep 30 >/dev/null 2>&1 & wait $!; exit 0;;\n"
		// SIGINT ignored survives the exec: only SIGKILL ends this one
		"stuck) trap '' INT; echo ready; exec sleep 30;;\n"
		"esac\n"
		"exit 3\n";

int main(void) {
	char dir_template[] = "/tmp/selftest_test_XXXXXX";
	const char *dir = mkdtemp(dir_template);
	if (dir == nullptr) {
		perror("mkdtemp");
		return 1;
	}
	std::string stub = std::string(dir) + "/stub-cli";
	FILE *f = fopen(stub.c_str(), "w");
	fputs(stub_script, f);
	fclose(f);
	chmod(stub.c_str(), 0755);

	selftest_runner_c runner(stub,
			[](const char *data, size_t len) {
				std::lock_guard<std::mutex> lock(sink_mutex);
				sink_output.append(data, len);
			},
			[]() {
				std::lock_guard<std::mutex> lock(sink_mutex);
				sink_changes++;
			});

	/*** the catalog ***/
	CHECK(!selftest_catalog().empty(), "catalog has tests");
	CHECK(selftest_info_by_id("latch-multi") != nullptr, "catalog finds latch-multi");
	CHECK(selftest_info_by_id("no-such-test") == nullptr, "catalog refuses unknown id");
	bool ids_unique = true;
	for (const selftest_info_t &a : selftest_catalog()) {
		int seen = 0;
		for (const selftest_info_t &b : selftest_catalog())
			if (!strcmp(a.id, b.id))
				seen++;
		if (seen != 1)
			ids_unique = false;
	}
	CHECK(ids_unique, "catalog ids are unique");
	// Every field is operator-facing text, and a null one would be printed as
	// such; a bus test calling itself machine-safe is the mistake that matters.
	bool text_present = true, bus_tests_unsafe = true;
	for (const selftest_info_t &t : selftest_catalog()) {
		if (t.label == nullptr || t.description == nullptr || t.warning == nullptr
				|| t.setup == nullptr || t.category == nullptr)
			text_present = false;
		if (!strcmp(t.category, "bus") && t.machine_safe)
			bus_tests_unsafe = false;
	}
	CHECK(text_present, "catalog entries carry every text field");
	CHECK(bus_tests_unsafe, "no bus test claims to be machine-safe");
	CHECK(*selftest_info_by_id("latch-single")->setup != 0,
			"the latch tests name the loopback jumpers");

	std::string error;

	/*** a passing run, output in order ***/
	reset_sink();
	CHECK(runner.start("pass", 0, 0, &error), "start pass");
	CHECK(wait_done(runner, 5000), "pass run ends");
	{
		selftest_runner_c::status_t st = runner.status();
		CHECK(st.verdict == "passed" && st.exit_code == 0, "pass verdict");
		CHECK(st.test == "pass" && st.started_at != 0 && st.ended_at != 0,
				"pass status names test and times");
	}
	CHECK(output() == "out1\nout2\n", "pass output in order");
	CHECK(changes() == 2, "changed fired on start and end");

	/*** the exit-code contract ***/
	reset_sink();
	runner.start("fail", 0, 0, &error);
	wait_done(runner, 5000);
	CHECK(runner.status().verdict == "failed" && runner.status().exit_code == 1,
			"exit 1 is failed");
	runner.start("refuse", 0, 0, &error);
	wait_done(runner, 5000);
	CHECK(runner.status().verdict == "error" && runner.status().exit_code == 2,
			"exit 2 is error");

	/*** one at a time, and a cooperative stop ***/
	reset_sink();
	CHECK(runner.start("wait-int", 0, 0, &error), "start wait-int");
	// wait for the child to say it is ready, so the SIGINT finds the trap set
	for (int waited = 0; waited < 5000 && output().find("ready") == std::string::npos;
			waited += 50)
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	CHECK(output().find("ready") != std::string::npos, "wait-int is running");
	CHECK(!runner.start("pass", 0, 0, &error), "second start refused");
	CHECK(error.find("already running") != std::string::npos,
			"refusal names the reason");
	CHECK(runner.stop(), "stop accepted");
	CHECK(wait_done(runner, 5000), "stopped child ends");
	CHECK(runner.status().verdict == "passed",
			"operator stop with exit 0 is a pass");

	/*** the SIGKILL escalation ***/
	reset_sink();
	CHECK(runner.start("stuck", 0, 0, &error), "start stuck");
	for (int waited = 0; waited < 5000 && output().find("ready") == std::string::npos;
			waited += 50)
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	runner.stop();
	CHECK(wait_done(runner, 10000), "stuck child is killed");
	CHECK(runner.status().verdict == "aborted" && runner.status().exit_code == -1,
			"killed child is aborted");

	/*** stop with nothing running ***/
	CHECK(!runner.stop(), "stop refused when idle");

	/*** a cli that cannot run ***/
	{
		reset_sink();
		selftest_runner_c broken(std::string(dir) + "/no-such-cli",
				[](const char *data, size_t len) {
					std::lock_guard<std::mutex> lock(sink_mutex);
					sink_output.append(data, len);
				}, nullptr);
		CHECK(broken.start("pass", 0, 0, &error), "start of a missing cli forks");
		CHECK(wait_done(broken, 5000), "missing cli run ends");
		CHECK(broken.status().verdict == "error", "missing cli is error");
		CHECK(output().find("cannot run") != std::string::npos,
				"missing cli says why");
	}

	unlink(stub.c_str());
	rmdir(dir);

	if (failures) {
		printf("%d FAILURES\n", failures);
		return 1;
	}
	printf("all selftest_runner tests passed\n");
	return 0;
}
