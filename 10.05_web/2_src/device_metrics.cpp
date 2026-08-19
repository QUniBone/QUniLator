/* device_metrics.cpp: turning a device's counters into rates

   Copyright (c) 2026, Hans Huebner, jal
   MIT license, see webserver.hpp for the full text.

   See device_metrics.hpp for what each case means.
*/
#include "device_metrics.hpp"

bool metric_sampler_c::sample(uint64_t total, uint64_t now_ms)
{
	if (!have_last) {
		// the first sample only anchors the next one
		last_total = total;
		last_ms = now_ms;
		have_last = true;
		return false;
	}

	// A total that has gone backwards is a counter that restarted, whatever
	// restarted it: re-anchor on what it now reads and leave the interval
	// unreported.
	if (total < last_total) {
		last_total = total;
		last_ms = now_ms;
		return false;
	}

	// Two samples of the same millisecond have no interval to divide by. Anchor
	// on the later one rather than keeping the older: the totals agree, so the
	// only thing lost is the wait.
	if (now_ms <= last_ms) {
		last_total = total;
		last_ms = now_ms;
		return false;
	}

	uint64_t delta = total - last_total;
	uint64_t elapsed = now_ms - last_ms;
	last_rate = (double) delta * 1000.0 / (double) elapsed;
	last_total = total;
	last_ms = now_ms;
	have_rate = true;
	return true;
}

double metric_percent(double rate, double reference_per_second)
{
	if (reference_per_second <= 0)
		return -1;
	return rate * 100.0 / reference_per_second;
}
