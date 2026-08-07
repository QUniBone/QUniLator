/* qunibusdevice.cpp: abstract device with interface to qunibusadapter

 Copyright (c) 2018-2020, Joerg Hoppe
 j_hoppe@t-online.de, www.retrocmp.com

 Permission is hereby granted, free of charge, to any person obtaining a
 copy of this software and associated documentation files (the "Software"),
 to deal in the Software without restriction, including without limitation
 the rights to use, copy, modify, merge, publish, distribute, sublicense,
 and/or sell copies of the Software, and to permit persons to whom the
 Software is furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 JOERG HOPPE BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.


 aug-2020	JH		adapted to QBUS
 6-feb-2020	JH		added symbol table
 12-nov-2018  JH      entered beta phase
 21-oct-2022 Kirsten McIntyre: "base_addr" param bitwidth updated from UNIBUS/QBUS

 abstract qunibus device
 maybe mass storage controller or other device implementing
 QBUS/UNIBUS IOpage registers.
 sets device register values depending on internal status,
 reacts on register read/write over QBUS/UNIBUS by evaluation of PRU events.
 */
//#include <string>
#include <vector>
#include <assert.h>
#include "logger.hpp"
#include "qunibus.h"
#include "qunibusadapter.hpp"
#include "qunibusdevice.hpp"

qunibusdevice_c::qunibusdevice_c() :		device_c() 
{
	handle = 0;
	register_count = 0;
	// A register is linked into PRU shared RAM only by install(). Until then
	// pru_iopage_register is null, so device logic that pushes register state
	// before the device is on the bus is recognised and skipped rather than
	// dereferencing an unmapped register.
	for (unsigned i = 0; i < MAX_IOPAGE_REGISTERS_PER_DEVICE; i++) {
		registers[i].pru_iopage_register = nullptr;
		registers[i].register_handle = 0;
	}
	// device is not yet enabled, QBUS/UNIBUS properties can be set
	base_addr.readonly = false;
    // Kristen McIntyre: reinitialize the base address's bitwidth now that we are initialized
    base_addr.bitwidth = qunibus->addr_width;	
	priority_slot.readonly = false;
	intr_vector.readonly = false;
	intr_level.readonly = false;
	default_base_addr = 0;
	default_priority_slot = 0;
	default_intr_vector = 0;
	default_intr_level = 0;

	log_channelmask = 0; // no logging until set
}

qunibusdevice_c::~qunibusdevice_c() 
{
}

// implements params, so must handle "change"
bool qunibusdevice_c::on_param_changed(parameter_c *param) 
{
	if (param == &enabled) {
		// plug/unplug device into QUNIBUS:
		if (enabled.new_value) {
			if (!on_before_install()) // possible callback to device
				return false ; // device denied enable
			// enable: lock QBUS/UNIBUS config
			base_addr.readonly = true;
			priority_slot.readonly = true;
			intr_vector.readonly = true;
			intr_level.readonly = true;
			if (!install()) { // visible on QBUS/UNIBUS
				// the adapter refused it - an address another device already
				// answers, or a full bus. Give the QBUS/UNIBUS parameters back
				// so the operator can move the device and try again, and deny
				// the enable: "enabled" stays false, which is what the web API
				// reports back.
				base_addr.readonly = false;
				priority_slot.readonly = false;
				intr_vector.readonly = false;
				intr_level.readonly = false;
				return false; // device denied enable
			}
			on_after_install() ;
		} else {
			// disable
			on_before_uninstall() ;
			uninstall();
			on_after_uninstall() ; // possible callback to device
			base_addr.readonly = false;
			priority_slot.readonly = false;
			intr_vector.readonly = false;
			intr_level.readonly = false;
		}
	}
	return device_c::on_param_changed(param); // more actions (for enable)
}

