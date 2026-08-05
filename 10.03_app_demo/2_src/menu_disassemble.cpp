/* menu_disassemble.cpp: paged disassembly of a memory region

 Copyright (c) 2026, Joerg Hoppe
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


 The disassembler itself is 90_common/src/pdp11disas.cpp and knows nothing
 about a bus. Here it is connected to the two memories "demo" can read - the
 QBUS/UNIBUS via DMA and the shared DDR memory - and wrapped in the page-wise
 output the "da"/"xda" commands use.

 Whoever else wants a code listing calls application_c::disassemble() with one
 of the two memory objects below, so this is not tied to one menu.
 */

#include <stdio.h>
#include <string.h>

#include "kbhit.h"
#include "application.hpp"	// own
#include "qunibus.h"
#include "ddrmem.h"

// instructions printed before the pager asks
#define DISASSEMBLE_PAGE_LINES	10

/**********************************************************************
 * the two memories a code listing can be made from
 **********************************************************************/

// QBUS/UNIBUS memory, read with DMA.
// One DATI per word would be correct but slow and would flood the bus, so a
// whole window is fetched at once and served from there - the same trick the
// EXAMINE command uses when it reads <n> words with one dma() call.
qunibus_disasmemory_c::qunibus_disasmemory_c(unsigned timeout_ms_)
{
	window_startaddr = 0;
	window_wordcount = 0;
	timeout_ms = timeout_ms_;
}

bool qunibus_disasmemory_c::get_word(uint32_t addr, uint16_t *word)
{
	if (addr & 1)
		return false;
	if (addr >= qunibus->addr_space_byte_count)
		return false;

	if (window_wordcount == 0 || addr < window_startaddr
			|| addr >= window_startaddr + 2 * window_wordcount) {
		// (re)fill the window, starting at the requested address
		unsigned wordcount = QUNIBUS_DISASMEMORY_WINDOW_WORDS;
		if (addr + 2 * wordcount > qunibus->addr_space_byte_count)
			wordcount = (qunibus->addr_space_byte_count - addr) / 2;
		if (wordcount == 0)
			return false;
		window_startaddr = addr;
		window_wordcount = 0;
		if (qunibus->dma(true, QUNIBUS_CYCLE_DATI, addr, window, wordcount,
				/*share_bus*/true, timeout_ms))
			window_wordcount = wordcount;
		else
			// A bus timeout somewhere in the window. How far the memory
			// reaches is then read word by word: a DMA of a single word
			// either succeeded or it did not, while for a block the address
			// it stopped at does not say whether that word arrived.
			// Only a listing running into the end of memory pays for this.
			for (unsigned i = 0; i < wordcount; i++)
				if (qunibus->dma(true, QUNIBUS_CYCLE_DATI, addr + 2 * i, &window[i], 1,
						/*share_bus*/true, timeout_ms))
					window_wordcount = i + 1;
				else
					break;
		if (window_wordcount == 0)
			return false;	// nothing readable at <addr>: bus timeout
	}
	if (addr >= window_startaddr + 2 * window_wordcount)
		return false;	// inside the window, but behind the timeout
	*word = window[(addr - window_startaddr) / 2];
	return true;
}

// The shared DDR memory, without any bus traffic. Only the emulated range
// answers, exam() says so.
bool ddrmem_disasmemory_c::get_word(uint32_t addr, uint16_t *word)
{
	if (addr & 1)
		return false;
	return ddrmem->exam(addr, word);
}

/**********************************************************************
 * the paged listing
 **********************************************************************/

// Print instructions from <startaddr>, DISASSEMBLE_PAGE_LINES at a time.
// <instruction_count> 0: go on until the user stops it.
// Returns the address after the last instruction printed, so a following
// "da" without argument continues there.
uint32_t application_c::disassemble(pdp11disas_memory_c& memory, uint32_t startaddr,
		unsigned instruction_count)
{
	uint32_t addr = startaddr;
	unsigned printed = 0;

	while (instruction_count == 0 || printed < instruction_count) {
		std::vector<pdp11disas_instruction_c> instructions;
		unsigned pagelines = DISASSEMBLE_PAGE_LINES;

		if (instruction_count && instruction_count - printed < pagelines)
			pagelines = instruction_count - printed;

		addr = pdp11disas_region(disassembler_options, memory, addr, pagelines, &instructions);
		for (unsigned i = 0; i < instructions.size(); i++)
			printf("%s\n", instructions[i].listing_line().c_str());
		printed += instructions.size();

		if (instructions.size() < pagelines) {
			// memory ended: say why, there is nothing more to page through.
			// Not with addr2text(): that one needs a known bus address width,
			// which a listing of DDR memory does not.
			printf("No readable memory at %0*o.\n", addr > 0777777 ? 8 : 6, addr);
			break;
		}
		if (instruction_count && printed >= instruction_count)
			break;

		// A command script must not be asked: it would answer with its next
		// command line. It gets the whole region in one go.
		if (script_active())
			continue;

		printf("-- more -- (ENTER = next page, ESC = end) ");
		fflush(stdout);
		int key = os_getkey();
		printf("\r                                          \r");
		fflush(stdout);
		// ESC, 'q' and a stdin which is no terminal end the listing
		if (key != '\r' && key != '\n' && key != ' ')
			break;
	}
	return addr;
}
