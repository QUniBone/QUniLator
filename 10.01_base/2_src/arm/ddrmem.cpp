/* ddrmem.cpp - Control the shared DDR RAM - used for QBUS/UNIBUS memory

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

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>

#include "logger.hpp"
#include "scriptpath.hpp"
#include "mailbox.h"
#include "iopageregister.h"
#include "ddrmem.h"

#include "application.hpp"

/* another singleton */
ddrmem_c *ddrmem;

ddrmem_c::ddrmem_c() 
{
	log_label = "DDRMEM";
	pmi_address_overlay = 0 ;
}

// check allocated memory and print info
void ddrmem_c::info() 
{
	INFO("Shared DDR memory: %u bytes available, %u bytes needed.", len, sizeof(ddrmem_t));
	if (len < sizeof(ddrmem_t)) {
		FATAL("Not enough shared DDR memory allocated by \"uio_pruss\"!\n"
				"To fix, use\n"
				"  modinfo uio_pruss\n"
				"  cat /sys/class/uio/uio0/maps/map1/size\n"
				"Change \"extram_pool_sz\" value for module uio_pruss:\n"
				"  vi /etc/modprobe.d/uio_pruss.conf");
	}

	INFO("  Virtual (ARM Linux-side) address: %p", base_virtual);
	INFO("  Physical (PRU-side) address:%x", base_physical);
	INFO("  %d bytes of " QUNIBONE_NAME" memory allocated", sizeof(qunibus_memory_t));
}

// An enabled range other than "slot" that shares an address with
// [startaddr, endaddr], or -1: two slots answering one cycle would have the
// PRU serve whichever is tested first, so a claim that overlaps is refused.
int ddrmem_c::overlapping_slot(unsigned slot, uint32_t startaddr, uint32_t endaddr) const
{
	for (unsigned i = 0; i < DDRMEM_RANGE_COUNT; i++) {
		if (i == slot || !ranges[i].enabled)
			continue;
		if (startaddr <= ranges[i].endaddr && ranges[i].startaddr <= endaddr)
			return (int) i;
	}
	return -1;
}

// which emulated range serves addr, -1 if none does
int ddrmem_c::slot_of(uint32_t addr) const
{
	for (unsigned slot = 0; slot < DDRMEM_RANGE_COUNT; slot++) {
		const range_t &r = ranges[slot];
		if (r.enabled && addr >= r.startaddr && addr <= r.endaddr)
			return (int) slot;
	}
	return -1;
}

// A transfer is served from DDR only if one range holds all of it: a block
// straddling the end of a range is half on the bus and must go over the bus.
// endaddr is the last word address, so the odd byte above it is in the range
// too — a DATOB there is served, as the PRU's limit address says.
bool ddrmem_c::contains(uint32_t addr, unsigned byte_count) const
{
	int slot = slot_of(addr);
	return slot >= 0 && byte_count > 0
			&& (uint64_t) addr + byte_count - 1 <= (uint64_t) ranges[slot].endaddr + 1;
}

// read/write ddr memory locally
// result: true =OK, else illegal address ("timeout")
bool ddrmem_c::deposit(uint32_t addr, uint16_t w)
{
	if (slot_of(addr) < 0)
		return false;
	base_virtual->memory.words[addr / 2] = w;
	return true;
}

bool ddrmem_c::exam(uint32_t addr, uint16_t *w)
{
	if (slot_of(addr) < 0)
		return false;
	*w = base_virtual->memory.words[addr / 2];
	return true;
}

// if CPU accesses memory direct and not via QBUS/UNIBUS
// PMI = "Private Memory Interconnect" = local memory bus on 11/44,60,84 etc.
// No QBUS/UNIBUS range, always memory present at all addresses
bool ddrmem_c::pmi_deposit(uint32_t addr, uint16_t w) 
{
	assert((addr & 1) == 0); // must be even
	assert(addr < qunibus->iopage_start_addr);
	base_virtual->memory.words[addr / 2] = w;
	return true;
}

bool ddrmem_c::pmi_exam(uint32_t addr, uint16_t *w) 
{
	assert((addr & 1) == 0); // must be even
//	assert(addr < qunibus->iopage_start_addr); IOPAGE ROM also allowed
	*w = base_virtual->memory.words[addr / 2];
	return true;
}


// IF an emulated CPU is working on DDRRAM directly via PMI
// AND an M9312 is overlaying addresses with 773000 
// THEN also PMI accesses must be redirected the same way.
//
// This is something not existent on real PDP11s with separate memory bus:
// The M9312 in IO UNIBUS cannot manipulated addresses on the Memory BUS.
void ddrmem_c::set_pmi_address_overlay(uint32_t _address_overlay) 
{
	pmi_address_overlay = _address_overlay ;
}


// independent of memory emulation, memory used for IOpage ROM is always accessible
bool ddrmem_c::iopage_deposit(uint32_t addr, uint16_t w) 
{
	if (addr < qunibus->iopage_start_addr || addr >= qunibus->addr_space_byte_count)
		return false;
	assert(len >= addr/2) ;
	base_virtual->memory.words[addr / 2] = w;
	return true;
}

bool ddrmem_c::iopage_exam(uint32_t addr, uint16_t *w) 
{
	if (addr < qunibus->iopage_start_addr || addr >= qunibus->addr_space_byte_count)
		return false;
	assert(len >= addr/2) ;
	*w = base_virtual->memory.words[addr / 2];
	return true;
}


// write to disk file

