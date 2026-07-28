/* kt11d.c: KT11-D memory management unit of the PDP-11/34

 See kt11d.h for the register model and how this hangs into the KD11-EA.

 The hot path (kt11d_relocate()) is inline in the header. What is here is
 everything which runs rarely: rebuilding the derived page[] descriptors,
 the abort bookkeeping, and the register decode.
 */

#include "cpu_core.h"
#include "kt11d.h"
#include <stdarg.h>
#include <string.h>	// memset

/* ACF, the access control field. On the KT11-D this is PDR<2:1>, two bits,
 and there are no "trap" modes - every illegal access aborts.
 (The KT11-C of the 11/45 has a 3 bit ACF in PDR<2:0> with trap variants;
 do not copy its encoding here.) */
enum {
	ACF_NONRESIDENT = 0,	// PDR<2:1> = 00: abort any access
	ACF_READONLY = 1,		// PDR<2:1> = 01: abort on write
	ACF_UNUSED = 2,			// PDR<2:1> = 10: not implemented, abort any access
	ACF_READWRITE = 3		// PDR<2:1> = 11: no restriction
};

#define PDR_ACF(pdr)	(((pdr) >> 1) & 3)
#define PDR_PLF(pdr)	(((pdr) >> 8) & 0177)

void
kt11d_reset(KT11D *mmu)
{
	// A bus INIT clears MMR0, which disables relocation. PAR/PDR are not
	// cleared by INIT on real hardware, but a cold start of the emulation
	// must not inherit a random map, so clear everything here: this is
	// called from kd11ea_reset(), i.e. on RESET and on console start.
	memset(mmu, 0, sizeof(*mmu));
	mmu->space = KT11D_SPACE_KERNEL;
	mmu->prev_space = KT11D_SPACE_KERNEL;
	kt11d_set_access(mmu, KT11D_SPACE_KERNEL, KT11D_MODE_KERNEL);
	kt11d_rebuild_all(mmu);
}

/* Bus INIT: the RESET opcode and console start.
 Unlike a power-up this does not clear the address map - the PAR/PDR pairs are
 not affected by INIT, so an OS which executes RESET keeps its mapping - but it
 does clear MMR0..MMR2, which among other things turns relocation off and
 releases an abort freeze. MAINDEC DFKTA-A checks it directly: it sets MMR0<8>,
 executes RESET and expects the bit to read back as zero; DFKTH-B tests it as
 "SR0 OR SR2 WERE NOT RESET BY A RESET". */
void
kt11d_init(KT11D *mmu)
{
	mmu->mmr0 = 0;
	mmu->mmr1 = 0;
	mmu->mmr2 = 0;
	mmu->mmr1_count = 0;
	kt11d_rebuild_all(mmu);
}

/* recompute the derived descriptor of one PAR/PDR pair.
 The only places which may change par[]/pdr[]/mmr0 must call this (or
 kt11d_rebuild_all()) afterwards - that is the whole coherency contract of
 the derived page[] array. */
void
kt11d_rebuild(KT11D *mmu, unsigned idx)
{
	kt11d_page_t *p = &mmu->page[idx];
	word pdr = mmu->pdr[idx];
	word par = mmu->par[idx];
	unsigned page = idx & 7;
	unsigned plf = PDR_PLF(pdr);

	// physical = (PAF<<6) + (va & 017777), and va & 017777 == va - (page<<13).
	// The subtraction may wrap; kt11d_relocate() masks the sum to 18 bits.
	p->base = ((uint32)(par & KT11D_PAR_WRITABLE) << 6) - ((uint32)page << 13);

	if(pdr & KT11D_PDR_ED){
		// expands downward: blocks plf..127 are inside the page
		p->blk_lo = plf;
		p->blk_span = 0177 - plf;
	}else{
		// expands upward: blocks 0..plf are inside the page
		p->blk_lo = 0;
		p->blk_span = plf;
	}

	switch(PDR_ACF(pdr)){
	case ACF_READWRITE:
		p->deny = 0;
		break;
	case ACF_READONLY:
		p->deny = KT11D_WRITE;
		break;
	case ACF_NONRESIDENT:
	case ACF_UNUSED:
	default:
		p->deny = KT11D_READ | KT11D_WRITE;
		break;
	}

	p->pdr_idx = idx;
}

void
kt11d_rebuild_all(KT11D *mmu)
{
	unsigned i;
	mmu->enabled = !!(mmu->mmr0 & KT11D_MMR0_ENABLE);
	mmu->frozen = !!(mmu->mmr0 & KT11D_MMR0_ABORTS);
	mmu->maint = !!(mmu->mmr0 & KT11D_MMR0_MAINT);
	for(i = 0; i < 16; i++)
		kt11d_rebuild(mmu, i);
}

