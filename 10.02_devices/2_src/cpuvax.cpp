/* cpuvax.cpp: the VAX-11/780 as an emulated processor

 Copyright (c) 2026, Hans Huebner
 hans@huebner.org
 MIT license, see qunibusdevice.hpp for the full text.

 cpuvax.hpp says what this is and why. Here is how it meets the core: the seam
 of cpuvax/simh_shim.h wants a console byte channel, a source of elapsed time
 and a loop that runs the processor in batches, and this file supplies all
 three from what the application already has.

 Only this file includes the seam, so cpuvax.hpp stays free of the core's
 types - the same arrangement cpu20.cpp has with the KA11.
 */

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include "logger.hpp"
#include "timeout.hpp"
#include "mailbox.h"
#include "qunibus.h"
#include "qunibusadapter.hpp"
#include "ddrmem.h"
#include "iopageregister.h"
#include "cpuvax.hpp"
#include "medium_write_protect.hpp"

#include "cpu_bus_adapter.h"          // unibone_prioritylevelchange()
#include "cpuvax/simh_shim.h"

/* What one instruction is worth on the emulation's own clock. A VAX-11/780 runs
   at about half a million instructions per second, so a real one spends two
   microseconds on an average instruction, and the emulated clock is charged
   that for each one executed.

   Which clock the guest measures itself against is a separate question, and
   cpu.cpp settles it the same way for the KA11: world time unless the build
   asks otherwise, so an operator's watch and the guest's agree even when the
   board runs the machine faster or slower than the original. */
#define VAX_INSTRUCTION_NS      2000

/* How long a bus cycle waits for an interrupt it has let through to arrive.
   The vector takes a few microseconds; the bound ends the wait for a request
   the arbitration holds back. */
#define BUS_INTR_DELIVERY_NS    200000

/* How long the console's WAIT DONE lets a program run before it gives up on
   it. The programs it waits for - the memory controller's ROM program among
   them - finish in a fraction of that. */
#define CONSOLE_WAIT_S          30

/* The processor the seam serves. Its callbacks are plain C functions and reach
   the device through this, and through the context the host structure carries. */
static cpuvax_c *the_vax = NULL;

cpuvax_c::cpuvax_c() :
    unibuscpu_c()
{
    name.value = "cpuvax";
    type_name.value = "VAX-11/780";
    log_label = "vax";

    // no registers on the bus, and no vector or slot of its own
    default_base_addr = 0;
    default_intr_vector = 0;
    default_intr_level = 0;
    priority_slot.value = 0;
    register_count = 0;

    emulation_speed.readonly = true;
    emulation_speed.value = 1;

    // What a VAX-11/780 did in a second, for the percentage beside the
    // instruction rate. The 780 is the machine the VUP is defined against, and
    // its native rate is about half a million instructions a second - the
    // "1 MIPS" it was sold as counts differently. See cpu20.cpp for what this
    // number is and is not.
    instructions.reference_per_second = 500000;

    // The most the range the board shares with the bus holds; see
    // configure_machine().
    memory_mb.value = 4;
    bootdevice.value = "RQ0";
    // the floppy is a file of the image tree, which is how the web interface
    // knows to offer the file browser for it
    image.content = parameter_c::CONTENT_IMAGE;
    batch_size.value = 10000;
    // The bus takes the I/O page whole by default: a machine whose peripherals
    // are on the bus wants nothing of the core's own answering there.
    bus_exclusive.value = true;
    // The peripherals of this machine are on the bus, and a device's transfer
    // goes out on it as well, which is what a device on a real UNIBUS does.
    // Clearing bus_dma puts the translation back in this program, which is
    // worth having to compare against but is not how the machine runs.
    bus_iopage.value = true;
    bus_dma.value = true;

    emulated_time.value = true;

    runmode.value = false;
    halt_switch.value = false;
    start_switch.value = false;
    continue_switch.value = false;
    auto_restart.value = false;
    console_8bit.value = false;

    // An examine or deposit is performed on every write, so the same address
    // can be read again or written twice running.
    bus_examine.action = true;
    bus_deposit.action = true;
    mem_examine.action = true;
    mem_deposit.action = true;

    // Running state, not configuration, so a saved machine does not carry a
    // stale program counter or a HALT nobody can see on screen.
    runmode.kind = parameter_c::PARAM_STATUS;
    pc.kind = parameter_c::PARAM_STATUS;
    psl.kind = parameter_c::PARAM_STATUS;
    cycle_count.kind = parameter_c::PARAM_STATUS;
    bus_cycles.kind = parameter_c::PARAM_STATUS;
    bus_timeouts.kind = parameter_c::PARAM_STATUS;
    bus_interrupts.kind = parameter_c::PARAM_STATUS;
    dma_words.kind = parameter_c::PARAM_STATUS;
    dma_failures.kind = parameter_c::PARAM_STATUS;
    dma_byte_offset.kind = parameter_c::PARAM_STATUS;
    ipl.kind = parameter_c::PARAM_STATUS;
    uba_init.kind = parameter_c::PARAM_STATUS;
    iopage_dispatches.kind = parameter_c::PARAM_STATUS;
    iopage_reclaims.kind = parameter_c::PARAM_STATUS;
    iopage_claimed.kind = parameter_c::PARAM_STATUS;
    bootdev_on_bus.kind = parameter_c::PARAM_STATUS;
    uba_cr.kind = parameter_c::PARAM_STATUS;
    intr_pending.kind = parameter_c::PARAM_STATUS;
    uba_dr.kind = parameter_c::PARAM_STATUS;
    nexus_req.kind = parameter_c::PARAM_STATUS;
    intr_vector_cell.kind = parameter_c::PARAM_STATUS;
    intr_vector_stored.kind = parameter_c::PARAM_STATUS;
    intr_level_stored.kind = parameter_c::PARAM_STATUS;
    halt_switch.kind = parameter_c::PARAM_STATUS;
    start_switch.kind = parameter_c::PARAM_STATUS;
    continue_switch.kind = parameter_c::PARAM_STATUS;
}

cpuvax_c::~cpuvax_c()
{
    the_flexi_timeout_controller->set_mode(flexi_timeout_c::world_time);
    the_vax = NULL;
}

/* ------------------------------------------------------------------------ */
/* The host the seam expects                                                 */
/*                                                                           */
/* The core is C and reaches back through function pointers, so these are     */
/* plain functions on the one installed processor.                           */
/* ------------------------------------------------------------------------ */

static int vax_console_get(void *context)
{
    cpuvax_c *vax = (cpuvax_c *) context;
    rs232byte_t byte;

    if (!vax->rs232adapter.rs232byte_rcv_poll(&byte))
        return -1;                              // nothing typed
    return byte.c & 0xff;
}

static void vax_console_put(void *context, int c)
{
    cpuvax_c *vax = (cpuvax_c *) context;
    rs232byte_t byte;

    memset(&byte, 0, sizeof byte);
    byte.c = (uint8_t) (c & 0xff);
    vax->rs232adapter.rs232byte_xmt_send(byte);
}

