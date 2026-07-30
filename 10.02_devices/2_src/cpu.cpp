/* cpu.cpp: model independent base class for emulated PDP-11 CPUs

 Copyright (c) 2018, Angelo Papenhoff, Joerg Hoppe

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


 24-jul-2026  JH     split model independent part off into cpu_base_c
 16-oct-2020  JH     merged VBIT changes by github jks-prv
 23-nov-2018  JH      created

 In worker() the emulation core of the derived class is running.
 Can do bus master DATI/DATO.
 */

#include <string.h>
#include <stdarg.h>

#include "logger.hpp"
#include "mailbox.h"
#include "gpios.hpp"	// ARM_DEBUG_PIN*

#include "qunibus.h"

#include "qunibusadapter.hpp"
#include "qunibusdevice.hpp"	// definition of class device_c
#include "cpu.hpp"
#include "cpu_bus_adapter.h"

/* If CPU_CONTROLLED_TIME,
   then time base for all devices, also KW11 50Hz, are
   derived from emulated CPU cycles.

   Otherwise time is Linux world time, proceding even if execution of emulation is stopped due to
   task scheduling.
 */
#undef CPU_CONTROLLED_TIME


/*** functions to be used by the emulation cores ***/

/* Adapter procs to the C emulation cores are not members of cpu_base_c
 and need one global reference.
 Set on install(), so several CPU models may be instantiated in parallel,
 but only one may be enabled. See cpu_base_c::on_before_install().
 */
static cpu_base_c *unibone_cpu = NULL;

// route "trace()" to unibone_cpu->logger
void unibone_log(unsigned msglevel, const char *srcfilename, unsigned srcline, const char *fmt,
                 ...)
{
    // gate on the CPU source's verbosity like logger_c::log does, so the trace
    // obeys the same per-source level as every other source in the one log
    if (logger->ignored(unibone_cpu, msglevel))
        return;
    va_list arg_ptr;
    va_start(arg_ptr, fmt);
    logger->vlog(unibone_cpu, msglevel, /*late_evaluation*/true, srcfilename, srcline, fmt, arg_ptr);
    va_end(arg_ptr);
}

void unibone_logdump(void)
{
//	logger->dump(logger->default_filepath);
    logger->dump(); // stdout
}

// called before opcode fetch of next instruction
// This is the point in time were INTR requests are checked and GRANTed
// (PRU implementation may limit NPR GRANTs also to this time)
void unibone_grant_interrupts(void)
{
    // after that the CPU should check for received INTR vectors
    // in its microcode service() step.c
    // allow PRU do to produce GRANT for device requests
    if (!mailbox_execute (ARM2PRU_ARB_GRANT_INTR_REQUESTS))
        return; // PRU not responding, reported by mailbox_execute()
    // Block CPU thread, often 60-80 us of idle spinning.
    // Bounded: if the PRU is stopped or restarted mid-instruction, an
    // unbounded spin would hang the CPU worker thread beyond recovery.
    uint64_t starttime_ns = timeout_c::abstime_ns();
    while (mailbox->arbitrator.ifs_intr_arbitration_pending) {
        if (timeout_c::abstime_ns() - starttime_ns > 100000000ull) {
            logger->log(unibone_cpu, LL_ERROR, false, __FILE__, __LINE__,
                        "unibone_grant_interrupts(): PRU arbitration pending for >100ms - PRU stopped or hung?");
            return;
        }
    }
}

// Access to UNIBUS, helper for foreign CPU emulation.
// Result: 1 = OK, 0 = bus timeout
// When "PMI": memory is not accessed via UNIBUS cycles,
// but by DDRAM is accessed directly.
// Then UNIBUS accesses only the IOPage.
// Motivation:
// - Fix for slow CPU execution time because of UNIBUS delays.
// - Option to implement CPUs with local 22bit memory later.
// - DEC also had separate IO and MEMORY Busses. See 11/44,60,70,84 and others

#define UNIBUS_ACCESS_NS	1000
// "real world" time for bus access. emulated timeout is stepped by this on every cycle.

