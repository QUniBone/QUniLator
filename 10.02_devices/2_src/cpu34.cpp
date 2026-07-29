/* cpu34.cpp: PDP-11/34 CPU, emulated by the KD11-EA core

 Copyright (c) 2026, Joerg Hoppe

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

 Everything model independent is in cpu_base_c, see cpu.cpp.
 */

#include <string.h>
#include <stdlib.h>

#include "logger.hpp"

#include "cpu34.hpp"
#include "cpu_core.h"
#include "cpu34/kt11d.h"
#include "cpu34/kd11ea.h"

cpu34_c::cpu34_c() :
    cpu_base_c()  // super class constructor
{
    // static config
    name.value = "CPU34";
    type_name.value = "PDP-11/34";
    log_label = "cpu34";

    // the memory management unit's own state, driven by the machine
    mmr0.kind = parameter_c::PARAM_STATUS;
    mmr1.kind = parameter_c::PARAM_STATUS;
    mmr2.kind = parameter_c::PARAM_STATUS;
    mmu_enabled.kind = parameter_c::PARAM_STATUS;

    // emulation core state. Not in the header, so cpu34.hpp stays free of kd11ea.h
    kd11ea = (struct KD11EA *) calloc(1, sizeof(struct KD11EA));
    assert(kd11ea);
    kd11ea_init(kd11ea); // the intr mutex, shared with the qunibusadapter thread
}

cpu34_c::~cpu34_c()
{
    free(kd11ea);
}

/*** interface to the KD11-EA emulation core ***/

void cpu34_c::core_condstep(void)
{
    kd11ea_condstep(kd11ea);
}

void cpu34_c::core_reset(void)
{
    // console START / power-up, not the RESET opcode: also clears the KT11-D
    kd11ea_power_reset(kd11ea);
}

void cpu34_c::core_setintr(uint16_t vector)
{
    kd11ea_setintr(kd11ea, vector);
}

void cpu34_c::core_pwrfail_trap(void)
{
    kd11ea_pwrfail_trap(kd11ea);
}

void cpu34_c::core_pwrup_vector_fetch(void)
{
    kd11ea_pwrup_vector_fetch(kd11ea);
}

void cpu34_c::core_printstate(void)
{
    kd11ea_printstate(kd11ea);
}

void cpu34_c::core_tracestate(void)
{
    kd11ea_tracestate(kd11ea);
}

enum cpu_base_c::cpu_state_e cpu34_c::core_get_state(void)
{
    return (enum cpu_state_e) kd11ea->state;
}

void cpu34_c::core_set_state(enum cpu_state_e state)
{
    kd11ea->state = (int) state;
}

uint16_t cpu34_c::core_get_pc(void)
{
    return kd11ea->r[7];
}

void cpu34_c::core_set_pc(uint16_t value)
{
    kd11ea->r[7] = value;
}

void cpu34_c::core_set_switches(uint16_t value)
{
    kd11ea->sw = value;
}

// The KD11-EA status word carries the current and previous mode above the
// priority, T bit and condition codes the KA11 also has. The memory
// management registers go out with it: they are internal to this CPU, so a
// status parameter is the only way to read them from outside.
void cpu34_c::core_publish_status(void)
{
    psw.value = kd11ea->psw;
    bus_addr.value = kd11ea->ba;
    bus_data.value = kd11ea->bdata;
    mmr0.value = kd11ea->mmu.mmr0;
    mmr1.value = kd11ea->mmu.mmr1;
    mmr2.value = kd11ea->mmu.mmr2;
    mmu_enabled.value = (kd11ea->mmu.enabled != 0);
}
