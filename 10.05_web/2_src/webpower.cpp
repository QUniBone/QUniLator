/* webpower.cpp: the machine the board carries while its power is off

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.
*/

#include <string.h>

#include <mutex>
#include <string>
#include <vector>

#include "device.hpp"
#include "parameter.hpp"
#include "qunibusdevice.hpp"
#include "qunibusadapter.hpp"
#include "panel.hpp"
#include "mscp_server.hpp"

#include "weblog.hpp"
#include "webpower.hpp"

// infrastructure: part of the bridge or of a controller's implementation, not
// of the emulated configuration. The bus adapter and the panel driver are the
// board itself; the MSCP/TMSCP protocol engines are device_c only so the
// logging macros work, and they are always active.
static bool device_is_infrastructure(device_c *d) {
	return dynamic_cast<qunibusadapter_c *>(d) != nullptr
			|| dynamic_cast<paneldriver_c *>(d) != nullptr
			|| dynamic_cast<mscp_server_base *>(d) != nullptr;
}

// The medium a drive holds is its "image" parameter, the path of the file the
// drive has open. Naming it keeps this module on the device_c/parameter_c
// abstraction, which is what makes it host testable.
//
// The parameter is moved with set(), the call the devices menu makes. A drive
// locks the parameter while a pack spins, and that lock speaks for the operator
// setting a value by hand on a running machine; the power switch reaches past
// it, the way pulling the supply does.
static const char *image_param_name = "image";

static parameter_string_c *image_param_of(device_c *dev) {
	return dev == nullptr ? nullptr
			: dynamic_cast<parameter_string_c *>(dev->param_by_name(image_param_name));
}

// A card of the dark machine, and the medium its drive holds.
struct carried_device_t {
	device_c *dev;
	std::string image;      // "" for a device with no medium
	bool had_image;         // whether the device has an image parameter at all
};

// The machine the board carries while it is dark, and what power-up configures
// it from. Guarded by its own mutex: it is written under operations_mutex by
// the power and edit calls, and read by the API handlers and the event poll,
// which must not wait on the machine.
static std::mutex carried_mutex;
static std::vector<carried_device_t> carried;
static bool machine_dark = false;

bool webpower_devices_are_off(void) {
	std::lock_guard<std::mutex> lock(carried_mutex);
	return machine_dark;
}

void webpower_forget(void) {
	std::lock_guard<std::mutex> lock(carried_mutex);
	carried.clear();
	machine_dark = true;
}

// This device's entry in the dark machine, or nullptr. Caller holds
// carried_mutex.
static carried_device_t *carried_entry_locked(device_c *dev) {
	if (!machine_dark)
		return nullptr;
	for (carried_device_t &d : carried)
		if (d.dev == dev)
			return &d;
	return nullptr;
}

bool webpower_is_in_machine(device_c *dev) {
	if (dev == nullptr)
		return false;
	std::lock_guard<std::mutex> lock(carried_mutex);
	return carried_entry_locked(dev) != nullptr || dev->enabled.value;
}

std::string webpower_param_value(device_c *dev, parameter_c *p) {
	if (p != nullptr && dev != nullptr && strcasecmp(p->name.c_str(), image_param_name) == 0) {
		std::lock_guard<std::mutex> lock(carried_mutex);
		const carried_device_t *d = carried_entry_locked(dev);
		if (d != nullptr && d->had_image)
			return d->image;
	}
	return p == nullptr ? std::string() : *p->render();
}

// What a power event moved, for the log: the operator reads this to see which
// cards the machine took out and put back.
static std::string device_names(const std::vector<carried_device_t> &devs) {
	if (devs.empty())
		return "no devices";
	std::string s;
	for (const carried_device_t &d : devs) {
		if (!s.empty())
			s += " ";
		s += d.dev->name.value;
	}
	return s;
}

// The devices to cycle, in registry order, sampled under mydevices_mutex so the
// power calls hold only operations_mutex while they work on them.
static std::vector<device_c *> configuration_devices(void) {
	std::vector<device_c *> devs;
	std::lock_guard<std::mutex> lock(device_c::mydevices_mutex);
	for (device_c *d : device_c::mydevices)
		if (!device_is_infrastructure(d))
			devs.push_back(d);
	return devs;
}

