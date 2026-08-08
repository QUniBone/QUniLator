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

// The whole KD11-EA for a reader outside it, including the two things a status
// parameter cannot show: the general registers, and the stack pointer of the
// mode the CPU is not in.
//
// The core keeps only the inactive stack pointer in stackpointer[] - R6 is
// whichever one the current mode uses - so both are reassembled here from the
// mode in PSW<15:14>. The core files every non-kernel mode under the user
// stack pointer, which is what an 11/34 does: the KT11-D has kernel and user
// and no supervisor mode between them.
void cpu34_c::core_get_snapshot(state_snapshot_c *snap)
{
    for (unsigned i = 0; i < 8; i++)
        snap->r[i] = kd11ea->r[i];
    snap->psw = kd11ea->psw;
    snap->ir = kd11ea->ir;
    snap->bus_addr = kd11ea->ba;
    snap->bus_data = kd11ea->bdata;
    snap->cycle_count = cycle_count.value;
    snap->state = (enum cpu_state_e) kd11ea->state;
    snap->has_modes = true;
    snap->has_stackpointers = true;
    bool kernel = ((kd11ea->psw >> 14) & 3) == KT11D_MODE_KERNEL;
    snap->sp_kernel = kernel ? kd11ea->r[6] : kd11ea->stackpointer[KD11EA_SP_KERNEL];
    snap->sp_user = kernel ? kd11ea->stackpointer[KD11EA_SP_USER] : kd11ea->r[6];
    snap->has_mmu = true;
    snap->mmu_enabled = (kd11ea->mmu.enabled != 0);
    snap->mmr0 = kd11ea->mmu.mmr0;
    snap->mmr1 = kd11ea->mmu.mmr1;
    snap->mmr2 = kd11ea->mmu.mmr2;
    // The page registers of both modes. par[]/pdr[] are one array indexed by
    // space + page, and the two spaces are eight apart; taken apart here so a
    // reader outside needs to know nothing about that layout.
    for (unsigned i = 0; i < 8; i++) {
        snap->kernel_par[i] = kd11ea->mmu.par[KT11D_SPACE_KERNEL + i];
        snap->kernel_pdr[i] = kd11ea->mmu.pdr[KT11D_SPACE_KERNEL + i];
        snap->user_par[i] = kd11ea->mmu.par[KT11D_SPACE_USER + i];
        snap->user_pdr[i] = kd11ea->mmu.pdr[KT11D_SPACE_USER + i];
    }
}
