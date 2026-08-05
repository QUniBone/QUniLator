/* webpower.hpp: the machine the board carries while its power is off

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

   What the machine *is* survives that, and is held here: the cards it carries
   and the medium in each drive. A drive's pack stays in the drive — the image
   file is closed and read afresh rather than lost — so a dark machine still
   describes the configuration it holds.

   While the power is off that record is the machine. An operator edits it
   through the same endpoints a running machine is edited through, nothing they
   change reaches the emulation, and power-up configures the machine from it. A
   card the machine will not take refuses the power-up and leaves it dark.

   The two halves are separate so the panel's AUX OFF/ON reads as it should:
   dc_off runs webpower_devices_off() and leaves the machine dark, dc_on runs
   webpower_devices_on() and brings it up from cold. A power cycle is the two
   in sequence.

   Every call expects the caller to hold device_configuration_c::operations_mutex,
   the same serialization every other device operation runs under.
*/
#ifndef _WEBPOWER_HPP_
#define _WEBPOWER_HPP_

#include <string>

class device_c;
class parameter_c;

// Take every enabled device out of the machine, holding its card set and media
// as the machine the board carries dark. Switching an already-dark machine off
// again changes nothing.
void webpower_devices_off(void);

// Configure the machine from what it carries: each device's medium first, then
// the device itself, in registry order so a controller is in the machine before
// the drives that hang off it. A device the machine refuses puts back out what
// had come up, leaves the machine dark with what it carries untouched, and
// answers false with the reason the device gave in *error.
bool webpower_devices_on(std::string *error);

// True while the machine is dark.
bool webpower_devices_are_off(void);

// Drop the record of the dark machine, for a caller about to destroy the device
// set it names. The record holds each card by address, and an address is only
// meaningful while the object behind it lives: a set built again over freed
// memory could hand one card's medium to another. The machine is left dark, and
// what it carries is established again from the configuration.
void webpower_forget(void);

// What the machine carries, whatever its power state. Losing power does not
// unplug a card or eject a pack, so everything that describes the machine — the
// device list, a configuration snapshot, the modified comparison — reads the
// device set through these two. The live "enabled" flag and the live parameter
// still say what is on the bus this instant, which is what the verbal device
// status and the lamps are built from.
bool webpower_is_in_machine(device_c *dev);
std::string webpower_param_value(device_c *dev, parameter_c *p);

// The cards the dark machine carries, named, for a message that has to say
// what is about to go onto the bus. "" once the machine is on, where the
// device list itself answers the question.
std::string webpower_carried_names(void);

// Put a card into the dark machine or take it out; taking a controller out
// takes the drives that hang off it too. Answers false with the reason in
// *error, the machine's power being on among them.
bool webpower_set_in_machine(device_c *dev, bool on, std::string *error);

// Put a medium in a drive of the dark machine, or take it out with an empty
// path. A drive given a medium is put in the machine, which needs its
// controller in the machine already.
bool webpower_set_image(device_c *dev, const std::string &path, std::string *error);

// The drive of the dark machine holding this image file, disregarding the drive
// named in "except"; "" when none does. Two drives holding one file would open
// the image twice and both write it.
std::string webpower_image_held_by(const std::string &path, const std::string &except);

#endif // _WEBPOWER_HPP_
