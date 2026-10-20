/* cpuvax.hpp: the VAX-11/780 as an emulated processor

 Copyright (c) 2026, Hans Huebner
 hans@huebner.org
 MIT license, see qunibusdevice.hpp for the full text.

 The VAX host of docs/plans/vax-unibus-plan.md, stage 1. A VAX-11/780 emulation runs
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

 A machine boots from the emulated UDA50 on the bus, or, with bootimage set,
 from the disk simh carries inside the core; bootdevice names the unit.
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
    dma_request_c data_transfer_request{this};

    /// The memory the processor has, in megabytes: 1, 2 or 4. The board shares
    /// 4 MB with the bus, and the processor's memory has to fit in it so that a
    /// device's transfer can reach it, so 4 is the most a board can give. The
    /// size is read when the processor is enabled; changing it means disabling
    /// the processor first.
    parameter_unsigned_c memory_mb = parameter_unsigned_c(this, "memory", "mem",/*readonly*/
                                     false, "MB", "%u", "Main memory size in megabytes.", 8, 10);

    /// What START boots. `bootdevice` is the argument of a 780 console's BOOT
    /// command, in SIMH's spelling: a drive of the UDA50 at 772150, `RQ0` to
    /// `RQ3` for the `uda` drive whose `unit` is 0 to 3 — `uda0` to `uda3`
    /// unless a unit was changed — or a drive of the RK611 at 777440, `HK0` to
    /// `HK7` for the `hk` drive with that unit, from which VMB loads the system
    /// over the bus. Boot flags follow as `/R5:` and a hex value, so `RQ0 /R5:1`
    /// stops VMS at `SYSBOOT>` for a conversational boot; with none, R5 is 0.
    /// An empty `bootdevice` boots nothing: START resets the processor and it
    /// halts again at once.
    ///
    /// `bootimage` stays empty on a working machine: the disks are the `uda`
    /// drives, cards on the UNIBUS like any other. A file named here is
    /// instead attached to a test MSCP controller that is part of the VAX
    /// emulation itself and never appears on the UNIBUS. It answers at the same
    /// 772150, so `bus_exclusive` has to be 0 for it, and it boots whatever
    /// `bootdevice` names. It is there for testing the processor with no
    /// devices configured; enabling the processor fails when it names no disk
    /// image file or an empty one.
    ///
    /// Both are read when the processor is enabled.
    //
    // The controller inside the core is the stage 1 scaffolding of
    // docs/vax-host.md.
    parameter_string_c bootimage = parameter_string_c(this, "bootimage", "bi",/*readonly*/false,
            "Test only: disk image for the emulation's own MSCP controller. Empty: boot from the uda.");
    parameter_string_c bootdevice = parameter_string_c(this, "bootdevice", "bd",/*readonly*/false,
            "What START boots: RQ0..RQ3, HK0..HK7 or CS, with boot flags as RQ0 /R5:1. Empty: no boot.");

    /// The console floppy: the RX01 drive that is part of a 780's console,
    /// which VMS calls `CSA1:` once `SYSGEN CONNECT CONSOLE` has made it
    /// known. `image` names the RX01 image in it, 256256 bytes, and empty
    /// leaves the drive empty. It is the processor's own drive, separate from
    /// the RX11 or RX211 on the UNIBUS, which VMS calls `DY` or `DX`.
    ///
    /// `bootdevice` set to `CS` boots from it, which is how a 780 starts
    /// standalone BACKUP.
    ///
    /// The floppy can be changed at any time, as on the real drive. What the
    /// machine writes reaches the file within a few seconds, and when the
    /// floppy is changed or the processor disabled. An RX01 has no write
    /// protection, so a protected file (mode 444) goes in as any other floppy
    /// and keeps none of what is written to it.
    parameter_string_c image = parameter_string_c(this, "image", "img",/*readonly*/false,
            "Console floppy (CSA1:): RX01 image. Empty: no floppy.");

    /// Debugging aids for the emulation itself. `core_debug` names a device
    /// the processor carries inside it, such as `RQ`, and writes the core's own
    /// trace of it to `/tmp/simh-<device>.log` on the board. `core_history`
    /// keeps that many of the last instructions executed and writes them to
    /// `/tmp/vax-history.log` after an interrupt arrives from the bus; it can
    /// only be changed while the processor is halted.
    //
    // What the trace says about a controller inside the processor is what the
    // same controller on the bus can be compared against; what the processor
    // does with a vector it has been given cannot be watched from the
    // device's side of the bus.
    parameter_string_c core_debug = parameter_string_c(this, "core_debug", "cdbg",/*readonly*/false,
            "Trace a device the core carries, e.g. \"RQ\". Empty: off.");
    parameter_unsigned_c core_history = parameter_unsigned_c(this, "core_history", "chst",/*readonly*/
                                        false, "", "%u", "Instructions of history kept. 0: off.", 32, 10);

    /// How the processor's UNIBUS adapter meets the bus. All three are on for a
    /// working machine, and are there to take the bus out of the picture when
    /// comparing against the emulation alone.
    ///
    /// `bus_iopage`: the processor's accesses to the I/O page become bus cycles
    /// and reach the devices on the bus. Off, the machine sees only the devices
    /// the processor carries inside it.
    ///
    /// `bus_dma`: a device's transfer to or from memory goes out on the bus and
    /// is translated through the adapter's map registers there, as on a real
    /// UNIBUS, so a card that is not emulated can take part. Off, the transfer
    /// is answered inside the program.
    ///
    /// `bus_exclusive`: the bus answers the whole I/O page, including the
    /// addresses of the controllers the processor carries inside it. Off, it
    /// answers only the addresses those leave free.
    ///
    /// `bus_iopage` and `bus_exclusive` are read when the processor is enabled.
    parameter_bool_c bus_iopage = parameter_bool_c(this, "bus_iopage", "bio",/*readonly*/
                                  false, "1 = UNIBUS register accesses go on the bus. 0 hides every bus device.");
    // With bus_dma clear the adapter hands the transfer over before it reaches
    // the bus and the map translation is done here; set, the PRU answers it
    // from the same memory through the same map registers.
    parameter_bool_c bus_dma = parameter_bool_c(this, "bus_dma", "bdma",/*readonly*/
                               false, "1 = a device's transfer is answered on the bus, not here.");
    parameter_bool_c bus_exclusive = parameter_bool_c(this, "bus_exclusive", "bx",/*readonly*/
                                     false, "1 = the bus owns the I/O page over the core's own devices.");

    /// What a real 780's console does with EXAMINE and DEPOSIT on a UNIBUS
    /// register, and a way to reach a device before an operating system has a
    /// driver for it. Writing an address (octal, as a UNIBUS address) to
    /// `bus_examine` reads that register and leaves the word in `bus_data`. To
    /// write one, set `bus_data` first, then write the address to
    /// `bus_deposit`. Every write makes the access, the same address again
    /// included, so a register can be read repeatedly. The access is made by
    /// the processor, so it needs the processor enabled; applying a
    /// configuration makes none.
    parameter_unsigned_c bus_examine = parameter_unsigned_c(this, "bus_examine", "bex",/*readonly*/
                                       false, "", "%06o", "Read this UNIBUS register into bus_data.", 18, 8);
    parameter_unsigned_c bus_deposit = parameter_unsigned_c(this, "bus_deposit", "bdep",/*readonly*/
                                       false, "", "%06o", "Write bus_data to this UNIBUS register.", 18, 8);
    parameter_unsigned_c bus_data = parameter_unsigned_c(this, "bus_data", "bdat",/*readonly*/
                                    false, "", "%06o", "Data of the last examine, or for the next deposit.", 16, 8);

    /// The other half of what a 780's console reaches: the processor's own
    /// memory, by physical address, in hex. Writing an address to
    /// `mem_examine` leaves the longword there in `mem_data`; to change it, set
    /// `mem_data` and write the address to `mem_deposit`. As on the bus, every
    /// write makes the access.
    parameter_unsigned_c mem_examine = parameter_unsigned_c(this, "mem_examine", "mex",/*readonly*/
                                       false, "", "%08x", "Read this physical longword into mem_data.", 32, 16);
    parameter_unsigned_c mem_deposit = parameter_unsigned_c(this, "mem_deposit", "mdep",/*readonly*/
                                       false, "", "%08x", "Write mem_data to this physical longword.", 32, 16);
    parameter_unsigned_c mem_data = parameter_unsigned_c(this, "mem_data", "md",/*readonly*/
                                    false, "", "%08x", "Data of the last memory examine, or for the next deposit.", 32, 16);

    /// What the UNIBUS adapter has carried since the processor was enabled:
    /// register accesses, those nothing answered, and interrupts taken from
    /// devices. `bus_timeouts` rising during a boot is normal: the operating
    /// system probes addresses to find out which devices are there.
    ///
    /// `dma_words` counts the words devices moved through the map registers
    /// (a byte written counts as one), and `dma_failures` the transfers the
    /// map refused because no valid map register covers the page — on a real
    /// adapter, a device writing where the operating system has not mapped
    /// anything. All of them start again from 0 when the processor is
    /// enabled.
    parameter_unsigned64_c bus_cycles = parameter_unsigned64_c(this, "bus_cycles", "bc",/*readonly*/
                                        true, "", "%u", "UNIBUS register accesses made", 63, 10);
    parameter_unsigned64_c bus_timeouts = parameter_unsigned64_c(this, "bus_timeouts", "bt",/*readonly*/
                                          true, "", "%u", "UNIBUS accesses that were not answered", 63, 10);
    parameter_unsigned64_c bus_interrupts = parameter_unsigned64_c(this, "bus_interrupts", "bint",/*readonly*/
                                            true, "", "%u", "interrupts taken from the bus", 63, 10);
    parameter_unsigned64_c dma_words = parameter_unsigned64_c(this, "dma_words", "dw",/*readonly*/
                                       true, "", "%u", "words a device moved through the map", 63, 10);
    parameter_unsigned64_c dma_failures = parameter_unsigned64_c(this, "dma_failures", "df",/*readonly*/
                                          true, "", "%u", "transfers the map registers refused", 63, 10);
    /// A map entry with the byte offset bit set moves the transfer one byte
    /// up, so the processor's address is odd where the device's is even. This
    /// counts the transfers that took that path, which is the one a driver's
    /// unaligned buffer takes.
    parameter_unsigned64_c dma_byte_offset = parameter_unsigned64_c(this, "dma_byte_offset", "dbo",/*readonly*/
                                             true, "", "%u", "transfers the map registers offset by a byte", 63, 10);

    /// How many instructions the processor runs before it looks at the
    /// switches and the power state again. The default is long enough that the
    /// check costs nothing and short enough that a HALT takes effect at once.
    //
    // The pass is a run of worker(); the terminate flag is checked with the
    // switches.
    parameter_unsigned_c batch_size = parameter_unsigned_c(this, "batch", "b",/*readonly*/
                                      false, "", "%u", "Instructions executed per worker pass.", 32, 10);

    /// The machine's clock advances with the instructions it executes, at the
    /// 2 µs an instruction a real 780 takes, so the interval timer and every
    /// device delay keep pace with the processor whatever else the board is
    /// doing. VMS calibrates its timing loops once at boot; on the wall clock,
    /// a boot the board's other work interrupts during that moment mis-times
    /// every driver wait from then on. Set to 0, the clock follows the wall,
    /// which is there to compare against real-time behaviour.
    parameter_bool_c emulated_time = parameter_bool_c(this, "emulated_time", "et",/*readonly*/
                                     false, "1 = machine time advances with executed instructions.");

    /// The processor's front panel, which the dashboard's RUN, HALT, START
    /// and CONT controls drive. `start_switch` and `continue_switch` act once
    /// and fall back to 0. Enabling the processor builds the machine and
    /// leaves it halted; START boots it from `bootdevice`.
    parameter_bool_c runmode = parameter_bool_c(this, "run_led", "r",/*readonly*/
                               true, "RUN LED: 1 = processor running, 0 = halted.");
    parameter_bool_c halt_switch = parameter_bool_c(this, "halt_switch", "h",/*readonly*/
                                   false, "HALT switch: 1 = processor stopped, 0 = processor may run.");
    parameter_bool_c start_switch = parameter_bool_c(this, "start_switch", "s",/*readonly*/
                                    false, "START action switch: 1 = reset the machine and boot it.");
    parameter_bool_c continue_switch = parameter_bool_c(this, "continue_switch", "c",/*readonly*/
                                       false, "CONT action switch: 1 = resume execution after a HALT.");

    // the front panel's view of the console switches
    parameter_bool_c *panel_run_led(void) override { return &runmode; }
    parameter_bool_c *panel_halt_switch(void) override { return &halt_switch; }
    parameter_bool_c *panel_start_switch(void) override { return &start_switch; }
    parameter_bool_c *panel_continue_switch(void) override { return &continue_switch; }

    /// Where the processor is, read whenever it stops and as it runs: the
    /// program counter, the processor status longword and the interrupt
    /// priority level it runs at, all in hex. `uba_init`, `iopage_dispatches`
    /// and `iopage_claimed` describe the UNIBUS adapter: whether it is being
    /// initialised, how many I/O page accesses the processor has made, and how
    /// many words of the I/O page the bus answers (4096 is all of it).
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
    /// A reset of a controller the processor carries inside it takes the boot
    /// device's registers back from the bus. They are handed back to the bus
    /// at once; this counts how often that happened, and `bootdev_on_bus`
    /// says whether the bus has them now.
    parameter_unsigned_c iopage_reclaims = parameter_unsigned_c(this, "iopage_reclaims", "ior",/*readonly*/
                                           true, "", "%u", "boot device registers reclaimed from the core", 32, 10);
    parameter_bool_c bootdev_on_bus = parameter_bool_c(this, "bootdev_on_bus", "bob",/*readonly*/
                                      true, "1 = the boot device's address is answered by the bus, now.");

    /// The UNIBUS adapter's registers and interrupt state, for diagnosing a
    /// machine that is not taking a device's interrupts. `intr_pending` is the
    /// bus request level whose vector the adapter is still holding; non-zero
    /// for long says the processor is not taking what it was given.
    /// `nexus_req` shows the requests BR4 to BR7 the adapter has posted and
    /// the processor has not yet taken.
    parameter_unsigned_c uba_cr = parameter_unsigned_c(this, "uba_cr", "ucr",/*readonly*/
                                  true, "", "%08x", "UNIBUS adapter control register.", 32, 16);
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
    // How fast the machine is running, for the performance panel. Like the
    // PDP-11 cores it reads the opcode counter rather than counting again; that
    // the counter restarts with the machine costs nothing, the sampler dropping
    // any interval a total fell across. See metric.hpp and cpu.hpp.
    metric_c instructions{this, "instructions", metric_c::UNIT_INSTRUCTION,
                                     "Instructions",
                                     [this]() { return cycle_count.value; }};

    // an emulated processor, to the interfaces that group devices by what they are
    const char *category(void) const override { return "cpu"; }

    // reached from the seam's C callbacks
    bool bus_read(unsigned addr, unsigned *data);
    bool bus_write(unsigned addr, unsigned data, bool byte);

    // a console examine or deposit on the bus, running or stopped
    bool console_bus_access(uint8_t cycle, unsigned addr, unsigned *data);

