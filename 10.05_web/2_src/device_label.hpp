/* device_label.hpp: human-readable device labels from a static per-type table

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.
*/

#ifndef DEVICE_LABEL_HPP_
#define DEVICE_LABEL_HPP_

#include <string>

// Render the plain-language label of a device from the static table, in the
// form "<code> <role>" with the instance appended where a machine can carry
// more than one: a drive takes its unit ("RA81 disk 0"), several boards of one
// type take their ordinal ("DZV11 serial mux 0"), and a machine that carries
// one of something takes no number ("UDA50 disk controller"). Internal devices
// with no DEC code render the bare role ("Front panel"). An unknown type_name
// falls back to the raw handle so every device still renders.
//
//   type_name  the device's type_name (e.g. "RA81")
//   name       the device handle, the fallback for an unknown type_name
//   unit       the "unit" parameter value, "" when the device has none
//   instance   the device's ordinal among the registry's devices of its type,
//              used only by the types numbered that way
std::string device_label(const std::string &type_name, const std::string &name,
		const std::string &unit, unsigned instance);

#endif // DEVICE_LABEL_HPP_
