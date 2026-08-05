/* cpu34.hpp: PDP-11/34 CPU, emulated by the KD11-EA core

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


 24-jul-2026  JH      created

 The KD11-EA core in cpu34/ started as a fork of the 11/20 KA11 core. It now
 has the KT11-D memory management (cpu34/kt11d.c) with kernel/user modes, plus
 EIS, MFPS/MTPS and the 11/34 SWAB behaviour.

 register_count stays 0 (set by cpu_base_c): the KT11-D registers (MMR0..MMR2
 and the kernel/user PAR/PDR blocks) are internal to the CPU, exactly as on
 real hardware, and are decoded by kd11ea.c dati()/dato(). They are therefore
 not published as QUNIBUS registers and cannot be reached by other bus
 masters - use "examine state" to see them.
 */
#ifndef _CPU34_HPP_
#define _CPU34_HPP_

#include "cpu.hpp"

// The KD11-EA core is plain C, see cpu34/kd11ea.h and cpu_core.h.
// Only cpu34.cpp includes those headers, so that cpu34.hpp stays free of the
// core's types.
struct KD11EA;

class cpu34_c: public cpu_base_c {
public:

    cpu34_c();
    ~cpu34_c();

    struct KD11EA *kd11ea; // CPU state

    // The KT11-D's own registers, published as status because the machine
    // drives them. They are not QUNIBUS registers (see the file header), so
    // this is the only way to read them from outside the CPU.
    // MMR0 holds the enable bit and, once an abort has frozen it, what caused
    // it; MMR1 the register changes of the aborted instruction; MMR2 the
    // virtual address that instruction was fetched from.
    parameter_unsigned_c mmr0 = parameter_unsigned_c(this, "MMR0", "mmr0",/*readonly*/
                                true, "", "%06o", "KT11-D status register 0.", 16, 8);
    parameter_unsigned_c mmr1 = parameter_unsigned_c(this, "MMR1", "mmr1",/*readonly*/
                                true, "", "%06o", "KT11-D status register 1: register changes of the aborted instruction.", 16, 8);
    parameter_unsigned_c mmr2 = parameter_unsigned_c(this, "MMR2", "mmr2",/*readonly*/
                                true, "", "%06o", "KT11-D status register 2: virtual address of the instruction.", 16, 8);
    parameter_bool_c mmu_enabled = parameter_bool_c(this, "mmu_enabled", "mmu",/*readonly*/
                                   true, "Memory management: 1 = relocation on, addresses are 18 bit.");

    // interface to the KD11-EA emulation core
    void core_condstep(void) override;
    void core_reset(void) override;
    void core_setintr(uint16_t vector) override;
    void core_pwrfail_trap(void) override;
    void core_pwrup_vector_fetch(void) override;
    void core_printstate(void) override;
    void core_tracestate(void) override;
    enum cpu_state_e core_get_state(void) override;
    void core_set_state(enum cpu_state_e state) override;
    uint16_t core_get_pc(void) override;
    void core_set_pc(uint16_t value) override;
    void core_set_switches(uint16_t value) override;
    void core_publish_status(void) override;
    void core_get_snapshot(state_snapshot_c *snap) override;
    // no core_apply_options(): the 11/34 has no CPU feature parameters.
    // EIS and MFPS/MTPS are always executed, SWAB always clears the V bit.
};

#endif
