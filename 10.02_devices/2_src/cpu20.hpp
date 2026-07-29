/* cpu20.hpp: PDP-11/20 CPU, emulated by Angelos KA11 core

 Copyright (c) 2018, Angelo Papenhoff, Joerg Hoppe
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


 24-jul-2026  JH      split off from cpu.hpp
 23-nov-2018  JH      created
 */
#ifndef _CPU20_HPP_
#define _CPU20_HPP_

#include "cpu.hpp"

// The KA11 core is plain C, see cpu20/ka11.h and cpu_core.h.
// Only cpu20.cpp includes those headers, so that cpu20.hpp stays free of the
// core's types.
struct KA11;

class cpu20_c: public cpu_base_c {
public:

    cpu20_c();
    ~cpu20_c();

    struct KA11 *ka11; // Angelos CPU state

    // 11/20 specific option
    parameter_bool_c swab_vbit = parameter_bool_c(this, "swab_vbit", "swab",/*readonly*/
                                 false, "SWAB instruction does not(=0) or does(=1) modify psw v-bit (=0 is standard 11/20 behavior)");

    // interface to the KA11 emulation core
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
    void core_apply_options(void) override;
    void core_publish_status(void) override;
};

#endif
