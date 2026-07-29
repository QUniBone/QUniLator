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

 With bus_iopage set, the UNIBUS adapter's window on the I/O page is live: the
 processor's register accesses become bus cycles, a device's interrupt reaches
 the adapter's request registers at the level it was granted at, and a device's
 transfer is answered here through the adapter's map registers rather than on
 the bus. docs/vax-host.md is the running record of what that reaches.

 A machine is booted either from the disk simh carries inside the core or from
 the emulated UDA50 on the bus, which bootdevice and bootimage choose between.
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

    // The core's own tracing for one of the devices it carries, written to
    // /tmp/simh-<device>.log. What it says about a controller inside the
    // processor is what the same controller on the bus can be compared against.
    parameter_string_c core_debug = parameter_string_c(this, "core_debug", "cdbg",/*readonly*/false,
            "Trace a device the core carries, e.g. \"RQ\". Empty: off.");

    // The last N instructions the processor executed. Written to
    // /tmp/vax-history.log one pass after an interrupt arrives from the bus,
    // which is the moment worth seeing: what the processor does with a vector
    // it has been given cannot be watched from the device's side of the bus.
    parameter_unsigned_c core_history = parameter_unsigned_c(this, "core_history", "chst",/*readonly*/
                                        false, "", "%u", "Instructions of history kept. 0: off.", 32, 10);

    // The UNIBUS adapter's window on the I/O page. With it the processor's
    // register accesses become bus cycles and reach the emulated devices;
    // without it the machine sees only what simh carries inside it.
    parameter_bool_c bus_iopage = parameter_bool_c(this, "bus_iopage", "bio",/*readonly*/
                                  false, "1 = UNIBUS register accesses go on the bus.");

    // Whether the bus gets the I/O page whole, over the addresses the
    // controller inside the core claims, or only what that controller leaves.
    parameter_bool_c bus_exclusive = parameter_bool_c(this, "bus_exclusive", "bx",/*readonly*/
                                     false, "1 = the bus owns the I/O page over the core's own devices.");

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

    // The other half of what a 780's console reaches: the processor's own
    // memory, by physical address. Writing an address to mem_examine leaves the
    // longword there in mem_data; writing mem_deposit puts mem_data back.
    parameter_unsigned_c mem_examine = parameter_unsigned_c(this, "mem_examine", "mex",/*readonly*/
                                       false, "", "%08x", "Read this physical longword into mem_data.", 32, 16);
    parameter_unsigned_c mem_deposit = parameter_unsigned_c(this, "mem_deposit", "mdep",/*readonly*/
                                       false, "", "%08x", "Write mem_data to this physical longword.", 32, 16);
    parameter_unsigned_c mem_data = parameter_unsigned_c(this, "mem_data", "md",/*readonly*/
                                    false, "", "%08x", "Data of the last memory examine, or for the next deposit.", 32, 16);

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
    parameter_unsigned64_c iopage_dispatches = parameter_unsigned64_c(this, "iopage_dispatches", "iod",/*readonly*/
                                               true, "", "%u", "I/O page accesses the processor made", 63, 10);
    parameter_unsigned_c iopage_claimed = parameter_unsigned_c(this, "iopage_claimed", "ioc",/*readonly*/
                                          true, "", "%u", "I/O page words the bus answers", 16, 10);
    parameter_bool_c bootdev_on_bus = parameter_bool_c(this, "bootdev_on_bus", "bob",/*readonly*/
                                      true, "1 = the boot device's address is answered by the bus, now.");
    parameter_unsigned_c uba_cr = parameter_unsigned_c(this, "uba_cr", "ucr",/*readonly*/
                                  true, "", "%08x", "UNIBUS adapter control register.", 32, 16);
    // The bus request level whose vector the adapter is still holding, if any.
    // Non-zero for long says the processor is not taking what it was given.
    parameter_unsigned_c intr_pending = parameter_unsigned_c(this, "intr_pending", "ip",/*readonly*/
                                        true, "", "%u", "BR level whose vector the adapter holds.", 8, 10);
    parameter_unsigned_c uba_dr = parameter_unsigned_c(this, "uba_dr", "udr",/*readonly*/
                                  true, "", "%08x", "UNIBUS adapter diagnostic control register.", 32, 16);
    parameter_unsigned_c nexus_req = parameter_unsigned_c(this, "nexus_req", "nrq",/*readonly*/
                                     true, "", "%x", "Adapter requests the processor has not taken, BR4..BR7.", 8, 16);
    parameter_unsigned_c intr_vector_cell = parameter_unsigned_c(this, "intr_vector_cell", "ivc",/*readonly*/
                                            true, "", "%06o", "Vector the adapter holds for the bus at BR5.", 16, 8);
    parameter_unsigned_c intr_vector_stored = parameter_unsigned_c(this, "intr_vector_stored", "ivs",/*readonly*/
                                              true, "", "%06o", "Vector read back at the last grant.", 16, 8);
    parameter_unsigned_c intr_level_stored = parameter_unsigned_c(this, "intr_level_stored", "ils",/*readonly*/
                                             true, "", "%u", "Level index it was stored at.", 8, 10);
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
                            console_access_deposit, console_access_mem_examine,
                            console_access_mem_deposit };
    volatile enum console_access_e console_access = console_access_none;
    volatile unsigned console_access_addr = 0;
    volatile unsigned console_access_data = 0;
    volatile bool console_access_ok = false;

    void service_console_access(void);
    bool request_console_access(enum console_access_e what, unsigned addr);

    unsigned history_countdown = 0;     // passes left before the history is written
    bool machine_running = false;       // the core is executing, not halted
    uint64_t instructions_at_start = 0; // sim_gtime() when the machine started

    bool configure_machine(void);       // memory, disk, boot; leaves it ready to run
    void machine_start(void);
    void machine_stop(const char *why);
    void publish_status(void);
};

#endif