/* One register access on the UNIBUS. The processor is a bus master here like
   any other, so the access goes through qunibusadapter and appears on the bus
   as a DATI or a DATO; what comes back is whether the bus answered. */
static int vax_bus_read(void *context, unsigned addr, unsigned *data)
{
    cpuvax_c *vax = (cpuvax_c *) context;

    return vax->bus_read(addr, data) ? 1 : 0;
}

static int vax_bus_write(void *context, unsigned addr, unsigned data, int byte)
{
    cpuvax_c *vax = (cpuvax_c *) context;

    return vax->bus_write(addr, data, byte != 0) ? 1 : 0;
}

static void vax_bus_init(void *context)
{
    (void) context;
    qunibus->init();
}

/* A device answers a register access and interrupts in the same moment: the
   UDA50 shows the next initialisation step in SA and raises its request, and
   ULTRIX's probe returns on seeing the step and counts the controller as one
   that does not interrupt unless the interrupt has already been taken. A real
   processor takes it at the next instruction. Here the arbitration grants only
   when the processor lets it, so after each of its bus cycles the processor
   lets a waiting request through and waits the few microseconds the vector
   takes to reach it. A level whose slot in the adapter is still full is not
   granted, and is left waiting. */
void cpuvax_c::deliver_bus_interrupts(void)
{
    unsigned held = simh_shim_bus_interrupt_pending();

    if (!qunibusadapter->intr_requested_above(held))
        return;
    unibone_prioritylevelchange((uint8_t) held);
    unibone_grant_interrupts();

    uint64_t start = timeout_c::abstime_ns();
    while (simh_shim_bus_interrupt_pending() == held
            && qunibusadapter->intr_requested_above(held)
            && timeout_c::abstime_ns() - start < BUS_INTR_DELIVERY_NS)
        ;
}

bool cpuvax_c::bus_read(unsigned addr, unsigned *data)
{
    uint16_t word;

    bus_cycles.value++;
    qunibusadapter->cpu_DATA_transfer(data_transfer_request, QUNIBUS_CYCLE_DATI,
                                      addr, &word);
    if (!data_transfer_request.success) {
        bus_timeouts.value++;
        DEBUG("DATI %06o: no answer", addr);
        return false;
    }
    *data = word;
    DEBUG("DATI %06o = %06o", addr, (unsigned) word);
    iotrace_note(0, addr, word, true);
    deliver_bus_interrupts();
    return true;
}

bool cpuvax_c::bus_write(unsigned addr, unsigned data, bool byte)
{
    uint16_t word = (uint16_t) data;

    bus_cycles.value++;
    qunibusadapter->cpu_DATA_transfer(data_transfer_request,
                                      byte ? QUNIBUS_CYCLE_DATOB : QUNIBUS_CYCLE_DATO,
                                      addr, &word);
    if (!data_transfer_request.success) {
        bus_timeouts.value++;
        DEBUG("DATO%s %06o = %06o: no answer", byte ? "B" : "", addr, (unsigned) word);
        iotrace_note(byte ? 2 : 1, addr, word, false);
        return false;
    }
    DEBUG("DATO%s %06o = %06o", byte ? "B" : "", addr, (unsigned) word);
    iotrace_note(byte ? 2 : 1, addr, word, true);
    deliver_bus_interrupts();
    return true;
}

/* A console examine or deposit on the bus. A running processor makes it as
   one of its own transfers. A stopped one neither arbitrates nor drives the
   PRU's master side, so a transfer of its own would wait for a grant nobody
   gives and hold this thread for good - and the next disable would cancel the
   thread with its locks held. The board makes the cycle itself then, as it
   does to load memory into a stopped machine, and bounds the wait so an
   address nothing answers reads as no answer. */
bool cpuvax_c::console_bus_access(uint8_t cycle, unsigned addr, unsigned *data)
{
    uint16_t word = (uint16_t) *data;

    if (machine_running)
        return cycle == QUNIBUS_CYCLE_DATI ? bus_read(addr, data)
               : bus_write(addr, *data, /*byte*/false);
    bus_cycles.value++;
    if (!qunibus->dma(/*blocking*/true, cycle, addr, &word, 1, /*share_bus*/false,
                      /*timeout_ms*/100)) {
        bus_timeouts.value++;
        return false;
    }
    if (cycle == QUNIBUS_CYCLE_DATI)
        *data = word;
    return true;
}

/* The ring keeps only the watched device's registers, so a stalled dialogue
   is not scrolled away by the traffic of a healthy one. Two threads write it -
   the processor's transfers and the adapter's interrupt injections - so the
   slot is claimed with an atomic counter; a torn entry costs a diagnostic
   line, not correctness. */
void cpuvax_c::iotrace_note(uint8_t op, uint32_t addr, uint16_t value, bool ok)
{
    struct timespec ts;

    if (op != 3) {
        if (iotrace_base == 0 || addr < iotrace_base || addr > iotrace_base + 2)
            return;
    }
    uint64_t seq = __sync_fetch_and_add(&iotrace_seq, 1);
    iotrace_entry *e = &iotrace[seq % IOTRACE_ENTRIES];
    clock_gettime(CLOCK_REALTIME, &ts);
    e->ns = (uint64_t) ts.tv_sec * 1000000000ull + ts.tv_nsec;
    e->addr = addr;
    e->value = value;
    e->op = op;
    e->ok = ok;
    e->seq = seq + 1;                   // nonzero marks the entry written
}

void cpuvax_c::iotrace_dump(void)
{
    static const char *opname[] = {"DATI", "DATO", "DATOB", "INTR"};
    uint64_t newest = iotrace_seq;
    uint64_t oldest = newest > IOTRACE_ENTRIES ? newest - IOTRACE_ENTRIES : 0;

    INFO("register-access trace, oldest first:");
    for (uint64_t s = oldest; s < newest; s++) {
        iotrace_entry e = iotrace[s % IOTRACE_ENTRIES];
        if (e.seq != s + 1)
            continue;                   // overwritten while dumping
        unsigned us = (unsigned) ((e.ns / 1000) % 1000000);
        unsigned sec = (unsigned) ((e.ns / 1000000000ull) % 86400);
        INFO("  #%llu %02u:%02u:%02u.%06u %-5s %06o = %06o%s",
             (unsigned long long) e.seq, sec / 3600, (sec / 60) % 60, sec % 60,
             us, opname[e.op & 3], e.addr, e.value, e.ok ? "" : " (no answer)");
    }
}

/* The time the core measures itself against: whichever clock the emulation is
   running on, so the guest and the device models agree on how long a thing
   took. The core uses it to work out how fast it is being executed, and from
   that how many instructions one tick of the guest's interval clock is worth. */
