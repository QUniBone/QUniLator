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

 With bus_iopage set, the UNIBUS adapter's window on the I/O page is live and
 the processor's register accesses become bus cycles, so the emulated devices
 of 10.02_devices answer them. What is still missing is the other direction: a
 device raising an interrupt cannot yet reach this processor, because the bus
 reports a vector without the request level a VAX needs to know which IPL to
 take it at - see docs/vax-host.md.

 A machine is booted from the disk simh carries inside the core, which is
 scaffolding until the emulated UDA50 on the bus takes over.
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
    void on_interrupt(uint16_t vector, uint8_t level) override;
    bool on_dma(uint8_t qunibus_cycle, uint32_t unibus_addr,
                uint16_t *buffer, uint32_t wordcount) override;
    void worker(unsigned instance) override;

    // The console terminal of the machine, which on a 780 is part of the
    // processor and not a device on the bus. It is carried on an rs232adapter
    // like the DL11's, so the web console binds to it the same way.
    rs232adapter_c rs232adapter;

    // A register access the processor makes on the UNIBUS, handled by
    // qunibusadapter as a DATI or DATO like any other bus master's.
    dma_request_c data_transfer_request = dma_request_c(this);

    parameter_unsigned_c memory_mb = parameter_unsigned_c(this, "memory", "mem",/*readonly*/
                                     false, "MB", "%u", "Main memory size in megabytes.", 8, 10);

    // The system volume, attached to the MSCP controller inside the core. It is
    // how stage 1 boots a machine at all, and it goes when the emulated UDA50
    // on the bus takes over.
    parameter_string_c bootimage = parameter_string_c(this, "bootimage", "bi",/*readonly*/false,
            "Disk image booted from the controller inside the processor. Empty: console only.");

    parameter_string_c bootdevice = parameter_string_c(this, "bootdevice", "bd",/*readonly*/false,
            "Unit the processor boots from, as the console names it.");

    // The UNIBUS adapter's window on the I/O page. With it the processor's
    // register accesses become bus cycles and reach the emulated devices;
    // without it the machine sees only what simh carries inside it.
    parameter_bool_c bus_iopage = parameter_bool_c(this, "bus_iopage", "bio",/*readonly*/
                                  false, "1 = UNIBUS register accesses go on the bus.");

    // What a real 780's console does with EXAMINE and DEPOSIT, and the only way
    // to reach a device register before an operating system has a driver for
    // it. Writing an address to bus_examine performs a DATI and leaves the
    // answer in bus_data; writing bus_deposit performs a DATO of bus_data.
    parameter_unsigned_c bus_examine = parameter_unsigned_c(this, "bus_examine", "bex",/*readonly*/
                                       false, "", "%06o", "Read this UNIBUS register into bus_data.", 18, 8);
    parameter_unsigned_c bus_deposit = parameter_unsigned_c(this, "bus_deposit", "bdep",/*readonly*/
                                       false, "", "%06o", "Write bus_data to this UNIBUS register.", 18, 8);
    parameter_unsigned_c bus_data = parameter_unsigned_c(this, "bus_data", "bd",/*readonly*/
                                    false, "", "%06o", "Data of the last examine, or for the next deposit.", 16, 8);

    parameter_unsigned64_c bus_cycles = parameter_unsigned64_c(this, "bus_cycles", "bc",/*readonly*/
                                        true, "", "%u", "UNIBUS register accesses made", 63, 10);
    parameter_unsigned64_c bus_timeouts = parameter_unsigned64_c(this, "bus_timeouts", "bt",/*readonly*/
                                          true, "", "%u", "UNIBUS accesses that were not answered", 63, 10);
    parameter_unsigned64_c bus_interrupts = parameter_unsigned64_c(this, "bus_interrupts", "bi",/*readonly*/
                                            true, "", "%u", "interrupts taken from the bus", 63, 10);
    parameter_unsigned64_c dma_words = parameter_unsigned64_c(this, "dma_words", "dw",/*readonly*/
                                       true, "", "%u", "words a device moved through the map", 63, 10);
    parameter_unsigned64_c dma_failures = parameter_unsigned64_c(this, "dma_failures", "df",/*readonly*/
                                          true, "", "%u", "transfers the map registers refused", 63, 10);

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
    parameter_unsigned_c ipl = parameter_unsigned_c(this, "IPL", "ipl",/*readonly*/
                               true, "", "%02x", "Interrupt priority level.", 8, 16);
    parameter_unsigned_c uba_init = parameter_unsigned_c(this, "uba_init", "ui",/*readonly*/
                                    true, "", "%u", "UNIBUS adapter init in progress.", 8, 10);
    parameter_unsigned_c uba_cr = parameter_unsigned_c(this, "uba_cr", "ucr",/*readonly*/
                                  true, "", "%08x", "UNIBUS adapter control register.", 32, 16);
    parameter_unsigned64_c cycle_count = parameter_unsigned64_c(this, "cycle_count", "cc",/*readonly*/
                                         true, "", "%u", "Instructions executed since the last start", 63, 10);

public:
    // reached from the seam's C callbacks
    bool bus_read(unsigned addr, unsigned *data);
    bool bus_write(unsigned addr, unsigned data, bool byte);

private:
    // A console examine or deposit asked for from the web thread. The transfer
    // itself belongs to the processor's own thread, which is the bus master, so
    // the request is left here and worker() performs it.
    enum console_access_e { console_access_none, console_access_examine,
                            console_access_deposit };
    volatile enum console_access_e console_access = console_access_none;
    volatile unsigned console_access_addr = 0;
    volatile unsigned console_access_data = 0;
    volatile bool console_access_ok = false;

    void service_console_access(void);
    bool request_console_access(enum console_access_e what, unsigned addr);

    bool machine_running = false;       // the core is executing, not halted
    uint8_t published_priority = 0xff;  // last level given to the arbitration
    uint64_t instructions_at_start = 0; // sim_gtime() when the machine started

    bool configure_machine(void);       // memory, disk, boot; leaves it ready to run
    void machine_start(void);
    void machine_stop(const char *why);
    void publish_status(void);
};

#endif
