/* simh_shim_timer.c: the clock side of the scp stand-in
 *
 * simh's sim_timer.c calibrates a simulator's interval clock against the host's
 * wall clock: it measures how many simulated instructions a real tick took and
 * adjusts the interval so the guest's line clock keeps real time, and it sleeps
 * the host when the guest is idle.
 *
 * The shim does neither. The interval a device asks for is the interval it
 * gets, and the guest's clock runs at whatever rate the embedding executes
 * instructions. On the BeagleBone the emulation has a time source of its own -
 * the flexible timeout controller the other device models share - and the
 * calibration belongs there, against that clock, not against the workstation's.
 *
 * Idling is off for the same reason: an embedded core must not put its host
 * thread to sleep, because the host thread has a bus to serve.
 */

#include "simh_shim.h"

#include <time.h>

#include "scp.h"
#include "sim_timer.h"
#include "simh_shim_internal.h"

/* ------------------------------------------------------------------------ */
/* Host time                                                                 */
/* ------------------------------------------------------------------------ */

uint32 sim_os_msec (void)
{
return (uint32) (simh_shim_elapsed_usec () / 1000.0);
}

void sim_timespec_diff (struct timespec *diff, struct timespec *min, struct timespec *sub)
{
diff->tv_sec = min->tv_sec - sub->tv_sec;
diff->tv_nsec = min->tv_nsec - sub->tv_nsec;
while (diff->tv_nsec < 0) {
    diff->tv_nsec += 1000000000;
    diff->tv_sec -= 1;
    }
}

/* ------------------------------------------------------------------------ */
/* The interval clock                                                        */
/* ------------------------------------------------------------------------ */

void sim_rtcn_get_time (struct timespec *now, int tmr)
{
double usec = simh_shim_elapsed_usec ();

(void) tmr;
now->tv_sec = (time_t) (usec / 1000000.0);
now->tv_nsec = (long) ((usec - (double) now->tv_sec * 1000000.0) * 1000.0);
}

/* The nominal interval, unadjusted. A device asks for the number of simulated
   instructions its tick is worth and gets exactly that back. */
int32 sim_rtcn_init_unit (UNIT *uptr, int32 time, int32 tmr)
{
(void) uptr;
(void) tmr;
return (time > 0) ? time : 1;
}

int32 sim_rtcn_calb (uint32 ticksper, int32 tmr)
{
(void) tmr;
if (ticksper == 0)
    return sim_vm_initial_ips;
return (int32) (sim_vm_initial_ips / (int32) ticksper);
}

t_stat sim_rtcn_tick_ack (uint32 time, int32 tmr)
{
(void) time;
(void) tmr;
return SCPE_OK;
}

/* ------------------------------------------------------------------------ */
/* Idling                                                                    */
/* ------------------------------------------------------------------------ */

t_bool sim_idle (uint32 tmr, int sin_cyc)
{
(void) tmr;
(void) sin_cyc;
return FALSE;                                           /* never sleeps */
}

t_stat sim_set_idle (UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
(void) uptr;
(void) val;
(void) cptr;
(void) desc;
return sim_messagef (SCPE_NOFNC, "idling is not available in the embedded build\n");
}

t_stat sim_clr_idle (UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
(void) uptr;
(void) val;
(void) cptr;
(void) desc;
return SCPE_OK;
}

t_stat sim_show_idle (FILE *st, UNIT *uptr, int32 val, CONST void *desc)
{
(void) uptr;
(void) val;
(void) desc;
fprintf (st, "idling disabled");
return SCPE_OK;
}