int unibone_dato(unsigned addr, unsigned data)
{
    bool success ;

    unibone_cpu->trigger.probe(addr, QUNIBUS_CYCLE_DATO) ; // register access for trigger system

    uint16_t wordbuffer = (uint16_t) data;
    the_flexi_timeout_controller->emu_step_ns(UNIBUS_ACCESS_NS);
    if (unibone_cpu->direct_memory.value && addr < qunibus->iopage_start_addr) {
        // Direct access Non-IOPage memory.
        ddrmem->pmi_deposit(addr, data);
        success = true;
    } else {
        qunibusadapter->cpu_DATA_transfer(unibone_cpu->data_transfer_request,
                                          QUNIBUS_CYCLE_DATO, addr, &wordbuffer);
        success = unibone_cpu->data_transfer_request.success;
    }

    // trace bus access
    if (unibone_cpu->cycle_trace_buffer.active)
        unibone_cpu->cycle_trace_buffer.add(qunibus_cycle_trace_entry_c(unibone_cpu->cycle_trace_entry_id++, addr >= qunibus->iopage_start_addr, addr, QUNIBUS_CYCLE_DATO, data, !success)) ;

    return success;
}

int unibone_datob(unsigned addr, unsigned data)
{
    bool success ;
    unibone_cpu->trigger.probe(addr, QUNIBUS_CYCLE_DATO) ; // register access for trigger system
    the_flexi_timeout_controller->emu_step_ns(UNIBUS_ACCESS_NS);
    if (unibone_cpu->direct_memory.value && addr < qunibus->iopage_start_addr) {
        // read-modify-write
        unsigned word_address = addr & ~1; // lower even address
        uint16_t w;
        ddrmem->pmi_exam(word_address, &w);
        if (addr & 1) // odd addr: set bits <8:15>
//			w = (w & 0xff) | (data << 8);
            w = (w & 0xff) | (data & 0xff00);
        else
            // even addr: set bits <0:7>
            w = (w & 0xff00) | (data & 0xff);
        ddrmem->pmi_deposit(word_address, w);
        success = true;
    } else {
        // TODO DATOB als 1 byte-DMA !
        uint16_t w = (uint16_t) data;
        qunibusadapter->cpu_DATA_transfer(unibone_cpu->data_transfer_request,
                                          QUNIBUS_CYCLE_DATOB, addr, &w);
        success = unibone_cpu->data_transfer_request.success;
    }

    // trace bus access
    if (unibone_cpu->cycle_trace_buffer.active)
        unibone_cpu->cycle_trace_buffer.add(qunibus_cycle_trace_entry_c(unibone_cpu->cycle_trace_entry_id++, addr >= qunibus->iopage_start_addr, addr, QUNIBUS_CYCLE_DATOB, data, !success)) ;

    return success;
}

int unibone_dati(unsigned addr, unsigned *data)
 {
    bool success ;
    uint16_t w;
    unibone_cpu->trigger.probe(addr, QUNIBUS_CYCLE_DATI) ; // register access for trigger system

    the_flexi_timeout_controller->emu_step_ns(UNIBUS_ACCESS_NS);
    if (unibone_cpu->direct_memory.value && addr < qunibus->iopage_start_addr) {
        // boot address redirection by M9312? addrs 24/26 now in M9312 IOpage
        addr |= ddrmem->pmi_address_overlay;
    }
    if (unibone_cpu->direct_memory.value
            && (addr < qunibus->iopage_start_addr || qunibusadapter->is_rom(addr))) {
        // Direct access Non-IOPage memory, or to emulated ROM
        ddrmem->pmi_exam(addr, &w);
        *data = w;
        success = true;
    } else {
        qunibusadapter->cpu_DATA_transfer(unibone_cpu->data_transfer_request,
                                          QUNIBUS_CYCLE_DATI, addr, &w);
        *data = w;
        success = unibone_cpu->data_transfer_request.success;
    }

    // trace bus access
    if (unibone_cpu->cycle_trace_buffer.active)
        unibone_cpu->cycle_trace_buffer.add(qunibus_cycle_trace_entry_c(unibone_cpu->cycle_trace_entry_id++, addr >= qunibus->iopage_start_addr, addr, QUNIBUS_CYCLE_DATI, *data, !success)) ;

    return success;
}