static double vax_elapsed_usec(void *context)
{
    UNUSED(context);
    if (the_flexi_timeout_controller->mode == flexi_timeout_c::emulated_time)
        return (double) the_flexi_timeout_controller->emu_now_ns / 1000.0;
    return (double) flexi_timeout_controller_c::world_now_ns() / 1000.0;
}

/* ------------------------------------------------------------------------ */
/* Installing                                                                */
/* ------------------------------------------------------------------------ */

bool cpuvax_c::on_before_install(void)
{
    // One processor at a time, which qunibusadapter also insists on: it keeps a
    // single registered CPU to hand an interrupt to.
    unibuscpu_c *installed = qunibusadapter->installed_cpu();

    if (installed != NULL && installed != this) {
        ERROR("%s can not be enabled: CPU %s is already active, disable it first.",
              name.value.c_str(), installed->name.value.c_str());
        return false;
    }
    the_vax = this;
    // From here the board is the machine; see unibuscpu_c::bus_owner.
    bus_owner = true;

    halt_switch.value = false;
    start_switch.value = false;
    continue_switch.value = false;
    machine_running = false;
    runmode.value = false;

    // the processor's thread is not running yet, so the copy is taken here
    boot_command = bootdevice.value;
    // The console serves its operator from now on, and greets the terminal
    // with its prompt once the processor's thread runs: the machine comes up
    // halted.
    simh_shim_console_operator_enable(1);
    if (!configure_machine(/*start_pressed*/false))
        return false;                           // qunibusdevice_c aborts the enable
    console_announce = true;

    // The processor is the bus master whether or not it is executing, so a
    // console examine reaches a device register on a stopped machine too.
    if (bus_iopage.value) {
        mailbox_execute(ARM2PRU_CPU_ENABLE, 1);
        set_bus_arbitration(/*arbitrating*/true);
    }

    {
        simh_shim_state_t state;

        simh_shim_state(&state);
        INFO("VAX-11/780 ready, %u MB of memory, %u words of the I/O page on the bus",
             (unsigned) memory_mb.value, state.iopage_claimed);
    }
    return true;
}

void cpuvax_c::on_after_uninstall(void)
{
    // The board is a peripheral again, and must ask for the bus. Cleared
    // before machine_stop(), which is what writes the mode.
    bus_owner = false;
    machine_stop("processor disabled");
    halt_switch.value = true;
    start_switch.value = false;
    continue_switch.value = false;
}

/* Memory, the disk the machine boots from, and the bootstrap itself. Leaves
   the processor sitting at its boot address, stopped; boot_runs says whether
   START is to run it from there. */
