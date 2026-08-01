/* webpower.cpp: the device half of a power cycle

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.
*/

#include <string.h>

#include <mutex>
#include <string>
#include <vector>

#include "logger.hpp"
#include "device.hpp"
#include "parameter.hpp"
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

// A card the power-down took out of the machine, and the medium its drive held.
struct down_device_t {
	device_c *dev;
	std::string image;      // "" for a device with no medium
	bool had_image;         // whether the device has an image parameter at all
};

// What power-up puts back, in registry order. Guarded by its own mutex: the
// device list is written under operations_mutex by the power calls, and read by
// the API handlers and the event poll, which must not wait on the machine.
static std::mutex down_mutex;
static std::vector<down_device_t> down_devices;
static bool devices_down = false;

bool webpower_devices_are_off(void) {
	std::lock_guard<std::mutex> lock(down_mutex);
	return devices_down;
}

// The entry recording what the power-down took out of this device, or nullptr.
// Caller holds down_mutex.
static const down_device_t *down_entry_locked(device_c *dev) {
	if (!devices_down)
		return nullptr;
	for (const down_device_t &d : down_devices)
		if (d.dev == dev)
			return &d;
	return nullptr;
}

bool webpower_is_in_machine(device_c *dev) {
	if (dev == nullptr)
		return false;
	std::lock_guard<std::mutex> lock(down_mutex);
	return down_entry_locked(dev) != nullptr || dev->enabled.value;
}

std::string webpower_param_value(device_c *dev, parameter_c *p) {
	if (p != nullptr && dev != nullptr && strcasecmp(p->name.c_str(), image_param_name) == 0) {
		std::lock_guard<std::mutex> lock(down_mutex);
		const down_device_t *d = down_entry_locked(dev);
		if (d != nullptr && d->had_image)
			return d->image;
	}
	return p == nullptr ? std::string() : *p->render();
}

// What a power event moved, for the log: the operator reads this to see which
// cards the machine took out and put back.
static std::string device_names(const std::vector<down_device_t> &devs) {
	if (devs.empty())
		return "no devices";
	std::string s;
	for (const down_device_t &d : devs) {
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

void webpower_devices_off(void) {
	if (webpower_devices_are_off())
		return; // already dark: what was taken out stays recorded

	// What the machine carries, read before any of it comes out: a controller
	// leaving takes its drives with it, so a drive read after its controller
	// has gone would look as though it had never been in the machine and would
	// stay out when the power came back.
	std::vector<down_device_t> taken;
	for (device_c *dev : configuration_devices()) {
		if (!dev->enabled.value)
			continue;
		down_device_t d;
		d.dev = dev;
		parameter_string_c *img = image_param_of(dev);
		d.had_image = img != nullptr;
		d.image = img == nullptr ? std::string() : img->value;
		taken.push_back(d);
	}
	{
		std::lock_guard<std::mutex> lock(down_mutex);
		down_devices = taken;
		devices_down = true;
	}

	// Registry order, so a controller leaves before the drives that hang off
	// it: its command engine is stopped and its worker joined before a drive
	// closes the file that engine reads. A drive its controller has already
	// taken with it is switched off again here to no effect.
	for (const down_device_t &d : taken) {
		d.dev->last_error.clear();
		d.dev->enabled.set(false);
		if (d.dev->enabled.value) {
			std::string why = d.dev->last_error;
			WEB_WARNING("%s stayed in the machine over power-down%s%s",
					d.dev->name.value.c_str(), why.empty() ? "" : ": ", why.c_str());
		}
		// The pack stays in the drive: the path is remembered and the parameter
		// cleared, which closes the file. Power-up sets it again and the medium
		// is read afresh, so a partially written image comes back as it stands
		// on disk rather than as the drive last had it in hand.
		if (d.had_image && !d.image.empty()) {
			parameter_string_c *img = image_param_of(d.dev);
			if (img != nullptr)
				img->set("");
			WEB_INFO("%s medium %s set aside for power-down",
					d.dev->name.value.c_str(), d.image.c_str());
		}
	}
	WEB_INFO("power down: %s out of the machine", device_names(taken).c_str());
}

void webpower_devices_on(void) {
	std::vector<down_device_t> restore;
	{
		std::lock_guard<std::mutex> lock(down_mutex);
		if (!devices_down)
			return;
		restore.swap(down_devices);
		devices_down = false;
	}

	// Registry order again, so a controller is back in the machine before its
	// drives. The medium goes in first: a drive announces its capacity from the
	// image it holds, and the controller reads that as the unit comes online.
	for (const down_device_t &d : restore) {
		if (d.had_image && !d.image.empty()) {
			parameter_string_c *img = image_param_of(d.dev);
			if (img != nullptr)
				img->set(d.image);
		}
		d.dev->last_error.clear();
		d.dev->enabled.set(true);
		if (!d.dev->enabled.value) {
			std::string why = d.dev->last_error;
			WEB_ERROR("%s did not come up with the power%s%s", d.dev->name.value.c_str(),
					why.empty() ? "" : ": ", why.c_str());
		}
	}
	WEB_INFO("power up: %s back in the machine", device_names(restore).c_str());
}

void webpower_recapture_if_off(void) {
	if (!webpower_devices_are_off())
		return;
	{
		std::lock_guard<std::mutex> lock(down_mutex);
		down_devices.clear();
		devices_down = false;
	}
	webpower_devices_off();
}
