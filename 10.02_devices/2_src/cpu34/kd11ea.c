/* kd11ea.c: PDP-11/34 (KD11-EA) CPU emulation core

 Forked from cpu20/ka11.c, Angelo Papenhoff's KA11 (PDP-11/20), then extended
 with the KT11-D memory management (cpu34/kt11d.c) and the kernel/user
 processor modes which come with it.

 All symbols are either static or prefixed kd11ea_/KD11EA: this core is linked
 into the same binary as the KA11 core of the 11/20.
 */

#include "cpu_core.h"
#include "kt11d.h"
#include "kd11ea.h"
#include <stdlib.h>

#include "cpu_debug_pins.h" // ARM_DEBUG_PIN*, no-ops off the BBB

// unibone_*() declared in cpu_bus_adapter.h, included via cpu_core.h

/* EIS (MUL, DIV, ASH, ASHC, XOR, SOB) and MFPS/MTPS are native 11/34
   instructions and always executed. On the 11/20 they do not exist.
   SWAB is a CPU feature which is still a runtime parameter of cpu20_c. */
#define KD11EA_SWAB_VBIT	1	// SWAB clears the V bit

enum {
	PSW_PR = 0340,
	PSW_T = 020,
	PSW_N = 010,
	PSW_Z = 004,
	PSW_V = 002,
	PSW_C = 001,
	// bits actually implemented: <15:14> current mode, <13:12> previous mode,
	// <7:5> priority, <4> T, <3:0> NZVC. The KD11-EA has no second register
	// set, so PSW<11> and the unused <10:8> do not exist and always read zero.
	PSW_MASK = 0170377,
};

/* Assign the PSW.
 Besides the value itself this switches the stack pointer if the processor
 mode changed, tells the MMU which address space is current now, and tells the
 QUNIBUS arbitrator the new priority level.
 Instructions which only touch the condition codes assign cpu->psw directly:
 going through here on every opcode would call into the arbitrator each time.
 */
void
kd11ea_set_psw(KD11EA *cpu, word newpsw)
{
	unsigned oldsp = ((cpu->psw>>14)&3) == KT11D_MODE_KERNEL ?
			KD11EA_SP_KERNEL : KD11EA_SP_USER;
	unsigned newsp;

	// the bits which have no flipflop can never be loaded, whatever the
	// source: a write to 777776, MTPS, RTI/RTT or a trap vector.
	newpsw &= PSW_MASK;
	newsp = ((newpsw>>14)&3) == KT11D_MODE_KERNEL ?
			KD11EA_SP_KERNEL : KD11EA_SP_USER;

	if(oldsp != newsp){
		cpu->stackpointer[oldsp] = cpu->r[6];
		cpu->r[6] = cpu->stackpointer[newsp];
	}
	cpu->psw = newpsw;
	kt11d_set_modes(&cpu->mmu, newpsw);
	unibone_prioritylevelchange((newpsw>>5) & 7);
}

// is the CPU in kernel mode? Stack limit and the RTI/RTT restrictions need it.
#define IS_KERNEL(cpu)	((((cpu)->psw>>14)&3) == KT11D_MODE_KERNEL)




enum {
	TRAP_STACK = 1,
	TRAP_PWR = 2,
};

#define ISSET(f) ((cpu->psw&(f)) != 0)


static word
sgn(word w)
{
	return (w>>15)&1;
}

static word
sxt(byte b)
{
	return (word)(int8_t)b;
}

/* The 16 -> 18 bit address mapping lives in kt11d.h: kt11d_relocate() either
 relocates through the KT11-D page table, or - when MMR0<0> is clear - maps
 the top 8K of the virtual space to the IO page, exactly as the 11/20 does.
 */

// the KT11-D registers are internal to the CPU and not readable over the bus,
// so these dumps are the only way to see the state of the memory management.
#define KD11EA_STATE_FMT \
	" R0 %06o R1 %06o R2 %06o R3 %06o R4 %06o R5 %06o R6 %06o R7 %06o\n" \
	" 10 %06o 11 %06o 12 %06o 13 %06o 14 %06o 15 %06o 16 %06o 17 %06o\n" \
	" BA %06o IR %06o PSW %06o (%s mode, KSP %06o USP %06o)\n%s"
#define KD11EA_STATE_ARGS \
	cpu->r[0], cpu->r[1], cpu->r[2], cpu->r[3], \
	cpu->r[4], cpu->r[5], cpu->r[6], cpu->r[7], \
	cpu->r[8], cpu->r[9], cpu->r[10], cpu->r[11], \
	cpu->r[12], cpu->r[13], cpu->r[14], cpu->r[15], \
	cpu->ba, cpu->ir, cpu->psw, IS_KERNEL(cpu) ? "kernel" : "user", \
	IS_KERNEL(cpu) ? cpu->r[6] : cpu->stackpointer[KD11EA_SP_KERNEL], \
	IS_KERNEL(cpu) ? cpu->stackpointer[KD11EA_SP_USER] : cpu->r[6], \
	mmubuf

void
kd11ea_tracestate(KD11EA *cpu)
{
	char mmubuf[512];
	kt11d_format(&cpu->mmu, mmubuf, sizeof(mmubuf));
	trace(KD11EA_STATE_FMT, KD11EA_STATE_ARGS);
}

void
kd11ea_printstate(KD11EA *cpu)
{
	char mmubuf[512];
	kt11d_format(&cpu->mmu, mmubuf, sizeof(mmubuf));
	printf(KD11EA_STATE_FMT, KD11EA_STATE_ARGS);
}

// to be called once when the object holding the KD11EA is created, before any
// other thread can call kd11ea_setintr(). kd11ea_reset() must never touch the
// mutex again: it runs on every RESET opcode, and re-initializing a mutex the
// qunibusadapter thread holds in kd11ea_setintr() is undefined behavior.
void
kd11ea_init(KD11EA *cpu)
{
	pthread_mutex_init(&cpu->mutex, NULL);
}

