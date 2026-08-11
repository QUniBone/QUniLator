/* qunibusadapter.hpp: connects multiple "qunibusdevices" to the PRU QBUS/UNIBUS interface

 Copyright (c) 2018-2020 Joerg Hoppe
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
 jul-2019     JH      rewrite: multiple parallel arbitration levels	 
 12-nov-2018  JH      entered beta phase

 */
#ifndef _QUNIBUSADAPTER_HPP_
#define _QUNIBUSADAPTER_HPP_

#include <atomic>

#include "iopageregister.h"
#include "priorityrequest.hpp"
#include "qunibusadapter.hpp"
#include "qunibusdevice.hpp"
#include "event_latency.hpp"

// for each priority arbitration level, theres a table with backplane slots.
//  Each device sits in a slot, the slot determinss the request priority within one level (BR4567,NP).
class priority_request_level_c {
public:
	// remember for each backplane slot wether the device has requested
	// INTR or DMA at this level
	priority_request_c* slot_request[PRIORITY_SLOT_COUNT + 1];
	// Optimization to find the high priorized slot in use very fast.
	// bit array: bit set -> slot<bitnr> has open request.
	uint32_t slot_request_mask;

	priority_request_c* active; // request currently handled by PRU, not in table anymore

	void clear();
};

class unibuscpu_c ;


// is a device_c. need a thread (but no params)
class qunibusadapter_c: public device_c {
public:
	// How long the PRU was left holding the bus, per device register event.
	// Public because it is read out for reporting and reset from there too.
	event_latency_c event_latency;

private:

	// handle arbitration for each of the 5 device request levels in parallel
	priority_request_level_c request_levels[PRIORITY_LEVEL_COUNT];

	// access of master CPU to memory not handled via priority arbitration
//	dma_request_c 	*cpu_data_transfer_request ; // needs no link to CPU

	pthread_mutex_t requests_mutex;

	// A DMA chunk the PRU is still executing, whose request was force-completed
	// by requests_cancel_scheduled() on an INIT or power event. The transfer
	// runs to its end and signals like any other, but nothing is waiting for it
	// any more: until that signal arrives the PRU still owns mailbox->dma, and
	// filling it for the next request would run two transfers together. Set on
	// the cancel, cleared by the completion that belongs to it. Written and read
	// only under requests_mutex.
	bool dma_orphan_on_pru;

	// When it was set, so a later INIT or power event can tell an orphan still
	// in flight - the PRU has microseconds of bus cycle left to run - from one
	// the PRU is never going to complete, whose holdoff would otherwise outlive
	// the bus epoch that caused it. Only meaningful while the latch is set.
	uint64_t dma_orphan_since_ns;

	// Ask the PRU to abandon the DMA it is holding. True if it did, which frees
	// mailbox->dma at once; false if the transfer is already on the bus, and
	// then its completion is still on its way. Under requests_mutex, which is
	// what serializes mailbox->dma.
	bool dma_cancel_on_pru(void);

	unibuscpu_c	*registered_cpu ; // only one unibuscpu_c may be registered

	// Helper map: find register via 8bit handle
	qunibusdevice_register_t *register_by_handle[MAX_IOPAGE_REGISTER_COUNT];
	

	void worker_init_event(void);
	void worker_power_event(signal_edge_enum aclo_edge, signal_edge_enum dclo_edge);
	void worker_deviceregister_event(void);
	// false: the signal belonged to a transfer cancelled while the PRU ran it,
	// and was discarded - the request standing in its place is still running
	bool worker_device_dma_chunk_complete_event(void);
	void worker_intr_complete_event(uint8_t level_index);
	void worker(unsigned instance) override; // background worker function

public:
	qunibusadapter_c();

	// The processor now installed, or NULL. A CPU device asks before it
	// installs itself, because register_device() asserts on a second one and an
	// operator deserves a refusal rather than an aborted emulator.
	unibuscpu_c *installed_cpu(void) { return registered_cpu; }

	bool on_param_changed(parameter_c *param) override;  // must implement

	// True while a device register access is being served and the PRU is
	// holding that bus cycle open. A device's own logic runs inside this
	// window, so a transfer it starts there begins against a stretched cycle.
	// Sampled by DMA() into its trace; nothing is decided by it.
	std::atomic<bool> deviceregister_servicing{false};

