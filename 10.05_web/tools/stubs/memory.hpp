/* memory.hpp: host-test stub

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.

   webconfigs.cpp places the MSV11 card in one step when a configuration names
   its range. The card answers out of the board's DDR through the PRU, which
   the host test has neither of, so the type is reduced to the placement call
   and every placement is taken.
*/
#ifndef _MEMORY_HPP_
#define _MEMORY_HPP_

#include <cstdint>
#include <string>

#include "device.hpp"

class memory_c: public device_c {
public:
	bool place_at(uint32_t /*start*/, uint32_t /*bytes*/) { return true; }
	bool place_at(uint32_t /*start*/, const std::string & /*sizespec*/) { return true; }
};

#endif // _MEMORY_HPP_
