/* device_label.hpp: human-readable device labels from a static per-type table

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.
*/

#ifndef DEVICE_LABEL_HPP_
#define DEVICE_LABEL_HPP_

#include <cstdint>
#include <string>

// Render the plain-language label of a device from the static table, in the
// form "<role> (<code>)". Instanced drives append their unit ("MSCP disk 0
// (RA81)"); same-type instances that are told apart by address carry the CSR
// address ("Serial line unit @777560 (DL11)"); internal devices with no DEC
// code render the bare role ("Front panel"). An unknown type_name falls back
// to the raw handle so every device still renders.
//
//   type_name  the device's type_name (e.g. "RA81")
//   name       the device handle, the fallback for an unknown type_name
//   unit       the "unit" parameter value, "" when the device has none
//   base_addr  the CSR base address, used only for address-disambiguated types
std::string device_label(const std::string &type_name, const std::string &name,
		const std::string &unit, uint32_t base_addr);

#endif // DEVICE_LABEL_HPP_
