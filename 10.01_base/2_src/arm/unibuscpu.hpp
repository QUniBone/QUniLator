/* unibuscpu.hpp: base class for all CPU implementations

 Copyright (c) 2019, Joerg Hoppe
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


27-aug-2019	JH      start
 */

#ifndef _UNIBUSCPU_HPP_
#define _UNIBUSCPU_HPP_


#include "qunibusdevice.hpp"

// a CPU is just a device with INTR facilities
class unibuscpu_c: public qunibusdevice_c {
	public:
			unibuscpu_c(): qunibusdevice_c() {
				power_event_ACLO_active = power_event_ACLO_inactive = power_event_DCLO_active = false ;
				} ;

//	enum power_event_enum   {power_event_none, power_event_ACLO_active, power_event_ACLO_inactive, power_event_DCLO_active} ;

	bool power_event_ACLO_active ;
	bool power_event_DCLO_active ;
	bool power_event_ACLO_inactive ;
//	enum power_event_enum power_event ;
		
	// Whether this processor is installed as the machine's own, which is what
	// decides who arbitrates the bus. An installed emulated processor means
	// the board *is* the machine: nothing outside it arbitrates, so the PRU
	// runs no client protocol while the processor is halted - which is what
	// lets the ARM reach memory to load it. With no emulated processor
	// installed the board is a peripheral of whatever machine it was fitted
	// to, and must ask for the bus before touching it. That is the safe
	// direction: asking on a bus nobody arbitrates costs a refused request,
	// while not asking on a bus somebody does arbitrate corrupts a running
	// machine.
	//
	// "enabled" cannot answer this. The parameter takes its new value after
	// on_before_install() and on_after_uninstall() have run, so during the
	// callbacks that have to set the mode it still reads as the old one.
	bool bus_owner = false;

	// Put the PRU in the arbitration mode this processor's state calls for.
	// "arbitrating" is whether the processor is granting the bus right now,
	// which for a PDP-11 core means it is executing and for the VAX means its
	// arbitrator is switched on. Defined in unibuscpu.cpp, where qunibus is in
	// scope.
	void set_bus_arbitration(bool arbitrating);

	// called by PRU on INTR. level is the BR level 4..7 the request was
	// granted at, which a processor that ranks its interrupts by priority
	// needs; a PDP-11 takes the vector alone and ignores it.
	virtual void on_interrupt(uint16_t vector, uint8_t level) = 0 ;

	
	// A processor whose memory is not on the bus answers a device's DMA
	// itself. Returns true when it did, and the transfer does not go to the
	// bus at all; the default is false, which leaves memory where it was.
	virtual bool on_dma(uint8_t qunibus_cycle, uint32_t unibus_addr,
			uint16_t *buffer, uint32_t wordcount) {
		(void) qunibus_cycle; (void) unibus_addr; (void) buffer; (void) wordcount;
		return false;
	}

	virtual void on_power_changed(signal_edge_enum aclo_edge, signal_edge_enum dclo_edge) ;
	virtual void on_init_changed(void) ;

	// The console view a front panel drives, common to every emulated
	// processor: the RUN lamp, the HALT toggle, and the momentary START and
	// CONTINUE switches. Each processor carries these as its own parameters
	// with its own wording; the accessors let the run controls of the web API
	// and the panel state derivation work on any of them. A processor without
	// one of the switches answers NULL for it.
	virtual parameter_bool_c *panel_run_led(void) { return nullptr; }
	virtual parameter_bool_c *panel_halt_switch(void) { return nullptr; }
	virtual parameter_bool_c *panel_start_switch(void) { return nullptr; }
	virtual parameter_bool_c *panel_continue_switch(void) { return nullptr; }
};

#endif
