/* cpu.hpp: model independent base class for emulated PDP-11 CPUs

 Copyright (c) 2018, Angelo Papenhoff, Joerg Hoppe
 j_hoppe@t-online.de, www.retrocmp.com

 Permission is hereby granted, free of charge, to any person obtaining a
 copy of this software and associated documentation files (the "Software"),
 to deal in the Software without restriction, including without limitation
 the rights to use, copy, modify, merge, publish, distribute, sublicense,
 and/or sell copies of the Software, and to permit persons to whom the
 Software is furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 JOERG HOPPE BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.


 24-jul-2026  JH      split model independent part off into cpu_base_c
 23-nov-2018  JH      created

 cpu_base_c implements everything an emulated PDP-11 CPU needs which does not
 depend on the CPU model: the console switches, the worker() thread driving
 emulation, QUNIBUS data transfers, power events and diagnostics.

 The actual instruction set emulation lives in a plain C "core"
 (cpu20/ka11.c, cpu34/kd11ea.c, ...). A derived class (cpu20_c, cpu34_c) owns
 such a core and connects it by implementing the core_*() hooks below.

 Only one CPU may be installed ("enabled") at a time: the core reaches back to
 the ARM side through global functions (see cpu_bus_adapter.h) which operate on
 a single installed CPU. Enabling a second one is refused, see on_before_install().
 */
#ifndef _CPU_HPP_
#define _CPU_HPP_

#include <assert.h>

#include "utils.hpp"
#include "timeout.hpp"
#include "qunibus.h"	// QUNIBUS_CYCLE_*, used by qunibus_tracer.hpp
//#include "qunibusadapter.hpp"
//#include "qunibusdevice.hpp"
#include "unibuscpu.hpp"
#include "qunibus_tracer.hpp"
#include "ringbuffer.hpp"

// on etraces QUNIBUS access
class qunibus_cycle_trace_entry_c {
public:
    uint64_t	id ;
    uint64_t	timestamp_ns ;
    bool iopage ;
    unsigned address ;
    uint8_t	cycle ; // DTAI
    uint16_t	data ;
    bool nxm ; // timeout, not existing memory
    qunibus_cycle_trace_entry_c() { }
    qunibus_cycle_trace_entry_c(uint64_t _id, bool _iopage, unsigned _address, uint8_t _cycle, uint16_t _data, bool _nxm) {
        id = _id ;
        this->timestamp_ns = timeout_c::abstime_ns() ;
        iopage = _iopage ;
        address = _address ;
        cycle = _cycle ;
        data = _data ;
        nxm = _nxm ;
    }
} ;

#include <fstream>
#include <sys/time.h>

#define qunibus_cycle_trace_buffer_size 16384
//#define qunibus_cycle_trace_buffer_size 4096
class qunibus_cycle_trace_buffer_c: public jnk0le::Ringbuffer<qunibus_cycle_trace_entry_c, qunibus_cycle_trace_buffer_size, false, 8> {
public:
    bool active = false ;
    // my insert() erases oldest entry
    void add(qunibus_cycle_trace_entry_c qcte) {
        if (isFull()) remove(100) ; // remove a large chunk, to speed up.
        assert( insert(qcte)) ;
    }

    // readout non-destructive. to clear, use "clearConsumer()"
    void dump(std::ostream *stream)
    {
        qunibus_cycle_trace_entry_c *cte ;
        char buffer[256] ;
        timeval now ;
        gettimeofday(&now, NULL);
        strftime(buffer, 26, "%F %T", localtime(&now.tv_sec));
        *stream << "// Sampled QUNIBUS cycles, saved at " << buffer << "\n";
        *stream << "id, timestamp, iopage, address, cycle, data, nxm\n" ;
        for (unsigned i=0 ; i < readAvailable() ; i++) {
            cte = at(i) ;
//        while (remove(cte)) {
            sprintf(buffer, "%llu, %llu, %d, %06o, %s, %06o, %d",
                    cte->id, cte->timestamp_ns, cte->iopage, cte->address, qunibus_c::control2text(cte->cycle), cte->data, cte->nxm) ;
            *stream << buffer << "\n";
        }
    }


    void dump(std::string filepath)
    {
        std::ofstream file_stream;
        file_stream.open(filepath, std::ofstream::out | std::ofstream::trunc);
        if (!file_stream.is_open()) {
            std::cout << "Can not open log file \"" << filepath << "\"! Aborting!\n";
            exit(2);
        }
        size_t fill = readAvailable() ;
        dump(&file_stream);

        file_stream.close();
        std::cout << "Dumped " << fill << " messages to file \"" << filepath << "\".\n";
    }
} ;


class cpu_base_c: public unibuscpu_c {
private:
    //qunibusdevice_register_t *switch_reg;
    //qunibusdevice_register_t *display_reg;

