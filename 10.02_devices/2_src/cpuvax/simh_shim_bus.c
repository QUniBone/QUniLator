/* simh_shim_bus.c: the UNIBUS I/O page, over the real bus
 *
 * The DW780 model of the vendored simh reaches a peripheral's registers through
 * the dispatch tables iodispR[] and iodispW[], which build_ubus_tab() fills in
 * from the devices simh itself carries. An emulated VAX on a UniBone wants the
 * other kind: the peripherals are the device models of 10.02_devices, they
 * answer on the bus, and a register access has to become a DATI or a DATO
 * there.
 *
 * So every address in the I/O page that no simh device claimed is pointed at
 * the two routines here, which hand the cycle to the embedding. simh's own
 * dispatch is left in place and keeps its addresses - which is what lets a
 * machine boot from the controller inside the core while the devices on the bus
 * are brought up around it. Nothing in the vendored tree is edited.
 *
 * The addresses differ on the two sides. A VAX names a UNIBUS register by its
 * place in the nexus window, IOPAGEBASE upwards; the bus names it by an
 * eighteen bit address in the top 8K of its space. The offset within the page
 * is the same in both, and that is the whole translation.
 */

#include "simh_shim.h"

#include "sim_defs.h"
#include "scp.h"
#include "vax_defs.h"
#include "simh_shim_internal.h"

/* The UNIBUS I/O page: the top 8K of the eighteen bit address space. */
#define UNIBUS_IOPAGE_BASE      0760000

extern t_stat (*iodispR[IOPAGESIZE >> 1])(int32 *dat, int32 ad, int32 md);
extern t_stat (*iodispW[IOPAGESIZE >> 1])(int32 dat, int32 ad, int32 md);

extern void uba_ub_nxm (int32 ua);

static unsigned shim_unibus_addr (int32 pa)
{
return UNIBUS_IOPAGE_BASE + (((uint32) pa) & IOPAGEMASK);
}

/* A read the bus did not answer is a nonexistent memory error of the adapter,
   which is what the DW780 reports for a device that is not there and what turns
   into a machine check if the processor was not expecting it. */
/* Counted where the adapter's dispatch enters, so a machine that reaches the
   bus can be told apart from one that never asks. */
unsigned shim_iopage_dispatches = 0;

static t_stat shim_bus_read (int32 *data, int32 pa, int32 mode)
{
unsigned value;

(void) mode;
shim_iopage_dispatches++;
if (!simh_shim_bus_read (shim_unibus_addr (pa), &value)) {
    uba_ub_nxm (pa);
    *data = 0;
    return SCPE_NXM;
    }
*data = (int32) (value & 0177777);
return SCPE_OK;
}

static t_stat shim_bus_write (int32 data, int32 pa, int32 mode)
{
shim_iopage_dispatches++;
/* WRITEB is the byte cycle, DATOB on the bus, and the byte the processor means
   is the one the address selects. */
if (!simh_shim_bus_write (shim_unibus_addr (pa), (unsigned) (data & 0177777),
                          (mode == WRITEB))) {
    uba_ub_nxm (pa);
    return SCPE_NXM;
    }
return SCPE_OK;
}

/* The claim, and one slot of it kept as a witness.

   A device may reconfigure itself while the machine runs - simh's MSCP
   controller ends its reset by running the auto-configuration - and that
   rebuilds the whole I/O page dispatch from the devices the core carries,
   taking back whatever the bus had claimed. Nothing announces it. So the claim
   is re-asserted whenever the witness shows it has been lost, which is one
   pointer compare on the path that runs it. */
static int shim_claim_exclusive = 0;
static uint32 shim_claim_witness = 0;
static int shim_claim_active = 0;

int simh_shim_bus_reassert (void)
{
if (!shim_claim_active)
    return 0;
if (iodispR[shim_claim_witness] == &shim_bus_read)
    return 0;                                           /* still ours */
simh_shim_bus_install (shim_claim_exclusive);
return 1;
}

/* Whether the bus still answers a given UNIBUS address. The count taken when
   the page was claimed says only what was done then; a device that
   reconfigures itself afterwards rebuilds the dispatch and takes its own
   addresses back, and only asking the table now tells whether it did. */
int simh_shim_bus_owns (unsigned unibus_addr)
{
uint32 idx = (unibus_addr & IOPAGEMASK) >> 1;

if (idx >= (IOPAGESIZE >> 1))
    return 0;
return (iodispR[idx] == &shim_bus_read);
}

