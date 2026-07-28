// Interface of 11/20 CPU emulator to UniBone
//
// This header needs cpu_core.h to be included first.

enum {
	KA11_STATE_HALTED = 0,
	KA11_STATE_RUNNING = 1,
	KA11_STATE_WAITING = 2
};


typedef struct KA11 KA11;
struct KA11
{
	word r[16];
	word b;		// B register before BUT JSRJMP
	word ba;	// bus address register of the current DATI/DATO
	word bdata;	// bus data register of the current DATI/DATO
	word ir;
	byte psw;
	int traps;
	int be;
	int state;

	// UniBone
	pthread_mutex_t mutex ;
	volatile bool external_intr ; // INTR by parallel thread pending
	volatile word external_intrvec;	// associated vector

	// cached unibone_trace_enabled(), refreshed once per instruction: the
	// per-cycle trace sites test only this flag when tracing is off
	bool tracing;

	word sw;
	int swab_vbit;
};


void ka11_tracestate(KA11 *cpu);
void ka11_printstate(KA11 *cpu);
void ka11_init(KA11 *cpu);	// once at creation, before other threads exist
void ka11_reset(KA11 *cpu);
void ka11_setintr(KA11 *cpu, unsigned vec);
void ka11_pwrfail_trap(KA11 *cpu);
void ka11_pwrup_vector_fetch(KA11 *cpu);
void ka11_condstep(KA11 *cpu);