	// bitwise options what state info to show on HALT
	static const int show_none = 0 ;
	static const int show_pc = 1 ;
	static const int show_trigger =2 ;
	static const int show_state = 4 ;
	static const int show_cycletrace = 8 ;

public:

	// run state of the emulation core.
	// values identical to the cores KA11_STATE_*/KD11EA_STATE_* codes.
	enum cpu_state_e {
		cpu_state_halted = 0,
		cpu_state_running = 1,
		cpu_state_waiting = 2
	};

	// Everything the processor holds, in the terms the models share, so a
	// caller reading it out - the debug panel, a diagnostic - needs to know
	// nothing about the model. What a model does not have it does not fill:
	// the KA11 has no modes, no second stack pointer and no memory
	// management, and says so through the has_* flags rather than publishing
	// zeroes that read like real values.
	//
	// Filling one costs no bus cycle and changes nothing: the core's registers
	// are read where they lie.
	//
	// It is only worth taking of a *halted* processor. Read from a foreign
	// thread while the core is executing, these are a handful of word-wide
	// reads of values changing every instruction: what comes back is a set of
	// numbers that were never all true at once. The web API therefore publishes
	// registers only while the CPU is halted, and any other caller wanting them
	// of a running machine has to know what it is looking at.
	struct state_snapshot_c {
		uint16_t r[8];		// R0..R5, SP of the current mode, PC
		uint16_t psw;
		uint16_t ir;		// instruction register: the opcode being run
		uint16_t bus_addr;	// address and data of the transfer in flight
		uint16_t bus_data;
		uint64_t cycle_count;
		enum cpu_state_e state;
		// PSW<15:14> and <13:12> carry the current and previous mode
		bool has_modes;
		// the model keeps a stack pointer per mode; sp_kernel/sp_user hold
		// them, including the one r[6] is currently showing
		bool has_stackpointers;
		uint16_t sp_kernel;
		uint16_t sp_user;
		// the model has memory management, and mmr0..mmr2 are its registers
		bool has_mmu;
		bool mmu_enabled;
		uint16_t mmr0;
		uint16_t mmr1;
		uint16_t mmr2;
		// The eight page address and page descriptor registers of each mode.
		// A KT11-D holds one set per mode and relocates through the set the
		// PSW names, so which of them is in force is a property of the
		// processor rather than of the MMU: the reader gets both and reads
		// the mode out of the status word.
		uint16_t kernel_par[8];
		uint16_t kernel_pdr[8];
		uint16_t user_par[8];
		uint16_t user_pdr[8];
	};

    cpu_base_c();
    virtual ~cpu_base_c();

    bool on_before_install(void) override ;
    void on_after_uninstall(void) override ;

    // Whether this processor claimed the machine's memory out of the board's
    // DDR when it was enabled, and so must give it back when it is disabled.
    // False when a memory card already answered the range.
    bool claimed_memory = false;

    // used for DATI/DATO, operated by qunibusadapter
    dma_request_c data_transfer_request{this};

    bool on_param_changed(parameter_c *param) override;  // must implement

    parameter_bool_c runmode = parameter_bool_c(this, "run_led", "r",/*readonly*/
                               true, "RUN LED: 1 = CPU running, 0 = halted.");
    parameter_bool_c halt_switch = parameter_bool_c(this, "halt_switch", "h",/*readonly*/
                                   false, "HALT switch: 1 = CPU stopped, 0 = CPU may run.");
    parameter_bool_c continue_switch = parameter_bool_c(this, "continue_switch", "c",/*readonly*/
                                       false, "CONT action switch: 1 = CPU restart after HALT. CONT+HALT = single step.");
    parameter_bool_c start_switch = parameter_bool_c(this, "start_switch", "s",/*readonly*/
                                    false, "START action switch: 1 = reset & start CPU from PC. START+HALT: reset.");

    parameter_bool_c direct_memory = parameter_bool_c(this, "pmi", "pmi",/*readonly*/
                                     false, "Private Memory Interconnect: CPU accesses memory internally, not over UNIBUS.");

    parameter_unsigned_c pc = parameter_unsigned_c(this, "PC", "pc",/*readonly*/
                              false, "", "%06o", "program counter helper register.", 16, 8);

    parameter_unsigned_c swreg = parameter_unsigned_c(this, "switch_reg", "swr",/*readonly*/
                                 false, "", "%06o", "Console switch register.", 16, 8);

    // What the core is doing, published by core_publish_status() on every
    // worker pass. The processor status word is the model's own width: the
    // KA11 keeps priority, T and the condition codes in a byte, the KD11-EA
    // adds the current and previous mode in the high half.
    parameter_unsigned_c psw = parameter_unsigned_c(this, "PSW", "psw",/*readonly*/
                               true, "", "%06o", "Processor status word.", 16, 8);
    // the bus address and data registers of the data transfer in progress -
    // the virtual address the CPU issued, before any relocation
    parameter_unsigned_c bus_addr = parameter_unsigned_c(this, "bus_addr", "ba",/*readonly*/
                                    true, "", "%06o", "Bus address register of the current data transfer.", 16, 8);
    parameter_unsigned_c bus_data = parameter_unsigned_c(this, "bus_data", "bd",/*readonly*/
                                    true, "", "%06o", "Bus data register of the current data transfer.", 16, 8);

