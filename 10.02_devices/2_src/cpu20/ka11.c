/* ka11.c: PDP-11/20 (KA11) CPU emulation core, Angelo Papenhoff's KA11.

 All symbols are either static or prefixed ka11_/KA11: this core is linked
 into the same binary as the KD11-EA core of the 11/34 (cpu34/kd11ea.c).
 */

#include "cpu_core.h"
#include "ka11.h"

#include "cpu_debug_pins.h" // ARM_DEBUG_PIN*, no-ops off the BBB

// unibone_*() declared in cpu_bus_adapter.h, included via cpu_core.h

static void
levelchange(word psw)
{
	unibone_prioritylevelchange(psw>>5);
}




enum {
	PSW_PR = 0340,
	PSW_T = 020,
	PSW_N = 010,
	PSW_Z = 004,
	PSW_V = 002,
	PSW_C = 001,
};

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

static uint32
ubxt(word a)
{
	return (a&0160000)==0160000 ? a|0600000 : a;
}

void
ka11_tracestate(KA11 *cpu)
{
	(void)cpu;
	trace(" R0 %06o R1 %06o R2 %06o R3 %06o R4 %06o R5 %06o R6 %06o R7 %06o\n"
		" 10 %06o 11 %06o 12 %06o 13 %06o 14 %06o 15 %06o 16 %06o 17 %06o\n"
		" BA %06o IR %06o PSW %03o\n"
		,
		cpu->r[0], cpu->r[1], cpu->r[2], cpu->r[3],
		cpu->r[4], cpu->r[5], cpu->r[6], cpu->r[7],
		cpu->r[8], cpu->r[9], cpu->r[10], cpu->r[11],
		cpu->r[12], cpu->r[13], cpu->r[14], cpu->r[15],
		cpu->ba, cpu->ir, cpu->psw);
}

void
ka11_printstate(KA11 *cpu)
{
	(void)cpu;
	printf(" R0 %06o R1 %06o R2 %06o R3 %06o R4 %06o R5 %06o R6 %06o R7 %06o\n"
		" 10 %06o 11 %06o 12 %06o 13 %06o 14 %06o 15 %06o 16 %06o 17 %06o\n"
		" BA %06o IR %06o PSW %03o\n"
		,
		cpu->r[0], cpu->r[1], cpu->r[2], cpu->r[3],
		cpu->r[4], cpu->r[5], cpu->r[6], cpu->r[7],
		cpu->r[8], cpu->r[9], cpu->r[10], cpu->r[11],
		cpu->r[12], cpu->r[13], cpu->r[14], cpu->r[15],
		cpu->ba, cpu->ir, cpu->psw);
}

// to be called once when the object holding the KA11 is created, before any
// other thread can call ka11_setintr(). ka11_reset() must never touch the
// mutex again: it runs on every RESET opcode, and re-initializing a mutex the
// qunibusadapter thread holds in ka11_setintr() is undefined behavior.
void
ka11_init(KA11 *cpu)
{
	pthread_mutex_init(&cpu->mutex, NULL);
}

// only to be called from ka11_condstep() thread.
// The devices on the bus are reset by unibone_bus_init(), which the RESET
// opcode pulses separately; there is no core-internal device list.
void
ka11_reset(KA11 *cpu)
{
	cpu->traps = 0;
	pthread_mutex_lock(&cpu->mutex) ;
	cpu->external_intr = 0;
	pthread_mutex_unlock(&cpu->mutex) ;
}

static int
dati(KA11 *cpu, int b)
{
	unsigned int d;

	if(!b && cpu->ba&1)
		goto be;

	/* internal registers */
	if((cpu->ba&0177400) == 0177400){
		switch(cpu->ba&0377){
		case 0170: case 0171:
			cpu->bdata = cpu->sw;
			goto ok;
		case 0376:
			cpu->bdata = cpu->psw;
			goto ok;
		case 0377:
			goto be;

		/* respond but don't return real data */
		case 0147:
			cpu->bdata = 0;
			goto ok;
		}
	}

	if(!unibone_dati(ubxt(cpu->ba)&~1, &d))
		goto be;
	cpu->bdata = W(d);
ok:
	if(cpu->tracing && unibone_trace_addr(cpu->ba))
		trace("DATI [%06o] => %06o\n", cpu->ba, cpu->bdata);
	cpu->be = 0;
	return 0;
be:
	trace("DATI [%06o]: NXM\n", cpu->ba);
	cpu->be++;
	return 1;
}