bool cpuvax_c::configure_machine(bool start_pressed, const char *console_line)
{
    simh_shim_host_t host;
    simh_shim_status_t r;
    char setting[64];

    // The adapter's counters describe this machine, from the moment it is built.
    bus_cycles.value = 0;
    bus_timeouts.value = 0;
    bus_interrupts.value = 0;
    dma_words.value = 0;
    dma_failures.value = 0;
    dma_byte_offset.value = 0;
    pru_dma_counts_seen = pru_dma_counts();
    boot_runs = true;
    console_waiting = false;

    memset(&host, 0, sizeof host);
    host.context = this;
    host.console_get = vax_console_get;
    host.console_put = vax_console_put;
    host.elapsed_usec = vax_elapsed_usec;
    // Which MSCP controller the machine has. With a volume named here it is the
    // one inside the core, which is scaffolding and keeps its own addresses;
    // with none, every peripheral is on the bus and the I/O page belongs to it
    // whole. The controller inside stays configured either way, because a boot
    // command reads its address out of its descriptor and that address is only
    // put there by the auto-configuration a device takes part in.
    bool internal_disk = !bootimage.value.empty();

    // The core's own controller creates a volume it cannot open, and a
    // bootstrap reading an empty volume reports only that it found no boot
    // file, so a name that reaches no disk image is refused here.
    if (internal_disk) {
        struct stat st;

        if (stat(bootimage.value.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
            ERROR("bootimage \"%s\" names no disk image file; it is a volume for the"
                  " emulation's own test controller, and empty boots %s from the uda",
                  bootimage.value.c_str(), boot_command.c_str());
            return false;
        }
        if (st.st_size == 0) {
            ERROR("bootimage \"%s\" is empty", bootimage.value.c_str());
            return false;
        }
    }
    // The console floppy is the one boot device the processor carries whatever
    // the bus holds, and with no floppy in it VMB reports only that it found
    // nothing to boot.
    if (!strncasecmp(boot_command.c_str(), "CS", 2)
            && (boot_command.size() == 2 || !isalpha((unsigned char) boot_command[2]))
            && image.value.empty()) {
        ERROR("bootdevice %s is the console floppy, and image names no floppy for it",
              boot_command.c_str());
        return false;
    }
    if (!bus_iopage.value)
        WARNING("bus_iopage is 0: the processor reaches none of the devices on the bus");
    else if (!bus_dma.value)
        WARNING("bus_dma is 0: a transfer by a device on the bus is answered by this"
                " program, so a card that is not emulated cannot reach memory");

    if (bus_iopage.value) {
        host.bus_read = vax_bus_read;
        host.bus_write = vax_bus_write;
        host.bus_init = vax_bus_init;
        host.bus_owns_iopage = bus_exclusive.value ? 1 : 0;
    }
    host.message_file = stdout;
    simh_shim_bind(&host);

    if ((r = simh_shim_reset()) != 0) {
        ERROR("VAX reset failed: %s", simh_shim_status_text(r));
        return false;
    }

    // With the peripherals on the bus, the controller inside the core is put
    // out of the way for good rather than merely overwritten. Re-asserting a
    // claim it keeps taking back is a fight, and a fight has a loser: an access
    // that lands while the controller owns its address again reaches a
    // controller with no volume in it.
    //
    // It is disabled only after that first reset, because the address it
    // answers at is assigned by the auto-configuration, and a device disabled
    // before that takes no part in it and keeps no address. Disabling
    // afterwards leaves the address in its descriptor, which is what the boot
    // command reads to tell the bootstrap where to look.
    if (!internal_disk && bus_iopage.value) {
        if ((r = simh_shim_set("RQ DISABLED")) != 0) {
            ERROR("VAX cannot put its own controller aside: %s", simh_shim_status_text(r));
            return false;
        }
        if ((r = simh_shim_reset()) != 0) {
            ERROR("VAX reset failed: %s", simh_shim_status_text(r));
            return false;
        }
    } else if ((r = simh_shim_set("RQ ENABLED")) != 0) {
        ERROR("VAX cannot configure its own controller: %s", simh_shim_status_text(r));
        return false;
    }
    // The reset cleared what the console tells the operating system, the
    // AUTO RESTART switch among it.
    simh_shim_console_auto_restart(auto_restart.value ? 1 : 0);
    simh_shim_console_8bit(console_8bit.value ? 1 : 0);

    // Memory goes through the processor's own setting, which sizes the array
    // and tells the memory controllers what they answer for.
    // Sizing the memory frees the array, so the core has to be holding its own
    // allocation and not the shared range it was last given.
    if (!simh_shim_memory_restore()) {
        ERROR("VAX memory could not be handed back to the processor");
        return false;
    }
    snprintf(setting, sizeof setting, "CPU %uM", (unsigned) memory_mb.value);
    if ((r = simh_shim_set(setting)) != 0) {
        ERROR("VAX memory size %u MB refused: %s",
              (unsigned) memory_mb.value, simh_shim_status_text(r));
        return false;
    }

    // The processor's memory goes into the range the board shares with the PRU,
    // because that is the memory a device on the bus has to be able to reach.
    // Until it is there, a transfer can only be answered by this program.
    {
        unsigned have = sizeof(ddrmem->base_virtual->memory);
        unsigned want = simh_shim_memory_size();

        if (want > have) {
            ERROR("VAX memory is %u MB and the board shares %u MB with the bus;"
                  " lower the memory parameter", want >> 20, have >> 20);
            return false;
        }
        if (!simh_shim_memory_relocate((void *) &ddrmem->base_virtual->memory, have)) {
            ERROR("VAX memory could not be moved into the shared range");
            return false;
        }
        INFO("%u MB of memory in the range shared with the bus, at 0x%08x",
             want >> 20, ddrmem->base_physical);

        // With the transfers going out as bus cycles, the board has to answer
        // them: everything below the I/O page is memory, and the map registers
        // say which part of the processor's memory each page of it reaches.
        if (bus_iopage.value && bus_dma.value) {
            if (!ddrmem->set_range(DDRMEM_RANGE_MEMORY, 0,
                                   qunibus->iopage_start_addr - 2)) {
                ERROR("the board would not answer memory below the I/O page");
                return false;
            }
            INFO("the bus answers memory %s..%s through the adapter's map",
                 qunibus->addr2text(0),
                 qunibus->addr2text(qunibus->iopage_start_addr - 2));
        }
    }

    // A bootdevice that names none of the processor's devices is a line of
    // console commands, and so is the line a restart hands over.
    bool console_commands = (console_line != nullptr);
    const std::string command = console_line != nullptr ? console_line : boot_command;

    {
        // What the bootstrap will be told to look for. A controller that takes
        // no part in the auto-configuration keeps no address, and a bootstrap
        // sent to address zero fails without ever reaching the bus.
        unsigned ba = 0;
        int on = 0;

        boot_ba_known = false;
        if (console_commands || (!command.empty() && !internal_disk
                && !simh_shim_device_info(command.c_str(), &ba, &on))) {
            // The console's command files boot from the UDA50 at the address
            // RQ answers at, so that address is the one watched.
            console_commands = true;
            if (simh_shim_device_info("RQ", &ba, &on) && ba != 0) {
                boot_ba = ba;
                boot_ba_known = true;
                simh_shim_bus_watch(ba);
                iotrace_base = ba & 0777777;
            }
        } else if (simh_shim_device_info(command.c_str(), &ba, &on)) {
            boot_ba = ba;
            boot_ba_known = true;
            INFO("boot device %s: %s, would answer at %06o",
                 command.c_str(), on ? "configured" : "not configured", ba);
            // A reset of the controller the core carries re-runs the
            // auto-configuration, which takes these registers back for the
            // core mid-run; watching them heals the claim within the same
            // event pass instead of a batch later. The console floppy has no
            // registers on the bus, and its address reads as zero.
            if (ba != 0)
                simh_shim_bus_watch(ba);
            // the info's address form carries the core's physical prefix;
            // transfers name the eighteen bit bus address
            iotrace_base = ba & 0777777;
        } else if (!command.empty())
            ERROR("the processor has no device called %s", command.c_str());
    }

    if (internal_disk
            && (r = simh_shim_attach(boot_command.c_str(), bootimage.value.c_str())) != 0) {
        ERROR("VAX cannot attach %s to %s: %s", bootimage.value.c_str(),
              boot_command.c_str(), simh_shim_status_text(r));
        return false;
    }

    // A reset leaves the drive as it was, so the floppy goes in afresh with
    // every build of the machine and is read as it now stands on disk.
    if (!insert_floppy(image.value))
        return false;

    if (command.empty()) {
        INFO("no bootdevice set, so nothing is booted and the VAX stays halted");
        return true;
    }

    if (console_commands) {
        char message[256];

        if (!start_pressed) {
            INFO("bootdevice holds console commands, which START runs: %s",
                 command.c_str());
            boot_runs = false;
            return true;
        }
        INFO("running console commands: %s", command.c_str());
        switch (simh_shim_console_begin(command.c_str(), message, sizeof message)) {
        case SIMH_SHIM_CONSOLE_HALTED:
            INFO("the console commands started nothing, so the VAX stays halted");
            boot_runs = false;
            return true;
        case SIMH_SHIM_CONSOLE_RUNNING:
            return true;
        case SIMH_SHIM_CONSOLE_WAITING:
            console_wait_begin();
            return true;
        case SIMH_SHIM_CONSOLE_ERROR:
            break;
        }
        ERROR("console commands failed at %s", message);
        return false;
    }

    if ((r = simh_shim_boot(command.c_str())) != 0) {
        ERROR("VAX cannot boot %s: %s", command.c_str(), simh_shim_status_text(r));
        return false;
    }
    INFO("booting from %s%s", command.c_str(),
         internal_disk ? "" : " on the bus");
    return true;
}

/* ------------------------------------------------------------------------ */
/* Running                                                                   */
/* ------------------------------------------------------------------------ */

void cpuvax_c::machine_start(bool announce)
{
    simh_shim_state_t state;

    simh_shim_state(&state);
    instructions_at_start = (uint64_t) state.instructions;
    cycle_count.value = 0;
    machine_running = true;
    runmode.value = true;

    // Tell the board there is a processor on the bus. Until this, the PRU runs
    // neither the arbitration a device's request needs nor the state machine
    // that catches the INTR which follows it, so a device can raise a request
    // and see nothing come of it.
    mailbox_execute(ARM2PRU_CPU_ENABLE, 1);
    set_bus_arbitration(/*arbitrating*/true);
    publish_unibus_map();
    // On emulated time the interval clock and every device delay advance with
    // the instructions executed, not with the wall. VMS calibrates its
    // software timing loops (EXE$GL_TENUSEC and friends) once at boot by
    // counting loop iterations between clock ticks; against a wall-time clock
    // on a board whose one core is shared with the device workers, a boot
    // preempted during that window calibrates the loops a thousandfold too
    // short and every timed wait then expires early - the boot limps through
    // 50-second timeout retries instead of taking interrupts. The clock side
    // is pinned to the defined instruction rate for the same reason: a
    // measured rate jitters, and the interval count VMS's timer self-test
    // interpolates from it then reads as a broken clock.
    if (emulated_time.value) {
        the_flexi_timeout_controller->set_mode(flexi_timeout_c::emulated_time);
        simh_shim_set_fixed_ips(1e9 / VAX_INSTRUCTION_NS);
    } else {
        the_flexi_timeout_controller->set_mode(flexi_timeout_c::world_time);
        simh_shim_set_fixed_ips(0.0);
    }
    if (announce)
        INFO("VAX running");
}

void cpuvax_c::machine_stop(const char *why)
{
    if (!machine_running)
        return;
    machine_running = false;
    runmode.value = false;
    ddrmem->base_virtual->unibus_map_active = 0;
    mailbox_execute(ARM2PRU_CPU_ENABLE, 0);
    set_bus_arbitration(/*arbitrating*/false);
    the_flexi_timeout_controller->set_mode(flexi_timeout_c::world_time);
    simh_shim_set_fixed_ips(0.0);
    publish_status();
    if (why != NULL)
        INFO("VAX halted at PC %08x: %s", (unsigned) pc.value, why);
}

/* The PRU's counts of the memory cycles it answered in the processor's range,
   and of what the map did with them. They wrap, so a reader takes differences. */
cpuvax_c::pru_dma_counts_t cpuvax_c::pru_dma_counts(void)
{
    pru_dma_counts_t c = { 0, 0, 0 };

    if (pru_iopage_registers == nullptr)
        return c;
    c.words = pru_iopage_registers->memory_read_count[DDRMEM_RANGE_MEMORY]
              + pru_iopage_registers->memory_write_count[DDRMEM_RANGE_MEMORY];
    c.refused = pru_iopage_registers->map_refused_count;
    c.byte_offset = pru_iopage_registers->map_byte_offset_count;
    return c;
}

void cpuvax_c::publish_status(void)
{
    simh_shim_state_t state;

    simh_shim_state(&state);
    pc.value = state.pc;
    psl.value = state.psl;
    ipl.value = state.ipl;
    uba_init.value = state.uba_init;
    iopage_dispatches.value = state.iopage_dispatches;
    iopage_claimed.value = state.iopage_claimed;
    if (boot_ba_known)
        bootdev_on_bus.value = simh_shim_bus_owns(boot_ba) != 0;
    iopage_reclaims.value = simh_shim_bus_reclaims();
    // A transfer answered on the bus never reaches on_dma(); the PRU counts it
    // instead, and what it counted since the last pass is added here.
    {
        pru_dma_counts_t now = pru_dma_counts();

        if (bus_iopage.value && bus_dma.value) {
            dma_words.value += (uint32_t) (now.words - pru_dma_counts_seen.words);
            dma_failures.value += (uint32_t) (now.refused - pru_dma_counts_seen.refused);
            dma_byte_offset.value += (uint32_t) (now.byte_offset - pru_dma_counts_seen.byte_offset);
        }
        pru_dma_counts_seen = now;
    }
    uba_cr.value = state.uba_cr;
    intr_pending.value = simh_shim_bus_interrupt_pending();
    uba_dr.value = state.uba_dr;
    nexus_req.value = state.nexus_req;
    intr_vector_cell.value = state.intr_vector_cell;
    intr_vector_stored.value = state.intr_vector_stored;
    intr_level_stored.value = state.intr_level_stored;
    cycle_count.value = (uint64_t) state.instructions - instructions_at_start;

    // What the arbitration on the bus compares a device's request against.
    //
    // It is the adapter's level, not the processor's. A DW780 takes a device's
    // request whenever it has a slot for it, latches the vector and posts the
    // request to the processor, which services it once its own IPL permits - so
    // a processor running at IPL 31 must still let the grant happen, or a
    // device it is waiting for can never announce itself. The arbitration
    // therefore only has to hold off a level whose slot is still full.
    //
    // Published every pass rather than when it changes. Granting an INTR leaves
    // the arbitration holding its grants until the processor writes the level
    // again, which is how a real one is told the vector has been taken; a level
    // written only when it differs would leave the bus held after the first
    // interrupt, and a device's transfer waits on the same grants.
    if (bus_iopage.value)
        unibone_prioritylevelchange((uint8_t) simh_shim_bus_interrupt_pending());
}

/* Ask the processor's thread for a bus cycle and wait for it. A bus master's
   transfer is serviced by the adapter against this device's own request, so it
   has to be issued by the thread that owns it. */
bool cpuvax_c::wait_for_console_access(enum console_access_e what)
{
    timeout_c timeout;
    unsigned waited_ms = 0;

    console_access_ok = false;
    console_access = what;
    while (console_access != console_access_none && waited_ms < 1000) {
        timeout.wait_ms(2);
        waited_ms += 2;
    }
    if (console_access != console_access_none) {
        console_access = console_access_none;
        return false;
    }
    return true;
}

bool cpuvax_c::request_console_access(enum console_access_e what, unsigned addr)
{
    if (!enabled.value) {
        ERROR("%s is not enabled", name.value.c_str());
        return false;
    }
    console_access_addr = addr;
    if (!wait_for_console_access(what)) {
        ERROR("the processor did not answer for %06o", addr);
        return false;
    }
    if (!console_access_ok) {
        ERROR("no answer from %06o", addr);
        return false;
    }
    return true;
}

/* Hand the adapter's map registers to the hardware that has to make the same
   translation. A device asked to do something reaches memory some time later
   and on another thread, so the copy is taken when the processor writes a
   device's register - which is what starts a transfer - and the map it
   programmed beforehand is therefore the one the hardware sees.

   Nothing to publish while the transfer is answered here: the core's own map
   is read directly then. */
/* The whole map at once, which is what starting the machine needs: the copy the
   bus translates through has to agree with the adapter before any device can
   move data. Afterwards each register is carried over as it is written. */
void cpuvax_c::publish_unibus_map(void)
{
    volatile ddrmem_t *shared = ddrmem->base_virtual;

    if (!bus_iopage.value || !bus_dma.value) {
        shared->unibus_map_active = 0;
        return;
    }
    // The array is volatile because the PRU reads it; the export writes plain
    // words, so it is filled through a local and copied over.
    {
        uint32_t page[UNIBUS_MAP_REGISTERS];
        unsigned n = simh_shim_map_export(page, UNIBUS_MAP_REGISTERS);
        unsigned i;

        for (i = 0; i < n; i++)
            shared->unibus_map[i] = page[i];
    }
    shared->unibus_map_active = 1;
}

/* One register, carried over the moment the processor writes it. A device can
   begin a transfer without the processor executing another bus cycle - our MSCP
   controller finds a command by polling the ring - so a map the bus only learned
   about at the next cycle would translate that transfer with the entry it had
   before, and the data would land somewhere else in memory. */
void simh_shim_map_changed(unsigned index, uint32_t value)
{
    volatile ddrmem_t *shared = ddrmem->base_virtual;

    if (shared == NULL || index >= UNIBUS_MAP_REGISTERS)
        return;
    shared->unibus_map[index] = value;
}

void cpuvax_c::service_console_access(void)
{
    unsigned data = console_access_data;

    switch (console_access) {
    case console_access_none:
        return;
    case console_access_examine:
        console_access_ok = console_bus_access(QUNIBUS_CYCLE_DATI, console_access_addr, &data);
        console_access_data = data;
        break;
    case console_access_deposit:
        console_access_ok = console_bus_access(QUNIBUS_CYCLE_DATO, console_access_addr, &data);
        break;
    case console_access_mem_examine:
        console_access_ok = simh_shim_mem_read(console_access_addr, &data) != 0;
        console_access_data = data;
        break;
    case console_access_mem_deposit:
        console_access_ok = simh_shim_mem_write(console_access_addr, data) != 0;
        break;
    case console_access_floppy:
        console_access_ok = insert_floppy(console_access_text);
        break;
    case console_access_boot_command:
        boot_command = console_access_text;
        console_access_ok = true;
        break;
    }
    console_access = console_access_none;
}

/* WAIT DONE: the program the console started runs until it halts or signals
   done, and for at most CONSOLE_WAIT_S seconds, since a program that never
   does would otherwise hold the console forever. */
void cpuvax_c::console_wait_begin(void)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    console_waiting = true;
    console_wait_deadline = (uint64_t) now.tv_sec + CONSOLE_WAIT_S;
}

