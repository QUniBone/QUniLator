/* pdp11disas.hpp: PDP-11 disassembler

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


 Turns PDP-11 words back into instruction text. One call disassembles one
 instruction, pdp11disas_region() calls that repeatedly - which is all a code
 listing is, because on a PDP-11 an instruction is only as long as its operands
 make it.

 The module is deliberately free of every QUniBone dependency: no bus, no PRU,
 no logger, no threads. Memory is reached through pdp11disas_memory_c, which the
 caller implements over whatever it has - the UNIBUS/QBUS via DMA, the shared
 DDR memory, a file image, the memory of an emulated CPU. That keeps it usable
 from anywhere in the tree and lets it be unit tested on the build machine
 (90_common/test/pdp11disas_test.cpp).

 The 11/20 has fewer instructions than the 11/70, and neither has the CIS. So a
 disassembler must be told which machine it is looking at, else it invents
 instructions the CPU would trap on. pdp11disas_options_c holds a CPU model and
 the instruction set options installed with it; an instruction outside that set
 is still disassembled, but marked "not available" and commented in the listing.
 The default is a PDP-11/20 with nothing added - the smallest instruction set,
 so nothing is silently accepted that the machine could not execute.

 The opcode table is a port of the disassembler of pdp11gui
 (common/Pdp11DisasU.pas), which was cross-checked against the opcode tables of
 SimH's PDP-11 simulator (pdp11_sys.c). The per-model instruction set
 assignments follow SimH's feature sets (pdp11_defs.h HAS_..., pdp11_cpumod.h
 SOP_... and OPT_...).

 Two things a *static* disassembler cannot know, both inherent, not defects:

 - F/D (single/double precision) and I/L (integer/long) FP11 instructions have
   identical opcode bits; only the live FPS register tells them apart. The F/I
   spelling is printed by default, "fpd"/"fpl" switch to the other one.
 - The in-line CIS instructions (movci, addni, ...) carry their descriptors as
   data words behind the opcode. Their format is variable, so only the opcode
   word is consumed and the descriptor words appear as their own (meaningless)
   listing lines - the same choice SimH's disassembler makes.
 */

#ifndef _PDP11DISAS_HPP_
#define _PDP11DISAS_HPP_

#include <stdint.h>
#include <string>
#include <vector>

// Longest instruction: opcode word + one index/immediate word per operand.
#define PDP11DISAS_MAX_WORDS	3

// Instruction set options a PDP-11 may or may not have. An instruction needing
// one which is not present is flagged in the listing.
enum pdp11disas_option_e {
	pdp11disas_opt_eis	= 0x0001,	// mul div ash ashc
	pdp11disas_opt_fis	= 0x0002,	// fadd fsub fmul fdiv (KE11-F, KEV11)
	pdp11disas_opt_fp11	= 0x0004,	// the 17xxxx floating point instructions
	pdp11disas_opt_cis	= 0x0008,	// commercial instruction set, 076xxx
	pdp11disas_opt_mmu	= 0x0010,	// mfpi mtpi mfpd mtpd
	pdp11disas_opt_mfps	= 0x0020,	// mfps mtps
	pdp11disas_opt_spl	= 0x0040,	// spl
	pdp11disas_opt_sxs	= 0x0080,	// sxt xor sob
	pdp11disas_opt_mark	= 0x0100,	// mark
	pdp11disas_opt_rtt	= 0x0200,	// rtt
	pdp11disas_opt_mfpt	= 0x0400,	// mfpt
	pdp11disas_opt_csm	= 0x0800,	// csm
	pdp11disas_opt_tswlk	= 0x1000,	// tstset wrtlck
	// not an instruction set, but a decode choice, see the file header:
	pdp11disas_opt_fpd	= 0x2000,	// print the FP11 double precision mnemonics
	pdp11disas_opt_fpl	= 0x4000	// print the FP11 "long integer" mnemonics
};

// What the disassembler assumes about the machine it is looking at.
class pdp11disas_options_c {
public:
	std::string cpu_model;	// "11/20"
	unsigned options;	// set of pdp11disas_option_e

	pdp11disas_options_c();	// PDP-11/20, no options: the smallest instruction set

