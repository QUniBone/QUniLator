/* cpu_debug_pins.h: optional ARM debug pins for the CPU emulation cores

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


 The emulation cores (cpu20/ka11.c, cpu34/kd11ea.c) used to include gpios.hpp
 directly, to have ARM_DEBUG_PIN*() available for timing measurements on a
 scope. That was their only dependency on the ARM side of QUniBone, and it kept
 them from being compiled anywhere else - in particular by the host compiler of
 the CPU test suite in 10.05_cputest, which drives the cores against the MAINDEC
 diagnostics with no BeagleBone in sight.

 So the include moved here, behind ARM: that symbol is defined by OS_CCDEFS in
 makefile_u/makefile_q for both the on-BBB and the cross build, and by nothing
 else. An ARM build therefore gets exactly what it got before; a host build gets
 debug pins which compile away to nothing.

 <pthread.h> is unconditional: KA11 and KD11EA carry a pthread_mutex_t for the
 INTR handoff from the qunibusadapter thread, and used to get that declaration
 through gpios.hpp by accident.

 Keep this header plain, portable C, like cpu_bus_adapter.h next to it.
 */
#ifndef _CPU_DEBUG_PINS_H_
#define _CPU_DEBUG_PINS_H_

#include <pthread.h>

#ifdef ARM

#include "gpios.hpp"	// the real ARM_DEBUG_PIN*(), driving the BBB LED pins

#else

// Host build (CPU test suite): no GPIOs. Keep the call sites compilable.
#define ARM_DEBUG_PIN0(val)		((void)0)
#define ARM_DEBUG_PIN1(val)		((void)0)
#define ARM_DEBUG_PIN2(val)		((void)0)
#define ARM_DEBUG_PIN3(val)		((void)0)
#define ARM_DEBUG_PIN(n,val)	((void)0)

#endif

#endif