// only to be called from kd11ea_condstep() thread.
// The devices on the bus are reset by unibone_bus_init(), which the RESET
// opcode pulses separately; there is no core-internal device list.
void
kd11ea_reset(KD11EA *cpu)
{
	cpu->traps = 0;
	pthread_mutex_lock(&cpu->mutex) ;
	cpu->external_intr = 0;
	pthread_mutex_unlock(&cpu->mutex) ;

	// INIT reaches MMR0..MMR2 of the KT11-D but not its address map: an OS
	// executing RESET keeps its PAR/PDR pairs, but relocation is switched off
	// and an abort freeze released. See kd11ea_power_reset() for the console
	// START / power-up case, which clears the map as well.
	kt11d_init(&cpu->mmu);
}

// console START and power-up: everything kd11ea_reset() does, plus the state
// which survives a RESET opcode - memory management, PSW and stack pointers.
// only to be called from kd11ea_condstep() thread
void
kd11ea_power_reset(KD11EA *cpu)
{
	kd11ea_reset(cpu);
	kt11d_reset(&cpu->mmu);	// clears MMR0, disabling relocation
	cpu->stackpointer[KD11EA_SP_KERNEL] = 0;
	cpu->stackpointer[KD11EA_SP_USER] = 0;
	cpu->trap_vector = 4;
	cpu->autoinc_reg = -1;
	kd11ea_set_psw(cpu, 0);	// kernel mode, priority 0
}

/* An autoincrement is not committed until the reference it computed the
 address for has been made: if that bus cycle times out or is aborted by the
 MMU, the KD11-EA leaves the register at the value it had before the
 instruction. An autodecrement is not backed out - it is part of forming the
 address and stands. MAINDEC DFKAB-D relies on both: it reads the first
 nonexistent address with `TSTB (R0)+`, records R0 in the trap 4 handler, and
 then expects `TSTB -(R0)` from one above it to abort with R0 left pointing at
 that same address.
 MMR1 is not touched here. It records what the instruction did to the
 registers and freezes on an abort, which is how the hardware presents it. */
static void
autoinc_pending(KD11EA *cpu, int r, int amount)
{
	cpu->autoinc_reg = r;
	cpu->autoinc_amount = amount;
}

static void
autoinc_commit(KD11EA *cpu)
{
	cpu->autoinc_reg = -1;
}

static void
autoinc_undo(KD11EA *cpu)
{
	if(cpu->autoinc_reg >= 0){
		cpu->r[cpu->autoinc_reg] -= cpu->autoinc_amount;
		cpu->autoinc_reg = -1;
	}
}

/* The CPU internal registers are decoded on the *physical* address: once
 relocation is on, the IO page is reached through a PAR and the virtual
 address says nothing about which register is meant.
 Decoded here: the PSW at 777776 (the 11/34 has MFPS/MTPS *as well as* the PSW
 address, not instead of it) and the KT11-D registers, which are internal to
 the KD11-EA and are therefore not published as QUNIBUS registers of cpu34_c. */
static int
dati(KD11EA *cpu, int b)
{
	// = 0: kt11d_relocate() sets pa on every success return, but that is
	// beyond what -Wmaybe-uninitialized can see at -O3
	uint32 pa = 0;
	unsigned int d;
	word w;

	if(!b && cpu->ba&1)
		goto be;

	if(kt11d_relocate(&cpu->mmu, cpu->ba, cpu->mmu.access_space, KT11D_READ, &pa))
		goto abort;

	/* internal registers */
	if(pa >= 0760000){
		if(kt11d_read_reg(&cpu->mmu, pa & ~1, &w)){
			cpu->bdata = w;
			goto ok;
		}
		switch(pa){
		case 0777570: case 0777571:
			cpu->bdata = cpu->sw;
			goto ok;
		case 0777776: case 0777777:
			// the odd byte reads PSW<15:8>: the caller shifts it down
			cpu->bdata = cpu->psw & PSW_MASK;
			goto ok;

		/* respond but don't return real data */
		case 0777547:
			cpu->bdata = 0;
			goto ok;
		}
	}

	if(!unibone_dati(pa&~1, &d))
		goto be;
	cpu->bdata = W(d);
ok:
	if(cpu->tracing && unibone_trace_addr(cpu->ba))
		trace("DATI [%06o] => %06o\n", cpu->ba, cpu->bdata);
	cpu->be = 0;
	autoinc_commit(cpu);
	return 0;
be:
	trace("DATI [%06o]: NXM\n", cpu->ba);
	cpu->trap_vector = 4;
	cpu->be++;
	autoinc_undo(cpu);
	return 1;
abort:
	cpu->trap_vector = KT11D_ABORT_VECTOR;
	cpu->be++;
	autoinc_undo(cpu);
	return 1;
}

static int
dato(KD11EA *cpu, int b)
{
	uint32 pa = 0;	// see dati()
	word mask;

	if(cpu->tracing && unibone_trace_addr(cpu->ba))
		trace("%s [%06o] <= %06o\n", b? "DATOB":"DATO", cpu->ba, cpu->bdata);
	if(!b && cpu->ba&1)
		goto be;

	if(kt11d_relocate(&cpu->mmu, cpu->ba, cpu->mmu.access_space, KT11D_WRITE, &pa))
		goto abort;

	/* internal registers */
	if(pa >= 0760000){
		// bdata already holds the byte in the half selected by the address
		mask = b ? (pa&1 ? 0177400 : 0377) : 0177777;
		if(kt11d_write_reg(&cpu->mmu, pa & ~1, cpu->bdata, mask))
			goto ok;
		switch(pa){
		case 0777570: case 0777571:
			/* can't write switches */
			goto ok;
		case 0777776: case 0777777:
			// Goes through kd11ea_set_psw(): a write may change the mode,
			// which switches the stack pointer and the MMU address space,
			// and PSW<7:5>, which the arbitrator has to be told about.
			// DATOB writes only the addressed half; mask has it already.
			// The non-existent bits are dropped by kd11ea_set_psw().
			// T is not writable this way, no more than by MTPS: only
			// RTI/RTT can set it.
			mask &= ~PSW_T;
			kd11ea_set_psw(cpu, (cpu->psw & ~mask) |
					(cpu->bdata & mask));
			goto ok;
		}
	}

	if(b){
		if(!unibone_datob(pa, cpu->bdata))
			goto be;
	}else{
		if(!unibone_dato(pa&~1, cpu->bdata))
			goto be;
	}
ok:
	cpu->be = 0;
	autoinc_commit(cpu);
	return 0;
be:
	cpu->trap_vector = 4;
	cpu->be++;
	autoinc_undo(cpu);
	return 1;
abort:
	cpu->trap_vector = KT11D_ABORT_VECTOR;
	cpu->be++;
	autoinc_undo(cpu);
	return 1;
}

