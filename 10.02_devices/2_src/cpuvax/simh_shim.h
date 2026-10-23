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

    /* Pulse INIT on the UNIBUS, which resets every card on it. The adapter
       calls it when it initializes. It goes with the pair above and may be
       NULL with them. */
    void (*bus_init) (void *context);

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

/* A removable medium: whatever the unit holds is taken out first, writing a
   buffered unit's contents back to its file when they changed. With readonly
   set the new file is kept as it stands: a buffered unit takes the guest's
   writes in memory and drops them when the medium comes out, and any other
   unit goes in write locked. An empty or NULL filename leaves the unit empty. */
simh_shim_status_t simh_shim_attach_medium (const char *unit_name, const char *filename,
                                            int readonly);

/* Write a buffered unit's contents back to its file when they have changed
   since it was attached or last written back. A unit simh reads whole into
   memory otherwise reaches its file only when it is detached. Call from the
   thread that runs the processor. */
simh_shim_status_t simh_shim_flush (const char *unit_name);

/* Set a device or unit parameter out of its modifier table, as scp's SET does:
   simh_shim_set("RQ0 RD54"), simh_shim_set("RQ UDA50"). */
simh_shim_status_t simh_shim_set (const char *setting);

/* Boot from a device, as scp's BOOT does: find it, and call its boot routine. */
simh_shim_status_t simh_shim_boot (const char *unit_name);

/* The VAX-11/780 console's command language, run against the console floppy:
   BOOT, @file, LOAD, DEPOSIT, EXAMINE, START, WAIT DONE and the rest, as
   vax780_console.c describes them. Commands are separated by semicolons, and
   BOOT and @ run command files off the floppy, whose lines the console prints
   on the terminal as it executes them.

   What the commands left the processor doing: */
typedef enum {
    SIMH_SHIM_CONSOLE_HALTED,       /* they ran out with nothing started */
    SIMH_SHIM_CONSOLE_RUNNING,      /* a START handed the processor over */
    SIMH_SHIM_CONSOLE_WAITING,      /* started, and WAIT DONE waits for it */
    SIMH_SHIM_CONSOLE_ERROR         /* a command failed; message says which */
} simh_shim_console_state_t;

/* Read every command file the line reaches and run the commands, until they
   end, START hands over or WAIT DONE waits. A failure writes the file, line and
   command into message. Call on the processor's thread with it stopped. */
simh_shim_console_state_t simh_shim_console_begin (const char *commands,
                                                   char *message, size_t message_size);

/* The program a WAIT DONE waited for is done: run the commands after it. */
simh_shim_console_state_t simh_shim_console_resume (char *message, size_t message_size);

/* True when a run ended because the program signalled the console that it is
   done, by halting or by the console's software-done code. */
int simh_shim_console_done (simh_shim_status_t status);

/* True when a run ended on the software-done code alone, which outside a WAIT
   DONE asks nothing of the console: the processor goes on. */
int simh_shim_software_done (simh_shim_status_t status);

/* True when a run ended because the operating system asked the console to
   reboot the machine. */
int simh_shim_reboot_requested (simh_shim_status_t status);

/* The operator's console: CTRL/P on the console terminal, the >>> prompt and
   the commands typed at it, as vax780_console.c describes them. Enabled, the
   console starts in program I/O mode, with the keyboard the program's. */
void simh_shim_console_operator_enable (int on);

/* What the operator's commands ask of the embedding. */
typedef enum {
    SIMH_SHIM_OPERATOR_NONE,        /* nothing */
    SIMH_SHIM_OPERATOR_HALT,        /* HALT: stop the processor, then report
                                       it with simh_shim_console_halted() */
    SIMH_SHIM_OPERATOR_RUN,         /* run the processor from where it stands */
    SIMH_SHIM_OPERATOR_WAIT,        /* run it, and WAIT DONE waits for it as
                                       for simh_shim_console_begin() */
    SIMH_SHIM_OPERATOR_STEP         /* execute *count instructions one at a
                                       time, simh_shim_console_halted() after
                                       each, with the prompt after the last */
} simh_shim_operator_t;

