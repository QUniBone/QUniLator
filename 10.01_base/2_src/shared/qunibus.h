/* qunibus.h: shared structs used in PRU and ARM

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
 19-may-2018  JH      created

 */

#ifndef _QUNIBUS_H_
#define _QUNIBUS_H_

#include <stdint.h>

// Setup for  "UNIBUS" or "QBUS"
#if defined(UNIBUS)
  #define QUNIBONE_NAME  "UniBone"
  #define QUNIBUS_NAME	"UNIBUS"
  #define QUNIBUS_PROBE_NAME "UniProbe"
  // the interactive program as it is installed and as an operator types it
  #define QUNILATOR_CLI_NAME "unibone-cli"
#elif defined(QBUS)
  #define QUNIBONE_NAME  "QBone"
  #define QUNIBUS_NAME	"QBUS"
  #define QUNIBUS_PROBE_NAME "QProbe"
  #define QUNILATOR_CLI_NAME "qbone-cli"
#else
  #error "Define UNIBUS or QBUS !"
#endif

// Product name of the emulator software, shown in the web interface and its
// startup banners. It names the software, independent of the bus and the cape
// it runs on - which QUNIBUS_NAME and QUNIBONE_NAME name.
#define QUNILATOR_NAME  "QUniLator"

// max size of memory for all systems, (22 bit addressing)
#define QUNIBUS_MAX_WORDCOUNT 0x200000 // 2MWords = 4MB

// bus transaction. can be directly assigned to UNIBUS lines C1,C0
// different coding on QBUS
#define QUNIBUS_CYCLE_DATI	0x00 // 16 bit word from slave to master
#define QUNIBUS_CYCLE_DATIP	0x01 // DATI, inhibts core restore. DATO must follow.
#define QUNIBUS_CYCLE_DATO	0x02 // 16 bit word from master to slave
#define QUNIBUS_CYCLE_DATOB	0x03 // 8 bit byte  from master to slave
// data<15:8> for a00 = 1, data<7:0> for a00 = 0
#define QUNIBUS_CYCLE_IS_DATI(c) (!((c) & 0x02)) // check for DATI/P
#define QUNIBUS_CYCLE_IS_DATO(c) (!!((c) & 0x02)) // check for DATO/B


// QBUS: BS7 is generated in ARM and transmitted to PRU as an extra high address bit
// it is threaded like the 23'nd address bit. also bit 6 on 3rd 8bit latch
#define QUNIBUS_IOPAGE_ADDR_BITMASK	((uint32_t)(1  << 22)) 

#if defined(UNIBUS)
// SSYN timeout after 20 milliseconds, not exactly defined
#define	QUNIBUS_TIMEOUT_PERIOD_US	20
#elif defined(QBUS)
#define	QUNIBUS_TIMEOUT_PERIOD_US	8	// defined in QBUS specs
#endif

#define UNIBUS_TIMEOUTVAL	0xffffffff // EXAM result for bus timeout

// probe_range(): no address in the range answered
#define QUNIBUS_PROBE_NONE	0xffffffff
// One word every 64 KB, the span of the smallest card a machine of this size
// carries, plus both ends of the range. A card answering anywhere across a
// probe point is found; silence proves nothing, and a card narrower than the
// step that falls between two points is silence.
#define QUNIBUS_PROBE_STEP	65536

/*
UNIBUS: DATIP never used.

QBUS: ARM uses only DATI,DATO, DATOB
DATIO(B) would only be used for perfect CPU emulations
Block modes DATBI and DATBO uses by PRU for DATI/DATO 16bit DMA requests > 1 word.
PRU: communicates only DATI,DATO, DATOB with ARM. 
DATIO neot implemented. DATBI/BO blockmode handeled automatically
*/



// This simple byte/word union works only because PDP-11,ARM and PRU all are little endian.
// "PDP-11 is little-endian (with least significant bytes first)."
// "BeagleBone Debian: lscpu shows that it is little endian."
// pru c-compile is NOT called with --endian=big
		
// hold memory of biggest supported bus
typedef struct {
	union {
		uint16_t words[QUNIBUS_MAX_WORDCOUNT];
		uint8_t bytes[2 * QUNIBUS_MAX_WORDCOUNT];
	};
} qunibus_memory_t;

#ifdef ARM
// included only by ARM code
#include "utils.hpp"
#include "logsource.hpp"
#include "timeout.hpp"

// parameter and functions for low level QBUS/UNIBUS control

class dma_request_c;
class intr_request_c;

class qunibus_c: public logsource_c {
public:
		unsigned addr_width ; // # of address bits. 0=unknown, else 16,18,22
		unsigned addr_space_word_count ; // # of 16 bit words in address space
		unsigned addr_space_byte_count ; // redundant = 2x bytecount
		unsigned iopage_start_addr ; // start addr of IOpage

		// First address of the memory a CPU module may answer on-module, and so
		// the ceiling for a card the board places. A KDJ11 carries its boot ROM
		// at 17370000..17757777 and answers that range without a bus cycle: a
		// card placed over it takes the write by DMA and the CPU reads its own
		// ROM back, which fails the ROM's RAM test at power-up. The reservation
		// covers that window and the block below it, which the ROM's memory
		// sizer walks into. 0 leaves the whole space up to the I/O page free.
		unsigned cpu_reserved_start ;