/* Mark the references that follow as belonging to the destination operand.
 Only maintenance mode (MMR0<8>, see kt11d_relocate()) looks at this, but it
 has to be right for every instruction, so it is set where the operand is
 handled rather than at the call sites. Cleared again at the start of the next
 instruction and on entry to the trap sequence, which are the two places a
 reference is made that belongs to no operand at all. */
static void
set_dest_ref(KD11EA *cpu, int is_dest)
{
	cpu->mmu.dest_ref = is_dest;
}

static int
addrop(KD11EA *cpu, int m, int b)
{
	int r;
	int ai;
	unsigned dest;
	r = m&7;
	m >>= 3;
	ai = 1 + (!b || (r&6)==6 || m&1);
	if(m == 0){
		assert(0);
		return 0;
	}
	// a pending autoincrement of an earlier operand has long been committed
	autoinc_commit(cpu);
	/* The reads made here form the address - an index word out of the
	   instruction stream, the pointer word of a deferred mode - and are not
	   the reference to the operand itself, so maintenance mode does not
	   relocate them however the operand is used. Only the access which
	   follows, in fetchop() or writedest(), is the destination reference.
	   MAINDEC DFKTA-A pins both halves of that down: a `CMP #x, @#y` in
	   maintenance mode whose y must be read unrelocated for the destination
	   reference to reach y at all, and a `CMPB #x, @#y` which then expects the
	   *relocated* y. See set_dest_ref(); "dest" carries the operand's own kind
	   across the reads below. */
	dest = cpu->mmu.dest_ref;
	set_dest_ref(cpu, 0);
	switch(m&6){
	case 0:		// REG
		cpu->b = cpu->ba = cpu->r[r];
		set_dest_ref(cpu, dest);
		return 0;	// this already is mode 1
	case 2:		// INC
		cpu->ba = cpu->r[r];
		cpu->b = cpu->r[r] = cpu->r[r] + ai;
		// the next bus cycle is the one this addresses: it undoes the
		// increment if it aborts
		autoinc_pending(cpu, r, ai);
		// MMR1 records the change, so an abort handler can undo it
		kt11d_log_register(&cpu->mmu, r, ai);
		break;
	case 4:		// DEC
		cpu->b = cpu->ba = cpu->r[r]-ai;
		// stack limit applies to the kernel stack only
		if(r == 6 && IS_KERNEL(cpu) && (cpu->ba&~0377) == 0) cpu->traps |= TRAP_STACK;
		cpu->r[r] = cpu->ba;
		kt11d_log_register(&cpu->mmu, r, -ai);
		break;
	case 6:		// INDEX
		cpu->ba = cpu->r[7];
		cpu->r[7] += 2;
		if(dati(cpu, 0)) return 1;
		cpu->b = cpu->ba = cpu->bdata + cpu->r[r];
		break;
	}
	if(m&1){
		if(dati(cpu, 0)) return 1;
		cpu->b = cpu->ba = cpu->bdata;
	}
	set_dest_ref(cpu, dest);
	return 0;

}

static int
fetchop(KD11EA *cpu, int t, int m, int b)
{
	int r;
	r = m&7;
	if((m&070) == 0)
		cpu->r[t] = cpu->r[r];
	else{
		if(dati(cpu, b)) return 1;
		cpu->r[t] = cpu->bdata;
		if(b && cpu->ba&1) cpu->r[t] = cpu->r[t]>>8;
	}
	if(b) cpu->r[t] = sxt(cpu->r[t]);
	return 0;
}

static int
readop(KD11EA *cpu, int t, int m, int b)
{
	set_dest_ref(cpu, t == 011);
	return !(addrop(cpu, m, b) == 0 && fetchop(cpu, t, m, b) == 0);
}

/* the destination field of an instruction which has only one: JMP, JSR, SXT,
 MFPS, MFPI/MTPI. addrop() with what it addresses marked as the destination. */
static int
addrdest(KD11EA *cpu, int m, int b)
{
	set_dest_ref(cpu, 1);
	return addrop(cpu, m, b);
}

static int
writedest(KD11EA *cpu, word v, int b)
{
	int d;
	set_dest_ref(cpu, 1);
	if((cpu->ir & 070) == 0){
		d = cpu->ir & 7;
		if(b) SETMASK(cpu->r[d], v, 0377);
		else cpu->r[d] = v;
	}else{
		if(cpu->ba&1) v <<= 8;
		cpu->bdata = v;
		if(dato(cpu, b)) return 1;
	}
	return 0;
}

static void
setnz(KD11EA *cpu, word w)
{
	cpu->psw &= ~(PSW_N|PSW_Z);
	if(w & 0100000) cpu->psw |= PSW_N;
	if(w == 0) cpu->psw |= PSW_Z;
}