// The dark machine in registry order, so a controller is handled before the
// drives that hang off it. The order is taken from the registry each time it is
// needed, which leaves an edit free to add a card wherever the operator does.
static std::vector<carried_device_t> carried_in_registry_order(void) {
	std::vector<device_c *> order = configuration_devices();
	std::vector<carried_device_t> out;
	std::lock_guard<std::mutex> lock(carried_mutex);
	for (device_c *dev : order)
		for (const carried_device_t &d : carried)
			if (d.dev == dev) {
				out.push_back(d);
				break;
			}
	return out;
}

// The card of the dark machine whose I/O-page window overlaps this one's, or ""
// when the window is free. Two cards answering one address would put two slaves
// on a cycle; the bus model takes that as a fatal error at registration, so a
// machine is never brought up carrying one.
// Caller holds carried_mutex.
static std::string address_conflict_locked(device_c *dev) {
	qunibusdevice_c *qd = dynamic_cast<qunibusdevice_c *>(dev);
	if (qd == nullptr || qd->register_count == 0)
		return "";
	qunibusdevice_c::iopage_range_t r = qd->iopage_range();
	for (const carried_device_t &d : carried) {
		qunibusdevice_c *other = dynamic_cast<qunibusdevice_c *>(d.dev);
		if (other == nullptr || other == qd || other->register_count == 0)
			continue;
		qunibusdevice_c::iopage_range_t o = other->iopage_range();
		if (r.addr_first < o.addr_beyond && o.addr_first < r.addr_beyond)
			return other->name.value;
	}
	return "";
}

// A new entry describing this device as it stands.
static carried_device_t entry_for(device_c *dev) {
	carried_device_t d;
	d.dev = dev;
	parameter_string_c *img = image_param_of(dev);
	d.had_image = img != nullptr;
	d.image = img == nullptr ? std::string() : img->value;
	return d;
}

// Take the given cards out of the emulation, each giving up the medium it
// holds. The pack stays in the drive: the path is held in the dark machine and
// the parameter cleared, which closes the file. Power-up sets it again and the
// medium is read afresh, so a partially written image comes back as it stands
// on disk rather than as the drive last had it in hand.
static void take_out(const std::vector<carried_device_t> &devs, const char *why_moved) {
	for (const carried_device_t &d : devs) {
		d.dev->last_error.clear();
		d.dev->enabled.set(false);
		if (d.dev->enabled.value) {
			std::string why = d.dev->last_error;
			WEB_WARNING("%s stayed in the machine over %s%s%s", d.dev->name.value.c_str(),
					why_moved, why.empty() ? "" : ": ", why.c_str());
		}
		if (d.had_image && !d.image.empty()) {
			parameter_string_c *img = image_param_of(d.dev);
			if (img != nullptr)
				img->set("");
			WEB_INFO("%s medium %s set aside for %s", d.dev->name.value.c_str(),
					d.image.c_str(), why_moved);
		}
	}
}

void webpower_devices_off(void) {
	if (webpower_devices_are_off())
		return; // already dark: the machine it carries stands

	// What the machine carries, read before any of it comes out: a controller
	// leaving takes its drives with it, so a drive read after its controller
	// has gone would look as though it had never been in the machine and would
	// stay out when the power came back.
	std::vector<carried_device_t> taken;
	for (device_c *dev : configuration_devices())
		if (dev->enabled.value)
			taken.push_back(entry_for(dev));
	{
		std::lock_guard<std::mutex> lock(carried_mutex);
		carried = taken;
		machine_dark = true;
	}

	// Registry order, so a controller leaves before the drives that hang off
	// it: its command engine is stopped and its worker joined before a drive
	// closes the file that engine reads. A drive its controller has already
	// taken with it is switched off again here to no effect.
	take_out(taken, "power-down");
	WEB_INFO("power down: %s out of the machine", device_names(taken).c_str());
}

