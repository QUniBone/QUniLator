/* simh_shim_timer.c: the clock side of the scp stand-in
 *
 * simh's sim_timer.c calibrates a simulator's interval clock against the host's
 * wall clock: it measures how many simulated instructions a real tick took and
 * adjusts the interval so the guest's line clock keeps real time. It also
 * sleeps the host when the guest is idle.
 *
 * The calibration is kept, because a guest needs it: an operating system paces
 * itself by its interval clock, and a clock that ticks faster than the core
 * gets work done leaves it running its own timer routine and nothing else. It
 * is measured here against the elapsed time the embedding supplies, which on
 * the BeagleBone will be the time source the other device models already share.
 *
 * Idling is not kept: an embedded core must not put its host thread to sleep,
 * because that thread has a bus to serve.
 */

#include "simh_shim.h"

#include <time.h>

#include "sim_defs.h"
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

/* How fast the core is actually running, in instructions per second of the
   host's time. A guest measures its own progress against its interval clock,
   so the number of instructions a tick is worth has to track the rate the
   embedding is executing them at: a clock left at a nominal rate ticks tens of
   times too often for the work done between ticks, and an operating system
   then spends every instruction in its own timer routine.

   The estimate starts at what the processor model publishes and is remeasured
   whenever a calibrated clock asks, over a window long enough that the
   measurement is not noise. */
#define SHIM_CALIBRATE_USEC     200000.0

static double shim_ips = 0.0;
static int32 shim_currd[SIM_NTIMERS + 1];               /* instructions per tick */

double simh_shim_ips (void)
{
return (shim_ips > 0.0) ? shim_ips : (double) sim_vm_initial_ips;
}

/* A clock nobody has calibrated yet is assumed to tick at the rate the VAX
   line clock does, which is the only one the 780 has. */
int32 simh_shim_tick_size (int32 tmr)
{
if ((tmr < 0) || (tmr > SIM_NTIMERS))
    tmr = SIM_NTIMERS;
if (shim_currd[tmr] > 0)
    return shim_currd[tmr];
return (int32) (simh_shim_ips () / 100.0);
}

int32 sim_rtcn_init_unit (UNIT *uptr, int32 time, int32 tmr)
{
(void) uptr;
(void) tmr;
return (time > 0) ? time : 1;
}

int32 sim_rtcn_calb (uint32 ticksper, int32 tmr)
{
static double base_usec[SIM_NTIMERS + 1];
static double base_gtime[SIM_NTIMERS + 1];
double now, executed, elapsed, per_tick;

if ((tmr < 0) || (tmr > SIM_NTIMERS))
    tmr = SIM_NTIMERS;
now = simh_shim_elapsed_usec ();
if (base_usec[tmr] == 0.0) {                            /* first time here */
    base_usec[tmr] = now;
    base_gtime[tmr] = sim_gtime ();
    }
else {
    elapsed = now - base_usec[tmr];
    executed = sim_gtime () - base_gtime[tmr];
    if ((elapsed >= SHIM_CALIBRATE_USEC) && (executed > 0.0)) {
        shim_ips = (executed * 1000000.0) / elapsed;
        base_usec[tmr] = now;
        base_gtime[tmr] = sim_gtime ();
        }
    }

if (ticksper == 0)
    return (int32) simh_shim_ips ();
per_tick = simh_shim_ips () / (double) ticksper;
if (per_tick < 1.0)
    per_tick = 1.0;
if (per_tick > (double) 0x3FFFFFFF)
    per_tick = (double) 0x3FFFFFFF;
shim_currd[tmr] = (int32) per_tick;
return shim_currd[tmr];
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