/* Read what the operator typed and run it. Call on the processor's thread
   between batches, running or not; running says which. */
simh_shim_operator_t simh_shim_console_operator (int running, unsigned *count);

/* The processor has stopped: the console prints HALTED AT and the program
   counter, after why when there is a reason to give, and takes the keyboard.
   prompt puts the >>> prompt after it. */
void simh_shim_console_halted (const char *why, int prompt);

/* The console takes the keyboard and prompts, with the processor halted. */
void simh_shim_console_prompt (void);

/* The processor was started from outside the console, by the front panel:
   the keyboard is the program's. */
void simh_shim_console_release (void);

/* Restarting a halted processor, and rebooting one whose operating system
   asked for it. The console keeps the warm and cold start flags the
   operating system clears once a restart or a boot has succeeded, and from
   them decides what is tried next: RESTAR.CMD first, then DEFBOO.CMD, then
   nothing, so a machine whose restart and boot both fail stays halted. A
   reboot request always boots. */
typedef enum {
    SIMH_SHIM_RESTART_WARM,         /* run RESTAR.CMD */
    SIMH_SHIM_RESTART_COLD,         /* bootstrap: DEFBOO.CMD */
    SIMH_SHIM_RESTART_NONE          /* both have failed: stay halted */
} simh_shim_restart_t;

simh_shim_restart_t simh_shim_console_restart (int reboot);

/* The restart or boot simh_shim_console_restart() chose has begun. Call after
   any reset of the machine, which clears the flags. */
void simh_shim_console_restart_begun (simh_shim_restart_t what);

/* The AUTO RESTART switch, as the console tells the operating system. A reset
   of the machine clears it, so it is set again after one. */
void simh_shim_console_auto_restart (int on);

/* How many bits of a character the console terminal shows: 8, or 7 for a
   terminal that ignores the parity bit an operating system may set. */
void simh_shim_console_8bit (int on);

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
    unsigned uba_dr;                                    /* adapter diagnostic control */
    unsigned nexus_req;                                 /* nexus requests, BR4..BR7 */
    unsigned intr_vector_cell;                          /* vector the adapter would return */
    unsigned intr_vector_stored;                        /* vector stored at the last grant */
    unsigned intr_level_stored;                         /* level it was stored at */
    double instructions;
} simh_shim_state_t;

void simh_shim_state (simh_shim_state_t *state);

/* A device on the bus is interrupting, at the bus request level it was granted
   at. Returns zero when the processor could not take it - an unknown level, or
   one still pending - and the caller should leave the request asserted.

   Called from whatever thread watches the bus, not from the one running the
   processor. */
int simh_shim_bus_interrupt (unsigned vector, unsigned br_level);

/* The highest bus request level whose vector the adapter is still holding,
   4 to 7, or zero when it holds none. The adapter has one slot per level, so
   this is the level at which a further request must be held off until the
   processor has taken the one already latched. */
unsigned simh_shim_bus_interrupt_pending (void);

/* Where a device the core carries would answer, and whether it is taking part
   at all. A boot command reads its address out of the same descriptor and tells
   the bootstrap, so this is what the bootstrap will be looking for. The name
   may carry boot flags after it, as in "RQ0/R5:1". Returns zero when there is
   no such device. */
int simh_shim_device_info (const char *name, unsigned *base_addr, int *enabled);

/* Turn the core's own tracing on for one device, into a file, with every debug
   flag that device declares. A NULL name closes it. Zero when there is no such
   device or the file could not be opened. */
int simh_shim_debug_device (const char *name, const char *filename);

/* Put the processor's memory where the embedding wants it, rather than where
   the core allocated it. A machine whose devices reach memory over a real bus
   needs that memory somewhere the bus hardware can see, which is not the heap.
   Call once, after the first reset has sized the memory; the core's own
   allocation is released. Zero when the area offered is too small.

   The core must not be asked to change its memory size afterwards - it would
   free an allocation it does not own. */
