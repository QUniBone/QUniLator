/* pru1_diag.c: the diagnostics block the ARM reads out of PRU memory

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
 */
#include <stdint.h>

#include "pru1_diag.h"

qbus_diag_t qbus_diag;

// The PRU loader leaves .common as it found it, so a fresh firmware would
// otherwise start counting from whatever the last one left behind.
void qbus_diag_init(void) {
	uint32_t i;

	qbus_diag.magic = QBUS_DIAG_MAGIC;
	qbus_diag.layout_version = QBUS_DIAG_LAYOUT_VERSION;
#ifdef QBUS_BUS_TRACE
	qbus_diag.trace_entries = QBUS_DIAG_TRACE_ENTRIES;
#else
	qbus_diag.trace_entries = 0;
#endif

	qbus_diag.bus_activity = 0;
	qbus_diag.intr_answered = 0;
	qbus_diag.iak_abandoned = 0;
	qbus_diag.orphan_rescued = 0;
	qbus_diag.dma_grant_refused = 0;
	qbus_diag.dma_abandoned = 0;

	qbus_diag.timeout_dal_lo = 0;
	qbus_diag.timeout_dal_hi = 0;
	qbus_diag.timeout_dal_ext = 0;
	qbus_diag.timeout_l4 = 0;
	qbus_diag.timeout_l6 = 0;
	qbus_diag.timeout_addr = 0;

	qbus_diag.trace_frozen = 0;
	qbus_diag.trace_count = 0;
	for (i = 0; i < QBUS_DIAG_TRACE_ENTRIES; i++) {
		qbus_diag.trace[i].ts = 0;
		qbus_diag.trace[i].l4 = 0;
		qbus_diag.trace[i].l6 = 0;
		qbus_diag.trace[i].l7 = 0;
		qbus_diag.trace[i].arb_state = 0;
	}
}