static void
step(KD11EA *cpu)
{
	uint by;
	uint br;
	uint b;
	uint c;
	uint src, dst, sf, df, sm, dm;
	word mask, sign;
	int inhov;
	int in_trap;	// the trap sequence is running, not an instruction
	word oldpsw;	// 16 bits since the KT11-D added the mode fields
	word trapped_pc, trapped_psw, newpc, newpsw;	// used by the trap sequence
	uint reg;
	int32_t prod;
	word sh;

#define SP	cpu->r[6]
#define PC	cpu->r[7]
#define SR	cpu->r[010]
#define DR	cpu->r[011]
#define TV	cpu->r[012]
#define BA	cpu->ba
#define PSW	cpu->psw

// Source strictly before destination: with `MOV R0,(R0)+` the source must be
// the value R0 had before the destination autoincremented it. A register
// operand goes through fetchop() alone, never readop(), so that addrop() does
// not overwrite cpu->ba - after the destination has been evaluated ba must
// still address it for writedest().
// Do not copy this into ka11.c: whether the destination's autoincrement or
// autodecrement is seen by a register source is a documented family difference
// (PDP-11 Architecture Handbook 1983, appendix B). The 11/34 is among the
// machines that do *not* modify the register first, the 11/20 among those that
// do, so the 11/20 core keeps evaluating the destination first.
#define RD_B	if(sm == 0) fetchop(cpu, 010, src, by);\
		else if(readop(cpu, 010, src, by)) goto be;\
		if(dm == 0) fetchop(cpu, 011, dst, by);\
		else if(readop(cpu, 011, dst, by)) goto be
#define RD_U	if(dm == 0) fetchop(cpu, 011, dst, by);\
		else if(readop(cpu, 011, dst, by)) goto be;\
		SR = DR
#define WR	if(writedest(cpu, b, by)) goto be
#define NZ	setnz(cpu, b)
#define SVC	goto service
#define TRAP(v)	TV = v; goto trap
#define CLC	cpu->psw &= ~PSW_C
#define CLV	cpu->psw &= ~PSW_V
#define CLCV	cpu->psw &= ~(PSW_V|PSW_C)
#define SEV	cpu->psw |= PSW_V
#define SEC	cpu->psw |= PSW_C
#define C	if(b & 0200000) SEC
#define NC	if((b & 0200000) == 0) SEC
#define CLNZ	cpu->psw &= ~(PSW_N|PSW_Z)
#define SEN	cpu->psw |= PSW_N
#define SEZ	cpu->psw |= PSW_Z
#define BXT	if(by) b = sxt(b)
#define BR	PC += br
#define CBR(c)	if(((c)>>(cpu->psw&017)) & 1) BR
// a push is the machine using the stack (JSR, MFPI, the trap sequence), never
// an operand of the instruction - a destination which happens to be -(SP) goes
// through addrop()/writedest() like any other. So it ends a destination
// reference, which matters in maintenance mode.
#define PUSH	SP -= 2; set_dest_ref(cpu, 0); if(!inhov && IS_KERNEL(cpu) && (SP&~0377) == 0) cpu->traps |= TRAP_STACK
// A pop is an autoincrement of SP like any other, and is undone the same way if
// the read it computed the address for aborts - see autoinc_pending(). Every
// POP here is immediately followed by the read, which either commits it or
// backs it out. MAINDEC DFKTF-A relies on it: it RTIs into a user stack on a
// non-resident page and, after the abort, expects the user SP unchanged.
#define POP	SP += 2; autoinc_pending(cpu, 6, 2)
// force the address space of the next dati()/dato(): trap vector fetches and
// the trap pushes are always kernel references, MFPI/MTPI use the previous mode
#define SPACE_KERNEL	kt11d_set_access(&cpu->mmu, KT11D_SPACE_KERNEL, KT11D_MODE_KERNEL)
#define SPACE_PREV	kt11d_set_access(&cpu->mmu, cpu->mmu.prev_space, cpu->mmu.prev_mode)
#define SPACE_RESTORE	kt11d_set_access(&cpu->mmu, cpu->mmu.space, cpu->mmu.mode)
#define OUT(a,d)	cpu->ba = (a); cpu->bdata = (d); if(dato(cpu, 0)) goto be
#define IN(d)	if(dati(cpu, 0)) goto be; d = cpu->bdata
#define INA(a,d)	cpu->ba = a; if(dati(cpu, 0)) goto be; d = cpu->bdata
#define TR(m)	if (cpu->tracing && unibone_trace_addr(PC-2)) trace("EXEC [%06o] "#m"\n", PC-2)
#define TRB(m)	if (cpu->tracing && unibone_trace_addr(PC-2)) trace("EXEC [%06o] "#m"%s\n", PC-2, by ? "B" : "")

	inhov = 0;
	in_trap = 0;

	// external interrupt from parallel threads? The unlocked read is the
	// fast path: the flag is only ever raised by the other thread, so a
	// stale 0 read here just takes the interrupt one instruction later -
	// no different from the interrupt arriving a moment later. A raised
	// flag is confirmed under the mutex before it is consumed.
	if (cpu->external_intr) {
		pthread_mutex_lock(&cpu->mutex) ;
		bool external_intr = cpu->external_intr ;
		word external_intrvec = cpu->external_intrvec ;
		cpu->external_intr = 0 ;
		pthread_mutex_unlock(&cpu->mutex) ;
		if (external_intr){
			cpu->state = KD11EA_STATE_RUNNING ;
			TRAP(external_intrvec);
		}
	}

	oldpsw = PSW;
	// MMR1 starts empty; MMR2 latches the address of this instruction only once
	// the fetch below has succeeded. Both stay frozen while MMR0 holds an abort.
	kt11d_instruction_start(&cpu->mmu);
	// the opcode fetch belongs to no operand
	set_dest_ref(cpu, 0);
	// whatever the last instruction autoincremented is committed: an abort
	// of this fetch may not undo it
	autoinc_commit(cpu);
	INA(PC, cpu->ir);
	kt11d_instruction_fetched(&cpu->mmu, PC);
	PC += 2;	/* don't increment on bus error! */
	by = !!(cpu->ir&B15);
	br = sxt(cpu->ir)<<1;
	src = cpu->ir>>6 & 077;
	sf = src & 7;
	sm = src>>3 & 7;
	dst = cpu->ir & 077;
	df = dst & 7;
	dm = dst>>3 & 7;
	if(by)	mask = M8, sign = B7;
	else	mask = M16, sign = B15;

	/* Binary */
	switch(cpu->ir & 0170000){
	case 0110000: case 0010000:	TRB(MOV);
		RD_B; CLV;
		b = SR; NZ;
		if(dm==0) cpu->r[df] = SR;
		// a bus timeout or an MMU abort on the destination traps, like
		// it does for every other instruction which writes memory
		else if(writedest(cpu, SR, by)) goto be;
		SVC;
	case 0120000: case 0020000:	TRB(CMP);
		RD_B; CLCV;
		b = SR + W(~DR) + 1; NC; BXT;
		if(sgn((SR ^ DR) & ~(DR ^ b))) SEV;
		NZ; SVC;
	case 0130000: case 0030000:	TRB(BIT);
		RD_B; CLV;
		b = DR & SR;
		NZ; SVC;
	case 0140000: case 0040000:	TRB(BIC);
		RD_B; CLV;
		b = DR & ~SR;
		NZ; WR; SVC;
	case 0150000: case 0050000:	TRB(BIS);
		RD_B; CLV;
		b = DR | SR;
		NZ; WR; SVC;
	case 0060000:			TR(ADD);
		by = 0; RD_B; CLCV;
		b = SR + DR; C;
		if(sgn(~(SR ^ DR) & (DR ^ b))) SEV;
		NZ; WR; SVC;
	case 0160000:			TR(SUB);
		by = 0; RD_B; CLCV;
		b = DR + W(~SR) + 1; NC;
		if(sgn((SR ^ DR) & (DR ^ b))) SEV;
		NZ; WR; SVC;

	/* Reserved instructions */
	case 0170000:
        goto ri;

	case 0070000:
		reg = (cpu->ir >> 6) & 07;
        switch(cpu->ir & 0177000) {
              default:
	    		// no FP11-A here: the whole group is reserved. FKABD1 sweeps
	    		// it to check that every one of them traps, so this may not
	    		// print anything.
	    		trace("-- ext: %o\n", cpu->ir);
                goto ri;

			case 0070000:		TR(MUL);
				RD_U;
              	cpu->psw &= ~(PSW_N|PSW_Z|PSW_V|PSW_C);
              	{
              		int32_t v1 = (int16_t) DR;
              		int32_t v2 = (int16_t) cpu->r[reg];
              		prod = v1 * v2;
					if(prod < -32768 || prod > 32767) {
						SEC;
					}
					if(prod == 0)
						SEZ;
					if(prod & B31)
						SEN;

              		if(reg & 0x1) {
              			//-- Odd register: store only lower 16 bits
						cpu->r[reg] = (word) prod;
              		} else {
              			cpu->r[reg + 1] = prod & 0xffff;
              			cpu->r[reg] = (word) (prod >> 16);
              		}
              	}
				SVC;

			case 0071000:		TR(DIV);
				RD_U;
              	cpu->psw &= ~(PSW_N|PSW_Z|PSW_V|PSW_C);
				// An odd register does not trap on the hardware; the pair
				// select simply wraps, so both halves are the same register
				// (result "unpredictable"). Same convention as ASHC.
				{
					int32_t dv = (int16_t) DR;
					prod = (uint32_t) cpu->r[reg | 1] | ((uint32_t) cpu->r[reg] << 16);
					if(DR == 0) {
						SEC;
						SEV;
					} else if(prod == INT32_MIN && dv == -1) {
						// -2^31 / -1 is +2^31, which no 32-bit divide can
						// represent: on a target where long is 32 bits the
						// ldiv() below would itself be undefined. The
						// quotient overflows 16 bits, so take the V path
						// here; its low 16 bits are zero either way, hence Z.
						SEV;
						SEZ;
					} else {
						ldiv_t d = ldiv(prod, dv);
						if(d.quot < -32768 || d.quot > 32767) {
							SEV;
						} else {
							cpu->r[reg] = (word) d.quot;
							cpu->r[reg | 1] = (word) (d.rem);
						}
						if(sgn(d.quot))
							SEN;
						if(0 == (d.quot & 0xffff))
							SEZ;
					}
				}
				SVC;

			case 0072000:		TR(ASH);
                // ASH
				RD_U;
              	cpu->psw &= ~(PSW_N|PSW_Z|PSW_V);
				b = cpu->r[reg];
				sh = (DR & 0x3f);				// Extract 6 bits
				if(sh & 0x20) {					// -ve?
					// we shift right
					sh = 0x40 - sh;					// +ve shift, 1..62
               		mask = sgn(b) ? 0xffff : 0x0;	// The previous sign gets shifted in
                    if(sh >= 17) {
                        // Really shifted out completely.
                		b = mask;
                		if(mask)
                			SEC;
                		else
                			CLC;
                		NZ;
                	} else {
						if(b & (1 << (sh - 1)))
							SEC;
						else
							CLC;
						b >>= sh;
						mask <<= (16 - sh);
						b |= mask;					// Sign extend
						b &= 0xffff;
						NZ;
						if(b & B15)
							SEN;
                	}
                } else {
                	// we shift left
                	if(sh == 0) {
                		//-- Nothing -> only set Z and N flags
                		NZ;
                	} else if(sh >= 17) {
                		// The hardware shifts iteratively and sets V on a sign
                		// change at *any* step. Shifting >= 17 moves every bit
                		// of a nonzero operand through the sign position, so
                		// any nonzero value overflows - not just negative ones.
                		if(b & 0xffff)
                			SEV;
						b = 0;
						CLC;
						SEZ;
					} else {
						//-- Loop, to handle overflow correctly: overflow is part of the last step!
						while(sh-- > 0) {
							if(b & B15) {
								SEC;
							} else {
								CLC;
							}
							uint ob = b;
							b <<= 1;
							ob ^= b;
							if(ob & B15) {					// Sign changed?
								SEV;
							}
						}
						NZ;
					}
                }
                b &= 0xffff;
				cpu->r[reg] = b;
				SVC;

              case 0073000:		TR(ASHC);
				RD_U;
              	cpu->psw &= ~(PSW_N|PSW_Z|PSW_V);
              	{
              		uint32_t val = ((uint32_t) cpu->r[reg] << 16) | cpu->r[reg | 1];	// The bitwise OR is intentional!

					sh = (DR & 0x3f);					// Extract 6 bits
					if(sh & 0x20) {						// -ve?
						// we shift right
						sh = 0x40 - sh;					// +ve shift, 1..32
                		uint32_t msk = val & B31 ? 0xffffffffL : 0x0;
						if(sh >= 32) {
							// Shift count -32: everything shifted out, all
							// sign bits remain, C gets the sign bit. Kept
							// out of the shift expressions below - a 32-bit
							// shift by 32 is undefined in C.
							val = msk;
							if(msk)
								SEC;
							else
								CLC;
						} else {
							if(val & (1u << (sh - 1)))
								SEC;
							else
								CLC;
							val >>= sh;
							msk <<= (32 - sh);
							val |= msk;				// Sign extend
						}
						if(val == 0)
							SEZ;
						if(val & B31)
							SEN;
                	} else {
						while(sh-- > 0) {
							if(val & B31) {
								SEC;
							} else {
								CLC;
							}
							uint32_t ob = val;
							val <<= 1;
							ob ^= val;
							if(ob & B31) {					// Sign changed?
								SEV;
							}
						}
						if(val == 0)
							SEZ;
						if(val & B31)
							SEN;
                	}
					if(reg & 0x1) {
						cpu->r[reg] = (word) val;		// Truncated result
					} else {
						cpu->r[reg] = (word) (val >> 16);
						cpu->r[reg + 1] = (word) val;
					}
				}
				SVC;
                break;

              case 0074000:		TR(XOR);
              	RD_U;
              	cpu->psw &= ~(PSW_N|PSW_Z|PSW_V);
				b = cpu->r[reg];
				b = DR ^ b;
				if(sgn(b)) {
					SEN;
				}
				NZ;
				WR; SVC;

			case 0077000:		TR(SOB);
				b = --(cpu->r[reg]);		// decrement reg
				if(b != 0) {
					//-- Jump
					mask = (cpu->ir & 077) << 1;			// Get jump offset (*2)
					cpu->r[7] -= mask;						// Decrement by offset
				}
				SVC;
		}
        // All else: not an EIS opcode
       	goto ri;
	}
	//-- remaining here is ir=x0xxxx

	/* Unary */
	switch(cpu->ir & 0007700){
	case 0005000:	TRB(CLR);
		RD_U; CLCV;
		b = 0;
		NZ; WR; SVC;
	case 0005100:	TRB(COM);
		RD_U; CLV; SEC;
		b = W(~SR);
		NZ; WR; SVC;
	case 0005200:	TRB(INC);
		RD_U; CLV;
		b = W(SR+1); BXT;
		if(sgn(~SR&b)) SEV;
		NZ; WR; SVC;
	case 0005300:	TRB(DEC);
		RD_U; CLV;
		b = W(SR+~0); BXT;
		if(sgn(SR&~b)) SEV;
		NZ; WR; SVC;
	case 0005400:	TRB(NEG);
		RD_U; CLCV;
		b = W(~SR+1); BXT; if(b) SEC;
		if(sgn(b&SR)) SEV;
		NZ; WR; SVC;
	case 0005500:	TRB(ADC);
		RD_U; c = ISSET(PSW_C); CLCV;
		b = SR + c; C; BXT;
		if(sgn(~SR&b)) SEV;
		NZ; WR; SVC;
	case 0005600:	TRB(SBC);
		RD_U; c = !ISSET(PSW_C)-1; CLCV;
		b = W(SR+c); if(c && SR == 0) SEC; BXT;
		if(sgn(SR&~b)) SEV;
		NZ; WR; SVC;
	case 0005700:	TRB(TST);
		RD_U; CLCV;
		b = SR;
		NZ; SVC;

	case 0006000:	TRB(ROR);
		RD_U; c = ISSET(PSW_C); CLCV;
		b = (SR&mask) >> 1; if(c) b |= sign; if(SR & 1) SEC; BXT;
		NZ; if((PSW>>3^PSW)&1) SEV;
		WR; SVC;
	case 0006100:	TRB(ROL);
		RD_U; c = ISSET(PSW_C); CLCV;
		b = (SR<<1) & mask; if(c) b |= 1; if(SR & B15) SEC; BXT;
		NZ; if((PSW>>3^PSW)&1) SEV;
		WR; SVC;
	case 0006200:	TRB(ASR);
		RD_U; c = ISSET(PSW_C); CLCV;
		b = W(SR>>1) | SR&B15; if(SR & 1) SEC; BXT;
		NZ; if((PSW>>3^PSW)&1) SEV;
		WR; SVC;
	case 0006300:	TRB(ASL);
		RD_U; CLCV;
		b = W(SR<<1); if(SR & B15) SEC; BXT;
		NZ; if((PSW>>3^PSW)&1) SEV;
		WR; SVC;

	case 0006400:
		// Only the byte form 106400 is MTPS, 006400 is MARK
		if(!by){
			TR(MARK);
			// pop the NN argument words the caller pushed, return to the
			// address in R5 and restore R5 from the stack. PC already points
			// past the MARK. Condition codes are not affected.
			SP = PC + 2*(cpu->ir & 077);
			PC = cpu->r[5];
			BA = SP; POP; IN(cpu->r[5]);
			SVC;
		}
		TR(MTPS);
		RD_U;
		// changes PSW<7:5>, so the arbitrator has to be told: go through
		// kd11ea_set_psw(). The mode bits are not affected, and neither is
		// the T bit - MTPS cannot set it.
		b = DR & (0377 & ~PSW_T);
		// In user mode MTPS may not change the priority PSW<7:5> either -
		// same restriction as RTI/RTT below - or a user program could lock
		// out interrupts.
		if(!IS_KERNEL(cpu))
			b = (b & ~PSW_PR) | (PSW & PSW_PR);
		kd11ea_set_psw(cpu, (cpu->psw & (0xff00|PSW_T)) | b);
		SVC;

	/* MFPI/MTPI address in the current mode but transfer in the previous
	   mode. The 11/34 has no separate I and D space, so MFPD/MTPD (the
	   1065xx/1066xx byte-bit variants) are the same instructions. */
	case 0006500:	TR(MFPI);
		by = 0;
		if(dm == 0){
			// register mode: R6 means the previous mode's stack pointer
			if(df == 6 && cpu->mmu.prev_space != cpu->mmu.space)
				b = cpu->stackpointer[cpu->mmu.prev_space == KT11D_SPACE_KERNEL ?
						KD11EA_SP_KERNEL : KD11EA_SP_USER];
			else
				b = cpu->r[df];
		}else{
			if(addrdest(cpu, dst, 0)) goto be;
			SPACE_PREV;
			if(dati(cpu, 0)){ SPACE_RESTORE; goto be; }
			SPACE_RESTORE;
			b = cpu->bdata;
		}
		CLV; NZ;
		PUSH; OUT(SP, b);
		SVC;

	case 0006600:	TR(MTPI);
		by = 0;
		BA = SP; POP; IN(b);
		CLV; NZ;
		if(dm == 0){
			if(df == 6 && cpu->mmu.prev_space != cpu->mmu.space)
				cpu->stackpointer[cpu->mmu.prev_space == KT11D_SPACE_KERNEL ?
						KD11EA_SP_KERNEL : KD11EA_SP_USER] = b;
			else
				cpu->r[df] = b;
		}else{
			if(addrdest(cpu, dst, 0)) goto be;
			cpu->bdata = b;
			SPACE_PREV;
			if(dato(cpu, 0)){ SPACE_RESTORE; goto be; }
			SPACE_RESTORE;
		}
		SVC;

	case 0006700:
		// Only the byte form 106700 is MFPS, 006700 is SXT
		if(!by){
			TR(SXT);
			if(dm != 0)
				if(addrdest(cpu, dst, 0)) goto be;
			// -1 if N is set, 0 if it is clear. setnz() then leaves N as it
			// was and sets Z exactly when N is clear, which is the rule; C is
			// not affected.
			b = ISSET(PSW_N) ? 0177777 : 0;
			CLV; NZ;
			if(dm == 0)
				cpu->r[df] = b;
			else
				WR;
			SVC;
		}
		TR(MFPS);
		// mode 0 is the register itself: addrop() computes an address and
		// starts at mode 1, so it must not be called for it. A byte
		// instruction, so the autoincrement/decrement step is 1.
		if(dm != 0)
			if(addrdest(cpu, dst, 1)) goto be;
		// PS<7> is sign extended into a register destination - like MOVB,
		// and unlike the other byte ops, which leave the high half alone.
		b = sxt(cpu->psw & 0377);
		CLV; NZ;	// C is not affected
		if(dm == 0)
			cpu->r[df] = b;
		else
			WR;
		SVC;
	}

	switch(cpu->ir & 0107400){
	case 0004000:
	case 0004400:	TR(JSR);
		if(dm == 0) goto ill;
		if(addrdest(cpu, dst, 0)) goto be;
		DR = cpu->ba;
		PUSH; OUT(SP, cpu->r[sf]);
		cpu->r[sf] = PC; PC = DR;
		SVC;
	case 0104000:	TR(EMT); TRAP(030);
	case 0104400:	TR(TRAP); TRAP(034);
	}

	/* Branches */
    // ! 000 0!! !xx xxx xxx    (! = at least one is non-zero)
    if((cpu->ir & 074000) == 0 && (cpu->ir & 0103400) != 0) {
        switch(cpu->ir & 0103400){
        case 0000400:	TR(BR); BR; SVC;
        case 0001000:	TR(BNE); CBR(0x0F0F); SVC;
        case 0001400:	TR(BEQ); CBR(0xF0F0); SVC;
        case 0002000:	TR(BGE); CBR(0xCC33); SVC;
        case 0002400:	TR(BLT); CBR(0x33CC); SVC;
        case 0003000:	TR(BGT); CBR(0x0C03); SVC;
        case 0003400:	TR(BLE); CBR(0xF3FC); SVC;
        case 0100000:	TR(BPL); CBR(0x00FF); SVC;
        case 0100400:	TR(BMI); CBR(0xFF00); SVC;
        case 0101000:	TR(BHI); CBR(0x0505); SVC;
        case 0101400:	TR(BLOS); CBR(0xFAFA); SVC;
        case 0102000:	TR(BVC); CBR(0x3333); SVC;
        case 0102400:	TR(BVS); CBR(0xCCCC); SVC;
        case 0103000:	TR(BCC); CBR(0x5555); SVC;
        case 0103400:	TR(BCS); CBR(0xAAAA); SVC;
        }
    }

	/* Misc */
	switch(cpu->ir & 0777300){
	case 0100:	TR(JMP);
		if(dm == 0) goto ill;
		if(addrdest(cpu, dst, 0)) goto be;
		PC = cpu->ba;
		SVC;
	case 0200:
        switch(cpu->ir&070){
        case 000:	TR(RTS);
            BA = SP; POP;
            PC = cpu->r[df];
            IN(cpu->r[df]);
            SVC;
        case 010: case 020: case 030:
            goto ri;
        case 040: case 050:	TR(CCC); PSW &= ~(cpu->ir&017); SVC;
        case 060: case 070:	TR(SEC); PSW |= cpu->ir&017; SVC;
        // the mask leaves eight values and each case above transfers control,
        // so this is unreachable. It names the trap the encodings without an
        // instruction take, and keeps the group from running into SWAB.
        default:	goto ri;
        }
	case 0300:	TR(SWAB);
		RD_U;
		if(KD11EA_SWAB_VBIT) {
		    CLCV;   // v-bit cleared, ZQKC compatible
		} else {
		    CLC;    // v-bit unchanged, actual 11/20 behavior
		}
		b = WD(DR & 0377, (DR>>8) & 0377);
		CLNZ; if(b & B7) SEN; if((b & M8) == 0) SEZ;
		WR; SVC;
	}

	/* Operate */
	switch(cpu->ir){
	case 0:	TR(HALT);
		// HALT is kernel-only on the KD11-EA: executed in user mode it is a
		// reserved instruction and traps through vector 10, leaving the
		// machine running. Only the 11/20 halts whatever the mode - it has
		// no modes to begin with.
		if(!IS_KERNEL(cpu))
			goto ri;
		cpu->state = KD11EA_STATE_HALTED; return;
	case 1:	TR(WAIT); cpu->state = KD11EA_STATE_WAITING; return ; // no traps
	case 2:
	case 6:
		if(cpu->ir == 2){ TR(RTI); }else{ TR(RTT); }
		BA = SP; POP; IN(PC);
		BA = SP; POP; IN(b);
		// In user mode neither RTI nor RTT may change the mode fields or the
		// priority: those bits keep their current value.
		if(!IS_KERNEL(cpu))
			b = (b & ~(0170000|PSW_PR)) | (PSW & (0170000|PSW_PR));
		// RTI takes a pending trace trap right away, RTT defers it by one
		// instruction. service: tests oldpsw, which gives the RTT behaviour.
		if(cpu->ir == 2 && (b & PSW_T))
			oldpsw |= PSW_T;
		kd11ea_set_psw(cpu, b);
		SVC;
	case 3:	TR(BPT); TRAP(014);
	case 4:	TR(IOT); TRAP(020);
	case 5:	TR(RESET);
		// Like HALT a kernel-only instruction, but this one does not trap:
		// outside kernel mode RESET is simply a no-op, so a user program
		// cannot INIT the bus out from under the devices.
		if(IS_KERNEL(cpu)){
			kd11ea_reset(cpu);
			unibone_bus_init();
		}
		SVC;
	}

	// All other instructions should be reserved now

ri:	TRAP(010);
ill:	TRAP(4);
be:	if(cpu->be > 1){
		printf("double bus error, HALT\n");
		trace("double bus error, HALT");
		cpu->state = KD11EA_STATE_HALTED;
		return;
	}
	trace("bus error at %06o\n", cpu->ba);
	/* An instruction loads its condition codes when it completes, so one that
	   aborted never loads them: the PSW pushed by the trap below holds the
	   codes from before it. MAINDEC DFKTF-A checks this with `SEC` followed by
	   an `ADC` whose destination is on a read-only page, and expects C still
	   set in the pushed PSW - our ADC had already cleared it, computing 0+C.
	   The trap sequence is not an instruction: an abort inside one must leave
	   the handler's freshly loaded PSW alone. */
	if(!in_trap)
		PSW = (PSW & ~017) | (oldpsw & 017);
	// 4 for a bus timeout or an odd address, 0250 for an MMU abort
	TRAP(cpu->trap_vector);

trap:
	in_trap = 1;
	// the vector fetch and the pushes below belong to no operand
	set_dest_ref(cpu, 0);
	if (cpu->tracing && unibone_trace_addr(PC-2))
		trace("TRAP %o\n", TV);
	trapped_pc = PC;
	trapped_psw = PSW;
	/* The vector is read through kernel space and *before* the mode switch:
	   PSW<15:14> may still say "user" at this point. Then the new PSW is
	   installed - which switches the stack pointer - and only then are the
	   old PSW and PC pushed, onto the new mode's stack. */
	SPACE_KERNEL;
	BA = TV;	if(dati(cpu, 0)) goto be;	newpc = cpu->bdata;
	BA = TV+2;	if(dati(cpu, 0)) goto be;	newpsw = cpu->bdata;
	SPACE_RESTORE;
	// previous mode of the new PSW := the mode the trap came from
	newpsw = (newpsw & ~030000) | ((trapped_psw >> 2) & 030000);
	kd11ea_set_psw(cpu, newpsw);
	PUSH; OUT(SP, trapped_psw);
	PUSH; OUT(SP, trapped_pc);
	PC = newpc;
	/* no trace trap after a trap */
	oldpsw = PSW;

	if (cpu->tracing && unibone_trace_addr(PC-2))
		kd11ea_tracestate(cpu);
	/* The trap sequence ends at an instruction boundary: the processor
	   arbitrates again here, *before* the first instruction of the handler
	   runs. That matters for the kernel stack limit, whose violation is
	   detected by the pushes just made - the resulting trap through vector 4
	   has to be taken now, not after the handler has had an instruction to
	   change vector 4 or the stack (MAINDEC DFKAB-D checks for exactly that:
	   its handler expects SP two words further down on entry). */
	SVC;

service:
	if(oldpsw & PSW_T){
		oldpsw &= ~PSW_T;
		TRAP(014);
	}else if(cpu->traps & TRAP_STACK){
		cpu->traps &= ~TRAP_STACK;
		inhov = 1;
		TRAP(4);
	}else if(cpu->traps & TRAP_PWR){
		cpu->traps &= ~TRAP_PWR;
		TRAP(024);
	}else
	// TODO? console stop
		/* fetch next instruction */
		return;
}

