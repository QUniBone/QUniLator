/* selftest_runner.hpp: run the cli's hardware self-tests as a child process

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.

   The service never runs a self-test in-process: the tests load their own PRU
   firmware and drive raw bus signals, so they run in "<name>-cli --selftest
   <test>", which takes the board claim like the interactive menu does - the
   service yields the machine, the child tests, the child's exit gives the
   machine back. This module owns only the child: spawning it, streaming its
   stdout, stopping it (SIGINT, then SIGKILL), and turning its exit status into
   a verdict. It knows nothing of civetweb, which is what makes it
   host-testable with a shell script for a child.
*/
#ifndef _SELFTEST_RUNNER_HPP_
#define _SELFTEST_RUNNER_HPP_

#include <ctime>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// One entry of the test catalog the API serves. The list is the service's:
// the cli refuses unknown names on its own, but what an operator is offered -
// names, warnings, sensible bounds - is decided here.
struct selftest_info_t {
	const char *id;            // the cli's --selftest name
	const char *label;
	const char *category;      // "bus" | "panel" | "memory"
	const char *description;
	const char *warning;       // "" = none
	const char *setup;         // hardware to fit before the run, "" = none
	// May be run with the board fitted in a machine that is more than an empty
	// terminated backplane. False for everything that drives raw bus signals:
	// see the acceptance-test procedure the frontend links to.
	bool machine_safe;
	bool unbounded;            // loops until stopped; wants a seconds bound
	unsigned default_seconds;  // suggested bound, 0 = self-bounded
	bool needs_addr_width;     // QBUS: pass the configured address width along
};

const std::vector<selftest_info_t> &selftest_catalog(void);
const selftest_info_t *selftest_info_by_id(const std::string &id);

class selftest_runner_c {
public:
	// What one run ended as. The exit-code contract is selftest.hpp in the cli:
	// 0 passed, 1 failed, 2 could not run; death by signal is "aborted".
	struct status_t {
		bool running = false;
		std::string test;      // the running test, or the last one; "" = never ran
		// A likely cause the test named for itself, from the last "HINT: " line
		// it printed (SELFTEST_HINT_PREFIX in the cli's selftest.hpp): the
		// missing loopback jumpers, above all. "" = the test said nothing.
		std::string hint;
		std::string verdict;   // "" while running: "passed","failed","error","aborted"
		int exit_code = -1;    // -1: killed by signal, or still running
		time_t started_at = 0;
		time_t ended_at = 0;
	};

	// output: each chunk the child prints, from the supervisor thread.
	// changed: a run started or ended; the callee reads status() for what.
	typedef std::function<void(const char *data, size_t len)> output_fn_t;
	typedef std::function<void(void)> changed_fn_t;

	selftest_runner_c(const std::string &cli_path, output_fn_t output,
			changed_fn_t changed);
	~selftest_runner_c();

	// Spawn "<cli> --selftest <test> [--seconds n] [--addresswidth w]".
	// addr_width 0 omits the option. False with the reason when a test is
	// already running or the spawn fails; the test name is NOT checked here -
	// the caller consults the catalog, and the cli is the final authority.
	bool start(const std::string &test, unsigned seconds, unsigned addr_width,
			std::string *error);

	// Ask the running child to stop: SIGINT now, SIGKILL if it has not exited
	// five seconds later. False when nothing runs. Idempotent while it runs.
	bool stop(void);

	status_t status(void);

	// Stop any child and wait the supervisor out. Called at service shutdown,
	// so the child's board claim is closed before the service's socket goes.
	void shutdown(void);

private:
	void supervise(int pipe_fd, pid_t pid);
	// Assemble the child's output into lines as it streams past and keep the
	// text of any that announces a likely cause. Bare CR ends a line too: the
	// progress bars redraw with it, so a line that never sees LF still ends.
	void scan_for_hint(const char *data, size_t len);

	std::string cli_path_;
	output_fn_t output_;
	changed_fn_t changed_;

	// op_mutex_ serializes start/shutdown against each other; mutex_ guards the
	// fields below and is never held across a join, which the supervisor's own
	// final status write would deadlock on.
	std::mutex op_mutex_;
	std::mutex mutex_;
	status_t status_;
	pid_t pid_ = -1;             // running child, -1 when idle
	time_t stop_requested_ = 0;  // when SIGINT was sent, 0 = not asked to stop
	std::string line_;           // the child's output line being assembled
	std::thread supervisor_;     // joined before reuse and at shutdown
};

#endif // _SELFTEST_RUNNER_HPP_