bool webpower_devices_on(std::string *error) {
	std::vector<carried_device_t> restore = carried_in_registry_order();
	{
		std::lock_guard<std::mutex> lock(carried_mutex);
		if (!machine_dark)
			return true;
		// Two cards at one address are read before any of them is installed:
		// the bus model ends the emulator over that collision, so the machine
		// stays dark and says which two cards it is.
		for (const carried_device_t &d : carried) {
			std::string other = address_conflict_locked(d.dev);
			if (other.empty())
				continue;
			std::string message = d.dev->name.value + " answers the addresses \""
					+ other + "\" answers";
			WEB_ERROR("power up refused: %s", message.c_str());
			if (error != nullptr)
				*error = message;
			return false;
		}
		carried.clear();
		machine_dark = false;
	}

	// Registry order, so a controller is in the machine before its drives. The
	// medium goes in first: a drive announces its capacity from the image it
	// holds, and the controller reads that as the unit comes online.
	for (size_t i = 0; i < restore.size(); i++) {
		const carried_device_t &d = restore[i];
		if (d.had_image && !d.image.empty()) {
			parameter_string_c *img = image_param_of(d.dev);
			if (img != nullptr)
				img->set(d.image);
		}
		d.dev->last_error.clear();
		d.dev->enabled.set(true);
		if (d.dev->enabled.value)
			continue;

		// A card the machine will not take leaves it standing dark, holding the
		// configuration as it was: the operator sees which card it was and can
		// change it before switching on again. What already came up goes back
		// out, so the machine is dark as a whole rather than half configured.
		std::string why = d.dev->last_error;
		std::string message = d.dev->name.value + " did not come up with the power"
				+ (why.empty() ? std::string() : ": " + why);
		WEB_ERROR("power up refused: %s", message.c_str());
		{
			std::lock_guard<std::mutex> lock(carried_mutex);
			carried = restore;
			machine_dark = true;
		}
		std::vector<carried_device_t> installed(restore.begin(), restore.begin() + i + 1);
		take_out(installed, "the refused power-up");
		if (error != nullptr)
			*error = message;
		return false;
	}
	WEB_INFO("power up: %s in the machine", device_names(restore).c_str());
	return true;
}

// ---- editing the dark machine ----
//
// While the board's power is off, nothing an operator changes reaches the
// emulation: the card set and the media are held here, and power-up configures
// the machine from them.

bool webpower_set_in_machine(device_c *dev, bool on, std::string *error) {
	if (dev == nullptr) {
		if (error != nullptr)
			*error = "unknown device";
		return false;
	}
	std::lock_guard<std::mutex> lock(carried_mutex);
	if (!machine_dark) {
		if (error != nullptr)
			*error = "the machine is switched on";
		return false;
	}
	if (on) {
		if (carried_entry_locked(dev) == nullptr) {
			std::string other = address_conflict_locked(dev);
			if (!other.empty()) {
				if (error != nullptr)
					*error = "device \"" + dev->name.value + "\" answers the addresses \""
							+ other + "\" answers";
				return false;
			}
			carried.push_back(entry_for(dev));
		}
		WEB_INFO("%s put in the switched-off machine", dev->name.value.c_str());
		return true;
	}
	// A controller takes its drives with it, the way unplugging one does on a
	// machine with its power on.
	std::vector<carried_device_t> kept;
	for (const carried_device_t &d : carried)
		if (d.dev != dev && d.dev->parent != dev)
			kept.push_back(d);
	carried.swap(kept);
	WEB_INFO("%s taken out of the switched-off machine", dev->name.value.c_str());
	return true;
}

bool webpower_set_image(device_c *dev, const std::string &path, std::string *error) {
	parameter_string_c *img = image_param_of(dev);
	if (img == nullptr) {
		if (error != nullptr)
			*error = "the device holds no medium";
		return false;
	}
	std::lock_guard<std::mutex> lock(carried_mutex);
	if (!machine_dark) {
		if (error != nullptr)
			*error = "the machine is switched on";
		return false;
	}
	carried_device_t *d = carried_entry_locked(dev);
	if (d == nullptr) {
		if (path.empty())
			return true; // an empty drive bay stays empty
		// Putting a medium in is the operator saying they want this drive in
		// the machine, so it goes in with the same call their own switch makes.
		// A drive on a controller the machine does not carry cannot come up at
		// all, so that is reported rather than leaving the drive silently out.
		if (dev->parent != nullptr && carried_entry_locked(dev->parent) == nullptr) {
			if (error != nullptr)
				*error = "put controller \"" + dev->parent->name.value
						+ "\" in the machine before putting a medium in "
						+ dev->name.value;
			return false;
		}
		carried.push_back(entry_for(dev));
		d = &carried.back();
	}
	d->image = path;
	WEB_INFO("%s medium %s held for power-up", dev->name.value.c_str(),
			path.empty() ? "removed" : path.c_str());
	return true;
}

std::string webpower_image_held_by(const std::string &path, const std::string &except) {
	if (path.empty())
		return "";
	std::lock_guard<std::mutex> lock(carried_mutex);
	if (!machine_dark)
		return "";
	for (const carried_device_t &d : carried)
		if (d.image == path && d.dev->name.value != except)
			return d.dev->name.value;
	return "";
}
