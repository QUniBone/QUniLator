/* webbus.hpp: one bus-master transfer at a time

   Copyright (c) 2026, Frits Jalvingh
   jal@etc.to
   MIT license, see webserver.hpp for the full text.
*/
#ifndef _WEBBUS_HPP_
#define _WEBBUS_HPP_

#include <mutex>

// The board makes its bus-master transfers through one DMA request object and
// one address-space-sized buffer, so exactly one of them may be in flight.
// Every handler that reaches the bus takes this lock first and holds it for the
// length of the transfer: the reads and writes of /api/memory, and the
// processor-register probe of /api/debug/cpu.
//
// It is also what keeps a completion landing in a live buffer: a stale PRU
// completion can run the copy after the request that asked for it is thought
// done, and the routine interrupt traffic of an enabled device makes those
// common.
//
// An inline function with a static rather than a mutex in a translation unit of
// its own: one instance across the program either way, and a lock this small
// does not need a source file.
inline std::mutex &web_bus_mutex(void) {
	static std::mutex m;
	return m;
}

// How long a transfer made for a web request may wait, in ms.
//
// It bounds the wait for the *bus*, not for a slave: a bus timeout is measured
// in microseconds, and a transfer that gets its cycle is done in well under a
// millisecond. What takes longer than this is a backplane that never grants the
// board anything - a machine switched off, or one whose processor is not
// arbitrating - and there the wait is forever. Unbounded, one such request
// parks a civetweb worker permanently, and a page that reads memory when it
// opens will do that once per visit until the server has no workers left.
//
// A caller passing this must give the transfer a buffer that outlives the call:
// a timed-out request stays scheduled and the PRU may still write into it.
static const unsigned web_bus_timeout_ms = 2000;

// The same bound for a single cycle that is *expected* to find nothing: a probe
// of the processor registers, or a word of a dump walking addresses no card
// answers. Shorter, because a caller making one of these per word would
// otherwise wait out the whole transfer bound for each of them - and anything
// near even this is not a slave failing to answer but the bus never granted.
static const unsigned web_bus_probe_timeout_ms = 250;

#endif // _WEBBUS_HPP_
