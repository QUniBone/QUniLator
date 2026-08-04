/* webserialports.hpp: which UART carries the Linux login — see webserialports.cpp

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.
*/

#ifndef _WEBSERIALPORTS_HPP_
#define _WEBSERIALPORTS_HPP_

#include <string>
#include <vector>

struct mg_context;

void webserialports_register(struct mg_context *ctx);

// The UARTs the board brings out, in order: "ttyS0", "ttyS1", "ttyS2". A port
// the board does not carry is left out.
std::vector<std::string> webserialports_all(void);

// Whether a Linux login answers on a port, which is what keeps the emulator
// off it. A name that is not one of the UARTs answers false.
bool webserialports_has_login(const std::string &port);

#endif