/* The program WAIT DONE waited for is done: the console runs the commands
   after it, on the processor's thread with the processor stopped between two
   batches. */
void cpuvax_c::console_resume(void)
{
    char message[256];

    console_waiting = false;
    switch (simh_shim_console_resume(message, sizeof message)) {
    case SIMH_SHIM_CONSOLE_HALTED:
        machine_stop("console commands done");
        return;
    case SIMH_SHIM_CONSOLE_RUNNING:
        return;
    case SIMH_SHIM_CONSOLE_WAITING:
        console_wait_begin();
        return;
    case SIMH_SHIM_CONSOLE_ERROR:
        break;
    }
    ERROR("console commands failed at %s", message);
    machine_stop("console command failed");
}

/* Put a floppy in the console drive, or with an empty path take it out. What
   the machine wrote to the one it held reaches that file first. An RX01 has no
   write protection, so a file the board may not write goes in as any other
   floppy: the machine may write to it, and what it writes is dropped when the
   floppy comes out, leaving the file as it was. */
bool cpuvax_c::insert_floppy(const std::string &path)
{
    simh_shim_status_t r;
    bool readonly = false;

    if (!path.empty()) {
        struct stat st;

        if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
            ERROR("console floppy \"%s\" names no image file", path.c_str());
            return false;
        }
        readonly = medium_write_protected(path);
    }
    if ((r = simh_shim_attach_medium("CS", path.c_str(), readonly ? 1 : 0)) != 0) {
        ERROR("console floppy \"%s\" could not go in: %s", path.c_str(),
              simh_shim_status_text(r));
        return false;
    }
    if (path.empty())
        INFO("console floppy drive empty");
    else
        INFO("console floppy %s in the drive%s", path.c_str(),
             readonly ? "; the file is protected and keeps none of what is written" : "");
    return true;
}

