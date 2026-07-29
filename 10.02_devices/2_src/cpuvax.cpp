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

#include <stdlib.h>
#include <string.h>

#include "logger.hpp"
#include "timeout.hpp"
#include "mailbox.h"
#include "qunibus.h"
#include "qunibusadapter.hpp"
#include "cpuvax.hpp"

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

/* The processor the seam serves. Its callbacks are plain C functions and reach
   the device through this, and through the context the host structure carries. */
static cpuvax_c *the_vax = NULL;

cpuvax_c::cpuvax_c() :
    unibuscpu_c()
{
    name.value = "cpuvax";
    type_name.value = "cpuvax_c";
    log_label = "vax";

    // no registers on the bus, and no vector or slot of its own
    default_base_addr = 0;
    default_intr_vector = 0;
    default_intr_level = 0;
    priority_slot.value = 0;
    register_count = 0;

    emulation_speed.readonly = true;
    emulation_speed.value = 1;

    memory_mb.value = 8;
    bootdevice.value = "RQ0";
    batch_size.value = 10000;

    runmode.value = false;
    halt_switch.value = false;
    start_switch.value = false;

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
    ipl.kind = parameter_c::PARAM_STATUS;
    uba_init.kind = parameter_c::PARAM_STATUS;
    iopage_dispatches.kind = parameter_c::PARAM_STATUS;
    uba_cr.kind = parameter_c::PARAM_STATUS;
    halt_switch.kind = parameter_c::PARAM_STATUS;
    start_switch.kind = parameter_c::PARAM_STATUS;
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

bool cpuvax_c::bus_read(unsigned addr, unsigned *data)
{
    uint16_t word;

    bus_cycles.value++;
    qunibusadapter->cpu_DATA_transfer(data_transfer_request, QUNIBUS_CYCLE_DATI,
                                      addr, &word);
    if (!data_transfer_request.success) {
        bus_timeouts.value++;
        return false;
    }
    *data = word;
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
        return false;
    }
    return true;
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

    halt_switch.value = false;
    start_switch.value = false;
    machine_running = false;
    runmode.value = false;

    if (!configure_machine())
        return false;                           // qunibusdevice_c aborts the enable

    // The processor is the bus master whether or not it is executing, so a
    // console examine reaches a device register on a stopped machine too.
    if (bus_iopage.value) {
        mailbox->param = 1;
        mailbox_execute(ARM2PRU_CPU_ENABLE);
        qunibus->set_arbitrator_active(true);
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
    machine_stop("processor disabled");
    halt_switch.value = true;
    start_switch.value = false;
}

/* Memory, the disk the machine boots from, and the bootstrap itself. Leaves
   the processor sitting at its boot address, stopped. */
bool cpuvax_c::configure_machine(void)
{
    simh_shim_host_t host;
    simh_shim_status_t r;
    char setting[64];

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

    if (bus_iopage.value) {
        host.bus_read = vax_bus_read;
        host.bus_write = vax_bus_write;
        host.bus_owns_iopage = internal_disk ? 0 : 1;
    }
    host.message_file = stdout;
    simh_shim_bind(&host);

    if ((r = simh_shim_reset()) != 0) {
        ERROR("VAX reset failed: %s", simh_shim_status_text(r));
        return false;
    }

    // Memory goes through the processor's own setting, which sizes the array
    // and tells the memory controllers what they answer for.
    snprintf(setting, sizeof setting, "CPU %uM", (unsigned) memory_mb.value);
    if ((r = simh_shim_set(setting)) != 0) {
        ERROR("VAX memory size %u MB refused: %s",
              (unsigned) memory_mb.value, simh_shim_status_text(r));
        return false;
    }

    if (internal_disk
            && (r = simh_shim_attach(bootdevice.value.c_str(), bootimage.value.c_str())) != 0) {
        ERROR("VAX cannot attach %s to %s: %s", bootimage.value.c_str(),
              bootdevice.value.c_str(), simh_shim_status_text(r));
        return false;
    }

    if (bootdevice.value.empty()) {
        INFO("no bootdevice set, the VAX comes up with its console only");
        return true;
    }

    if ((r = simh_shim_boot(bootdevice.value.c_str())) != 0) {
        ERROR("VAX cannot boot %s: %s", bootdevice.value.c_str(), simh_shim_status_text(r));
        return false;
    }
    INFO("booting from %s%s", bootdevice.value.c_str(),
         internal_disk ? "" : " on the bus");
    return true;
}

/* ------------------------------------------------------------------------ */
/* Running                                                                   */
/* ------------------------------------------------------------------------ */

void cpuvax_c::machine_start(void)
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
    mailbox->param = 1;
    mailbox_execute(ARM2PRU_CPU_ENABLE);
    qunibus->set_arbitrator_active(true);
    published_priority = 0xff;              // republish on the next pass
#ifdef CPU_CONTROLLED_TIME
    // Every device model's delays are then measured against the machine rather
    // than against the board, at the cost of a guest clock that runs at
    // whatever multiple of real time the board manages.
    the_flexi_timeout_controller->set_mode(flexi_timeout_c::emulated_time);
#else
    the_flexi_timeout_controller->set_mode(flexi_timeout_c::world_time);
#endif
    INFO("VAX running");
}

