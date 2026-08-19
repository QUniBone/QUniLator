/* metric.hpp: what a device has done, counted for the performance panel

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.

   A metric is a monotone total of work a device has done since it was built -
   opcodes executed, bytes moved, frames sent. Nothing reads a total: the web
   layer samples it once a second and publishes the difference as a rate, which
   is the only number an operator can use. The total is therefore never reset
   and never displayed, and its width (64 bits) is chosen so it cannot wrap in
   the life of a board.

   Metrics sit beside parameters rather than among them. A parameter is a value
   of the machine - something an operator sets, or a state the emulation reaches
   and holds - and every one of them travels in the event stream on change, is
   compared against a saved configuration, and is rendered in the menus. A
   counter that turns a million times a second is none of those things: it has
   no state to report, no place in a configuration, and publishing it on change
   would be publishing it continuously. Keeping the two apart is what lets the
   CPU count every instruction without the parameter machinery seeing any of it.

   Two ways a device feeds one:

   - counted: the device calls add() as it works. The counter is atomic, so a
     device whose work is spread over several threads (a controller's worker and
     its drives' image access) needs no lock. Use this where the events are
     thousands a second at most.

   - read: the device already keeps the total somewhere, and the metric is given
     a function that reads it when sampled. This costs the device nothing at all
     and is what the emulated processors use: their opcode counters turn in the
     instruction loop, where an atomic add per instruction would be a tax on the
     one number the panel exists to report. Such a total need not even be
     monotone - the CPU's cycle_count restarts at every HALT - because the
     sampler drops any interval a total fell across rather than reporting a
     negative rate; see 10.05_web/2_src/device_metrics.hpp.
 */
#ifndef _METRIC_HPP_
#define _METRIC_HPP_

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class device_c;

class metric_c {
public:
	// What one count is, so a reader formats the rate without knowing the
	// device: bytes become KB/s, everything else is counted per second. The
	// unit travels with the metric rather than being inferred from its name.
	enum unit_e {
		UNIT_COUNT,      // discrete events: transfers, frames, interrupts
		UNIT_BYTE,       // bytes moved
		UNIT_INSTRUCTION // opcodes executed by an emulated processor
	};

	metric_c(device_c *_device, const std::string &_name, unit_e _unit,
			const std::string &_label);
	metric_c(device_c *_device, const std::string &_name, unit_e _unit,
			const std::string &_label, std::function<uint64_t()> _source);

	device_c *device;
	std::string name;   // the key an API caller matches on
	unit_e unit;
	// What the panel writes at the head of the row: a couple of words in the
	// device's own terms ("Read", "Frames sent"). It is a label and not a
	// sentence - the row is a label, a number and a trace, and a description
	// would not fit beside them.
	std::string label;

	// The rate this device's original ran at, in units a second, or 0 when
	// there is nothing to compare against. Only an emulated processor sets one:
	// it is what turns "412000 instructions a second" into "about the speed of
	// the machine this is". See cpu20.cpp/cpu34.cpp for where the figures come
	// from. A device that moves data has no such number - a drive's rate is set
	// by what the guest asks of it, not by how fast the emulation could go.
	double reference_per_second = 0;

	// Add to a counted metric. Hot path: relaxed, because the only ordering
	// that matters is against the sampler's read a second later, and the
	// sampler tolerates reading a value from either side of any store.
	void add(uint64_t n) {
		count.fetch_add(n, std::memory_order_relaxed);
	}

	// The total as the sampler reads it.
	uint64_t total(void) const {
		if (source)
			return source();
		return count.load(std::memory_order_relaxed);
	}

private:
	std::atomic<uint64_t> count{0};
	std::function<uint64_t()> source; // set for a read metric, empty otherwise
};

#endif // _METRIC_HPP_