void cpuvax_c::worker(unsigned instance)
{
    UNUSED(instance);                           // only one
    timeout_c timeout;

    // Lowest priority, no wait: what is left of the processor goes to emulation.
    worker_init_realtime_priority(none_rt);
    timeout.wait_us(1);

    while (!workers_terminate) {
        service_console_access();

        // The core holds the console floppy in memory, so what the machine
        // writes is carried to the file every few seconds, and an unexpected
        // end of the program costs at most that much.
        {
            struct timespec now;

            clock_gettime(CLOCK_MONOTONIC, &now);
            if ((uint64_t) now.tv_sec >= floppy_flush_due) {
                simh_shim_status_t r = simh_shim_flush("CS");

                if (r != 0)
                    WARNING("console floppy could not be written: %s",
                            simh_shim_status_text(r));
                floppy_flush_due = (uint64_t) now.tv_sec + 3;
            }
        }

        // The console greets its terminal once the machine is built.
        if (console_announce) {
            console_announce = false;
            if (!machine_running)
                simh_shim_console_prompt();
        }

        if (start_switch.value) {
            start_switch.value = false;         // momentary action
            machine_stop(NULL);
            console_waiting = false;
            if (configure_machine(/*start_pressed*/true) && boot_runs && !halt_switch.value) {
                simh_shim_console_release();
                machine_start();
            } else
                simh_shim_console_prompt();
        }

        // CONTINUE picks up where the processor halted: the core keeps its
        // register state across a stop, so resuming is enabling it again.
        if (continue_switch.value) {
            continue_switch.value = false;      // momentary action
            if (!machine_running && !halt_switch.value) {
                simh_shim_console_release();
                machine_start();
            }
        }

        // The HALT switch stops the processor as the console's HALT does, and
        // the console takes the terminal.
        if (halt_switch.value && machine_running) {
            machine_stop("HALT switch");
            console_waiting = false;
            simh_shim_console_halted(NULL, 1);
        }

        console_operator();

        if (!machine_running) {
            timeout.wait_ms(10);                // nothing to run; leave the board alone
            continue;
        }

        // Let the arbitration on the bus give a device its GRANT, so a
        // request raised since the last pass becomes an INTR the processor can
        // take in this one. The KA11 does the same from its own service step.
        if (bus_iopage.value)
            unibone_grant_interrupts();

        uint64_t before = cycle_count.value;
        simh_shim_status_t r = simh_shim_run((int) batch_size.value);

        // The passes right after an interrupt hold the processor's answer to
        // it, so the history is written out once they have run.
        if (history_countdown > 0 && --history_countdown == 0) {
            if (simh_shim_history_dump("/tmp/vax-history.log", 0))
                INFO("instruction history written to /tmp/vax-history.log");
        }

        publish_status();

        // Charge the emulation clock for what the batch executed, so a device
        // model's delay lasts as long against the machine as its manual says.
        uint64_t executed = cycle_count.value - before;
        if (executed > 0)
            the_flexi_timeout_controller->emu_step_ns(
                (unsigned) (executed * VAX_INSTRUCTION_NS));

        // A program the console's WAIT DONE waits for has halted or signalled
        // it is done, and the console takes up the commands after it.
        if (console_waiting) {
            struct timespec now;

            clock_gettime(CLOCK_MONOTONIC, &now);
            if (simh_shim_console_done(r)) {
                console_resume();
                continue;
            }
            if ((uint64_t) now.tv_sec >= console_wait_deadline) {
                console_waiting = false;
                ERROR("console command WAIT DONE: the program did not signal done"
                      " within %u seconds", CONSOLE_WAIT_S);
                machine_stop("WAIT DONE timed out");
                continue;
            }
        }

        // The batch running out is the ordinary end of a pass. Anything else
        // stopped the processor - HALT is the one an operator expects - or is
        // a request to the console: a reboot, or a software-done signal that
        // no WAIT DONE waits for, which the processor runs on past.
        if (simh_shim_halted(r))
            processor_halted(NULL);
        else if (simh_shim_reboot_requested(r)) {
            machine_stop("the operating system asked for a reboot");
            console_restart(/*reboot*/true);
        } else if (simh_shim_software_done(r))
            continue;
        else if (!simh_shim_batch_ended(r))
            processor_halted(simh_shim_status_text(r));
    }
}

