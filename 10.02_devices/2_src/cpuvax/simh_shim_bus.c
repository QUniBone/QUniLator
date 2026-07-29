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
static t_stat shim_bus_read (int32 *data, int32 pa, int32 mode)
{
unsigned value;

(void) mode;
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
/* WRITEB is the byte cycle, DATOB on the bus, and the byte the processor means
   is the one the address selects. */
if (!simh_shim_bus_write (shim_unibus_addr (pa), (unsigned) (data & 0177777),
                          (mode == WRITEB))) {
    uba_ub_nxm (pa);
    return SCPE_NXM;
    }
return SCPE_OK;
}

/* Point every unclaimed word of the I/O page at the bus. Called after each
   reset, because reset_all() rebuilds the tables from the devices simh has. */
void simh_shim_bus_install (void)
{
uint32 i;

for (i = 0; i < (IOPAGESIZE >> 1); i++) {
    if (iodispR[i] == NULL)
        iodispR[i] = &shim_bus_read;
    if (iodispW[i] == NULL)
        iodispW[i] = &shim_bus_write;
    }
}

/* And take it off again, so a build with no bus behind it - the workstation
   harness - reports a nonexistent device rather than calling into nothing. */
void simh_shim_bus_remove (void)
{
uint32 i;

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
