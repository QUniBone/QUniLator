/* device_configuration.hpp: host-test stub

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.

   webconfigs.cpp takes device_configuration_c::operations_mutex around an apply
   or a save. The host test only needs that one lock, not the full device set,
   so the class is reduced to it. The mutex is defined in config_test.cpp.

   The memory card comes in the way the real header brings it: webconfigs.cpp
   names memory_c without including it, so this is where the type reaches it.
*/
#ifndef _DEVICE_CONFIGURATION_HPP_
#define _DEVICE_CONFIGURATION_HPP_

#include <mutex>

#include "memory.hpp"

class device_configuration_c {
public:
	static std::mutex operations_mutex;
};

#endif // _DEVICE_CONFIGURATION_HPP_
