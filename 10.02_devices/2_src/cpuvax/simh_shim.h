/* simh_shim.h: the seam between the simh VAX core and its embedding
 *
 * The vendored simh VAX-11/780 reaches the world in two directions. Downwards,
 * through the files of 91_3rd_party/simh_vax/VAX, which model the processor and
 * the machine. Upwards, through the couple of hundred entry points that simh's
 * own command interpreter, scp.c, supplies to every simulator it hosts: an
 * event queue, a device list, console characters, a clock calibration, and a
 * large surface of command, help and breakpoint machinery.
 *
 * The shim is the second of those. It supplies what scp.c, sim_console.c,
 * sim_timer.c and sim_tmxr.c supply, so the core can run inside a program that
 * has its own main loop, its own console and its own idea of time - which is
 * what the QUniLator application is. simh's file layer, sim_fio.c, is kept as
 * it is; it depends on nothing above it.
 *
 * The shim is deliberately smaller than what it replaces. Everything the core
 * reaches only through a SET, SHOW or HELP table is a stub that says so, and
 * simh's asynchronous I/O is left out by building the core without
 * SIM_ASYNCH_IO. What remains real is the part an executing processor needs:
 * the event queue, device reset and attach, the console byte stream, the
 * interval clock, and the loader that places a bootstrap in memory.
 *
 * The embedding supplies a host through simh_shim_bind(). On the workstation
 * that host is shim_main.c, which drives stdin and stdout and the wall clock;
 * on the BeagleBone it will be the CPU device of the QUniLator application.
 */

#ifndef SIMH_SHIM_H_
#define SIMH_SHIM_H_

#include <stdio.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The seam is stated in plain types so that an embedding written in C++ can
   include this without simh's own headers coming with it. A status is one of
   simh's SCPE_ codes, and zero is success. */
typedef int simh_shim_status_t;

/* What the embedding provides. Every member is required. */
typedef struct {
    void *context;                                      /* passed back to each call */

    /* One character from the console keyboard, or -1 when none is waiting.
       Never blocks: the shim polls it from the instruction loop. */
    int (*console_get) (void *context);

    /* One character to the console printer. */
    void (*console_put) (void *context, int c);

    /* Elapsed time in microseconds, from any fixed origin. The clock
       calibration divides differences of this, so only the rate matters. */
    double (*elapsed_usec) (void *context);

    /* A register access on the UNIBUS, when the embedding has one. Both return
       zero when the bus did not answer, which the adapter reports as its own
       nonexistent memory error. A NULL pair leaves the processor with only the
       peripherals simh carries inside it.

       The address is an eighteen bit UNIBUS address, and a write with byte set
       is a DATOB. */
    int (*bus_read) (void *context, unsigned addr, unsigned *data);
    int (*bus_write) (void *context, unsigned addr, unsigned data, int byte);

    /* Set when every peripheral is on the bus, so the I/O page belongs to it
       whole. Clear when the machine also carries a controller inside the core,
       which then keeps its own addresses. */
    int bus_owns_iopage;

    /* Where simulator messages go. NULL sends them to stdout. */
    FILE *message_file;
} simh_shim_host_t;

/* Bind the host. Call once, before anything else. */
void simh_shim_bind (const simh_shim_host_t *host);

/* Reset every device in sim_devices[], as scp's RESET ALL does. */
simh_shim_status_t simh_shim_reset (void);

/* Run the processor for at most max_instructions, then return. The count is
   approximate in the same way sim_interval is: an instruction that spans
   several memory cycles charges more than one. Returns what sim_instr()
   returned, which is SCPE_STOP when the batch simply ran out. */
simh_shim_status_t simh_shim_run (int max_instructions);

/* Attach a file to a unit named as scp names it, "RQ0" or "FL". */
simh_shim_status_t simh_shim_attach (const char *unit_name, const char *filename);

/* Set a device or unit parameter out of its modifier table, as scp's SET does:
   simh_shim_set("RQ0 RD54"), simh_shim_set("RQ UDA50"). */
simh_shim_status_t simh_shim_set (const char *setting);

/* Boot from a device, as scp's BOOT does: find it, and call its boot routine. */
simh_shim_status_t simh_shim_boot (const char *unit_name);

/* Text for a status code, for a host that wants to report one. */
const char *simh_shim_status_text (simh_shim_status_t status);

/* What the processor is doing, for an embedding that publishes it. The
   instruction count is simh's own, which charges a long instruction more than
   one, so it counts work and not opcodes. */
typedef struct {
    uint32_t pc;
    uint32_t psl;
    unsigned ipl;                                       /* current priority */
    unsigned iopage_claimed;                            /* I/O page words on the bus */
    unsigned uba_init;                                  /* adapter init in progress */
    unsigned iopage_dispatches;                         /* I/O page accesses the core made */
    unsigned uba_cr;                                    /* adapter control register */
    double instructions;
} simh_shim_state_t;

void simh_shim_state (simh_shim_state_t *state);

/* A device on the bus is interrupting, at the bus request level it was granted
   at. Returns zero when the processor could not take it - an unknown level, or
   one still pending - and the caller should leave the request asserted.

   Called from whatever thread watches the bus, not from the one running the
   processor. */
int simh_shim_bus_interrupt (unsigned vector, unsigned br_level);

/* A device on the bus is moving data to or from memory. The address is the
   eighteen bit one the device drives; what it reaches is the processor's memory
   through the adapter's map registers, which is where an unallocated map entry
   fails the transfer. write is the device's direction: a device performing DATO
   is writing memory. Returns zero when the transfer did not complete.

   Called from the device's thread, not the processor's. */
int simh_shim_bus_dma (int write, unsigned addr, uint16_t *buffer, unsigned words);

/* True when the last run ended because the processor executed HALT. */
int simh_shim_halted (simh_shim_status_t status);

/* True when the last run ended only because the batch ran out, which is the
   ordinary end of a pass and not a reason to stop the machine. */
int simh_shim_batch_ended (simh_shim_status_t status);

#ifdef __cplusplus
}
#endif

#endif /* SIMH_SHIM_H_ */
