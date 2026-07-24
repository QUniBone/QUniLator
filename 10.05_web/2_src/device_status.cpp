/* device_status.cpp: verbal runtime status of a disk drive

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.
*/

#include "device_status.hpp"

const char *disk_status(disk_family_e family, const disk_signals_c &s) {
	if (!s.enabled)
		return "off";
	if (!s.has_image)
		return "idle";
	// A transfer outranks the online/coming-online split: a drive only
	// transfers once it is online, and the access lamp is the freshest signal.
	if (s.activity)
		return "busy";
	// online tells loaded from ready. Drives with no modelled spin-up are ready
	// the moment a medium is present (the non-spinning fast path); the RL walks
	// its state machine and is online only at lock-on.
	bool online;
	switch (family) {
	case disk_family_e::rl:
		online = (s.rl_state == DISK_STATUS_RL_LOCK_ON);
		break;
	case disk_family_e::generic:
	default:
		online = true;
		break;
	}
	return online ? "ready" : "loaded";
}
