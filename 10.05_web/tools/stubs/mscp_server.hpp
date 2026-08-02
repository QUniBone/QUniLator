/* mscp_server.hpp: host-test stub — see stubs/qunibusadapter.hpp

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.

   Both MSCP protocol engines are classified as infrastructure through their
   common base, so the stub carries the same three types the real header does.
   A polymorphic class is all the dynamic_cast needs.
*/
#ifndef _MSCP_SERVER_HPP_
#define _MSCP_SERVER_HPP_

class mscp_server_base {
public:
	virtual ~mscp_server_base() {}
};

class mscp_disk_server: public mscp_server_base {
};

class mscp_tape_server: public mscp_server_base {
};

using mscp_server = mscp_disk_server;

#endif // _MSCP_SERVER_HPP_