/* Point the I/O page at the bus. Called after each reset, because reset_all()
   rebuilds the tables from the devices simh has.

   Exclusive takes the whole page, over the addresses simh's own devices claimed
   as well. That is what a machine whose peripherals are all on the bus wants,
   and it leaves those devices otherwise intact - which matters, because a boot
   command reads a controller's address out of its descriptor, and the address
   is only put there by the auto-configuration that a device takes part in.
   Sharing is for a machine that also carries a controller inside the core. */
unsigned simh_shim_bus_install (int exclusive)
{
uint32 i;
unsigned claimed = 0;

for (i = 0; i < (IOPAGESIZE >> 1); i++) {
    if (exclusive || (iodispR[i] == NULL)) {
        if (claimed == 0)
            shim_claim_witness = i;                     /* watch the first */
        iodispR[i] = &shim_bus_read;
        claimed++;
        }
    if (exclusive || (iodispW[i] == NULL))
        iodispW[i] = &shim_bus_write;
    }
shim_claim_exclusive = exclusive;
shim_claim_active = (claimed > 0);
return claimed;
}

/* And take it off again, so a build with no bus behind it - the workstation
   harness - reports a nonexistent device rather than calling into nothing. */
void simh_shim_bus_remove (void)
{
uint32 i;

shim_claim_active = 0;

for (i = 0; i < (IOPAGESIZE >> 1); i++) {
    if (iodispR[i] == &shim_bus_read)
        iodispR[i] = NULL;
    if (iodispW[i] == &shim_bus_write)
        iodispW[i] = NULL;
    }
}

/* ------------------------------------------------------------------------ */
/* An interrupt from the bus                                                 */
/*                                                                           */
/* The adapter keeps a bitmask of pending requests per hardware IPL and a     */
/* vector for each bit in it, and hands the processor whichever it finds when */
/* the processor is ready to look. A request from the bus takes one reserved  */
/* bit: the arbitration lets one interrupt through at a time, holding further */
/* grants until the processor has taken this one, so one is enough.           */
/*                                                                           */
/* This runs on the thread that watches the bus, not on the one executing     */
/* instructions, which is how the PDP-11 cores here take an interrupt too.    */
/* ------------------------------------------------------------------------ */

#define SHIM_INT_BIT    31                      /* reserved for the bus */

extern int32 int_req[IPL_HLVL];
extern int32 int_vec[IPL_HLVL][32];
extern void uba_eval_int (void);

int simh_shim_bus_interrupt (unsigned vector, unsigned br_level)
{
int32 lvl = (int32) br_level - 4;                       /* BR4..BR7 -> 0..3 */

if ((lvl < 0) || (lvl >= IPL_HLVL))
    return 0;
if (int_req[lvl] & (1 << SHIM_INT_BIT))                 /* one still pending */
    return 0;
int_vec[lvl][SHIM_INT_BIT] = (int32) vector;
int_req[lvl] |= (1 << SHIM_INT_BIT);
uba_eval_int ();
return 1;
}

/* ------------------------------------------------------------------------ */
/* DMA from a device on the bus                                              */
/*                                                                           */
/* A device names an eighteen bit UNIBUS address; the memory it means is the  */
/* processor's, somewhere in a much larger space, and the adapter's map       */
/* registers are what join the two. The vendored model already does all of    */
/* it - the map lookup, the byte offset bit that lets a transfer start on an  */
/* odd boundary, and the invalid map entry that must fail the transfer rather */
/* than corrupt memory - so a transfer here is one call into it.              */
/*                                                                           */
/* The direction is the device's: a device performing DATO is writing memory. */
/* ------------------------------------------------------------------------ */

extern int32 Map_ReadW (uint32 ba, int32 bc, uint16 *buf);
extern int32 Map_WriteW (uint32 ba, int32 bc, const uint16 *buf);

int simh_shim_bus_dma (int write, unsigned addr, uint16_t *buffer, unsigned words)
{
int32 bytes = (int32) (words * 2);
int32 untransferred;

if (write)
    untransferred = Map_WriteW (addr, bytes, (const uint16 *) buffer);
else
    untransferred = Map_ReadW (addr, bytes, (uint16 *) buffer);
return (untransferred == 0);
}
