/* testcore_cpu34.cpp: testcore_c on top of the KD11-EA (PDP-11/34) core

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

 Mirrors cpu34.cpp, minus everything that needs a QUNIBUS.

 A separate compilation unit from testcore_cpu20.cpp, mirroring the
 cpu20.cpp/cpu34.cpp split on the application side.
 */

#include <stdlib.h>
#include <string.h>

#include "testcore.hpp"

#include "cpu_core.h"
#include "cpu34/kt11d.h"
#include "cpu34/kd11ea.h"

class testcore_cpu34_c: public testcore_c {
private:
    struct KD11EA kd11ea;
public:
    testcore_cpu34_c()
    {
        memset(&kd11ea, 0, sizeof(kd11ea));
        kd11ea_init(&kd11ea);
    }

    const char *name(void) override
    {
        return "cpu34";
    }

    void power_reset(void) override
    {
        // console START / power-up, not the RESET opcode: also clears the KT11-D
        kd11ea_power_reset(&kd11ea);
    }

    void condstep(void) override
    {
        kd11ea_condstep(&kd11ea);
    }

    void printstate(void) override
    {
        kd11ea_printstate(&kd11ea);
    }

    void setintr(uint16_t vector) override
    {
        kd11ea_setintr(&kd11ea, vector);
    }

    state_e get_state(void) override
    {
        return (state_e) kd11ea.state;
    }

    void set_state(state_e state) override
    {
        kd11ea.state = (int) state;
    }

    uint16_t get_pc(void) override
    {
        return kd11ea.r[7];
    }

    void set_pc(uint16_t value) override
    {
        kd11ea.r[7] = value;
    }

    void set_switches(uint16_t value) override
    {
        kd11ea.sw = value;
    }
};

testcore_c *testcore_create_cpu34(void)
{
    return new testcore_cpu34_c();
}
