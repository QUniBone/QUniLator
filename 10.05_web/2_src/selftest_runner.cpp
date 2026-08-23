/* selftest_runner.cpp: run the cli's hardware self-tests as a child process

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.
*/

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/wait.h>

#include "selftest_runner.hpp"

// How long a SIGINT gets to work before SIGKILL. The tests poll the flag every
// few bus cycles, so a healthy child is gone in well under a second; five
// seconds means the child is stuck, not slow.
static const time_t stop_grace_seconds = 5;

// What a test prints when it can name a likely cause for what it just found;
// the contract is SELFTEST_HINT_PREFIX in the cli's selftest.hpp, repeated
// here for the same reason the exit codes are - this module builds without the
// cli's headers. A hint is one line of prose, so a line longer than this is
// not one and is left in the scrollback where it belongs.
static const char hint_prefix[] = "HINT: ";
static const size_t max_line_length = 1024;

// The loopback jumpers the latch tests need. The grant chain leaves the board
// on one pin and comes back on another, so the grant bits of latch 0 (UNIBUS)
// or 6 (QBUS) read back nothing unless the two are strapped together - a test
// run without them fails on exactly those wires. They are fitted for the test
// and pulled again afterwards: a machine cannot arbitrate over a shorted chain.
#if defined(UNIBUS)
static const char *latch_jumper_setup =
		"Fit the 5 loopback jumpers on BG4, BG5, BG6, BG7 and NPG (each IN to "
		"OUT) before the run, and remove them again afterwards - the grant "
		"bits read back nothing without them.";
#else
static const char *latch_jumper_setup =
		"Fit the 2 loopback jumpers on IAKI-IAKO and DMGI-DMGO before the run, "
		"and remove them again afterwards - the grant bits read back nothing "
		"without them.";
#endif

// The catalog. Descriptions, warnings and setup notes are operator-facing text
// the frontend shows verbatim; ids are the cli's --selftest names. QBUS and
// UNIBUS carry almost the same list - the M9302 terminator test exists only
// where there is an M9302, and the probe board has a bus-specific name.
const std::vector<selftest_info_t> &selftest_catalog(void) {
	static const std::vector<selftest_info_t> catalog = {
		{ "latch-single", "Bus latches, one by one", "bus",
			"Writes patterns into each of the 8 bus latch registers from the ARM "
			"and reads them back over the bus wires. Names the wire when a bit "
			"does not come back.",
			"Drives raw bus signals: run only on an empty bus.",
			latch_jumper_setup, false, true, 32, false },
		{ "latch-multi", "Bus latches, all at once", "bus",
			"The PRU exercises all 8 latch registers at full speed with random "
			"values and counts every access that reads back wrong.",
			"Drives raw bus signals: run only on an empty bus.",
			latch_jumper_setup, false, true, 10, false },
		{ "latch-timing", "Latch timing stress", "bus",
			"Maximum-speed read/write sequences on the address and data latches. "
			"Errors are signalled on PRU1.12 for a logic analyzer; the run "
			"itself only reports that it ran.",
			"Drives raw bus signals: run only on an empty bus.",
			latch_jumper_setup, false, true, 10, false },
#if defined(UNIBUS)
		{ "m9302-sack", "M9302 SACK turnaround", "bus",
			"Stimulates the GRANT lines BG4-BG7 and NPG and checks that the "
			"M9302 terminator answers with SACK.",
			"Drives raw bus signals: run only on an empty bus with an M9302 "
			"terminator.",
			"The M9302 must be the terminator, and the BG*/NPG loopback "
			"jumpers of the latch tests must be off: the M9302 answers SACK "
			"only when the grants reach it.",
			false, true, 10, false },
#endif
		{ "probe-leds", "Probe board LEDs", "bus",
			"Switches every bus signal on, then oscillates them one by one in "
			"the order of the probe board's LEDs. The LEDs are the result; "
			"watch them.",
			"Drives raw bus signals: run only on an empty bus.",
			"The probe board must be plugged into the backplane; its LEDs are "
			"what the test reports.",
			false, false, 0, false },
		{ "panel-lamps", "Panel lamps", "panel",
			"Lights each lamp of the I2C console panel for half a second. "
			"Refuses when no panel is fitted.",
			"", "", true, false, 0, false },
		{ "panel-loopback", "Panel loopback", "panel",
			"Every panel switch drives its lamp until the test is stopped. "
			"Refuses when no panel is fitted.",
			"", "", true, true, 0, false },
		{ "gpio-loopback", "Card switches and LEDs", "panel",
			"The card's 4 switches drive its 4 LEDs and the button drives "
			"bus_enable, until the test is stopped.",
			"The button switches the card's bus drivers on: run it on an "
			"empty bus.",
			"", false, true, 0, false },
		{ "mem-sizer", "Size the memory", "memory",
			"Reads upward from address 0 until the bus times out and reports "
			"the range the machine's memory answers.",
			"The machine goes down while the test runs.",
			"", true, false, 0, true },
		{ "mem-address", "Memory test, address pattern", "memory",
			"Fills the machine's memory with each word's own address, then "
			"reads it back over and over.",
			"Overwrites all memory; the machine goes down while the test runs.",
			"", true, true, 30, true },
		{ "mem-random", "Memory test, random", "memory",
			"Fills the machine's memory with fresh random values each pass and "
			"reads them back in random order.",
			"Overwrites all memory; the machine goes down while the test runs.",
			"", true, true, 30, true },
	};
	return catalog;
}

