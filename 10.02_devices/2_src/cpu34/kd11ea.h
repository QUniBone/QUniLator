/* kd11ea.h: interface of the PDP-11/34 (KD11-EA) CPU emulator to QUniBone

 Forked from cpu20/ka11.h (Angelo Papenhoff's KA11), then extended with the
 KT11-D memory management and the kernel/user processor modes which come with
 it. The MMU itself lives in cpu34/kt11d.c.

 Every externally visible symbol is prefixed kd11ea_/KD11EA, because this core
 is linked into the same binary as the KA11 core of the 11/20.

 The change log of this directory is cpu34/CHANGES.md.

 This header needs cpu_core.h and cpu34/kt11d.h to be included first.
 */
#ifndef _KD11EA_H_
#define _KD11EA_H_

#include "kt11d.h"	// KT11D, the memory management state

enum {
	KD11EA_STATE_HALTED = 0,
	KD11EA_STATE_RUNNING = 1,
	KD11EA_STATE_WAITING = 2
};

// index into KD11EA.stackpointer[]
enum {
	KD11EA_SP_KERNEL = 0,
	KD11EA_SP_USER = 1
};


typedef struct KD11EA KD11EA;
struct KD11EA
{
	word r[16];
	word b;		// B register before BUT JSRJMP
	word ba;	// bus address register of the current DATI/DATO
	word bdata;	// bus data register of the current DATI/DATO
	word ir;
	// PSW<15:14> current mode, <13:12> previous mode, <7:5> priority,
	// <4> T, <3:0> NZVC. Never assign it directly: kd11ea_set_psw() also
	// switches the stack pointer and tells the MMU and the arbitrator.
	word psw;
	// the stack pointer of the mode which is *not* current. R6 holds the
	// active one. Indexed by KD11EA_SP_KERNEL / KD11EA_SP_USER.
	word stackpointer[2];
	int traps;
	int be;
	int state;
	// vector of the trap the "be" path has to take: 4 for a bus timeout,
	// 0250 for a memory management abort.
	word trap_vector;
	// an autoincrement done by addrop() whose reference has not been made
	// yet: it is only committed once that bus cycle completes. Register
	// number, or -1 for none, and the amount added.
	int autoinc_reg;
	int autoinc_amount;

	KT11D mmu;

	// UniBone
	pthread_mutex_t mutex ;
	volatile bool external_intr ; // INTR by parallel thread pending
	volatile word external_intrvec;	// associated vector

	// cached unibone_trace_enabled(), refreshed once per instruction: the
	// per-cycle trace sites test only this flag when tracing is off
	bool tracing;

	word sw;
};


void kd11ea_set_psw(KD11EA *cpu, word newpsw);
void kd11ea_tracestate(KD11EA *cpu);
void kd11ea_printstate(KD11EA *cpu);
void kd11ea_init(KD11EA *cpu);		// once at creation, before other threads exist
void kd11ea_reset(KD11EA *cpu);		// RESET opcode: does not touch the KT11-D
void kd11ea_power_reset(KD11EA *cpu);	// console START / power-up: clears all
void kd11ea_setintr(KD11EA *cpu, unsigned vec);
void kd11ea_pwrfail_trap(KD11EA *cpu);
void kd11ea_pwrup_vector_fetch(KD11EA *cpu);
void kd11ea_condstep(KD11EA *cpu);

#endif
