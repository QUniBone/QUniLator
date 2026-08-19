/* device_metrics.hpp: turning a device's counters into rates

   Copyright (c) 2026, Hans Huebner, jal
   MIT license, see webserver.hpp for the full text.

   A device counts what it has done and never anything else (see metric.hpp);
   the number an operator can read is the rate, which only exists between two
   samples. This is that arithmetic, and nothing else: no devices, no JSON, no
   clock of its own - the caller passes the total and the time it read it at.
   Keeping it here rather than inside the poll is what lets the reset and clock
   cases be tested on the host, the way device_status.cpp is.

   The awkward cases are all about the total not being the simple monotone thing
   it looks like:

   - the first sample of a metric has nothing to subtract from, and no rate;
   - a total can go backwards, and the difference would be a large negative rate
     rendered as a large positive one. It happens for one expected reason - a
     counter the device resets, as an emulated processor's opcode count restarts
     at every HALT - and for two that should not - a torn read of a 64-bit
     counter on a 32-bit machine, a device rebuilt under a reused address. All
     three make the difference meaningless, so all three are treated alike: the
     interval is dropped and the sampler re-anchors on what it now reads. The
     panel shows one gap rather than one lie;
   - two samples at the same millisecond divide by zero.

   In every one of those the answer is "no rate yet", which the caller publishes
   as nothing at all. A missing number is read as "not known"; a wrong one is
   read as the machine.
*/
#ifndef _DEVICE_METRICS_HPP_
#define _DEVICE_METRICS_HPP_

#include <cstdint>

// The sampler's memory of one metric: what it read, and when. One of these per
// metric, held across polls by whoever is polling.
class metric_sampler_c {
public:
	// Take a sample of a metric's total, read at `now_ms` on a monotonic clock.
	// Returns true when a rate could be computed, leaving it in rate(); false
	// when this sample only anchors the next one.
	bool sample(uint64_t total, uint64_t now_ms);

	// Units per second, from the last sample() that returned true.
	double rate(void) const { return last_rate; }

	// Whether a rate has ever been computed. A metric that has been sampled
	// twice reports 0/s while the device is idle, which is a fact; one that has
	// been sampled once reports nothing, which is a different fact.
	bool known(void) const { return have_rate; }

private:
	uint64_t last_total = 0;
	uint64_t last_ms = 0;
	double last_rate = 0;
	bool have_last = false;
	bool have_rate = false;
};

// A rate as a percentage of what the original ran at. Returns -1 when there is
// nothing to compare against, which is every metric but an emulated
// processor's: the caller then publishes the rate alone.
double metric_percent(double rate, double reference_per_second);

#endif // _DEVICE_METRICS_HPP_