/* What the operator typed at the console, served between two batches. */
void cpuvax_c::console_operator(void)
{
    unsigned count = 0;

    switch (simh_shim_console_operator(machine_running ? 1 : 0, &count)) {
    case SIMH_SHIM_OPERATOR_NONE:
        return;
    case SIMH_SHIM_OPERATOR_HALT:
        machine_stop("console HALT");
        console_waiting = false;
        simh_shim_console_halted(NULL, 1);
        return;
    case SIMH_SHIM_OPERATOR_RUN:
        // the console's CONTINUE, START or BOOT releases the panel's HALT
        halt_switch.value = false;
        machine_start();
        return;
    case SIMH_SHIM_OPERATOR_WAIT:
        halt_switch.value = false;
        machine_start();
        console_wait_begin();
        return;
    case SIMH_SHIM_OPERATOR_STEP:
        console_step(count);
        return;
    }
}

/* NEXT: the processor executes one instruction for each step and halts after
   it, and the console reports where it stopped. */
void cpuvax_c::console_step(unsigned count)
{
    for (unsigned i = 0; i < count; i++) {
        machine_start(/*announce*/false);
        simh_shim_status_t r = simh_shim_run(1);
        machine_stop(NULL);
        bool ended = simh_shim_batch_ended(r) != 0;
        bool last = (i + 1 == count) || !ended;

        simh_shim_console_halted(ended || simh_shim_halted(r) ? NULL : simh_shim_status_text(r),
                                 last ? 1 : 0);
        if (last)
            return;
    }
}

/* The processor halted on its own. With AUTO RESTART off the console takes
   the terminal and waits at its prompt; on, it starts the machine again. */
void cpuvax_c::processor_halted(const char *why)
{
    machine_stop(why != NULL ? why : "HALT instruction");
    console_waiting = false;
    simh_shim_console_halted(why, auto_restart.value ? 0 : 1);
    if (auto_restart.value)
        console_restart(/*reboot*/false);
}

/* A restart, or the reboot the operating system asked for, as a 780's console
   runs it: RESTAR.CMD off the console floppy, whose restart referee halts when
   there is no system to restart, then DEFBOO.CMD. The restart runs on the
   machine as it stands, since the system to restart is in its memory; a boot
   builds the machine afresh, as START does. With no floppy in the drive the
   restart fails at once and the boot is bootdevice's. The processor is
   stopped when this is called. */
void cpuvax_c::console_restart(bool reboot)
{
    bool floppy = !image.value.empty();
    simh_shim_restart_t what = simh_shim_console_restart(reboot ? 1 : 0);

    if (what == SIMH_SHIM_RESTART_WARM) {
        simh_shim_console_restart_begun(what);
        if (floppy) {
            char message[256];

            INFO("AUTO RESTART: running RESTAR.CMD");
            switch (simh_shim_console_begin("@RESTAR", message, sizeof message)) {
            case SIMH_SHIM_CONSOLE_RUNNING:
                machine_start();
                return;
            case SIMH_SHIM_CONSOLE_WAITING:
                machine_start();
                console_wait_begin();
                return;
            case SIMH_SHIM_CONSOLE_HALTED:
                break;
            case SIMH_SHIM_CONSOLE_ERROR:
                WARNING("RESTAR.CMD failed at %s", message);
                break;
            }
        }
        // the restart started nothing, which is how it fails
        what = simh_shim_console_restart(0);
    }
    if (what == SIMH_SHIM_RESTART_COLD) {
        INFO("%s: booting %s", reboot ? "reboot" : "AUTO RESTART",
             floppy ? "DEFBOO.CMD off the console floppy" : boot_command.c_str());
        bool built = configure_machine(/*start_pressed*/true, floppy ? "BOOT" : nullptr);

        // the build reset the flags; the boot is under way from here
        simh_shim_console_restart_begun(what);
        if (built && boot_runs) {
            simh_shim_console_release();
            machine_start();
            return;
        }
        if (!built)
            ERROR("the console could not boot the machine");
        simh_shim_console_prompt();
        return;
    }
    INFO("restart and boot have both failed, so the VAX stays halted");
    simh_shim_console_prompt();
}

/* ------------------------------------------------------------------------ */
/* The bus, which stage 1 does not have                                      */
/* ------------------------------------------------------------------------ */

void cpuvax_c::on_after_register_access(qunibusdevice_register_t *device_reg,
                                        uint8_t unibus_control, DATO_ACCESS access)
{
    UNUSED(device_reg);
    UNUSED(unibus_control);
    UNUSED(access);
    // no registers on the bus
}

/* A device on the bus is moving data. The memory it means is this processor's,
   reached through the adapter's map registers, so the transfer is answered here
   and never goes to the bus. */
bool cpuvax_c::on_dma(uint8_t qunibus_cycle, uint32_t unibus_addr,
                      uint16_t *buffer, uint32_t wordcount)
{
    if (!bus_iopage.value)
        return false;                           // no adapter, no map
    if (bus_dma.value)
        return false;                           // the bus answers it, not us

    // The device's direction: DATO and DATOB write memory, DATI reads it.
    bool write = (qunibus_cycle != QUNIBUS_CYCLE_DATI);

    // The first transfers of a machine's life are the ones worth seeing: the
    // command ring a bootstrap hands over, whether what the controller reads
    // back is what the processor wrote, and where the map registers send it -
    // a bootstrap loading an image writes consecutive pages, and a translation
    // that piles them on one address shows up here before anywhere else.
    static unsigned traced = 0;
    unsigned phys = 0;
    int mapped = simh_shim_map_addr(unibus_addr, &phys);

    // The byte offset bit of a map entry lands the transfer one byte up, which
    // is what an odd processor address for an even bus address means.
    if (mapped && (phys & 1))
        dma_byte_offset.value++;

    if (traced < 200) {
        traced++;
        DEBUG("dma %s %06o -> %s%08x, %u words, first %06o %06o",
              write ? "write" : "read", (unsigned) unibus_addr,
              mapped ? "" : "invalid ", phys, (unsigned) wordcount,
              (unsigned) buffer[0], wordcount > 1 ? (unsigned) buffer[1] : 0);
    }

    if (!simh_shim_bus_dma(write ? 1 : 0, unibus_addr, buffer, wordcount)) {
        dma_failures.value++;
        WARNING("the map registers refused %u words at %06o",
                (unsigned) wordcount, (unsigned) unibus_addr);
        return true;                            // answered, and it failed
    }
    if (traced <= 200 && !write)
        DEBUG("dma read  %06o gave %06o %06o", (unsigned) unibus_addr,
              (unsigned) buffer[0], wordcount > 1 ? (unsigned) buffer[1] : 0);
    dma_words.value += wordcount;
    return true;
}