// CPU has changed the arbitration level, just forward
// if this is called as result of INTR fector PC and PSW fetch,
// mailbox->arbitrator.cpu_priority_level was CPU_PRIORITY_LEVEL_FETCHING
// In that case, PRU is allowed now to grant BGs again.
void unibone_prioritylevelchange(uint8_t level)
{
    mailbox->arbitrator.ifs_priority_level = level;
}

// CPU executes RESET opcode -> pulses INIT line
void unibone_bus_init()
{
    qunibus->init();
}


// Is trace() output live at all? trace() records at LL_DEBUG, so this is the
// logger level gate. The cores cache the answer in cpu->tracing once per
// instruction: with tracing off, all their per-cycle trace sites cost one
// flag test each instead of a call chain into the logger.
bool unibone_trace_enabled()
{
    return !logger->ignored(unibone_cpu, LL_DEBUG) ;
}

// shall this address be traced? Only consulted when tracing is live;
// with the address filter disabled, everything is traced.
bool unibone_trace_addr(uint16_t a)
{
    return !unibone_cpu->tracer.enabled || unibone_cpu->tracer.addr[a] ;
}



cpu_base_c::cpu_base_c() :
    unibuscpu_c()  // super class constructor
{
    // static config, derived classes set name/type_name/log_label
    default_base_addr = 0;  // none
    default_intr_vector = 0;
    default_intr_level = 0;
    priority_slot.value = 0;  // not used

    // init parameters
    emulation_speed.readonly = true ; // displays only speed of emulation
    runmode.value = false;
    start_switch.value = false;
    halt_switch.value = false;
    continue_switch.value = false;
    direct_memory.value = false;
    emulation_speed.value = 0.1 ; // non-PMI speed,  see on_param_changed() also

    // current CPUs do not publish registers to the bus
    // must be qunibusdevice_c then!
    register_count = 0;

    // Running state, not configuration: the RUN LED, the live program counter
    // the CPU writes back, and the executed-opcode count.
    runmode.kind = parameter_c::PARAM_STATUS;
    pc.kind = parameter_c::PARAM_STATUS;
    cycle_count.kind = parameter_c::PARAM_STATUS;
    psw.kind = parameter_c::PARAM_STATUS;
    bus_addr.kind = parameter_c::PARAM_STATUS;
    bus_data.kind = parameter_c::PARAM_STATUS;
    // The console switches are console state too. START and CONTINUE are
    // momentary: the worker acts on them and clears them again, so what they
    // hold is never a setting. HALT is a switch position, and a configuration
    // that restored it would leave the machine unable to run with nothing on
    // screen to say why. The switch register is different — it is operator
    // data the running program reads, so it stays configuration.
    halt_switch.kind = parameter_c::PARAM_STATUS;
    start_switch.kind = parameter_c::PARAM_STATUS;
    continue_switch.kind = parameter_c::PARAM_STATUS;
}

cpu_base_c::~cpu_base_c()
{
    // restore
    the_flexi_timeout_controller->set_mode(flexi_timeout_c::world_time);
    if (unibone_cpu == this)
        unibone_cpu = NULL; // deleted without "disable" before
}

// called when "enabled" goes true, before registers plugged to UNIBUS
// result false: configuration error, do not install
bool cpu_base_c::on_before_install(void)
{
    // The emulation cores reach the ARM side through the global unibone_*()
    // functions, which work on a single installed CPU. And qunibusadapter
    // accepts only one registered CPU too.
    if (unibone_cpu != NULL && unibone_cpu != this && unibone_cpu->enabled.value) {
        ERROR("%s can not be enabled: CPU %s is already active, disable it first.",
              name.value.c_str(), unibone_cpu->name.value.c_str());
        return false; // qunibusdevice_c aborts the "enable"
    }
    unibone_cpu = this; // the CPU the cores reach, until another one installs

    halt_switch.value = false;
// all other switches parsed synchronically in worker()
    start_switch.value = false;
    continue_switch.value = false;
// enable active: assert CPU starts stopped
    stop("CPU stopped", show_none);
    return true;
}

void cpu_base_c::on_after_uninstall(void)
{
// all other switches parsed synchronically in worker()
    start_switch.value = false;
    halt_switch.value = true;
    // HALT disabled CPU
    stop(NULL, show_none);

    // unibone_cpu keeps pointing here. The worker thread is stopped after this
    // callback returns (device_c::on_param_changed), and until it does it goes
    // on executing instructions whose bus cycles reach the ARM side through
    // unibone_cpu - clearing it here is a null pointer under a running CPU.
    // A disabled CPU with a stopped worker is a harmless target, and the next
    // model to install takes the pointer over in on_before_install().
}

