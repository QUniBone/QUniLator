/* pru1_diag.h: bus diagnostics the ARM reads out of PRU memory

 Copyright (c) 2026, Hans Huebner

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
 THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.


 Everything a tool reads out of the PRU lives in one struct at one symbol,
 `qbus_diag`. A reader locates that symbol in pru1_code_qbus.out.map, checks
 the magic and the layout version, and then knows every field's offset. It
 never pokes at the state machines' own variables, whose offsets move with
 every rebuild and whose meaning a stale reader cannot tell from a fresh one.

 Raise QBUS_DIAG_LAYOUT_VERSION whenever a field moves, so a reader built
 against an older layout says so instead of printing plausible fiction.
 */
#ifndef _PRU1_DIAG_H_
#define _PRU1_DIAG_H_

#include <stdint.h>

// "QDIA" - a reader that does not find this at the head of the struct is
// looking at the wrong address (or a firmware without diagnostics).
#define QBUS_DIAG_MAGIC		0x51444941
#define QBUS_DIAG_LAYOUT_VERSION	1

// The bus trace ring costs four latch reads per main loop pass, on the DMA
// path as well as the idle one, and a slice of the PRU's nearly-full
// instruction memory. It stays out of a normal build; define QBUS_BUS_TRACE
// to build the firmware that carries it.
#ifdef QBUS_BUS_TRACE
#define QBUS_DIAG_TRACE_ENTRIES	64
#else
#define QBUS_DIAG_TRACE_ENTRIES	1	// unused, keeps the struct one shape
#endif

typedef struct {
	uint32_t ts;		// IEP counter at the transition, 5ns a tick
	uint8_t l4;		// SYNC/DIN/DOUT/RPLY/WTBT/BS7/REF/INIT
	uint8_t l6;		// IRQ4-7/DMR/RIAKI/RDMGI/SACK
	uint8_t l7;		// decoded IAKI4-7/DMG
	uint8_t arb_state;	// sm_arb.state at the transition
} qbus_diag_trace_entry_t;

typedef struct {
	uint32_t magic;			// QBUS_DIAG_MAGIC
	uint32_t layout_version;	// QBUS_DIAG_LAYOUT_VERSION
	uint32_t trace_entries;		// 0 when the firmware carries no ring

	// Bus activity, counted in the slave passes only so the DMA path is
	// untouched. A physical PDP-11 drives SYNC for every fetch, and an
	// idle one still takes the line clock, so a counter that stops for a
	// second on a powered machine means the processor is wedged.
	uint32_t bus_activity;

	// Arbitration outcomes
	uint32_t intr_answered;		// vector transfers completed normally
	uint32_t iak_abandoned;		// acknowledge withdrawn before DIN
	uint32_t orphan_rescued;	// acknowledge answered for a canceled request
	uint32_t dma_grant_refused;	// grant ignored, DMR not committed
	uint32_t dma_abandoned;		// transfer ended by the address-state guard

	// Latches sampled inside the DMA timeout branch, while the failing
	// cycle still drives the bus: DAL7:0, DAL15:8, DAL21:16 with BS7, the
	// data-control latch, the request latch, and the transfer's address.
	uint32_t timeout_dal_lo;
	uint32_t timeout_dal_hi;
	uint32_t timeout_dal_ext;
	uint32_t timeout_l4;
	uint32_t timeout_l6;
	uint32_t timeout_addr;

	// A DMA ending in timeout freezes the ring, so it still holds that
	// transfer's own cycles when it is read later. Cleared by the reader
	// to re-arm.
	uint32_t trace_frozen;
	uint32_t trace_count;		// total transitions; slot = count % entries
	qbus_diag_trace_entry_t trace[QBUS_DIAG_TRACE_ENTRIES];
} qbus_diag_t;

extern qbus_diag_t qbus_diag;

void qbus_diag_init(void);

#endif // _PRU1_DIAG_H_
