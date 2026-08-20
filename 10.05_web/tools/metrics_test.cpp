/* metrics_test.cpp: host test of the metric rate arithmetic

   Copyright (c) 2026, jal
   MIT license, see any source file header for the full text.

   device_metrics.cpp turns a device's monotone counter into the rate the
   performance panel shows. It touches no device and no clock - the caller hands
   it the total and the time - so it runs on the development host with no
   hardware. What is worth testing is not the division but the cases where there
   is no rate to report: the first sample, a counter that restarted, a clock
   that did not move.

   Build & run: 10.05_web/tools/run_config_test.sh
*/

#include <cmath>
#include <cstdio>

#include "device_metrics.hpp"

static int failures = 0;
static int checks = 0;

static void check(bool ok, const char *what) {
	checks++;
	if (!ok) {
		failures++;
		fprintf(stderr, "FAIL: %s\n", what);
	}
}

static bool near(double a, double b) {
	return std::fabs(a - b) < 1e-6;
}

// The ordinary case: a counter climbing at a steady rate over whole seconds,
// and over the odd intervals a 100 ms poll actually produces.
static void test_plain_rate(void) {
	metric_sampler_c m;

	check(!m.sample(0, 1000), "the first sample has nothing to subtract from");
	check(!m.known(), "and so reports no rate at all");

	check(m.sample(500, 2000), "the second sample yields a rate");
	check(m.known(), "which is then known");
	check(near(m.rate(), 500), "500 counts in a second is 500/s");

	check(m.sample(1000, 3000), "and the next");
	check(near(m.rate(), 500), "a steady counter holds its rate");

	// a poll that ran late still reports the rate over the interval it measured
	check(m.sample(1750, 4500), "a late poll still samples");
	check(near(m.rate(), 500), "750 counts in 1500 ms is 500/s");

	// an idle device reports zero, which is a fact and not an absence
	check(m.sample(1750, 5500), "an idle device still samples");
	check(near(m.rate(), 0), "a counter that did not move is 0/s");
	check(m.known(), "0/s is a known rate, not a missing one");
}

// A counter the device resets - an emulated processor's opcode count, which
// restarts at every HALT. The interval the reset falls in has no meaning and is
// dropped; the next one is measured from the restarted counter.
static void test_reset(void) {
	metric_sampler_c m;
	m.sample(0, 1000);
	check(m.sample(400000, 2000), "the machine ran for a second");
	check(near(m.rate(), 400000), "400 kHz");

	// HALT: cycle_count restarts. The next sample reads less than the last.
	check(!m.sample(0, 3000), "a restarted counter yields no rate");
	check(m.known(), "though the rate that was known before still stands");
	check(near(m.rate(), 400000), "and is not overwritten by the dropped interval");

	// and the sampler is anchored on the new counter, not the old
	check(m.sample(200000, 4000), "the interval after the reset samples");
	check(near(m.rate(), 200000), "measured from the restarted counter");
}

// The same holds when nothing was supposed to have reset: a torn read or a
// rebuilt device is what makes a total fall, and neither difference means
// anything, so both are dropped the same way.
static void test_backwards_unexpectedly(void) {
	metric_sampler_c m;
	m.sample(1000, 1000);
	m.sample(2000, 2000);
	check(near(m.rate(), 1000), "climbing normally");
	check(!m.sample(50, 3000), "a total that fell yields no rate");
	check(m.sample(1050, 4000), "and the sampler re-anchors on it");
	check(near(m.rate(), 1000), "measuring from where it now stands");
}

// Two samples of the same millisecond, and a clock that went backwards.
static void test_clock(void) {
	metric_sampler_c m;
	m.sample(0, 1000);
	check(!m.sample(100, 1000), "no interval, no rate");
	check(!m.sample(200, 900), "a clock that went backwards yields no rate");
	check(m.sample(1200, 1900), "and the next interval is measured from there");
	check(near(m.rate(), 1000), "1000 counts in a second");
}

// The percentage beside an emulated processor's rate, and its absence for
// everything else.
static void test_percent(void) {
	check(near(metric_percent(285000, 285000), 100), "at the original's speed is 100%");
	check(near(metric_percent(142500, 285000), 50), "half of it is 50%");
	check(near(metric_percent(570000, 285000), 200), "faster than the original goes past 100%");
	check(near(metric_percent(0, 285000), 0), "a halted machine is 0%");
	check(metric_percent(86000, 0) < 0, "a metric with no reference has no percentage");
	check(metric_percent(86000, -1) < 0, "nor one with a nonsense reference");
}

int main(void) {
	test_plain_rate();
	test_reset();
	test_backwards_unexpectedly();
	test_clock();
	test_percent();

	printf("metrics_test: %d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
