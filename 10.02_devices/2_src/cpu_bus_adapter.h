/* cpu_bus_adapter.h: contract between a CPU emulation core and the QUNIBUS adapter

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


 A CPU emulation core (cpu20/ka11.c, cpu34/kd11ea.c, ...) is plain C code which
 knows nothing about QUNIBUS, devices or C++. Everything it needs from the ARM
 side is reached through the unibone_*() functions declared here.

 They are implemented once, in cpu.cpp, on top of the currently installed
 cpu_base_c. Only one CPU may be installed at a time, so a single set of
 functions serves every core.

 Keep this header plain, portable C: the cores are compiled as C++ (see the
 makefiles), but are written in C.
 */
#ifndef _CPU_BUS_ADAPTER_H_
#define _CPU_BUS_ADAPTER_H_

#include <stdint.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

// logging: route the cores "trace()" to unibone_cpu->logger
void unibone_log(unsigned msglevel, const char *srcfilename, unsigned srcline, const char *fmt, ...);
void unibone_logdump(void);
#define LL_DEBUG 5 // see logger.hpp
#define trace(...) unibone_log(LL_DEBUG, __FILE__, __LINE__, __VA_ARGS__)

// QUNIBUS data transfers by the CPU as bus master.
// Result: 1 = OK, 0 = bus timeout (non existing memory)
int unibone_dati(unsigned addr, unsigned *data);
int unibone_dato(unsigned addr, unsigned data);
int unibone_datob(unsigned addr, unsigned data);

// called before opcode fetch: let the PRU GRANT pending device requests
void unibone_grant_interrupts(void);

// CPU changed its priority level (PSW<7:5>)
void unibone_prioritylevelchange(uint8_t level);

// CPU executed a RESET opcode: pulse the INIT line
void unibone_bus_init(void);

// selective tracing of EXEC cycles.
// unibone_trace_enabled(): is trace() output going anywhere at all? The cores
// cache the answer in their cpu->tracing flag once per instruction, so an
// inactive tracer costs a flag test per trace site, not a call.
// unibone_trace_addr(): address filter, consulted only when tracing is live.
bool unibone_trace_enabled(void);
bool unibone_trace_addr(uint16_t a);

#ifdef __cplusplus
}
#endif

#endif
