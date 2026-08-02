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

// The name this board answers to, its first label.
std::string websystem_hostname(void);

void websystem_register(struct mg_context *ctx);

#endif // _WEBSYSTEM_HPP_
