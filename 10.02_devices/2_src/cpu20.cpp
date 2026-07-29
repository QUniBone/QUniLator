/* cpu20.cpp: PDP-11/20 CPU, emulated by Angelos KA11 core

 Copyright (c) 2018, Angelo Papenhoff, Joerg Hoppe

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


 24-jul-2026  JH      split off from cpu.cpp
 16-oct-2020  JH      merged VBIT changes by github jks-prv
 23-nov-2018  JH      created

 Everything model independent is in cpu_base_c, see cpu.cpp.
 */

#include <string.h>
#include <stdlib.h>

#include "logger.hpp"

#include "cpu20.hpp"
#include "cpu_core.h"
#include "cpu20/ka11.h"

cpu20_c::cpu20_c() :
    cpu_base_c()  // super class constructor
{
    // static config
    name.value = "CPU20";
    type_name.value = "PDP-11/20";
    log_label = "cpu20";

    swab_vbit.value = false;

    // emulation core state. Not in the header, so cpu20.hpp stays free of ka11.h
    ka11 = (struct KA11 *) calloc(1, sizeof(struct KA11));
    assert(ka11);
    ka11_init(ka11); // the intr mutex, shared with the qunibusadapter thread
}

cpu20_c::~cpu20_c()
{
    free(ka11);
}

/*** interface to the KA11 emulation core ***/

void cpu20_c::core_condstep(void)
{
    ka11_condstep(ka11);
}

void cpu20_c::core_reset(void)
{
    ka11_reset(ka11);
}

void cpu20_c::core_setintr(uint16_t vector)
{
    ka11_setintr(ka11, vector);
}

void cpu20_c::core_pwrfail_trap(void)
{
    ka11_pwrfail_trap(ka11);
}

void cpu20_c::core_pwrup_vector_fetch(void)
{
    ka11_pwrup_vector_fetch(ka11);
}

void cpu20_c::core_printstate(void)
{
    ka11_printstate(ka11);
}

void cpu20_c::core_tracestate(void)
{
    ka11_tracestate(ka11);
}

enum cpu_base_c::cpu_state_e cpu20_c::core_get_state(void)
{
    return (enum cpu_state_e) ka11->state;
}

void cpu20_c::core_set_state(enum cpu_state_e state)
{
    ka11->state = (int) state;
}

uint16_t cpu20_c::core_get_pc(void)
{
    return ka11->r[7];
}

void cpu20_c::core_set_pc(uint16_t value)
{
    ka11->r[7] = value;
}

void cpu20_c::core_set_switches(uint16_t value)
{
    ka11->sw = value;
}

void cpu20_c::core_apply_options(void)
{
    ka11->swab_vbit = (swab_vbit.value == true);
}

// The KA11 keeps its status in a byte: priority, T and the condition codes.
// It has no memory management and no modes, so there is nothing above bit 7.
void cpu20_c::core_publish_status(void)
{
    psw.value = ka11->psw;
    bus_addr.value = ka11->ba;
    bus_data.value = ka11->bdata;
}