public:
    // The processor's last accesses to the boot device's registers, and the
    // interrupts injected between them, kept in a small ring that is always
    // on: a stalled initialisation dialogue is diagnosed from what the
    // processor actually read, and logging at that moment changes the timing
    // enough to hide it. iotrace_dump writes the ring to the log.
    struct iotrace_entry {
        uint64_t seq;
        uint64_t ns;                    // CLOCK_REALTIME, for journal times
        uint32_t addr;                  // bus address, or vector for an INTR
        uint16_t value;
        uint8_t op;                     // 0 DATI, 1 DATO, 2 DATOB, 3 INTR
        uint8_t ok;
    };
    static const unsigned IOTRACE_ENTRIES = 64;

    /// Writes the processor's last 64 accesses to the boot device's
    /// registers, and the interrupts between them, to the log, together with
    /// the instruction history when `core_history` is on. It acts once and
    /// falls back to 0.
    parameter_bool_c iotrace_dump_switch = parameter_bool_c(this, "iotrace_dump", "iotd",/*readonly*/
                                           false, "1 = write the register-access trace to the log.");

private:
    // What the PRU had counted at the last publish_status(), for the transfers
    // it answers on the bus with bus_dma set.
    struct pru_dma_counts_t {
        uint32_t words;
        uint32_t refused;
        uint32_t byte_offset;
    };
    pru_dma_counts_t pru_dma_counts_seen = { 0, 0, 0 };
    pru_dma_counts_t pru_dma_counts(void);

    iotrace_entry iotrace[IOTRACE_ENTRIES];
    uint64_t iotrace_seq = 0;
    uint32_t iotrace_base = 0;          // the watched registers; 0 = off
    void iotrace_note(uint8_t op, uint32_t addr, uint16_t value, bool ok);
    void iotrace_dump(void);

    // A console examine or deposit asked for from the web thread. The transfer
    // itself belongs to the processor's own thread, which is the bus master, so
    // the request is left here and worker() performs it.
    // A change of the console floppy goes the same way: the floppy is read and
    // written by the processor's thread.
    enum console_access_e { console_access_none, console_access_examine,
                            console_access_deposit, console_access_mem_examine,
                            console_access_mem_deposit, console_access_floppy };
    volatile enum console_access_e console_access = console_access_none;
    volatile unsigned console_access_addr = 0;
    volatile unsigned console_access_data = 0;
    volatile bool console_access_ok = false;
    std::string console_access_floppy_path;

    void service_console_access(void);
    void publish_unibus_map(void);
    bool wait_for_console_access(enum console_access_e what);
    bool request_console_access(enum console_access_e what, unsigned addr);

    bool insert_floppy(const std::string &path);    // on the processor's thread
    uint64_t floppy_flush_due = 0;                  // CLOCK_MONOTONIC second of the next write-back

    unsigned history_countdown = 0;     // passes left before the history is written
    bool machine_running = false;       // the core is executing, not halted
    uint64_t instructions_at_start = 0; // sim_gtime() when the machine started

    bool configure_machine(void);       // memory, disk, boot; leaves it ready to run
    void machine_start(void);
    void machine_stop(const char *why);
    void publish_status(void);
};

#endif