// define default values for device BASE address and INTR
void qunibusdevice_c::set_default_bus_params(uint32_t _default_base_addr,
		unsigned _default_priority_slot, unsigned _default_intr_vector,
		unsigned _default_intr_level) 
{
	assert(_default_priority_slot <= PRIORITY_SLOT_COUNT); // bitmask!

	// make proper 16/18/22 bit IOpage address: start + addr<12:0>
	if (qunibus->addr_width == 0)
		FATAL("Address width of " QUNIBUS_NAME " not yet known!") ;
	_default_base_addr = qunibus->iopage_start_addr + (_default_base_addr & 0x1fff) ;
	default_base_addr = _default_base_addr;
	default_priority_slot = _default_priority_slot;
	default_intr_vector = this->intr_vector.new_value = _default_intr_vector;
	default_intr_level = this->intr_level.new_value = _default_intr_level;
	// call device.on_param_change(), may be supressed
	base_addr.set(default_base_addr);
	priority_slot.set(default_priority_slot);
	intr_vector.set(default_intr_vector);
	intr_level.set(default_intr_level);

	// An unplugged device may be re-jumpered: its bus placement is editable while
	// it is off the bus, and locked again when it is installed (on_param_changed
	// for "enabled"). This is also what makes base_addr and the interrupt fields
	// settable in a saved configuration — the config model captures the writable
	// state of an unplugged device.
	base_addr.readonly = false;
	priority_slot.readonly = false;
	intr_vector.readonly = false;
	intr_level.readonly = false;
}

bool qunibusdevice_c::install(void)
{
	// registration is refused for a bad register configuration, an address that
	// belongs to another device, or an exhausted bus, rather than aborting;
	// leave the device off the bus instead of powering on an unregistered
	// device. The refusal is passed back so the device does not end up reading
	// as enabled while it carries no handle - it would abort on the next
	// uninstall, which is the abort this refusal exists to avoid.
	if (!qunibusadapter->register_device(*this)) {
		ERROR("device %s not installed: registration refused", name.value.c_str());
		return false;
	}
	// now has handle
	warn_on_slot_collisions();

	// Reset device by generating DCLO power cycle.
	// Do not toggle ACLO, meant for run/stop conditions (CPU start, M9312 boot vector redirection)
	on_power_changed(SIGNAL_EDGE_NONE, SIGNAL_EDGE_RAISING);
	// ACLO active, DCLO inactive
	on_power_changed(SIGNAL_EDGE_NONE, SIGNAL_EDGE_FALLING);
	return true;
}

void qunibusdevice_c::uninstall(void) {
	qunibusadapter->unregister_device(*this);
}

qunibusdevice_register_t *qunibusdevice_c::register_by_name(std::string _name) 
{
	unsigned i;
	for (i = 0; i < register_count; i++) {
		qunibusdevice_register_t *reg = &(registers[i]);
		if (reg->name[0] && !strcasecmp(_name.c_str(), reg->name))
			return reg;
	}
	return NULL;
}

qunibusdevice_register_t *qunibusdevice_c::register_by_unibus_address(uint32_t addr) 
{
	unsigned i;
	for (i = 0; i < register_count; i++) {
		qunibusdevice_register_t *reg = &(registers[i]);
		if (reg->addr == addr)
			return reg;
	}
	return NULL;
}

// set value of QBUS/UNIBUS register which can be read by DATI
// passive register: simply set value in shared PRU RAM
// active: set additionally "read" flipflops
void qunibusdevice_c::set_register_dati_value(qunibusdevice_register_t *device_reg,
		uint16_t value, const char *debug_info) 
{
//	if (device_reg->active_on_dati)
	// always set dati_flipflops, needed to restore value written with DATO
	device_reg->active_dati_flipflops = value;
	// The register is mapped into PRU shared RAM only while the device is
	// installed on the bus. A device that pushes status before install (an RX
	// box powered while its controller is disabled) has no bus presence yet;
	// keep the latched value but skip the shared-RAM write and the dump.
	if (device_reg->pru_iopage_register == nullptr)
		return;
	device_reg->pru_iopage_register->value = value;

	// signal: changed by device logic
	log_register_event(debug_info, device_reg);
}
/*
 // same as if QBUS/UNIBUS DATO has written the register
 void qunibusdevice_c::set_register_dato_value(qunibusdevice_register_t *device_reg,
 uint16_t value) {
 if (device_reg->active_on_dato) {
 device_reg->active_dato_flipflops = value;
 // do not change value seen with QBUS/UNIBUS DATI
 } else
 device_reg->pru_iopage_register->value = value;
 }
 */