const selftest_info_t *selftest_info_by_id(const std::string &id) {
	for (const selftest_info_t &t : selftest_catalog())
		if (id == t.id)
			return &t;
	return nullptr;
}

selftest_runner_c::selftest_runner_c(const std::string &cli_path,
		output_fn_t output, changed_fn_t changed) :
		cli_path_(cli_path), output_(output), changed_(changed) {
}

selftest_runner_c::~selftest_runner_c() {
	shutdown();
}

bool selftest_runner_c::start(const std::string &test, unsigned seconds,
		unsigned addr_width, std::string *error) {
	std::lock_guard<std::mutex> op_lock(op_mutex_);
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (status_.running) {
			if (error != nullptr)
				*error = "a self-test is already running: " + status_.test;
			return false;
		}
	}
	// the previous run's supervisor has set running=false and is done; reap it
	if (supervisor_.joinable())
		supervisor_.join();

	int pipefd[2];
	if (pipe(pipefd) < 0) {
		if (error != nullptr)
			*error = std::string("pipe: ") + strerror(errno);
		return false;
	}

	char seconds_str[16], width_str[16];
	snprintf(seconds_str, sizeof seconds_str, "%u", seconds);
	snprintf(width_str, sizeof width_str, "%u", addr_width);
	std::vector<const char *> argv;
	argv.push_back(cli_path_.c_str());
	argv.push_back("--selftest");
	argv.push_back(test.c_str());
	argv.push_back(seconds_str);
	if (addr_width != 0) {
		argv.push_back("--addresswidth");
		argv.push_back(width_str);
	}
	argv.push_back(nullptr);

	pid_t pid = fork();
	if (pid < 0) {
		if (error != nullptr)
			*error = std::string("fork: ") + strerror(errno);
		close(pipefd[0]);
		close(pipefd[1]);
		return false;
	}
	if (pid == 0) {
		// A service that dies whole must take the test with it; the claim-fd
		// watchdog in the child covers the service restarting in an orderly
		// way, this covers it not getting that far. The signal is delivered
		// when the forking THREAD exits, which is why the supervisor thread
		// below lives until the child is reaped.
		prctl(PR_SET_PDEATHSIG, SIGINT);
		dup2(pipefd[1], STDOUT_FILENO);
		dup2(pipefd[1], STDERR_FILENO);
		close(pipefd[0]);
		close(pipefd[1]);
		execv(cli_path_.c_str(), const_cast<char *const *>(argv.data()));
		// exec failed: say so through the pipe and answer "could not run"
		fprintf(stderr, "cannot run %s: %s\n", cli_path_.c_str(), strerror(errno));
		_exit(2);
	}
	close(pipefd[1]);

	{
		std::lock_guard<std::mutex> lock(mutex_);
		status_ = status_t();
		status_.running = true;
		status_.test = test;
		status_.started_at = time(nullptr);
		pid_ = pid;
		stop_requested_ = 0;
		line_.clear(); // the previous run's last line is not this run's first
	}
	supervisor_ = std::thread(&selftest_runner_c::supervise, this, pipefd[0], pid);
	if (changed_)
		changed_();
	return true;
}