		// The address a card must stay below: the reservation when there is
		// one, the I/O page otherwise.
		unsigned memory_limit_addr(void) const {
			return cpu_reserved_start ? cpu_reserved_start : iopage_start_addr ;
		}

private:

	timeout_c timeout;

	// false: no running CPU on QBUS/UNIBUS (physical or emulated)
	// 	devices do DMA without NPR/NPG protocol
	// true:  active CPU. devices perform Request/Grant/SACK protocoll
	bool arbitrator_active;

	// Where probe_range() has its DATI land. A probe bounds its wait, and a
	// request that times out stays scheduled with the adapter still holding
	// this pointer (see dma()), so it may not be a local of the probing call -
	// the words would be written into a stack frame that is gone. What the
	// address answered with is never read; only whether it answered at all.
	uint16_t probe_word_buffer;

	// disabled
	uint8_t probe_grant_continuity(bool error_if_closed) ;

public:

	qunibus_c();
	~qunibus_c();

	void set_addr_width(unsigned addr_width) ;
	void assert_addr_width(void) ;

	static char *control2text(uint8_t control);
	static char *data2text(unsigned val);
	char *addr2text(unsigned addr) ;

	
	bool parse_addr(char *txt, uint32_t *addr);

	bool parse_word(char *txt, uint16_t *word);
		bool parse_level(	char *txt, uint8_t *level);
		bool parse_vector(char *txt, uint16_t max_vector,	uint16_t *vector);
		bool parse_slot(char *txt, uint8_t *priority_slot);

	void init(void);

	void set_cpu_bus_activity(bool active) ;
	void set_arbitrator_active(bool active);

	bool get_arbitrator_active(void);

	void powercycle(int phase = 3);
#if defined(QBUS)	
	void set_halt(bool active) ;
#endif

#if defined(UNIBUS)
	void set_address_overlay(uint32_t address_overlay) ;
	bool is_address_overlay_active(void) ;
#endif

	// functions of qunibusadapter to do simple DMA 
	dma_request_c *dma_request;
	//intr_request_c *intr_request;

	// share_bus leaves the running machine bus bandwidth by pausing as long as
	// the transfer took; a cycle the board makes for itself asks for none.
	// timeout_ms bounds the wait for the transfer, see qunibusadapter_c::DMA():
	// 0 waits for good, which is what a caller that cannot go on without the
	// data wants. A caller that can - a probe - bounds it, and must then give a
	// buffer that outlives the call, because a timed-out request stays
	// scheduled and the PRU may still write into it.
	bool dma(bool blocking,
			uint8_t qunibus_cycle, uint32_t startaddr, uint16_t *buffer, unsigned wordcount,
			bool share_bus = true, unsigned timeout_ms = 0);

	// timeout_ms as for dma(): 0 waits for good. A caller that must not be
	// parked by a backplane which never grants the bus - a machine switched
	// off, or one whose processor is not arbitrating - gives a bound, and
	// then `words` has to outlive the call.
	void mem_read(uint16_t *words,
			uint32_t unibus_start_addr, uint32_t unibus_end_addr, bool *timeout,
			unsigned timeout_ms = 0);
	void mem_write(uint16_t *words,
			unsigned unibus_start_addr, unsigned unibus_end_addr, bool *timeout,
			unsigned timeout_ms = 0);

	void mem_access_random(
			uint8_t unibus_control, uint16_t *words, uint32_t unibus_start_addr,
			uint32_t unibus_end_addr, bool *timeout, uint32_t *block_counter);

	// Size the machine's own memory: DATI ascending from 0 until the bus times
	// out, returning the first address nothing implements (0 = no memory at
	// all). Every chunk of the sweep is bounded, so a backplane that grants
	// the board nothing ends it instead of parking the caller for good; that
	// case answers 0 with *no_grant set, when a flag is given for it.
	uint32_t test_sizer(bool *no_grant = nullptr);

	// Probing a range: the lowest address in it that a slave answers, or
	// QUNIBUS_PROBE_NONE when nothing does.
	uint32_t probe_range(uint32_t startaddr, uint32_t endaddr,
			uint32_t step = QUNIBUS_PROBE_STEP);

	// DATI one address that may not answer: true with *w set when a slave did,
	// false when the cycle timed out. Unlike mem_read() a timeout is an answer
	// here rather than an error, so it is neither logged nor counted against
	// the bus. share_bus leaves a running machine its bandwidth, which a read
	// aimed at a live machine wants and a read of empty space does not.
	//
	// timeout_ms also bounds the wait for the *bus*, which a bus timeout does
	// not: on a backplane where nothing arbitrates, the board asks for a cycle
	// it is never granted, and an unbounded probe parks its caller for good.
	// With a timeout given, `w` must point at storage outliving the call.
	bool probe_word(uint32_t addr, uint16_t *w, bool share_bus = true,
			unsigned timeout_ms = 0);

	uint16_t testwords[QUNIBUS_MAX_WORDCOUNT];

	// false = stopped on a bus timeout or a data mismatch
	bool test_mem(uint32_t start_addr,	uint32_t end_addr, unsigned mode);
	
	void test_mem_print_error(uint32_t mismatch_count, uint32_t start_addr, uint32_t end_addr,  uint32_t cur_test_addr, uint16_t cur_mem_val) ;

};

extern qunibus_c *qunibus;

#endif /* ARM */

#endif /* _UNIBUS_H_ */
