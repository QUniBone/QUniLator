/* simh_shim_tmxr.c: the terminal-multiplexer side of the scp stand-in
 *
 * simh's sim_tmxr.c gives a simulated terminal multiplexer a set of telnet or
 * serial lines on the host. The 780's standard devices reach it in two places:
 * the console terminal registers itself with tmxr_set_console_units() so that a
 * telnet console can take it over, and the console floppy's line schedules its
 * polls through the multiplexer's coscheduling.
 *
 * Neither applies to an embedded core. Its console is the byte channel of
 * simh_shim.h, and its lines belong to the emulated serial devices the
 * QUniLator application already has on the bus. So every entry point here says
 * there is no line: an attach fails, a poll finds nothing, a coschedule falls
 * back to the plain event queue.
 */

#include "simh_shim.h"

#include "sim_defs.h"
#include "scp.h"
#include "sim_tmxr.h"
#include "simh_shim_internal.h"

/* sim_tmxr.h redirects the scheduling calls of any file that includes it to the
   multiplexer's own, so that a line's polling stays aligned with its clock.
   That redirection is what this file implements, so it has to be undone here or
   each routine below would call itself. */
#undef sim_activate
#undef sim_activate_abs
#undef sim_activate_after
#undef sim_activate_after_abs

/* Scheduling. Without a multiplexer to align with, a coscheduled poll is an
   ordinary event. */

t_stat tmxr_activate (UNIT *uptr, int32 interval)
{
return sim_activate (uptr, interval);
}

t_stat tmxr_activate_abs (UNIT *uptr, int32 interval)
{
return sim_activate_abs (uptr, interval);
}

t_stat tmxr_activate_after (UNIT *uptr, uint32 usecs)
{
return sim_activate_after (uptr, usecs);
}

t_stat tmxr_activate_after_abs (UNIT *uptr, uint32 usecs)
{
return sim_activate_after_abs (uptr, usecs);
}

t_stat tmxr_clock_coschedule (UNIT *uptr, int32 interval)
{
return sim_activate (uptr, interval);
}

t_stat tmxr_clock_coschedule_abs (UNIT *uptr, int32 interval)
{
return sim_activate_abs (uptr, interval);
}

/* Coscheduling counts in ticks of a calibrated clock, not in instructions: the
   VAX interval timer asks to be woken one tick from now for its usual 10ms
   interval, and a tick is however many instructions the core gets through in
   that time. */
t_stat tmxr_clock_coschedule_tmr (UNIT *uptr, int32 tmr, int32 ticks)
{
int32 size = simh_shim_tick_size (tmr);
fprintf (stderr, "coschedule_tmr: tmr=%d ticks=%d size=%d\n", tmr, ticks, size);
return sim_activate (uptr, ticks * size);
}

t_stat tmxr_clock_coschedule_tmr_abs (UNIT *uptr, int32 tmr, int32 ticks)
{
return sim_activate_abs (uptr, ticks * simh_shim_tick_size (tmr));
}

/* Lines. There are none. */

t_stat tmxr_attach_ex (TMXR *mp, UNIT *uptr, CONST char *cptr, t_bool async)
{
(void) mp;
(void) uptr;
(void) cptr;
(void) async;
return sim_messagef (SCPE_NOFNC, "terminal multiplexer lines are not available in the embedded build\n");
}

t_stat tmxr_detach (TMXR *mp, UNIT *uptr)
{
(void) mp;
(void) uptr;
return SCPE_OK;
}

int32 tmxr_poll_conn (TMXR *mp)
{
(void) mp;
return -1;                                              /* no connection */
}

void tmxr_poll_rx (TMXR *mp)
{
(void) mp;
}

void tmxr_poll_tx (TMXR *mp)
{
(void) mp;
}

int32 tmxr_getc_ln (TMLN *lp)
{
(void) lp;
return 0;                                               /* no character */
}

t_stat tmxr_putc_ln (TMLN *lp, int32 chr)
{
(void) lp;
(void) chr;
return SCPE_LOST;
}

t_stat tmxr_reset_ln (TMLN *lp)
{
(void) lp;
return SCPE_OK;
}

t_stat tmxr_set_console_units (UNIT *rxuptr, UNIT *txuptr)
{
(void) rxuptr;
(void) txuptr;
return SCPE_OK;                                         /* the shim owns the console */
}

t_stat tmxr_set_notelnet (TMXR *mp)
{
(void) mp;
return SCPE_OK;
}
