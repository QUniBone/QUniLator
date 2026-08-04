/* unibuscpu.hpp: host-test stub

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.

   webconfigs.cpp names unibuscpu_c in a dynamic_cast, to recognise the
   configuration that puts an emulated processor on the bus and warn about it.
   The real class carries the interrupt and DMA callbacks of a processor and
   reaches into the PRU; the host test needs only a type a synthetic device can
   derive from, so the cast compiles and a test can produce a device it matches.
*/
#ifndef _UNIBUSCPU_HPP_
#define _UNIBUSCPU_HPP_

#include "qunibusdevice.hpp"

class unibuscpu_c: public qunibusdevice_c {
public:
	virtual ~unibuscpu_c() {}
};

#endif // _UNIBUSCPU_HPP_
