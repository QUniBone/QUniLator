/* testcore_cpu20.cpp: testcore_c on top of the KA11 (PDP-11/20) core

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

 Mirrors cpu20.cpp, minus everything that needs a QUNIBUS.
 */

#include <stdlib.h>
#include <string.h>

#include "testcore.hpp"

#include "cpu_core.h"
#include "cpu20/ka11.h"

class testcore_cpu20_c: public testcore_c {
private:
    struct KA11 ka11;
public:
    testcore_cpu20_c()
    {
        memset(&ka11, 0, sizeof(ka11));
        ka11_init(&ka11);
        // The 11/20 SWAB leaves V unchanged; cpu20_c defaults swab_vbit to
        // false too, and the ZKA* diagnostics expect the real 11/20 behaviour.
        ka11.swab_vbit = 0;
    }

    const char *name(void) override
    {
        return "cpu20";
    }

    void power_reset(void) override
    {
        ka11_reset(&ka11);
    }

    void condstep(void) override
    {
        ka11_condstep(&ka11);
    }

    void printstate(void) override
    {
        ka11_printstate(&ka11);
    }

    void setintr(uint16_t vector) override
    {
        ka11_setintr(&ka11, vector);
    }

    state_e get_state(void) override
    {
        return (state_e) ka11.state;
    }

    void set_state(state_e state) override
    {
        ka11.state = (int) state;
    }

    uint16_t get_pc(void) override
    {
        return ka11.r[7];
    }

    void set_pc(uint16_t value) override
    {
        ka11.r[7] = value;
    }

    void set_switches(uint16_t value) override
    {
        ka11.sw = value;
    }
};

testcore_c *testcore_create_cpu20(void)
{
    return new testcore_cpu20_c();
}