// get value of QBUS/UNIBUS register which has been written by DATO
uint16_t qunibusdevice_c::get_register_dato_value(qunibusdevice_register_t *device_reg) 
{
	if (device_reg->active_on_dato)
		return device_reg->active_dato_flipflops;
	else if (device_reg->pru_iopage_register != nullptr)
		return device_reg->pru_iopage_register->value;
	else
		return 0; // not installed on the bus: no mapped value to read
}

// write the reset value into all registers (helper for QBUS/UNIBUS INIT)
// this done here by qunibusdevice.worker() and
// corrects the qunibusdevice.register active_dati_flipflops.
// 1st time done by PRU: iopageregisters_reset_values() 
// as DATI/DATO accesses to registers which are not active(no callback events)
// are not delayed until INIT finishes.
void qunibusdevice_c::reset_unibus_registers() 
{
	unsigned i;
	for (i = 0; i < register_count; i++) {
		qunibusdevice_register_t *reg = &(registers[i]);
		set_register_dati_value(reg, reg->reset_value, __func__);
		// The write-side latch resets with the register. A DATOB writes one
		// byte and takes the other from this latch (qunibusadapter DATOB
		// merge); after INIT that byte must reflect the reset register, or a
		// byte write resurrects pre-INIT bits — a DZV11 TCR high-byte DTR
		// write, for one, would re-enable a transmitter cleared by INIT.
		reg->active_dato_flipflops = reg->reset_value;
		// log_register_event("RESET", reg) ;
	}
}

// log register state changes:
//  print event info
// - print full register content to logger
// "active" registers are printed as <datiflipflop></datoflipflop>

void qunibusdevice_c::log_register_event(const char *change_info,
		qunibusdevice_register_t *changed_reg) 
{
	// do not use std:: string .. hand coded because of performance
	char buffer[1024];
	unsigned i;

	if (logger->ignored(this, LL_DEBUG))
		return; // channel not enabled: quick return

	buffer[0] = 0;

	// event info could be like "DATI CS" or "CHNG DA"
	if (change_info)
		strcat(buffer, change_info);
	if (changed_reg) {
		strcat(buffer, " ");
		strcat(buffer, changed_reg->name);
	}
	if (change_info || changed_reg)
		strcat(buffer, ":");

	// dump all registers.
	// if too much (examples: memory emulator): only the changed register
	if (register_count <= 8) {
		for (i = 0; i < register_count; i++) {
			char buff1[80];
			qunibusdevice_register_t *reg = &(registers[i]);
			if (reg->active_on_dati || reg->active_on_dato)
				sprintf(buff1, " %s=%06o/%06o", reg->name, reg->pru_iopage_register->value,
						reg->active_dato_flipflops);
			else
				sprintf(buff1, " %s=%06o", reg->name, reg->pru_iopage_register->value);
			strcat(buffer, buff1);
		}
	} else {
		// only the changed register
		sprintf(buffer, "%s=%06o", changed_reg->name, changed_reg->pru_iopage_register->value);
	}
	DEBUG_FAST(buffer);
}

// search device in global list mydevices[]
qunibusdevice_c *qunibusdevice_c::find_installed_by_request_slot(uint8_t priority_slot,
		const qunibusdevice_c *except)
{
	std::list<device_c *>::iterator devit;
	for (devit = device_c::mydevices.begin(); devit != device_c::mydevices.end(); ++devit) {
		qunibusdevice_c *ubdevice = dynamic_cast<qunibusdevice_c *>(*devit);
		if (ubdevice && ubdevice != except && ubdevice->is_installed()) {
			// all dma and intr requests
			for (std::vector<dma_request_c *>::iterator reqit = ubdevice->dma_requests.begin();
					reqit < ubdevice->dma_requests.end(); reqit++)
				if ((*reqit)->get_priority_slot() == priority_slot)
					return ubdevice;
			for (std::vector<intr_request_c *>::iterator reqit = ubdevice->intr_requests.begin();
					reqit < ubdevice->intr_requests.end(); reqit++)
				if ((*reqit)->get_priority_slot() == priority_slot)
					return ubdevice;
		}
	}
	return NULL;
}

