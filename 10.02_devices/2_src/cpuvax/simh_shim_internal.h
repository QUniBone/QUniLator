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

/* One register access on the embedding's bus. Zero when it did not answer. */
int simh_shim_bus_read (unsigned addr, unsigned *data);
int simh_shim_bus_write (unsigned addr, unsigned data, int byte);

/* Point the unclaimed part of the I/O page at that bus, or take it off again.
   Defined in simh_shim_bus.c, which only a build with a bus links. */
unsigned simh_shim_bus_install (void);
void simh_shim_bus_remove (void);

/* Reset every device and then take the I/O page back, which every path that
   resets has to use: reset_all() alone drops what the bus had claimed. */
t_stat shim_reset_devices (void);

#endif /* SIMH_SHIM_INTERNAL_H_ */