bool cpu_base_c::on_param_changed(parameter_c *param)
{
    if (param == &direct_memory) {
        // speed feedback, as measured
        // see cpu_base_c() also
        emulation_speed.value = direct_memory.new_value ? 0.5 : 0.1 ;
    } else if (param == &cycle_tracefilepath) {
	    cycle_trace_buffer.active = ! cycle_tracefilepath.new_value.empty() ;
    }
    return qunibusdevice_c::on_param_changed(param); // more actions (for enable)
}

// start CPU logic on PRU and switch arbitration mode
void cpu_base_c::start()
{
    // For debugging, trigger conditions and tracer address ranges can be set
    // here before the CPU starts, see qunibus_tracer.hpp:
    //	trigger.condition_add(trigger_condition_c(0777170, TRIGGER_DATO)) ;
    //	tracer.enable(026276, 026400) ;
    trigger.conditions_clear() ;

    runmode.value = true;
    mailbox->param = 1;
    mailbox_execute(ARM2PRU_CPU_ENABLE);
    qunibus->set_arbitrator_active(true);
    pc.readonly = true; // can only be set on stopped CPU
    core_set_state(cpu_state_running);
    // time base of all device emulators now based on CPU opcode execution
#ifdef CPU_CONTROLLED_TIME
    // only point to switch
    the_flexi_timeout_controller->set_mode(flexi_timeout_c::emulated_time);
#else
    the_flexi_timeout_controller->set_mode(flexi_timeout_c::world_time);
#endif
    cycle_count.value = 0;

    // 	what if CONT while WAITING??
}

// stop CPU logic on PRU and switch arbitration mode
void cpu_base_c::stop(const char * info, int show_options)
{
    // time base of all device emulators now based on "real world" time
    the_flexi_timeout_controller->set_mode(flexi_timeout_c::world_time);

    core_set_state(cpu_state_halted);
    pc.readonly = false;
    pc.value = core_get_pc(); // update for editing

    runmode.value = false;
    mailbox->param = 0;
    mailbox_execute(ARM2PRU_CPU_ENABLE);
    qunibus->set_arbitrator_active(false);

    if (info && strlen(info)) {
        // "info" is caller text: always an argument, never a format string
        if (show_options & show_pc)
            INFO("%s at %06o", info, core_get_pc());
        else
            INFO("%s", info);
    }
	if (show_options & show_trigger) {
		trigger.print(stdout) ;
	}
	if (show_options & show_state) {
		core_printstate() ;
		core_tracestate() ; // DEBUG_FAST log
	}
	if ((show_options & show_cycletrace) && !cycle_tracefilepath.value.empty()) {
		cycle_trace_buffer.dump(cycle_tracefilepath.value) ;
	}

}

