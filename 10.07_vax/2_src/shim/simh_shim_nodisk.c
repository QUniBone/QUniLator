/* simh_shim_nodisk.c: the disk layer the plain shim build does not carry
 *
 * The UNIBUS map of pdp11_io_lib.c offers one routine out of simh's disk layer,
 * the ATTACH-time writer of a DEC standard bad block table. The plain shimmed
 * core has no controller that attaches a disk - on the board the disks are
 * emulated devices on the bus - so reaching it means a device arrived without
 * its support, and it says so.
 *
 * The disk configuration, vax780-shim-disk, links simh's sim_disk.c instead and
 * gets the real one.
 */

#include "simh_shim.h"

#include "scp.h"
#include "sim_disk.h"

t_stat sim_disk_pdp11_bad_block (UNIT *uptr, int32 sec, int32 wds)
{
(void) sec;
(void) wds;
return sim_messagef (SCPE_NOFNC, "%s: writing a bad block table needs simh's disk layer\n",
                     sim_uname (uptr));
}