/* PSW<15:14> (current mode) or PSW<13:12> (previous mode) changed.
 Only kernel (00) and user (11) exist; the 11/34 has no supervisor mode, and
 an attempt to select it behaves like user. */
void
kt11d_set_modes(KT11D *mmu, word psw)
{
	mmu->mode = (psw >> 14) & 3;
	mmu->prev_mode = (psw >> 12) & 3;
	// the space is only ever kernel or user; a reference in one of the two
	// modes which have no space of their own aborts before it is used
	mmu->space = mmu->mode == KT11D_MODE_KERNEL ?
			KT11D_SPACE_KERNEL : KT11D_SPACE_USER;
	mmu->prev_space = mmu->prev_mode == KT11D_MODE_KERNEL ?
			KT11D_SPACE_KERNEL : KT11D_SPACE_USER;
	kt11d_set_access(mmu, mmu->space, mmu->mode);
}

/* An access was refused. Record the cause in MMR0, which freezes MMR0<6:1>,
 MMR1 and MMR2 until software clears the abort bits.
 Always returns 1, so kt11d_relocate() can "return kt11d_abort(...)". */
int
kt11d_abort(KT11D *mmu, word va, unsigned space, unsigned access)
{
	unsigned page = (va >> 13) & 7;
	unsigned idx = space + page;
	word pdr = mmu->pdr[idx];
	unsigned blk = (va >> 6) & 0177;
	kt11d_page_t *p = &mmu->page[idx];
	word bits = 0;

	// recompute the exact cause from the raw PDR. The hot path only knows
	// *that* the access is illegal, not why.
	switch(PDR_ACF(pdr)){
	case ACF_READWRITE:
		break;
	case ACF_READONLY:
		if(access & KT11D_WRITE)
			bits |= KT11D_MMR0_ABORT_READONLY;
		break;
	default:
		bits |= KT11D_MMR0_ABORT_NONRESIDENT;
		break;
	}
	if((unsigned)(blk - p->blk_lo) > p->blk_span)
		bits |= KT11D_MMR0_ABORT_LENGTH;

	trace("MMU abort [%06o] %s, page %u %s, MMR0 bits %06o\n", va,
			(access & KT11D_WRITE) ? "write" : "read", page,
			space == KT11D_SPACE_KERNEL ? "kernel" : "user", bits);

	if(!mmu->frozen){
		mmu->mmr0 = (mmu->mmr0 & ~(KT11D_MMR0_MODE|KT11D_MMR0_PAGE)) | bits
				| (mmu->access_mode << 5)
				| (page << 1);
		mmu->frozen = 1;
	}
	return 1;
}

/* A reference made in a processor mode the KD11-EA does not have, PSW<15:14> =
 01 or 10. There is no page table to consult, so it aborts like a non-resident
 page, with MMR0<6:5> holding the mode which does not exist - MAINDEC DFKTA-A
 sets PSW to 040000, expects the next reference to abort and MMR0 to read
 100040. DFKTH-B tests it as "ILLEGAL MODE 01 NOT ABORTED". */
int
kt11d_abort_mode(KT11D *mmu, word va)
{
	unsigned page = (va >> 13) & 7;

	trace("MMU abort [%06o], illegal mode %o, MMR0 bits %06o\n", va,
			mmu->access_mode, KT11D_MMR0_ABORT_NONRESIDENT);

	if(!mmu->frozen){
		mmu->mmr0 = (mmu->mmr0 & ~(KT11D_MMR0_MODE|KT11D_MMR0_PAGE))
				| KT11D_MMR0_ABORT_NONRESIDENT
				| (mmu->access_mode << 5)
				| (page << 1);
		mmu->frozen = 1;
	}
	return 1;
}

/*** register decode. Addresses are 18 bit physical. ***/

