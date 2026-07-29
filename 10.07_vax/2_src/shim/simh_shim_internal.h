/* simh_shim_internal.h: what the shim's own files share
 *
 * simh_shim.c holds the host binding, and the console and timer files reach the
 * host through these. Nothing outside shim/ uses them; simh_shim.h is the seam
 * an embedding sees.
 */

#ifndef SIMH_SHIM_INTERNAL_H_
#define SIMH_SHIM_INTERNAL_H_

/* One character from the host's console keyboard, or -1 when none is waiting. */
int simh_shim_console_get (void);

/* One character to the host's console printer. */
void simh_shim_console_put (int c);

/* The host's elapsed time in microseconds, from any fixed origin. */
double simh_shim_elapsed_usec (void);

/* How fast the core is running, in instructions per second of the host's time,
   as the last calibrated clock measured it. Every delay a device asks for in
   microseconds is converted with this. */
double simh_shim_ips (void);

/* How many instructions one tick of calibrated clock tmr is worth, which is
   what a device means when it asks to be scheduled a number of ticks ahead. */
int32 simh_shim_tick_size (int32 tmr);

#endif /* SIMH_SHIM_INTERNAL_H_ */