	// Set the CPU model and with it the instruction sets it is built with.
	// Accepted as "11/34", "1134" or "34", case insensitive.
	// false: unknown model, nothing changed.
	bool set_cpu_model(const std::string& model);

	// Enable/disable a single option by name ("eis", "fp11", "cis", ...),
	// overriding what the model brought. false: unknown option name.
	bool set_option(const std::string& name, bool enable);

	bool has(unsigned required_options) const {
		return (required_options & ~options) == 0;
	}

	// for help texts and messages
	static std::string cpu_model_list(void);	// "11/03 11/04 ... 11/94"
	static std::string option_list(void);		// "eis fis fp11 ... fpl"
	static std::string option_info(const std::string& name);	// what it enables
	std::string as_text(void) const;	// "cpu 11/34, options: eis mmu ..."
	// names of the options in <option_set>, space separated, "none" if empty
	static std::string options_as_text(unsigned option_set);
};

// Memory the disassembler reads its words from. Implemented by the caller over
// the bus, over DDR memory, over a file image, ...
class pdp11disas_memory_c {
public:
	virtual ~pdp11disas_memory_c() {
	}
	// false: <addr> does not hold a readable word. The disassembler then
	// falls back to ".word" and stops the region.
	virtual bool get_word(uint32_t addr, uint16_t *word) = 0;
};

// A pdp11disas_memory_c over a plain array of words, for callers which have the
// code in a buffer already (and for the unit test).
class pdp11disas_buffermemory_c: public pdp11disas_memory_c {
public:
	// <words> holds <wordcount> words, the first one is at address <startaddr>
	pdp11disas_buffermemory_c(const uint16_t *words, unsigned wordcount, uint32_t startaddr = 0);
	bool get_word(uint32_t addr, uint16_t *word) override;
private:
	const uint16_t *words;
	unsigned wordcount;
	uint32_t startaddr;
};

// One disassembled instruction.
class pdp11disas_instruction_c {
public:
	uint32_t addr = 0;	// address of the opcode word
	unsigned wordcount = 0;	// words the instruction occupies. 0: nothing readable
	uint16_t word[PDP11DISAS_MAX_WORDS] = { 0, 0, 0 };
	std::string mnemonic;		// "mov", ".word"
	std::string operands;		// "#001776,sp"
	unsigned required_options = 0;	// 0: instruction of the base set
	bool known = false;		// false: no instruction on any PDP-11
	bool available = false;		// known, and required_options all present
	bool truncated = false;		// a word of the instruction is not readable
	std::string cpu_model;		// the model it was disassembled for, for the comment
	// addresses named by the operands which have a well known meaning:
	// device registers, MMU/CPU registers, trap and interrupt vectors
	std::vector<uint32_t> known_addresses;

	// "mov     #001776,sp", with "; ..." appended if something is wrong
	std::string text(void) const;
	// "001000  012706 001776         mov     #001776,sp"
	std::string listing_line(void) const;
	// the comment alone, "" if the instruction is plain and available
	std::string comment(void) const;
};

// What the address <addr> is on a PDP-11: a device register, a processor or
// memory management register, or a trap/interrupt vector. "" if <addr> means
// nothing in particular. Only the low 16 bits are looked at.
// Usable on its own - EXAMINE, a memory dump or a bus trace can name an
// address with it just as well.
std::string pdp11disas_address_info(uint32_t addr);

// Disassemble the instruction at <addr>. Returns the address of the next one.
// *result is always filled; result->wordcount == 0 means <addr> itself could
// not be read and disassembly cannot go on.
uint32_t pdp11disas_instruction(const pdp11disas_options_c& options,
		pdp11disas_memory_c& memory, uint32_t addr, pdp11disas_instruction_c *result);

// Disassemble <instruction_count> instructions from <addr>, appending them to
// *result. Returns the address after the last one. Stops early on unreadable
// memory, so result->size() may be less than <instruction_count>.
uint32_t pdp11disas_region(const pdp11disas_options_c& options, pdp11disas_memory_c& memory,
		uint32_t addr, unsigned instruction_count, std::vector<pdp11disas_instruction_c> *result);

#endif