void selftest_runner_c::scan_for_hint(const char *data, size_t len) {
	for (size_t i = 0; i < len; i++) {
		char c = data[i];
		if (c != '\n' && c != '\r') {
			// a line past the bound is not a hint; keep dropping until it ends
			if (line_.size() < max_line_length)
				line_ += c;
			continue;
		}
		if (!line_.compare(0, sizeof hint_prefix - 1, hint_prefix)) {
			std::string text = line_.substr(sizeof hint_prefix - 1);
			while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
				text.pop_back();
			bool fresh;
			{
				std::lock_guard<std::mutex> lock(mutex_);
				fresh = status_.hint != text;
				status_.hint = text; // the last one stands: it saw the most
			}
			// a stepped test names the cause at the step that failed, so say so
			// then rather than at the verdict
			if (fresh && changed_)
				changed_();
		}
		line_.clear();
	}
}

// The supervisor owns the child from fork to waitpid: it streams the pipe,
// escalates a stop that the child ignored, and writes the verdict. It is the
// thread that forked, so the child's PDEATHSIG stays armed for its lifetime.
void selftest_runner_c::supervise(int pipe_fd, pid_t pid) {
	for (;;) {
		struct pollfd pfd = { pipe_fd, POLLIN, 0 };
		int r = poll(&pfd, 1, 500);
		if (r > 0) {
			char buf[4096];
			ssize_t n = read(pipe_fd, buf, sizeof buf);
			if (n <= 0)
				break; // EOF or error: the child is gone
			scan_for_hint(buf, (size_t) n);
			if (output_)
				output_(buf, (size_t) n);
		}
		// every pass, output or not: is a stop being ignored?
		std::lock_guard<std::mutex> lock(mutex_);
		if (stop_requested_ != 0
				&& time(nullptr) - stop_requested_ >= stop_grace_seconds) {
			kill(pid, SIGKILL);
			stop_requested_ = 0; // once
		}
	}
	close(pipe_fd);

	int wstatus = 0;
	while (waitpid(pid, &wstatus, 0) < 0 && errno == EINTR)
		;

	{
		std::lock_guard<std::mutex> lock(mutex_);
		status_.running = false;
		status_.ended_at = time(nullptr);
		if (WIFEXITED(wstatus)) {
			status_.exit_code = WEXITSTATUS(wstatus);
			switch (status_.exit_code) {
			case 0: status_.verdict = "passed"; break;
			case 1: status_.verdict = "failed"; break;
			default: status_.verdict = "error"; break;
			}
		} else {
			status_.exit_code = -1;
			status_.verdict = "aborted";
		}
		pid_ = -1;
		stop_requested_ = 0;
	}
	if (changed_)
		changed_();
}

bool selftest_runner_c::stop(void) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (!status_.running || pid_ < 0)
		return false;
	if (stop_requested_ == 0) {
		stop_requested_ = time(nullptr);
		kill(pid_, SIGINT);
	}
	return true;
}

selftest_runner_c::status_t selftest_runner_c::status(void) {
	std::lock_guard<std::mutex> lock(mutex_);
	return status_;
}

void selftest_runner_c::shutdown(void) {
	std::lock_guard<std::mutex> op_lock(op_mutex_);
	stop();
	{
		// a stop() at shutdown must not wait the full grace: the service is
		// going down and the child's claim has to close with it
		std::lock_guard<std::mutex> lock(mutex_);
		if (status_.running && pid_ > 0)
			stop_requested_ = time(nullptr) - stop_grace_seconds;
	}
	if (supervisor_.joinable())
		supervisor_.join();
}