static int
dato(KA11 *cpu, int b)
{
	if(cpu->tracing && unibone_trace_addr(cpu->ba))
		trace("%s [%06o] <= %06o\n", b? "DATOB":"DATO", cpu->ba, cpu->bdata);
	if(!b && cpu->ba&1)
		goto be;

	/* internal registers */
	if((cpu->ba&0177400) == 0177400){
		switch(cpu->ba&0377){
		case 0170: case 0171:
			/* can't write switches */
			goto ok;
		case 0376:
			/* writes 0 for the odd byte.
			   I think this is correct. */
			cpu->psw = cpu->bdata;
			levelchange(cpu->psw);
			goto ok;
		case 0377:
			goto be;
		}
	}

	if(b){
		if(!unibone_datob(ubxt(cpu->ba), cpu->bdata))
			goto be;
	}else{
		if(!unibone_dato(ubxt(cpu->ba)&~1, cpu->bdata))
			goto be;
	}
ok:
	cpu->be = 0;
	return 0;
be:
	cpu->be++;
	return 1;
}

static int
addrop(KA11 *cpu, int m, int b)
{
	int r;
	int ai;
	r = m&7;
	m >>= 3;
	ai = 1 + (!b || (r&6)==6 || m&1);
	if(m == 0){
		assert(0);
		return 0;
	}
	switch(m&6){
	case 0:		// REG
		cpu->b = cpu->ba = cpu->r[r];
		return 0;	// this already is mode 1
	case 2:		// INC
		cpu->ba = cpu->r[r];
		cpu->b = cpu->r[r] = cpu->r[r] + ai;
		break;
	case 4:		// DEC
		cpu->b = cpu->ba = cpu->r[r]-ai;
		if(r == 6 && (cpu->ba&~0377) == 0) cpu->traps |= TRAP_STACK;
		cpu->r[r] = cpu->ba;
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
	return 0;

}

static int
fetchop(KA11 *cpu, int t, int m, int b)
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
readop(KA11 *cpu, int t, int m, int b)
{
	return !(addrop(cpu, m, b) == 0 && fetchop(cpu, t, m, b) == 0);
}

static int
writedest(KA11 *cpu, word v, int b)
{
	int d;
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
setnz(KA11 *cpu, word w)
{
	cpu->psw &= ~(PSW_N|PSW_Z);
	if(w & 0100000) cpu->psw |= PSW_N;
	if(w == 0) cpu->psw |= PSW_Z;
}

static void
step(KA11 *cpu)
{
	uint by;
	uint br;
	uint b;
	uint c;
	uint src, dst, sf, df, sm, dm;
	word mask, sign;
	int inhov;
	byte oldpsw;

#define SP	cpu->r[6]
#define PC	cpu->r[7]
#define SR	cpu->r[010]
#define DR	cpu->r[011]
#define TV	cpu->r[012]
#define BA	cpu->ba
#define PSW	cpu->psw

#define RD_B	if(sm != 0) if(readop(cpu, 010, src, by)) goto be;\
		if(dm != 0) if(readop(cpu, 011, dst, by)) goto be;\
		if(sm == 0) fetchop(cpu, 010, src, by);\
		if(dm == 0) fetchop(cpu, 011, dst, by)
#define RD_U	if(dm != 0) if(readop(cpu, 011, dst, by)) goto be;\
		if(dm == 0) fetchop(cpu, 011, dst, by);\
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
#define PUSH	SP -= 2; if(!inhov && (SP&~0377) == 0) cpu->traps |= TRAP_STACK
#define POP	SP += 2
#define OUT(a,d)	cpu->ba = (a); cpu->bdata = (d); if(dato(cpu, 0)) goto be
#define IN(d)	if(dati(cpu, 0)) goto be; d = cpu->bdata
#define INA(a,d)	cpu->ba = a; if(dati(cpu, 0)) goto be; d = cpu->bdata
#define TR(m)	if (cpu->tracing && unibone_trace_addr(PC-2)) trace("EXEC [%06o] "#m"\n", PC-2)
#define TRB(m)	if (cpu->tracing && unibone_trace_addr(PC-2)) trace("EXEC [%06o] "#m"%s\n", PC-2, by ? "B" : "")

	inhov = 0;

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
			cpu->state = KA11_STATE_RUNNING ;
			TRAP(external_intrvec);
		}
	}

	oldpsw = PSW;
	INA(PC, cpu->ir);
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
		// EIS (MUL, DIV, ASH, ASHC, XOR, SOB): not implemented by the 11/20.
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

	// MTPS/MFPS (0006400/0006700) are 11/34 and LSI-11 instructions,
	// the 11/20 has no such opcodes.
	case 0006400:
	case 0006500:
	case 0006600:
	case 0006700:
		goto ri;
	}

	switch(cpu->ir & 0107400){
	case 0004000:
	case 0004400:	TR(JSR);
		if(dm == 0) goto ill;
		if(addrop(cpu, dst, 0)) goto be;
		DR = cpu->b;
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
		if(addrop(cpu, dst, 0)) goto be;
		PC = cpu->b;
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
		if(cpu->swab_vbit) {
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
	case 0:	TR(HALT); cpu->state = KA11_STATE_HALTED; return;
	case 1:	TR(WAIT); cpu->state = KA11_STATE_WAITING; return ; // no traps
	case 2:	TR(RTI);
		BA = SP; POP; IN(PC);
		BA = SP; POP; IN(PSW);
		levelchange(cpu->psw) ;
		SVC;
	case 3:	TR(BPT); TRAP(014);
	case 4:	TR(IOT); TRAP(020);
	case 5:	TR(RESET); ka11_reset(cpu); unibone_bus_init() ; SVC;
	}

	// All other instructions should be reserved now

ri:	TRAP(010);
ill:	TRAP(4);
be:	if(cpu->be > 1){
		printf("double bus error, HALT\n");
		trace("double bus error, HALT");
		cpu->state = KA11_STATE_HALTED;
		return;
	}
	trace("bus error at %06o\n", cpu->ba);
	TRAP(4);

trap:
	if (cpu->tracing && unibone_trace_addr(PC-2))
		trace("TRAP %o\n", TV);
	PUSH; OUT(SP, PSW);
	PUSH; OUT(SP, PC);
	INA(TV, PC);
	INA(TV+2, PSW);
	levelchange(PSW);
	/* no trace trap after a trap */
	oldpsw = PSW;

	if (cpu->tracing && unibone_trace_addr(PC-2))
		ka11_tracestate(cpu);
	return;		// TODO: is this correct?
//	SVC;

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
ka11_setintr(KA11 *cpu, unsigned vec)
{
	pthread_mutex_lock(&cpu->mutex) ;
	cpu->external_intr = true;
	cpu->external_intrvec = vec;
	trace("INTR vec=%03o\n", vec) ;
	pthread_mutex_unlock(&cpu->mutex) ;
}

// only to be called from ka11_condstep() thread

void
ka11_pwrfail_trap(KA11 *cpu)
{
	cpu->traps |= TRAP_PWR;
}

// only to be called from ka11_condstep() thread
// if locked, will lock DATI and unibus adapter()!
void
ka11_pwrup_vector_fetch(KA11 *cpu)
{
	// caller must have issued reset()
	// cpu->traps &= ~TRAP_PWR; // no, would be a fix
	cpu->tracing = unibone_trace_enabled();
	INA(024, PC);
	INA(024+2, PSW);
	return ;
be:
	trace("BE\n");
	cpu->be++ ;
}

void
ka11_condstep(KA11 *cpu)
{
	if(cpu->state == KA11_STATE_RUNNING || cpu->state == KA11_STATE_WAITING)
		// GRANT Interrupts before opcode fetch, or when CPU is on WAIT
		unibone_grant_interrupts() ;

	if((cpu->state == KA11_STATE_RUNNING) ||
	   (cpu->state == KA11_STATE_WAITING && cpu->traps)
	   || (cpu->state == KA11_STATE_WAITING && cpu->external_intr) ){
		cpu->state = KA11_STATE_RUNNING;
		// external_intr WAIT handled atomically in ka11_setintr() !

		// is trace() output live at all? Cached once per instruction so
		// the trace sites cost a flag test each when it is not.
		cpu->tracing = unibone_trace_enabled();
		step(cpu);
	}
}