int simh_shim_memory_relocate (void *area, unsigned bytes);

/* Give the memory back to the core, so it holds an allocation it may free.
   Required before anything that resizes memory. Harmless when the core already
   owns it. */
int simh_shim_memory_restore (void);

/* How much memory the machine has, so the embedding can check its area first. */
unsigned simh_shim_memory_size (void);

/* Copy the UNIBUS adapter's map registers out, for hardware that has to do the
   same translation the core does. Returns how many were copied. */
unsigned simh_shim_map_export (uint32_t *dest, unsigned count);

/* Keep the last N instructions the processor executed, and write them out.
   Zero instructions switches the history off and frees it. The dump is the
   oldest kept first, which is the order they ran in. */
int simh_shim_history (unsigned instructions);
int simh_shim_history_dump (const char *filename, unsigned instructions);

/* Fix the instructions-per-second the clock side works with, instead of
   measuring it. On emulated time the rate is defined - a microsecond of
   machine time is a fixed number of instructions - and a measured rate
   jitters with every calibration window, which the guest's timer self-test
   reads as a broken interval clock. Zero returns to measuring. */
void simh_shim_set_fixed_ips (double ips);

/* Whether the bus, rather than a device the core carries, still answers this
   UNIBUS address. */
int simh_shim_bus_owns (unsigned unibus_addr);

/* Watch this UNIBUS address - the boot device's - on every reassert pass: a
   device reset inside the core re-runs the auto-configuration, which takes
   the address back for the core without touching the rest of the page, and
   the whole-page witness never notices. The count says how often the claim
   had to be healed that way. */
void simh_shim_bus_watch (unsigned unibus_addr);
unsigned simh_shim_bus_reclaims (void);

/* Where the adapter's map registers send an eighteen bit UNIBUS address. Zero
   when the entry covering it is not valid, leaving *phys alone. Asking leaves
   the adapter's own error state as it was, so a trace may call it freely. */
int simh_shim_map_addr (unsigned unibus_addr, unsigned *phys);

/* One longword of the processor's memory, by physical address, which is what a
   780's console EXAMINE and DEPOSIT reach. Zero when the address is outside the
   memory the machine has. The address is taken to the longword it lies in. */
int simh_shim_mem_read (unsigned phys_addr, unsigned *value);
int simh_shim_mem_write (unsigned phys_addr, unsigned value);

/* A device on the bus is moving data to or from memory. The address is the
   eighteen bit one the device drives; what it reaches is the processor's memory
   through the adapter's map registers, which is where an unallocated map entry
   fails the transfer. write is the device's direction: a device performing DATO
   is writing memory. Returns zero when the transfer did not complete.

   Called from the device's thread, not the processor's. */
int simh_shim_bus_dma (int write, unsigned addr, uint16_t *buffer, unsigned words);

/* Put back what a rebuild of the core's I/O tables took from the bus: the
   dispatch entries for the addresses it answers and the vector the adapter
   hands over for each pending request. Idempotent and cheap when nothing was
   lost. build_dib_tab() calls it, because sim_instr() evaluates pending
   interrupts right after that rebuild and before any event runs. */
int simh_shim_bus_reassert (void);

/* One of the adapter's map registers has taken a new value. The bus hardware
   translates a device's address through a copy of the map, and a device can
   start a transfer without the processor executing another bus cycle first, so
   a change has to reach that copy when it is made rather than when the
   processor is next seen. Called on the processor's thread.

   simh_shim_map_changed is supplied by the device that owns the machine; the
   core calls it. */
void simh_shim_map_changed (unsigned index, uint32_t value);

/* True when the last run ended because the processor executed HALT. */
int simh_shim_halted (simh_shim_status_t status);

/* True when the last run ended only because the batch ran out, which is the
   ordinary end of a pass and not a reason to stop the machine. */
int simh_shim_batch_ended (simh_shim_status_t status);

#ifdef __cplusplus
}
#endif

#endif /* SIMH_SHIM_H_ */
