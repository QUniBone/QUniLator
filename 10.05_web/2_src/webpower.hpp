/* webpower.hpp: the device half of a power cycle

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.

   Driving DCLO/ACLO on the bus resets a device's registers and leaves
   everything else it holds — its worker threads, its controller state machine,
   its drive mechanics and the media those drives hold — running across the
   cycle. A card that loses its supply keeps none of that.

   So the power cycle takes every card out of the machine and puts it back:
   each enabled device is switched off through the "enabled" parameter and
   switched on again, which is the teardown the devices menu and the REST
   parameter endpoint already run — worker threads stopped and started, the
   device unregistered from the bus and re-installed, and install()'s own DCLO
   cycle over the fresh state.

   A drive's pack stays in the drive: the medium it holds is remembered while
   the drive is out of the machine and put back as it comes up, so the image
   file is closed and read afresh rather than lost.

   The two halves are separate so the panel's AUX OFF/ON reads as it should:
   dc_off runs webpower_devices_off() and leaves the machine dark, dc_on runs
   webpower_devices_on() and brings it up from cold. A power cycle is the two
   in sequence.

   Both calls expect the caller to hold device_configuration_c::operations_mutex,
   the same serialization every other device operation runs under.
*/
#ifndef _WEBPOWER_HPP_
#define _WEBPOWER_HPP_

#include <string>

class device_c;
class parameter_c;

// Take every enabled device out of the machine, remembering what to bring back
// and what medium each drive held. Switching an already-dark machine off again
// changes nothing.
void webpower_devices_off(void);

// Put back what webpower_devices_off() took out: each device's medium first,
// then the device itself, in registry order so a controller is in the machine
// before the drives that hang off it. A device the machine refuses is logged
// with the reason it gave and left out.
void webpower_devices_on(void);

// True while the devices are out of the machine.
bool webpower_devices_are_off(void);

// A configuration applied to a switched-off machine leaves it switched off. The
// devices the new configuration names are what power-up will bring in, so they
// are taken back out and remembered in place of what the last one held.
void webpower_recapture_if_off(void);

// What the machine carries, whatever its power state. Losing power does not
// unplug a card or eject a pack, so everything that describes the machine — the
// device list, a configuration snapshot, the modified comparison — reads the
// device set through these two. The live "enabled" flag and the live parameter
// still say what is on the bus this instant, which is what the verbal device
// status and the lamps are built from.
bool webpower_is_in_machine(device_c *dev);
std::string webpower_param_value(device_c *dev, parameter_c *p);

#endif // _WEBPOWER_HPP_
