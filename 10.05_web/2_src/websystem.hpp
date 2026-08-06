/* websystem.hpp: the board's own name and the operator's ssh key

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.
*/

#ifndef _WEBSYSTEM_HPP_
#define _WEBSYSTEM_HPP_

#include <string>

#include "civetweb.h"

// The board's own name: one DNS label, propagated by qunilator-rename. On
// refusal, false with the reason in *error.
bool websystem_set_hostname(const std::string &name, std::string *error);

// Give the machine a fixed address instead of a leased one. address carries its
// prefix (192.168.1.50/24); router and dns may be empty, and dns holds one or
// more separated by spaces.
//
// This is written as a drop-in beside the bridge's own configuration rather
// than into it: qunilator-setup copies its files back whenever they differ from
// the package's, and would put an edited one on DHCP again at the next run.
// Removing the drop-in is what returns a machine to DHCP.
bool websystem_set_static_address(const std::string &address,
		const std::string &router, const std::string &dns, std::string *error);

// The name this board answers to, its first label.
std::string websystem_hostname(void);

void websystem_register(struct mg_context *ctx);

#endif // _WEBSYSTEM_HPP_