void cpuvax_c::machine_stop(const char *why)
{
    if (!machine_running)
        return;
    machine_running = false;
    runmode.value = false;
    mailbox->param = 0;
    mailbox_execute(ARM2PRU_CPU_ENABLE);
    qunibus->set_arbitrator_active(false);
    the_flexi_timeout_controller->set_mode(flexi_timeout_c::world_time);
    publish_status();
    if (why != NULL)
        INFO("VAX halted at PC %08x: %s", (unsigned) pc.value, why);
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
    uba_cr.value = state.uba_cr;
    cycle_count.value = (uint64_t) state.instructions - instructions_at_start;

    // What the arbitration on the bus compares a device's request against. The
    // VAX ranks its own interrupts IPL 14 to 17 where the bus ranks them BR4 to
    // BR7, and anything below that leaves every device free to interrupt.
    if (bus_iopage.value) {
        uint8_t arbitration_level = (state.ipl >= 0x14)
                                    ? (uint8_t) (state.ipl - 0x10) : 0;

        if (arbitration_level != published_priority) {
            published_priority = arbitration_level;
            unibone_prioritylevelchange(arbitration_level);
        }
    }
}

/* Ask the processor's thread for a bus cycle and wait for it. A bus master's
   transfer is serviced by the adapter against this device's own request, so it
   has to be issued by the thread that owns it. */
bool cpuvax_c::request_console_access(enum console_access_e what, unsigned addr)
{
    timeout_c timeout;
    unsigned waited_ms = 0;

    if (!enabled.value) {
        ERROR("%s is not enabled", name.value.c_str());
        return false;
    }
    console_access_addr = addr;
    console_access_ok = false;
    console_access = what;
    while (console_access != console_access_none && waited_ms < 1000) {
        timeout.wait_ms(2);
        waited_ms += 2;
    }
    if (console_access != console_access_none) {
        console_access = console_access_none;
        ERROR("the processor did not answer for %06o", addr);
        return false;
    }
    if (!console_access_ok) {
        ERROR("no answer from %06o", addr);
        return false;
    }
    return true;
}

void cpuvax_c::service_console_access(void)
{
    unsigned data = console_access_data;

    switch (console_access) {
    case console_access_none:
        return;
    case console_access_examine:
        console_access_ok = bus_read(console_access_addr, &data);
        console_access_data = data;
        break;
    case console_access_deposit:
        console_access_ok = bus_write(console_access_addr, data, false);
        break;
    }
    console_access = console_access_none;
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

        if (start_switch.value) {
            start_switch.value = false;         // momentary action
            machine_stop(NULL);
            if (configure_machine() && !halt_switch.value)
                machine_start();
        }

        if (halt_switch.value && machine_running)
            machine_stop("HALT switch");

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

        publish_status();

        // Charge the emulation clock for what the batch executed, so a device
        // model's delay lasts as long against the machine as its manual says.
        uint64_t executed = cycle_count.value - before;
        if (executed > 0)
            the_flexi_timeout_controller->emu_step_ns(
                (unsigned) (executed * VAX_INSTRUCTION_NS));

        // The batch running out is the ordinary end of a pass. Anything else
        // stopped the processor, and HALT is the one an operator expects.
        if (simh_shim_halted(r))
            machine_stop("HALT instruction");
        else if (!simh_shim_batch_ended(r))
            machine_stop(simh_shim_status_text(r));
    }
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

    // The device's direction: DATO and DATOB write memory, DATI reads it.
    bool write = (qunibus_cycle != QUNIBUS_CYCLE_DATI);

    if (!simh_shim_bus_dma(write ? 1 : 0, unibus_addr, buffer, wordcount)) {
        dma_failures.value++;
        WARNING("the map registers refused %u words at %06o",
                (unsigned) wordcount, (unsigned) unibus_addr);
        return true;                            // answered, and it failed
    }
    dma_words.value += wordcount;
    return true;
}

/* A device on the bus is interrupting. The bus ranks its requests BR4 to BR7
   and the VAX ranks its own IPL 14 to 17; the UNIBUS adapter is what lines the
   two up, one for one, and hands the processor the vector at the matching
   level. */
void cpuvax_c::on_interrupt(uint16_t vector, uint8_t level)
{
    if (!simh_shim_bus_interrupt(vector, level)) {
        WARNING("interrupt vector %06o at BR%u not taken", (unsigned) vector,
                (unsigned) level);
        return;
    }
    bus_interrupts.value++;
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
    if (param == &enabled)
        return qunibusdevice_c::on_param_changed(param);

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

    // The machine is built when it is enabled, so its size and its volume are
    // read then; changing them under a running processor would describe a
    // machine that is not there.
    if ((param == &memory_mb || param == &bootimage || param == &bootdevice
            || param == &bus_iopage) && enabled.value) {
        ERROR("%s can only be changed while the processor is disabled",
              param->name.c_str());
        return false;
    }
    return qunibusdevice_c::on_param_changed(param);
}
