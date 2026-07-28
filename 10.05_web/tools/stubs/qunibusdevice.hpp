/* qunibusdevice.hpp: host-test stub

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.

   webconfigs.cpp reads a bus device's backplane placement to validate a
   configuration: the "slot" parameter, the DEC default behind it, and the
   arbitration requests whose own slots give the device its footprint. The real
   header reaches into the PRU shared memory and the arbitration engine, so the
   host test supplies those four members and nothing else. A synthetic device
   derives from this class to be seen as a bus device.
*/
#ifndef _QUNIBUSDEVICE_HPP_
#define _QUNIBUSDEVICE_HPP_

#include <stdint.h>

#include <vector>

#include "device.hpp"
#include "parameter.hpp"

#define PRIORITY_SLOT_COUNT	32	// backplane slot numbers 0..31 may be used

class priority_request_c {
public:
	uint8_t priority_slot = 0;
	uint8_t get_priority_slot(void) { return priority_slot; }
};

class dma_request_c: public priority_request_c {
};

class intr_request_c: public priority_request_c {
};

class qunibusdevice_c: public device_c {
public:
	parameter_unsigned_c priority_slot = parameter_unsigned_c(this, "slot", "sl",
			false, "", "%d", "backplane slot #", 16, 10);
	uint8_t default_priority_slot = 0;

	std::vector<dma_request_c *> dma_requests;
	std::vector<intr_request_c *> intr_requests;
};

#endif // _QUNIBUSDEVICE_HPP_
