/* testbus.hpp: the fake QUNIBUS the CPU cores are tested against

 Copyright (c) 2026, Joerg Hoppe
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


 A CPU emulation core reaches the world only through the unibone_*() functions
 of 10.02_devices/2_src/cpu_bus_adapter.h. On a BeagleBone cpu.cpp implements
 them on top of qunibusadapter, the PRUs and a real backplane. Here they are
 implemented on top of a word array and two register stubs, which is all the
 MAINDEC instruction diagnostics need and which needs no hardware at all.

 Like cpu.cpp, this works on one installed instance at a time: the core calls
 free functions, so there is a single "current" testbus_c, see install().

 Everything here is deterministic - no threads, no wall clock, no randomness.
 The runner relies on that: a failing run is replayed identically with tracing
 armed, to print the instructions leading up to the failure.
 */
#ifndef _TESTBUS_HPP_
#define _TESTBUS_HPP_

#include <stdint.h>
#include <stdio.h>
#include <string>
#include <vector>

class testcore_c;

class testbus_c {
public:
    // The KL11 console. The MAINDECs report "end of pass, no errors" by
    // printing a BEL, which is how a test run is recognized as passed.
    // See svc_kl11() in cpu20/pdp11-master/kl11.c, where the same trick is
    // used by the AUTODIAG build of the upstream emulator.
    static const unsigned KL11_RCSR = 0777560;
    static const unsigned KL11_RBUF = 0777562;
    static const unsigned KL11_XCSR = 0777564;
    static const unsigned KL11_XBUF = 0777566;
    // Both halves of a KL11 interrupt on BR4, at the standard console vectors.
    static const unsigned KL11_RCV_VECTOR = 0060;
    static const unsigned KL11_XMIT_VECTOR = 0064;
    static const unsigned KL11_BR_LEVEL = 4;
    // KW11-L line clock. Answers, but never requests an interrupt: it never
    // ticks, so its MONITOR bit never sets.
    static const unsigned KW11_LKS = 0777546;

    explicit testbus_c(unsigned memory_words);

    // make this the instance the unibone_*() functions work on, and name the
    // core they deliver interrupt vectors to
    void install(testcore_c *core);

    unsigned memory_words(void) const { return (unsigned) memory.size(); }
    // Deposit by the tape loader, bypassing the bus. A tape block may hold an
    // odd number of bytes, so the loader works in bytes.
    // "addr" is a 16 bit byte address. Result false: outside of memory.
    bool mem_deposit_byte(uint16_t addr, uint8_t value);

    /*** state observed by the runner ***/

    // a BEL was printed: the diagnostic completed a pass without errors
    bool bell = false;
    // End of pass announced in text instead of with a BEL, as the 11/34 basic
    // instruction tapes do ("END PASS 1"). Empty: no such text is looked for.
    // Set from the "pass-text" option, see cputest.cpp.
    std::string pass_text;
    // pass_text has just been printed
    bool pass_text_seen = false;
    // the run is over and the diagnostic completed a pass without errors
    bool passed(void) const { return bell || pass_text_seen; }
    // Is a BEL the end-of-pass signal? True for every MAINDEC, but a tape which
    // exercises the console itself sends the whole character set as data, BEL
    // included, and would be judged passed on its seventh character. Such a tape
    // turns this off with "bell-is-pass = 0" in its .opt sidecar, and then has
    // to be judged by hand - see 3_tapes/cpu34/FKTGC0.BIC.opt.
    bool bell_is_pass = true;
    // everything else the diagnostic printed, kept back and only shown on failure
    std::string console_output;

    /*** tracing ***/

    // While false, unibone_log() returns immediately: formatting a trace line
    // per instruction costs more than the emulation itself, and a passing run
    // executes over 100 million of them.
    bool tracing = false;
    // where trace output goes while tracing is on
    FILE *trace_stream = stdout;

    /*** implementation of the cpu_bus_adapter.h contract ***/

    int dati(unsigned addr, unsigned *data);
    int dato(unsigned addr, unsigned data);
    int datob(unsigned addr, unsigned data);
    // bus INIT, pulsed by the RESET opcode: puts the register stubs back into
    // their power-up state, which above all clears the interrupt enables.
    void bus_init(void);
    // The CPU is at an instruction boundary and lets pending device requests
    // be granted. This is where the PRU arbitrator's job is done instead.
    void grant_interrupts(void);
    // the CPU loaded a new PSW<7:5>
    void set_cpu_priority(uint8_t level) { cpu_priority = level; }

private:
    // 16 bit word addressed. Physical addresses at or above this are I/O page
    // or non existing memory.
    std::vector<uint16_t> memory;

    // the core interrupt vectors are delivered to, see install()
    testcore_c *core = nullptr;

    // KL11 register state. Only what a diagnostic can observe.
    uint16_t kl11_rcsr = 0;
    uint16_t kl11_xcsr = 0200;	// transmitter ready from the start

    /* A character takes time to go out, and READY is down while it does. The
     character itself is delivered to console_output the moment it is written,
     but READY comes back only this many instructions later, and that is when
     the transmitter asks for its interrupt. FKABD1 needs it: it writes a
     character, enables the interrupt while the transmitter is still busy and
     executes a WAIT, and expects the interrupt to end the WAIT - so the READY
     edge has to fall after the two instructions between the write and the
     WAIT. The exact figure does not matter, a real console is thousands of
     instructions slow; it is kept small because a diagnostic prints a lot and
     polls READY in between. */
    static const unsigned KL11_XMIT_TIME = 8;
    unsigned kl11_xmit_busy = 0;	// instructions left of the transmission

    /*** interrupts ***/

    // PSW<7:5> of the CPU, tracked through unibone_prioritylevelchange().
    // On a BeagleBone this is what cpu.cpp hands to the PRU arbitrator.
    uint8_t cpu_priority = 0;
    // Interrupt request flipflops, one per KL11 half. Set when the ready/done
    // flag comes up with the interrupt enable on, or when the enable is turned
    // on while the flag is already up. Cleared by the GRANT, by clearing the
    // enable, and by INIT - a request survives until it is served, but a served
    // one is not repeated until the device becomes ready again.
    bool kl11_rcv_request = false;
    bool kl11_xmit_request = false;

    // Result 0 = nobody answered (bus timeout / NXM), 1 = handled.
    int io_read(unsigned addr, unsigned *data);
    int io_write(unsigned addr, unsigned data);

    void console_put(uint8_t c);
};

#endif
