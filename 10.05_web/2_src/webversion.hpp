/* webversion.hpp: the version and package identity the service reports

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.
*/
#ifndef _WEBVERSION_HPP_
#define _WEBVERSION_HPP_

#include <string>
#include "civetweb.h"

// The Debian package that owns this binary: "qbone" on a QBUS cape, "unibone"
// on a UNIBUS one. It is also the unit, the binary and the apt source file.
std::string webversion_package(void);

// The version from packaging/debian/changelog, compiled in by the makefiles.
std::string webversion_version(void);

// The compile timestamp as ISO 8601 in the build machine's local time.
std::string webversion_built(void);

// GET /api/version, and writes /run/qunilator/version so a helper outside the
// process can tell which version is running without authenticating.
void webversion_register(struct mg_context *ctx);

#endif
