/* testcore.hpp: uniform handle on one of the CPU emulation cores

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


 The same job cpu20_c and cpu34_c do for the application: hide which C core is
 behind it. Deliberately shaped like the core_*() virtuals of cpu_base_c
 (10.02_devices/2_src/cpu.hpp), so the two stay recognizably parallel - only
 without the QUNIBUS device, the parameters and the worker thread.

 Both cores are linked into this one binary. That is safe because their symbols
 are prefixed ka11_ and kd11ea_, which is also why the application can offer
 both CPU models at once.

 The two implementations live in separate .cpp files, mirroring the
 cpu20.cpp/cpu34.cpp split on the application side; only this abstract class
 crosses between them.
 */
#ifndef _TESTCORE_HPP_
#define _TESTCORE_HPP_

#include <stdint.h>

class testcore_c {
public:
    // identical to KA11_STATE_*/KD11EA_STATE_* and cpu_base_c::cpu_state_e
    enum state_e {
        state_halted = 0, state_running = 1, state_waiting = 2
    };

    virtual ~testcore_c() { }

    virtual const char *name(void) = 0;

    // console START / power-up: clear the core (and, on the 11/34, the KT11-D)
    virtual void power_reset(void) = 0;
    // execute one instruction, if running
    virtual void condstep(void) = 0;
    // dump the registers to stdout
    virtual void printstate(void) = 0;

    // deliver an interrupt vector, as cpu_base_c::on_interrupt() does when a
    // granted device puts one on the bus. The core takes it before its next
    // instruction; whether it may is decided by the arbitrator, not here.
    virtual void setintr(uint16_t vector) = 0;

    virtual state_e get_state(void) = 0;
    virtual void set_state(state_e state) = 0;
    virtual uint16_t get_pc(void) = 0;
    virtual void set_pc(uint16_t value) = 0;
    // console switch register, readable by the diagnostic
    virtual void set_switches(uint16_t value) = 0;

    // Result nullptr: unknown core name.
    // Caller owns the returned object.
    static testcore_c *create(const char *name);
    static const char *known_names(void);	// for the usage message
};

// implemented in testcore_cpu20.cpp / testcore_cpu34.cpp
testcore_c *testcore_create_cpu20(void);
testcore_c *testcore_create_cpu34(void);

#endif
