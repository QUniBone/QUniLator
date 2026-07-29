/* vax780_shim_syslist.c: the device list of the shim build
 *
 * The device list simh's vax780_syslist.c carries names every peripheral the
 * stock simulator offers, most of them UNIBUS controllers out of the PDP-11
 * simulator. An embedded core has no use for those: on the board the UNIBUS
 * peripherals are the emulated devices of 10.02_devices, reached over the real
 * bus through the adapter, and a second set inside the CPU model would be two
 * devices at one address.
 *
 * So the list here is the machine and nothing else - the processor, its memory
 * management, the SBI, the memory controllers, the UNIBUS and MASSBUS adapters,
 * the interval and time-of-year clocks, and the console terminal and floppy.
 *
 * SHIM_WITH_DISK adds simh's own MSCP controller to that list, for the test
 * configuration 10.07_vax/2_src/makefile builds as vax780-shim-disk. It exists
 * so the shim can be shown carrying an operating system before any bus work
 * starts, and so a later failure can be told apart: a VMS that boots from this
 * disk and not from the emulated one puts the fault below the shim. It answers
 * at the address the emulated UDA50 answers at, so the two are alternatives and
 * never a configuration.
 *
 * The binary loader is simh's, unchanged in effect: it reads a byte stream into
 * memory or into one of the console ROMs, which is how the 780 console places
 * VMB before starting a boot.
 */

#include "vax_defs.h"

char sim_name[] = "VAX 11/780";

void vax_init (void)
{
extern const char *sim_savename;

sim_savename = "VAX780";
}

extern DEVICE cpu_dev;
extern DEVICE tlb_dev;
extern DEVICE sbi_dev;
extern DEVICE mctl_dev[MCTL_NUM];
extern DEVICE uba_dev;
extern DEVICE mba_dev[MBA_NUM];
extern DEVICE clk_dev;
extern DEVICE tmr_dev;
extern DEVICE tti_dev, tto_dev;
extern DEVICE fl_dev;
extern DEVICE uw_dev;
#ifdef SHIM_WITH_DISK
extern DEVICE rq_dev;
#endif

DEVICE *sim_devices[] = {
    &cpu_dev,
    &tlb_dev,
    &sbi_dev,
    &mctl_dev[0],
    &mctl_dev[1],
    &uba_dev,
    &mba_dev[0],
    &mba_dev[1],
    &clk_dev,
    &tmr_dev,
    &tti_dev,
    &tto_dev,
    &fl_dev,
    &uw_dev,
#ifdef SHIM_WITH_DISK
    &rq_dev,
#endif
    NULL
    };

/* Binary loader, as vax780_syslist.c writes it: an absolute system image is a
   byte stream with neither origin nor relocation, placed at the origin the -O
   switch gives, or in one of the console ROMs under -R or -S. */

t_stat sim_load (FILE *fileref, CONST char *cptr, CONST char *fnam, int flag)
{
t_stat r;
int32 val;
uint32 origin, limit;

(void) fnam;
if (flag)                                               /* dump? */
    return sim_messagef (SCPE_NOFNC, "Command Not Implemented\n");
origin = 0;                                             /* memory */
limit = (uint32) cpu_unit.capac;
if (sim_switches & SWMASK ('O')) {                      /* origin? */
    origin = (int32) get_uint (cptr, 16, 0xFFFFFFFF, &r);
    if (r != SCPE_OK)
        return SCPE_ARG;
    }

while ((val = Fgetc (fileref)) != EOF) {                /* read byte stream */
    if (sim_switches & SWMASK ('R')) {                  /* ROM0? */
        if (origin >= ROMSIZE)
            return SCPE_NXM;
        rom_wr_B (ROM0BASE + origin, val);
        }
    else if (sim_switches & SWMASK ('S')) {             /* ROM1? */
        if (origin >= ROMSIZE)
            return SCPE_NXM;
        rom_wr_B (ROM1BASE + origin, val);
        }
    else {
        if (origin >= limit)                            /* NXM? */
            return SCPE_NXM;
        WriteB (origin, val);                           /* memory */
        }
    origin = origin + 1;
    }
return SCPE_OK;
}
