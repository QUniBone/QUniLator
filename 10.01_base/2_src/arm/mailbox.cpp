/* mailbox.cpp: datastructs common to ARM and PRU

 Copyright (c) 2018, Joerg Hoppe
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


 12-nov-2018  JH      entered beta phase
 */

#define _MAILBOX_CPP_

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <sched.h>

#include "pru.hpp"
#include "pru_backend.hpp"
#include "logger.hpp"
#include "timeout.hpp"
#include "ddrmem.h"
#include "mailbox.h"

// is located in PRU 12kb shared memory.
// address symbol "" fetched from linker map

volatile mailbox_t *mailbox = NULL;

// Init all fields, most to 0's
int mailbox_connect(void) 
{
	void *pru_shared_dataram;
	// get pointer to RAM
	pru_shared_dataram = pru_backend->map_ram(PRU_MAILBOX_RAM_ID);
	if (pru_shared_dataram == NULL) {
		return -1;

	}
	// prussdrv_map_prumem( PRU_MAILBOX_RAM_ID, &pru_shared_dataram) ;
	// point to struct inside RAM
	mailbox = (mailbox_t *) ((char *) pru_shared_dataram + PRU_MAILBOX_RAM_OFFSET);

	// now ARM and PRU can access the mailbox

	memset((void*) mailbox, 0, sizeof(mailbox_t));

	// tell PRU location of shared DDR RAM
	mailbox->ddrmem_base_physical = (ddrmem_t *) ddrmem->base_physical;

	return 0;
}

void mailbox_print(void) 
{
	printf("INFO: Content of mailbox to PRU:\n"
			"arm2pru: req=0x%x\n", mailbox->arm2pru_req);
}

/* simulate simple register accesses:
 * write test_addr + OP,
 * result in "val"
 */
static unsigned n = 0;
void mailbox_test1()
{
	unsigned reg_sel = 0;
	mailbox_lock_c lock; // mailbox_test shares its union with every other payload
	for (reg_sel = 0; reg_sel < 8; reg_sel++) {
		//TODO: memory barrier??
//		__sync_synchronize() ;
		mailbox->mailbox_test.addr = n & 0xff;
//		__sync_synchronize() ;
		while (mailbox->mailbox_test.addr != (n & 0xff))
			; // cache ?
		__sync_synchronize(); // write to arm2pru_req must be last operation
		mailbox->arm2pru_req = ARM2PRU_MAILBOXTEST1; // go!
		while (mailbox->arm2pru_req)
			; // wait until processed
//		__sync_synchronize() ;
		n++;
		// PRU copies addr to val and may output on GPIOs
		/*
		 if (mailbox->mailbox_test.val != mailbox->mailbox_test.addr) {
		 printf("?");
		 fflush(stdout);
		 }
		 */
	}
}

/* start cmd to PRU via mailbox. Wait until ready
 * mailbox union members must have been filled - under this same lock, see
 * mailbox.h: the payload and the request are one command.
 */

pthread_mutex_t arm2pru_mutex = PTHREAD_MUTEX_INITIALIZER ;

mailbox_lock_c::mailbox_lock_c()
{
	pthread_mutex_lock(&arm2pru_mutex) ;
}

mailbox_lock_c::~mailbox_lock_c()
{
	pthread_mutex_unlock(&arm2pru_mutex) ;
}

// The PRU main loop ACKs every request within a few of its iterations
// (microseconds). If the PRU firmware is stopped, crashed or restarted, an
// unbounded poll spins forever and the calling thread (e.g. the CPU worker,
// which executes a request per emulated instruction) hangs beyond recovery.
// Bound the spin, so a dead PRU produces a message instead of a silent freeze.
#define ARM2PRU_TIMEOUT_MS	100

// How long the wait for the PRU stays a plain spin. A request the PRU is
// running takes microseconds, and a spin is the cheapest way to see it end; one
// that has taken a thousand times that is a PRU that is not answering, and then
// the spin is a core burned for the rest of the ARM2PRU timeout - with whatever
// lock the caller holds. The adapter cancels a DMA from under requests_mutex,
// which is the lock every device thread and the emulated processor go through.
#define ARM2PRU_SPIN_NS	1000000ull	// 1 ms

bool mailbox_execute_locked(uint8_t request)
{
// write to arm2pru_req must be last memory operation
	__sync_synchronize();
	uint64_t starttime_ns = timeout_c::abstime_ns() ;
	uint64_t waited_ns ;
	while (mailbox->arm2pru_req != ARM2PRU_NONE) {
		// wait for previous request to complete
		waited_ns = timeout_c::abstime_ns() - starttime_ns ;
		if (waited_ns > ARM2PRU_TIMEOUT_MS * 1000000ull) {
			printf("ERROR: mailbox_execute(%u): PRU busy with request %u for %u ms - PRU stopped or hung?\n",
					(unsigned) request, (unsigned) mailbox->arm2pru_req, ARM2PRU_TIMEOUT_MS);
			return false ;
		}
		if (waited_ns > ARM2PRU_SPIN_NS)
			sched_yield() ;
	}

	mailbox->arm2pru_req = request; // go!

	// wait until ACKed
	starttime_ns = timeout_c::abstime_ns() ;
	while (mailbox->arm2pru_req == request) {
		waited_ns = timeout_c::abstime_ns() - starttime_ns ;
		if (waited_ns > ARM2PRU_TIMEOUT_MS * 1000000ull) {
			printf("ERROR: mailbox_execute(%u): PRU did not ACK within %u ms - PRU stopped or hung?\n",
					(unsigned) request, ARM2PRU_TIMEOUT_MS);
			mailbox->arm2pru_req = ARM2PRU_NONE; // back to idle for a later restarted PRU
			return false ;
		}
		if (waited_ns > ARM2PRU_SPIN_NS)
			sched_yield() ;
	}
	// result false = error
	return (mailbox->arm2pru_req == ARM2PRU_NONE) ;
}

bool  mailbox_execute(uint8_t request)
{
	mailbox_lock_c lock ;
	return mailbox_execute_locked(request) ;
}

bool  mailbox_execute(uint8_t request, uint32_t param)
{
	mailbox_lock_c lock ;
	mailbox->param = param ;
	return mailbox_execute_locked(request) ;
}