void ddrmem_c::save(char *fname) 
{
	FILE *fout;
	unsigned wordcount = qunibus->addr_space_word_count;
	unsigned n;
	fout = fopen(fname, "wb");
	if (!fout) {
		ERROR(fileErrorText("Error opening file %s for write", fname));
		return;
	}
	n = fwrite((void *) base_virtual->memory.words, 2, wordcount, fout);
	fclose(fout);

	if (n != wordcount) {
		ERROR("ddrmem_save(): tried to write %u words, only %u successful.", wordcount, n);
	}

}

// load from disk file
void ddrmem_c::load(char *fname) 
{
	FILE *fin;
	// a file a script names is looked for next to the script, see scriptpath.hpp
	std::string fpath = scriptpath_resolve(fname);
	fin = fopen(fpath.c_str(), "rb");
	if (!fin) {
		ERROR(fileErrorText("Error opening file %s for read", fpath.c_str()));
		return;
	}
	// try to read max address range, shorter files are OK
	fread((void *) base_virtual->memory.words, 2, qunibus->addr_space_word_count, fin);
	fclose(fin);
}

// fill whole memory with 0
void ddrmem_c::clear(void) 
{
	memset((void *) base_virtual, 0, sizeof(qunibus_memory_t));
}

// fill one range with 0, so a machine starts on cleared memory rather than on
// whatever the last run left in the DDR reservation
void ddrmem_c::clear_range(uint32_t startaddr, uint32_t endaddr)
{
	if (startaddr > endaddr)
		return;
	memset((void *) (base_virtual->memory.bytes + startaddr), 0, endaddr - startaddr + 2);
}

// fill one range with a word value, both bytes of each word written
void ddrmem_c::fill_range(uint32_t startaddr, uint32_t endaddr, uint16_t fillword)
{
	volatile uint16_t *wordaddr = base_virtual->memory.words + startaddr / 2;
	for (uint32_t addr = startaddr; addr <= endaddr; addr += 2)
		*wordaddr++ = fillword;
}

// fill whole memory with pattern, with local code
void ddrmem_c::fill_pattern(void) 
{
	unsigned n;
	volatile uint16_t *wordaddr = base_virtual->memory.words;
	for (n = 0; n < qunibus->addr_space_word_count; n++) {
		*wordaddr++ = ~n;
	}
}

// fill whole memory with pattern, by PRU
void ddrmem_c::fill_pattern_pru(void) 
{
	// ddrmem_base_physical and _len already set
	assert((uint32_t )mailbox->ddrmem_base_physical == base_physical);
	mailbox_execute(ARM2PRU_DDR_FILL_PATTERN);
}

// set corrected values for one emulated memory range
// start > end: disable the slot
// precondition:  deviceregister_init()
// result: false = error
bool ddrmem_c::set_range(unsigned slot, uint32_t startaddr, uint32_t endaddr)
{
	bool error;

	assert(slot < DDRMEM_RANGE_COUNT);
	range_t &range = ranges[slot];

	pru_iopage_registers->iopage_start_addr = qunibus->iopage_start_addr ;

	// need position of iopage and # of words in memory
	if (qunibus->addr_width == 0 || qunibus->iopage_start_addr == 0)
		// Error in GUI?
		FATAL("Address width of QBUS not yet known!") ;

	range.startaddr = startaddr;
	range.endaddr = endaddr;

	range.enabled = (startaddr < qunibus->addr_space_byte_count
			&& endaddr < qunibus->addr_space_byte_count
			&& startaddr <= endaddr);
	if (!range.enabled) {
		pru_iopage_registers->memory_start_addr[slot] = 0 ;
		pru_iopage_registers->memory_limit_addr[slot] = 0 ; // disable in PRU
		return true;
	}
	error = true;
	int other = overlapping_slot(slot, startaddr, endaddr);
	if (endaddr >= qunibus->addr_space_byte_count)
		WARNING("End addr %s exceeds %d bit address range", qunibus->addr2text(endaddr), qunibus->addr_width);
	if (endaddr >= qunibus->iopage_start_addr)
		WARNING("End addr %s in IO page", qunibus->addr2text(endaddr));
	// addresses must fit page borders
	else if (startaddr % 2)
		WARNING("Start addr %s is no word address", qunibus->addr2text(startaddr));
	else if (endaddr % 2)
		WARNING("End addr %s is no word address", qunibus->addr2text(endaddr));
	else if (other >= 0)
		WARNING("Range %s..%s overlaps the range %s..%s of slot %d",
				qunibus->addr2text(startaddr), qunibus->addr2text(endaddr),
				qunibus->addr2text(ranges[other].startaddr),
				qunibus->addr2text(ranges[other].endaddr), other);
	else {
		// mark address range as "memory emulation"
		pru_iopage_registers->memory_start_addr[slot] = startaddr ;
		// endaddr is even, limit_addr is first invalid address -> endaddr+2 included for DATOB
		pru_iopage_registers->memory_limit_addr[slot] = endaddr+2 ;
		error = false ;
	}
	if (error)
		range.enabled = false;
	return !error;
}

// implement an QBUS/UNIBUS memory card with DDR memory
// PRU act as slave to qunibus master cycles
void ddrmem_c::unibus_slave(uint32_t startaddr, uint32_t endaddr) 
{
	char *s, buf[20]; // dummy
	// this command starts QBUS/UNIBUS slave logic in PRU
	set_range(DDRMEM_RANGE_MEMORY, startaddr, endaddr);
	mailbox->arm2pru_req = ARM2PRU_DDR_SLAVE_MEMORY;
	printf("Hit 'q' ENTER to end.\n");
	do { // only this code wait for input under Eclipse
		s = app->inputline.readline(buf, sizeof(buf), NULL);
	} while (strlen(s) == 0);
	// clearing  arm2pru_req stops the emulation
	mailbox_execute(ARM2PRU_NONE);
}

