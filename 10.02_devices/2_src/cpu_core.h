/* cpu_core.h: basic types and macros shared by the CPU emulation cores

 From Angelo Papenhoff's standalone aap emulator (11.h there), trimmed to what
 the cores use. One copy for all cores: cpu20/ka11.c, cpu34/kd11ea.c and
 cpu34/kt11d.c include it, as do their C++ wrappers cpu20.cpp/cpu34.cpp and
 the test harness in 10.05_cputest. It replaces the former per-core copies
 cpu20/11.h and cpu34/11.h, which could never meet in one compilation unit.

 The interface to the ARM side is cpu_bus_adapter.h, included here. Keep this
 header plain, portable C: the cores are written in C.
 */
#ifndef _CPU_CORE_H_
#define _CPU_CORE_H_

#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <pthread.h>	// the core structs embed the interrupt mutex

// unibone_*() interface to the QUNIBUS adapter, plus trace()
#include "cpu_bus_adapter.h"

typedef uint8_t uint8, byte;
typedef uint16_t uint16, word;
typedef uint32_t uint32;
// same as the typedef glibc's <sys/types.h> may already have made: the old
// 11.h pulled that in transitively, the cores use the name in step()
typedef unsigned int uint;

#define WD(hi, lo) W((hi)<<8 | (lo))
#define W(w) ((word)(w))
#define M8  0377
#define M16 0177777
#define B7  0000200
#define B15 0100000
#define B31 0x80000000L
#define nil NULL

#define SETMASK(l, r, m) l = (((l)&~(m)) | ((r)&(m)))

#endif
