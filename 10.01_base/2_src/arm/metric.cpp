/* metric.cpp: what a device has done, counted for the performance panel

   Copyright (c) 2026, Hans Huebner, jal
   MIT license, see webserver.hpp for the full text.

   See metric.hpp. A metric registers itself with its device the way a parameter
   does, so a device declares one as a member and nothing else has to know it
   exists. The constructor arguments carry the leading underscore parameter.cpp
   uses for the same reason: the members they set have the names they want.
 */
#include "metric.hpp"
#include "device.hpp"

metric_c::metric_c(device_c *_device, const std::string &_name, unit_e _unit,
		const std::string &_label) :
		device(_device), name(_name), unit(_unit), label(_label) {
	if (device)
		device->metrics.push_back(this);
}

metric_c::metric_c(device_c *_device, const std::string &_name, unit_e _unit,
		const std::string &_label, std::function<uint64_t()> _source) :
		device(_device), name(_name), unit(_unit), label(_label),
		source(_source) {
	if (device)
		device->metrics.push_back(this);
}