// kernel PDR 772300..772316, kernel PAR 772340..772356
// user   PDR 777600..777616, user   PAR 777640..777656
// MMR0..MMR2 777572..777576
// Result of the lookup: pointer to the register image, or NULL.
// "*is_pdr" tells whether a write has to rebuild a page descriptor.
static word *
lookup(KT11D *mmu, uint32 pa, unsigned *idx, int *writable_mask)
{
	unsigned i;

	// the same predicate the hot path uses to keep a write to one of these
	// from setting the W bit of the page it lies in: whether an address is a
	// register of ours is decided in one place only.
	if(!kt11d_is_own_register(pa))
		return nil;

	switch(pa){
	case 0777572:
		*idx = ~0u;
		*writable_mask = KT11D_MMR0_WRITABLE;
		return &mmu->mmr0;
	case 0777574:
		*idx = ~0u;
		*writable_mask = 0;	// read only
		return &mmu->mmr1;
	case 0777576:
		*idx = ~0u;
		*writable_mask = 0;	// read only
		return &mmu->mmr2;
	}

	if(pa >= 0772300 && pa <= 0772316){			// kernel PDR
		i = KT11D_SPACE_KERNEL + ((pa - 0772300) >> 1);
		*idx = i; *writable_mask = KT11D_PDR_WRITABLE;
		return &mmu->pdr[i];
	}
	if(pa >= 0772340 && pa <= 0772356){			// kernel PAR
		i = KT11D_SPACE_KERNEL + ((pa - 0772340) >> 1);
		*idx = i; *writable_mask = KT11D_PAR_WRITABLE;
		return &mmu->par[i];
	}
	if(pa >= 0777600 && pa <= 0777616){			// user PDR
		i = KT11D_SPACE_USER + ((pa - 0777600) >> 1);
		*idx = i; *writable_mask = KT11D_PDR_WRITABLE;
		return &mmu->pdr[i];
	}
	if(pa >= 0777640 && pa <= 0777656){			// user PAR
		i = KT11D_SPACE_USER + ((pa - 0777640) >> 1);
		*idx = i; *writable_mask = KT11D_PAR_WRITABLE;
		return &mmu->par[i];
	}
	return nil;
}

/* Result: 1 = address belongs to the MMU and *w is set, 0 = not mine */
int
kt11d_read_reg(KT11D *mmu, uint32 pa, word *w)
{
	unsigned idx;
	int writable_mask;
	word *reg = lookup(mmu, pa, &idx, &writable_mask);
	if(reg == nil)
		return 0;
	*w = *reg;
	return 1;
}

/* "mask" selects the bits to change, for DATOB: 0377, 0177400 or 0177777.
 Result: 1 = address belongs to the MMU and was written, 0 = not mine */
int
kt11d_write_reg(KT11D *mmu, uint32 pa, word w, word mask)
{
	unsigned idx;
	int writable_mask;
	word *reg = lookup(mmu, pa, &idx, &writable_mask);
	if(reg == nil)
		return 0;

	mask &= writable_mask;
	*reg = (*reg & ~mask) | (w & mask);

	if(idx == ~0u){
		// MMR0: relocation enable and the abort/freeze bits may have changed
		kt11d_rebuild_all(mmu);
	}else{
		// Writing a PAR or a PDR clears the "page was written into" flag.
		mmu->pdr[idx] &= ~KT11D_PDR_W;
		kt11d_rebuild(mmu, idx);
	}
	return 1;
}

// append to buf, never writing past bufsize. Returns the new fill.
static size_t
appendf(char *buf, size_t bufsize, size_t fill, const char *fmt, ...)
{
	va_list ap;
	int n;
	if(fill + 1 >= bufsize)
		return fill;
	va_start(ap, fmt);
	n = vsnprintf(buf+fill, bufsize-fill, fmt, ap);
	va_end(ap);
	if(n < 0)
		return fill;
	fill += n;
	return fill >= bufsize ? bufsize-1 : fill;
}

void
kt11d_format(KT11D *mmu, char *buf, size_t bufsize)
{
	unsigned i;
	size_t n = 0;

	if(bufsize == 0)
		return;
	buf[0] = 0;
	n = appendf(buf, bufsize, n, " MMR0 %06o MMR1 %06o MMR2 %06o  relocation %s%s\n",
			mmu->mmr0, mmu->mmr1, mmu->mmr2,
			mmu->enabled ? "on" : "off", mmu->frozen ? ", MMR0-2 frozen by abort" : "");
	for(i = 0; i < 2; i++){
		unsigned base = i ? KT11D_SPACE_USER : KT11D_SPACE_KERNEL;
		unsigned j;
		n = appendf(buf, bufsize, n, " %s PAR", i ? "user  " : "kernel");
		for(j = 0; j < 8; j++)
			n = appendf(buf, bufsize, n, " %06o", mmu->par[base+j]);
		n = appendf(buf, bufsize, n, "\n %s PDR", i ? "user  " : "kernel");
		for(j = 0; j < 8; j++)
			n = appendf(buf, bufsize, n, " %06o", mmu->pdr[base+j]);
		n = appendf(buf, bufsize, n, "\n");
	}
}