// A slot is a backplane position, and the grant chain runs through it, so two
// devices on the bus in one slot have no defined arbitration order between
// them. Report that when it happens; unplugged devices share slots freely.
void qunibusdevice_c::warn_on_slot_collisions(void)
{
	std::vector<priority_request_c *> requests;
	requests.insert(requests.end(), dma_requests.begin(), dma_requests.end());
	requests.insert(requests.end(), intr_requests.begin(), intr_requests.end());
	for (std::vector<priority_request_c *>::iterator reqit = requests.begin();
			reqit != requests.end(); reqit++) {
		uint8_t slot = (*reqit)->get_priority_slot();
		qunibusdevice_c *other = find_installed_by_request_slot(slot, this);
		if (other)
			WARNING("Slot %u used by device %s is also used by %s", (unsigned) slot,
					name.value.c_str(), other->name.value.c_str());
	}
}

// returns a string of form
// reg_first-reg_last, slots from-to, DMA, INTR level1/vec1,level2/vec2,...
char *qunibusdevice_c::get_qunibus_resource_info(void) 
{
	static char buffer[1024];
	char tmpbuff[256];
	buffer[0] = 0;

	// get register address range
	// use parameter "base_addr", register struct only valid after qunibusadapter.install()
	if (register_count == 0)  // cpu is a device without register interface
		strcpy(tmpbuff, "");
	else if (register_count == 1)
		sprintf(tmpbuff, "addr %s", qunibus->addr2text(base_addr.value));
	else
		sprintf(tmpbuff, "addr %s-%s (%d regs)", qunibus->addr2text(base_addr.value),
				qunibus->addr2text(base_addr.value + 2 * (register_count - 1)), register_count);
	strcat(buffer, tmpbuff);

	// get priority slot range from DMA request and intr_requests
	uint8_t slot_from = 0xff, slot_to = 0;
	for (std::vector<dma_request_c *>::iterator it = dma_requests.begin(); it < dma_requests.end();
			it++) {
		slot_from = std::min(slot_from, (*it)->get_priority_slot());
		slot_to = std::max(slot_to, (*it)->get_priority_slot());
	}
	for (std::vector<intr_request_c *>::iterator it = intr_requests.begin();
			it < intr_requests.end(); it++) {
		slot_from = std::min(slot_from, (*it)->get_priority_slot());
		slot_to = std::max(slot_to, (*it)->get_priority_slot());
	}

	if (slot_from > slot_to) // no requests: use device parameter
		slot_from = slot_to = priority_slot.value;
	if (slot_from == slot_to)
		sprintf(tmpbuff, ", slot %u", (unsigned) slot_from);
	else
		sprintf(tmpbuff, ", slots %u-%u", (unsigned) slot_from, (unsigned) slot_to);
	strcat(buffer, tmpbuff);

	//  DMA channels
	if (dma_requests.size() > 0) {
		if (dma_requests.size() == 1)
			sprintf(tmpbuff, ", DMA");
		else
			sprintf(tmpbuff, ", %uxDMA", dma_requests.size());
		strcat(buffer, tmpbuff);
	}
	//  Interrupts
	if (intr_requests.size() > 4) {
		// that crazy testcontroller has 31*4 !
		sprintf(tmpbuff, "%d INTRs", intr_requests.size());
		strcat(buffer, tmpbuff);
	} else if (intr_requests.size() > 0) {
		const char *sep = ":";
		strcat(buffer, ", INTRs");
		for (std::vector<intr_request_c *>::iterator it = intr_requests.begin();
				it < intr_requests.end(); it++) {
			sprintf(tmpbuff, "%s%d/%03o", sep, (*it)->get_level(), (*it)->get_vector());
			strcat(buffer, tmpbuff);
			sep = ",";
		}
	}

	return buffer;
}