// to be called from parallel threads to signal async intr
// (unibusadapter worker thread)
void
kd11ea_setintr(KD11EA *cpu, unsigned vec)
{
	pthread_mutex_lock(&cpu->mutex) ;
	cpu->external_intr = true;
	cpu->external_intrvec = vec;
	trace("INTR vec=%03o\n", vec) ;
	pthread_mutex_unlock(&cpu->mutex) ;
}

// only to be called from kd11ea_condstep() thread

void
kd11ea_pwrfail_trap(KD11EA *cpu)
{
	cpu->traps |= TRAP_PWR;
}

// only to be called from kd11ea_condstep() thread
// if locked, will lock DATI and unibus adapter()!
void
kd11ea_pwrup_vector_fetch(KD11EA *cpu)
{
	// caller must have issued reset()
	// cpu->traps &= ~TRAP_PWR; // no, would be a fix
	cpu->tracing = unibone_trace_enabled();
	INA(024, PC);
	BA = 024+2; if(dati(cpu, 0)) goto be;
	// through kd11ea_set_psw(): the vector may select user mode, which picks
	// the other stack pointer and address space
	kd11ea_set_psw(cpu, cpu->bdata);
	return ;
be:
	trace("BE\n");
	cpu->be++ ;
}

void
kd11ea_condstep(KD11EA *cpu)
{
	if(cpu->state == KD11EA_STATE_RUNNING || cpu->state == KD11EA_STATE_WAITING)
		// GRANT Interrupts before opcode fetch, or when CPU is on WAIT
		unibone_grant_interrupts() ;

	if((cpu->state == KD11EA_STATE_RUNNING) ||
	   (cpu->state == KD11EA_STATE_WAITING && cpu->traps)
	   || (cpu->state == KD11EA_STATE_WAITING && cpu->external_intr) ){
		cpu->state = KD11EA_STATE_RUNNING;
		// external_intr WAIT handled atomically in kd11ea_setintr() !

		// is trace() output live at all? Cached once per instruction so
		// the trace sites cost a flag test each when it is not.
		cpu->tracing = unibone_trace_enabled();
		step(cpu);
	}
}

// ka11.c has a run() here, the driver of the standalone emulator.
// QUniBone steps the CPU from cpu_base_c::worker() instead.