	// list of registered devices.
	// Defines GRANT priority:
	// Lower index = "nearer to CPU" = higher priority
	qunibusdevice_c *devices[MAX_DEVICE_HANDLE + 1];

	// Current state of these QUNIBUS signals. Written by the adapter's worker
	// thread when the PRU signals an edge, read by every device thread and by
	// the web layer, so they are atomics rather than the `volatile bool`s they
	// used to be: `volatile` orders nothing against other objects and a plain
	// concurrent read/write is a data race the compiler is free to break.
	// Sequentially consistent by default, which is what makes a DMA() that
	// looks at line_INIT see the flag and the state the worker set around it in
	// one order.
	std::atomic<bool> line_INIT{false};

	// True while an INIT pulse too short for this thread to have seen is being
	// played out to the devices. line_INIT carries the level each half of the
	// replay stands for - devices are told of the negate and then of the assert,
	// which is what a device that resets on an edge needs - so for one sweep it
	// reads the opposite of what the bus is doing. Device threads take it as
	// their admission gate, and admitting a transfer in the middle of an INIT is
	// what this says no to. Ask bus_init_active(), not line_INIT, for that.
	std::atomic<bool> init_replay{false};

	// Whether a device may put anything on the bus: INIT is asserted, or an
	// INIT is being replayed to the devices.
	bool bus_init_active(void) { return line_INIT || init_replay; }
	std::atomic<bool> line_DCLO{false};
	std::atomic<bool> line_ACLO{false};

	void on_power_changed(signal_edge_enum aclo_edge, signal_edge_enum dclo_edge) override; // must implement
	void on_init_changed(void) override; // must implement

	bool register_device(qunibusdevice_c& device);
	void unregister_device(qunibusdevice_c& device);

	bool register_rom(uint32_t address) ;
	void unregister_rom(uint32_t address) ;
	bool is_rom(uint32_t address) ;

	// Helper for request processing
	void requests_init(void);

	void request_schedule(priority_request_c& request);
	void requests_cancel_scheduled(void);
	priority_request_c *request_activate_lowest_slot(unsigned level_index);
//	bool request_is_active(		unsigned level_index);
	bool request_is_blocking_active(uint8_t level_index);
	void request_active_complete(unsigned level_index, bool signal_complete);
	void request_execute_active_on_PRU(unsigned level_index);

	// Give up on a DMA that outlasted its caller's deadline: take it back from
	// the PRU if it never started, and retire it from the schedule tables.
	// True if it was retired here.
	bool dma_request_abandon(dma_request_c& dma_request);

	// one emulated-processor bus access, folded into the running summary
	void cpu_access_profile_note(uint64_t wall_ns, uint64_t cpu_ns,
			uint8_t qunibus_cycle, uint32_t unibus_addr);

	// A blocking DMA waits for the transfer to finish. timeout_ms bounds that
	// wait: a transfer the PRU never completes otherwise parks the calling
	// worker for good, and everything that waits for that worker to stop waits
	// with it. 0 keeps the unbounded wait, which is what a caller that cannot
	// make progress without the data wants.
	//
	// A timed-out request is left scheduled, because the PRU may still be
	// working on it and its buffer is where the words would land: the caller
	// gets success=false and must leave that buffer alive. It is a report of a
	// bus that has stopped answering, not a way to cancel a transfer.
	void DMA(dma_request_c& dma_request, bool blocking, uint8_t qunibus_cycle,
			uint32_t unibus_addr, uint16_t *buffer, uint32_t wordcount,
			unsigned timeout_ms = 0);
	void INTR(intr_request_c& intr_request, qunibusdevice_register_t *interrupt_register,
			uint16_t interrupt_register_value);
	void cancel_INTR(intr_request_c& intr_request);

	void cpu_DATA_transfer(dma_request_c& dma_request, uint8_t qunibus_cycle, uint32_t unibus_addr, uint16_t *buffer);

	void print_pru_iopage_register_map(void);

		void debug_init(void) ;
	void debug_snapshot(void) ;
};

extern qunibusadapter_c *qunibusadapter; // another Singleton

#endif
