/* cpuvax.hpp: the VAX-11/780 as an emulated processor

 Copyright (c) 2026, Hans Huebner
 hans@huebner.org
 MIT license, see qunibusdevice.hpp for the full text.

 The VAX host of docs/vax-unibus-plan.md, stage 1. A VAX-11/780 emulation runs
 as a device of the application, with its own memory and its own console, so
 the UNIBUS device models can eventually be driven by VMS and by the VAX
 diagnostics - software written by people who never saw this emulator.

 The processor itself is the vendored simh core of 91_3rd_party/simh_vax,
 reached through the seam cpuvax/simh_shim.h defines. This class is the
 embedding that seam expects: it supplies the console byte channel and the time
 source, and drives the core in batches from worker().

 It descends from unibuscpu_c rather than from cpu_base_c, which is shaped for
 the PDP-11: a sixteen bit program counter, a console switch register, and a
 trap through vector 24 on a power failure. A VAX has none of those.

 What is not here yet is the bus. Stage 1 gives the processor memory and a
 console and nothing else, which is what the plan asks of it; the UNIBUS
 adapter's register window and the interrupt path are stage 2. Until then a
 machine is booted from the disk simh carries inside the core, which is
 scaffolding and says so - see docs/vax-host.md.
 */
#ifndef _CPUVAX_HPP_
#define _CPUVAX_HPP_

#include "unibuscpu.hpp"
#include "rs232adapter.hpp"

class cpuvax_c: public unibuscpu_c {
public:
    cpuvax_c();
    ~cpuvax_c();

    bool on_before_install(void) override;
    void on_after_uninstall(void) override;
    bool on_param_changed(parameter_c *param) override;
    void on_after_register_access(qunibusdevice_register_t *device_reg,
                                  uint8_t unibus_control, DATO_ACCESS access) override;
    void on_power_changed(signal_edge_enum aclo_edge, signal_edge_enum dclo_edge) override;
    void on_init_changed(void) override;
    void on_interrupt(uint16_t vector) override;
    void worker(unsigned instance) override;

    // The console terminal of the machine, which on a 780 is part of the
    // processor and not a device on the bus. It is carried on an rs232adapter
    // like the DL11's, so the web console binds to it the same way.
    rs232adapter_c rs232adapter;

    parameter_unsigned_c memory_mb = parameter_unsigned_c(this, "memory", "mem",/*readonly*/
                                     false, "MB", "%u", "Main memory size in megabytes.", 8, 10);

    // The system volume, attached to the MSCP controller inside the core. It is
    // how stage 1 boots a machine at all, and it goes when the emulated UDA50
    // on the bus takes over.
    parameter_string_c bootimage = parameter_string_c(this, "bootimage", "bi",/*readonly*/false,
            "Disk image booted from the controller inside the processor. Empty: console only.");

    parameter_string_c bootdevice = parameter_string_c(this, "bootdevice", "bd",/*readonly*/false,
            "Unit the processor boots from, as the console names it.");

    // How many instructions worker() runs before it looks at the switches, the
    // power events and the terminate flag again. Long enough that the check
    // costs nothing, short enough that a HALT is not noticeably late.
    parameter_unsigned_c batch_size = parameter_unsigned_c(this, "batch", "b",/*readonly*/
                                      false, "", "%u", "Instructions executed per worker pass.", 32, 10);

    parameter_bool_c runmode = parameter_bool_c(this, "run_led", "r",/*readonly*/
                               true, "RUN LED: 1 = processor running, 0 = halted.");
    parameter_bool_c halt_switch = parameter_bool_c(this, "halt_switch", "h",/*readonly*/
                                   false, "HALT switch: 1 = processor stopped, 0 = processor may run.");
    parameter_bool_c start_switch = parameter_bool_c(this, "start_switch", "s",/*readonly*/
                                    false, "START action switch: 1 = reset the machine and boot it.");

    parameter_unsigned_c pc = parameter_unsigned_c(this, "PC", "pc",/*readonly*/
                              true, "", "%08x", "Program counter.", 32, 16);
    parameter_unsigned_c psl = parameter_unsigned_c(this, "PSL", "psl",/*readonly*/
                               true, "", "%08x", "Processor status longword.", 32, 16);
    parameter_unsigned64_c cycle_count = parameter_unsigned64_c(this, "cycle_count", "cc",/*readonly*/
                                         true, "", "%u", "Instructions executed since the last start", 63, 10);

private:
    bool machine_running = false;       // the core is executing, not halted
    uint64_t instructions_at_start = 0; // sim_gtime() when the machine started

    bool configure_machine(void);       // memory, disk, boot; leaves it ready to run
    void machine_start(void);
    void machine_stop(const char *why);
    void publish_status(void);
};

#endif
