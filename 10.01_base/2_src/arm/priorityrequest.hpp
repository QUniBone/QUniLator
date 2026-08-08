/* priorityrequest.hpp: DMA or iNTR request of an device

 Copyright (c) 2019 Joerg Hoppe
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


 jul-2019     JH      start: multiple parallel arbitration levels	 
 */

/*
 Handling priorities of Arbitration Requests:
 1. Priority of arbitration levels
 Ascending priority: INTR BR4,5,6,7,  DMA NPR
 => 5 Priority Arbitration Levels encoded with index 0..4
 2. Priority within one request level
 Priority for Requests of same level given by backplane slot.
 Backplane closests to CPU is granted first => highest priority

 So priority of a request is given by two coordinates: level and slot.

 Implementation:
 all 5 levels are handled in parallel: priority_request_level_c [5])
 In each level, a refernece array[slot] holds open requests.
 For fast determination of lowest active slot, a bitarray
 marks active slots.
 */
#ifndef _PRIORITYREQUEST_HPP_
#define _PRIORITYREQUEST_HPP_

#include <stdint.h>
#include <pthread.h>
#include <atomic>

#include "logsource.hpp"

// linear indexes for different QBUS/UNIBUS arbitration levels
#define PRIORITY_LEVEL_INDEX_BR4	0 
#define PRIORITY_LEVEL_INDEX_BR5	1 
#define PRIORITY_LEVEL_INDEX_BR6	2 
#define PRIORITY_LEVEL_INDEX_BR7	3 
#define PRIORITY_LEVEL_INDEX_NPR	4 
#define PRIORITY_LEVEL_COUNT	5

#define PRIORITY_SLOT_COUNT	32	// backplane slot numbers 0..31 may be used

class qunibusdevice_c;

// (almost) abstract base class for dma and intr requests
class priority_request_c: public logsource_c {
	friend class intr_request_c;
	friend class dma_request_c;
	friend class qunibusadapter_c;
private:
	qunibusdevice_c *device; // this device owns the request
	// maybe NULL, in request is not used for device meualtion
	// (test console EXAM, DEPOSIT for example).

	// internal priority index of a request level, see REQUEST_INDEX_*
	uint8_t level_index; // is BR4567,NPR level - 4

	uint8_t priority_slot; // backplane priority_slot which triggered request
public:
	// State shared between the requesting device thread and
	// qunibusadapter::worker(). Atomic, not `volatile`: `complete` is the flag a
	// waiting device polls, and the words the transfer landed in the buffer plus
	// `success` must be visible to it once it sees the flag set. A sequentially
	// consistent store/load pair gives that release/acquire relationship; a
	// `volatile` write gives nothing but a promise not to elide the access.
	std::atomic<bool> executing_on_PRU; // true between schedule to PRU and compelte signal
	std::atomic<bool> complete;

	// PRU -> signal -> worker() -> request -> device. INTR/DMA
	pthread_mutex_t complete_mutex;
	pthread_cond_t complete_cond; // PRU signal notifies request on completeness

	priority_request_c(qunibusdevice_c *device);
	virtual ~priority_request_c(); // not used, but need dynamic_cast

	void set_priority_slot(uint8_t slot);
	uint8_t get_priority_slot(void) {
		return priority_slot;
	}
	// BR4..BR7 for an INTR; a DMA request arbitrates on NPR and reports 8,
	// one past BR7, so that level and slot together identify a request's
	// place in arbitration whatever kind it is.
	uint8_t get_level(void) {
		return level_index + 4;
	}
};

class dma_request_c: public priority_request_c {
	friend class qunibusadapter_c;
public:
	dma_request_c(qunibusdevice_c *device);

	~dma_request_c();
	// const for all chunks
	uint8_t qunibus_control; // DATI,DATO
	uint32_t qunibus_start_addr;
	uint32_t qunibus_end_addr;
	uint16_t* buffer;
	uint32_t wordcount;

	bool is_cpu_access; // true if DMA is CPU memory access

	// A transfer that asks a question the bus answers by not answering: a probe
	// of an address to see whether anything lives there. Its timeout is the
	// result, not a fault, so it is not reported as one.
	bool timeout_expected;

	// DMA transaction are divided in to smaller DAT transfer "chunks" 
	uint32_t chunk_max_words; // max is PRU capacity PRU_MAX_DMA_WORDCOUNT (512)
	uint32_t chunk_qunibus_start_addr; // current chunk
	uint32_t chunk_words; // size of current chunks

	// DMA can fail with bus timeout. Set by the adapter worker before it sets
	// `complete`, read by the device once it sees that - see the note there.
	std::atomic<bool> success;

	// return ptr to chunk pos in buffer
	uint16_t *chunk_buffer_start(void) {
		return buffer + (chunk_qunibus_start_addr - qunibus_start_addr) / 2;
	}

	// words already transfered in previous chunks
	uint32_t wordcount_completed_chunks(void) {
		return (chunk_qunibus_start_addr - qunibus_start_addr) / 2;
	}

};

struct qunibusdevice_register_struct;
// forward

class intr_request_c: public priority_request_c {
	friend class qunibusadapter_c;
public:
	enum interrupt_edge_enum {
		INTERRUPT_EDGE_NONE, INTERRUPT_EDGE_RAISING, INTERRUPT_EDGE_FALLING
	};
private:
	uint16_t vector; // PDP-11 interrupt vector

	// optionally register, with which a device signals presence of interrupt condition
	struct qunibusdevice_register_struct *interrupt_register;
	uint16_t interrupt_register_value;

	// static level of some device INTR signal. 
	// raising edge calculated with edge_detect()
	bool signal_level;

public:
	intr_request_c(qunibusdevice_c *device);

	~intr_request_c();

	void set_level(uint8_t level);
	void set_vector(uint16_t vector);
	// uint16_t, like the vector itself: a PDP-11 vector is a 9 bit byte address
	// into the trap page, and the floating vectors of a second controller live
	// above 0400. Returning uint8_t silently dropped that bit.
	uint16_t get_vector(void) {
		return vector;
	}

	// service for device logic: calculate change of static INTR condition
	void edge_detect_reset() {
		signal_level = 0;
	}

	// detect raising edge of interrupt level
	enum interrupt_edge_enum edge_detect(bool new_signal_level) {
		if (signal_level == new_signal_level)
			return INTERRUPT_EDGE_NONE;
		else {
			// change: which edge?
			signal_level = new_signal_level;
			if (signal_level)
				return INTERRUPT_EDGE_RAISING;
			else
				return INTERRUPT_EDGE_FALLING;
		}
	}

};

#endif