/* A device on the bus is interrupting. The bus ranks its requests BR4 to BR7
   and the VAX ranks its own IPL 14 to 17; the UNIBUS adapter is what lines the
   two up, one for one, and hands the processor the vector at the matching
   level. */
void cpuvax_c::on_interrupt(uint16_t vector, uint8_t level)
{
    DEBUG("INTR vector %06o at BR%u", (unsigned) vector, (unsigned) level);
    if (!simh_shim_bus_interrupt(vector, level)) {
        WARNING("interrupt vector %06o at BR%u not taken", (unsigned) vector,
                (unsigned) level);
        iotrace_note(3, vector, level, false);
        return;
    }
    iotrace_note(3, vector, level, true);
    bus_interrupts.value++;
    if (core_history.value)
        history_countdown = 2;                  // let the dispatch run, then dump
}

void cpuvax_c::on_power_changed(signal_edge_enum aclo_edge, signal_edge_enum dclo_edge)
{
    unibuscpu_c::on_power_changed(aclo_edge, dclo_edge);
}

void cpuvax_c::on_init_changed(void)
{
    // A machine that is running is not reset by INIT; the console START switch
    // is what rebuilds it.
}

bool cpuvax_c::on_param_changed(parameter_c *param)
{
    if (param == &enabled) {
        bool ok = qunibusdevice_c::on_param_changed(param);

        // Disabled, the processor's thread has stopped, and the floppy comes
        // out of the drive here, which writes what the machine wrote to it.
        // The parameter keeps naming it for the next time.
        if (ok && !enabled.new_value)
            simh_shim_attach_medium("CS", NULL, 0);
        return ok;
    }

    // The floppy is changed by the processor's own thread, which is the one
    // reading it; a machine that is not enabled takes it when it is built.
    // The boot command is read when START builds the machine. A running
    // processor's thread holds its own copy, which this hands the new one to.
    if (param == &bootdevice) {
        if (enabled.value) {
            console_access_text = bootdevice.new_value;
            if (!wait_for_console_access(console_access_boot_command)) {
                ERROR("the processor did not take the new bootdevice");
                return false;
            }
        }
        return true;
    }

    // The switch is read by the console when the processor halts, and shown to
    // the operating system through the console's communication region.
    if (param == &auto_restart) {
        auto_restart.value = auto_restart.new_value;
        if (enabled.value)
            simh_shim_console_auto_restart(auto_restart.value ? 1 : 0);
        return true;
    }

    if (param == &console_8bit) {
        console_8bit.value = console_8bit.new_value;
        if (enabled.value)
            simh_shim_console_8bit(console_8bit.value ? 1 : 0);
        return true;
    }

    if (param == &image) {
        if (enabled.value) {
            console_access_text = image.new_value;
            if (!wait_for_console_access(console_access_floppy)) {
                ERROR("the processor did not take the console floppy");
                return false;
            }
            if (!console_access_ok)
                return false;
        }
        image.value = image.new_value;
        return true;
    }

    // A configuration apply puts every parameter back to its default, and a
    // console access is an action, not a setting: during an apply the value is
    // taken and no bus cycle is made.
    if (config_apply_immediate && (param == &bus_examine || param == &bus_deposit
            || param == &mem_examine || param == &mem_deposit))
        return true;

    // A console examine or deposit is performed by the processor's own thread,
    // which is the bus master; this one only asks and waits for the answer.
    if (param == &bus_examine) {
        if (!request_console_access(console_access_examine, bus_examine.new_value))
            return false;
        bus_examine.value = bus_examine.new_value;
        bus_data.value = console_access_data;
        INFO("%06o = %06o", bus_examine.value, bus_data.value);
        return true;
    }
    if (param == &bus_deposit) {
        console_access_data = bus_data.value;
        if (!request_console_access(console_access_deposit, bus_deposit.new_value))
            return false;
        bus_deposit.value = bus_deposit.new_value;
        INFO("%06o := %06o", bus_deposit.value, bus_data.value);
        return true;
    }
    if (param == &iotrace_dump_switch) {
        if (iotrace_dump_switch.new_value) {
            iotrace_dump();
            // with the history armed, write it too: a stall that takes no
            // interrupts never triggers the automatic dump
            if (core_history.value
                    && simh_shim_history_dump("/tmp/vax-history.log", 0))
                INFO("instruction history written to /tmp/vax-history.log");
        }
        iotrace_dump_switch.value = false;  // momentary action
        return true;
    }
    if (param == &core_debug) {
        std::string want = core_debug.new_value;
        char path[128];

        if (want.empty()) {
            simh_shim_debug_device(NULL, NULL);
            INFO("core tracing off");
        } else {
            snprintf(path, sizeof path, "/tmp/simh-%s.log", want.c_str());
            if (!simh_shim_debug_device(want.c_str(), path)) {
                ERROR("no device %s in the core, or %s could not be opened",
                      want.c_str(), path);
                return false;
            }
            INFO("tracing %s into %s", want.c_str(), path);
        }
        core_debug.value = want;
        return true;
    }
    if (param == &core_history) {
        // The core frees and reallocates the ring, and the thread running
        // instructions writes into it, so the size is only settable while that
        // thread is not running.
        if (machine_running) {
            ERROR("core_history can only be changed while the processor is stopped");
            return false;
        }
        if (!simh_shim_history(core_history.new_value)) {
            ERROR("the core would not keep %u instructions of history",
                  (unsigned) core_history.new_value);
            return false;
        }
        core_history.value = core_history.new_value;
        INFO("keeping %u instructions of history", (unsigned) core_history.value);
        return true;
    }
    if (param == &mem_examine) {
        if (!request_console_access(console_access_mem_examine, mem_examine.new_value))
            return false;
        mem_examine.value = mem_examine.new_value;
        mem_data.value = console_access_data;
        INFO("%08x = %08x", (unsigned) mem_examine.value, (unsigned) mem_data.value);
        return true;
    }
    if (param == &mem_deposit) {
        console_access_data = mem_data.value;
        if (!request_console_access(console_access_mem_deposit, mem_deposit.new_value))
            return false;
        mem_deposit.value = mem_deposit.new_value;
        INFO("%08x := %08x", (unsigned) mem_deposit.value, (unsigned) mem_data.value);
        return true;
    }

    // The machine is built when it is enabled, so its size and its volume are
    // read then; changing them under a running processor would describe a
    // machine that is not there.
    if ((param == &memory_mb || param == &bootimage
            || param == &bus_iopage || param == &bus_exclusive) && enabled.value) {
        ERROR("%s can only be changed while the processor is disabled",
              param->name.c_str());
        return false;
    }
    return qunibusdevice_c::on_param_changed(param);
}