    parameter_unsigned64_c cycle_count = parameter_unsigned64_c(this, "cycle_count", "cc",/*readonly*/
                                         true, "", "%u", "CPU opcodes executed since last HALT", 63, 10);

    // How fast the machine is running, for the performance panel. It reads
    // cycle_count rather than counting for itself: the opcode counter is
    // incremented in the instruction loop, and an atomic add per instruction
    // would be a cost paid a million times a second to measure a number sampled
    // once. That cycle_count restarts at every HALT costs nothing here - the
    // sampler drops any interval a total fell across.
    //
    // The model this is emulating sets reference_per_second in its constructor,
    // and that is what the percentage is against. cpu_base_c leaves it at 0, so
    // a model that has not said how fast its original ran reports a rate and no
    // percentage rather than a percentage of nothing.
    metric_c instructions{this, "instructions", metric_c::UNIT_INSTRUCTION,
                                     "Instructions",
                                     [this]() { return cycle_count.value; }};

    parameter_unsigned_c breakpoint = parameter_unsigned_c(this, "breakpoint", "bp",/*readonly*/
                                      false, "", "%06o", "Stop when CPU fetches opcode from octal address. 0 = disable", 16, 8);

    parameter_string_c cycle_tracefilepath = parameter_string_c(this, "cycle_tracefilepath", "ctf",/*readonly*/false,
            "If set, CPU cycle trace is active and dumped to file on HALT.") ;


    void start(void);
    void stop(const char * info, int show_options=show_none);

    // Every emulated processor is a "cpu" to the interfaces that group devices
    // by what they are; nothing else on a backplane executes instructions.
    const char *category(void) const override { return "cpu"; }

    // the front panel's view of the console switches
    parameter_bool_c *panel_run_led(void) override { return &runmode; }
    parameter_bool_c *panel_halt_switch(void) override { return &halt_switch; }
    parameter_bool_c *panel_start_switch(void) override { return &start_switch; }
    parameter_bool_c *panel_continue_switch(void) override { return &continue_switch; }

    // background worker function
    void worker(unsigned instance) override;

    // called by qunibusadapter on emulated register access
    void on_after_register_access(qunibusdevice_register_t *device_reg, uint8_t unibus_control, DATO_ACCESS access)
    override;

    void on_interrupt(uint16_t vector, uint8_t level) override;

    /*** interface to the CPU model specific emulation core ***/
    // implemented by cpu20_c, cpu34_c, ... by forwarding to their C core.

    // execute one instruction, if the core is RUNNING (or WAITING with work pending)
    virtual void core_condstep(void) = 0;
    // power-up/console START: clear the cores state
    virtual void core_reset(void) = 0;
    // an INTR vector was received from the bus. Called from a foreign thread!
    virtual void core_setintr(uint16_t vector) = 0;
    // ACLO active while running: trap through vector 24
    virtual void core_pwrfail_trap(void) = 0;
    // ACLO inactive: load PC and PSW from vector 24
    virtual void core_pwrup_vector_fetch(void) = 0;
    // diagnostic dumps of the cores registers
    virtual void core_printstate(void) = 0;
    virtual void core_tracestate(void) = 0;

    virtual enum cpu_state_e core_get_state(void) = 0;
    virtual void core_set_state(enum cpu_state_e state) = 0;
    virtual uint16_t core_get_pc(void) = 0;
    virtual void core_set_pc(uint16_t value) = 0;
    // console switch register, readable by the CPU
    virtual void core_set_switches(uint16_t value) = 0;
    // copy CPU model specific option parameters into the core.
    // called on every worker() loop, most models have none.
    virtual void core_apply_options(void) { }
    // Everything the core holds, for a reader outside it: see state_snapshot_c.
    // Unlike core_publish_status() this is not on the worker's path - it is
    // filled when somebody asks - so it may carry what a status parameter per
    // register would cost too much to publish on every instruction.
    virtual void core_get_snapshot(state_snapshot_c *snap) = 0;
    // Copy what the core is doing into the status parameters, so the web
    // interface and the menus read it without knowing the model. Called on
    // every worker pass; a model publishes its own extras here too, as the
    // 11/34 does with its memory management registers.
    virtual void core_publish_status(void) = 0;

    //diagnostic
    trigger_c	trigger ;
    tracer_c	tracer ;

    // ring buffer for bus DATI/DATO accesses
    // need to set cachelinesize, 0 causes error. 8?
    uint64_t cycle_trace_entry_id = 0 ; // enumerate samples
    qunibus_cycle_trace_buffer_c cycle_trace_buffer;


};

#endif