// background worker.
// Started/stopped on param "enable"
void cpu_base_c::worker(unsigned instance)
{
    UNUSED(instance); // only one
    timeout_c timeout;

    power_event_ACLO_active = power_event_ACLO_inactive = power_event_DCLO_active = false;

// run with lowest priority, but without wait()
// => get all remaining CPU power
    worker_init_realtime_priority(none_rt);

    timeout.wait_us(1);

    while (!workers_terminate) {
        // RUN led
        runmode.value = (core_get_state() > 0); // RUNNING, WAITING
        if (runmode.value)
            pc.value = core_get_pc(); // update for display

        // CONT starts
        // if HALT+CONT: only one single step
        if (continue_switch.value && !runmode.value) {
            start(); // HALTED -> RUNNING
        }
        continue_switch.value = false; // momentary action

        core_set_switches(swreg.value & 0xffff);

        if (!runmode.value && start_switch.value) {
            // START, or HALT+START: reset system
            core_set_pc(pc.value & 0xffff);
            qunibus->init();
            core_reset();
            if (!halt_switch.value) {
                // START without HALT
                start(); // HALTED -> RUNNING
            }
        }
        start_switch.value = false; // momentary action

        enum cpu_state_e prev_core_state = core_get_state();
        core_condstep();
        if (core_get_state() != cpu_state_halted && trigger.has_triggered()) {
            stop("Halted by trigger conditions:", show_pc+show_trigger+show_state+show_cycletrace);
        } else  if (breakpoint.value && core_get_state() != cpu_state_halted && breakpoint.value == core_get_pc()) {
            stop("CPU HALT by breakpoint", show_pc+show_state+show_cycletrace);
        } else  if (prev_core_state > 0 && core_get_state() == cpu_state_halted) {
            // CPU run on HALT, sync runmode
            stop("CPU HALT by opcode", show_pc+show_state+show_cycletrace);
        }
        // running CPU: produce emulated time for all devices
        if (core_get_state() == cpu_state_running) {
            cycle_count.value++;
        } else if (core_get_state() == cpu_state_waiting)
            // we should us "world" time here, but want to avoid permanent time-source switching
            // so just assume this here is called every 500ns (estimated average worker loop time)
            the_flexi_timeout_controller->emu_step_ns(500);
        // if cpu_state_halted: world time is used, see start() / stop()

        // serialize asynchronous power events
        // ACLO inactive & no HALT: reboot
        // ACLO inactive & HALT: boot on CONT
        // ACLO: power fail trap, if running.
        if (runmode.value && power_event_ACLO_active) {
            core_pwrfail_trap();
        }
        power_event_ACLO_active = false; // processed

        // DCLO: halt, like "enable = 0"
        if (runmode.value && power_event_DCLO_active) {
            stop("CPU HALT by DCLO", show_pc);
        }
        power_event_DCLO_active = false; // processed
        if (power_event_ACLO_inactive) {
            // Reboot
            stop("ACLO", show_pc);
            // execute this with real-world time, else lock (CPU not step() ing here)
            qunibus->init();		// reset devices
            start();		// start CPU logic on PRU, is bus master now
            INFO("ACLO inactive: fetch vector");
            core_reset();
            core_pwrup_vector_fetch();
            // M9312 logic here: vectror redirection for 300ms
            power_event_ACLO_inactive = false;		// processed
        }

        // HALT: activating stops
        // Must be last, to undo power-up and CONT
        // after HALT+power-up: only vector fecth executed
        // after CONT+HALT: one step executed
        if (halt_switch.value && runmode.value) {
            // HALT position inside instructions !!!
            stop("CPU HALT by switch", show_pc+show_state+show_cycletrace);
        }

        core_apply_options();
        core_publish_status();

        // A running CPU is meant to take the core it is given: every pass
        // executes an instruction. A halted one steps nothing, and this loop
        // would spin at full speed polling the switches - on a single-core
        // board that starves everything else in userspace, ssh included, and
        // the machine goes unreachable while looking alive to the network.
        // Poll at a rate no operator pressing START can notice.
        if (core_get_state() == cpu_state_halted)
            timeout.wait_ms(1);
    }
}

// process DATI/DATO access to one of my "active" registers
// !! called asynchronuously by PRU, with SSYN asserted and blocking UNIBUS.
// The time between PRU event and program flow into this callback
// is determined by ARM Linux context switch
//
// UNIBUS DATO cycles let dati_flipflops "flicker" outside of this proc:
//      do not read back dati_flipflops.
void cpu_base_c::on_after_register_access(qunibusdevice_register_t *device_reg,
                                     uint8_t unibus_control, DATO_ACCESS access)
 {
// nothing todo
    UNUSED(device_reg);
    UNUSED(unibus_control);
    UNUSED(access);
}

// CPU received interrupt vector from UNIBUS
// PRU triggers this via qunibusadapter worker thread,
// mailbox->arbitrator.cpu_priority_level is CPU_PRIORITY_LEVEL_FETCHING
// CPU fetches PSW and calls unibone_prioritylevelchange(), which
// sets mailbox->arbitrator.cpu_priority_level and
// PRU is allowed now to grant BGs again.
void cpu_base_c::on_interrupt(uint16_t vector, uint8_t level) {
    UNUSED(level); // a PDP-11 takes the vector; the level only arbitrated it
// CPU sequence:
// push PSW to stack
// push PC to stack
// PC := *vector
// PSW := *(vector+2)
    core_setintr(vector);
}

