/* qunibus.cpp: utilities to handle QBUS/UNIBUS functions

 Copyright (c) 2018, Joerg Hoppe
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

 jul-2019     JH      rewrite: multiple parallel arbitration levels
 12-nov-2018  JH      entered beta phase
 */

#define _QUNIBUS_CPP_

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <assert.h>

#include "pru.hpp"
#include "logger.hpp"
#include "gpios.hpp"
#include "bitcalc.h"
#include "memoryimage.hpp"
#include "mailbox.h" // for test of PRU code
#include "utils.hpp" // for test of PRU code
#include "qunibusadapter.hpp" // DMA, INTR

#include "qunibus.h"

/* Singleton */
qunibus_c *qunibus;

qunibus_c::qunibus_c()
{
    log_label = "QUNIBUS";
    addr_width = 0; // has to be set by user with set_addr_width()
    addr_space_word_count = 0;
    addr_space_byte_count = 0;
    iopage_start_addr = 0;
    cpu_reserved_start = 0;
    probe_word_buffer = 0;
#if defined(UNIBUS)
    set_addr_width(18); // const
#endif

    dma_request = new dma_request_c(NULL);
    // priority backplane slot # for helper DMA not important, as typically used stand-alone
    // (no other devioces on the backplane active, except perhaps "testcontroller")
    dma_request->set_priority_slot(16);
}

qunibus_c::~qunibus_c()
{
    delete dma_request;
}

// recalc memory and iopage limits
void qunibus_c::set_addr_width(unsigned _addr_width)
{
    // a 16- or 18-bit machine fills its space up to the I/O page
    cpu_reserved_start = 0;
    switch (_addr_width) {
    case 18:
        addr_space_word_count = 0x20000; // 128 KWord = 256 KByte
        iopage_start_addr = 0760000;
        break;
#if defined(QBUS) 	// UNIBUS allows only 18 bit
    case 16:
        addr_space_word_count = 0x8000; // 32 KWord = 64 KByte
        iopage_start_addr = 0160000;
        break;
    case 22:
        addr_space_word_count = 0x200000;// 2 MWord = 4 MByte
        iopage_start_addr = 017760000;
        // The top 128 KB below the I/O page belong to the CPU module: that is
        // where a KDJ11 answers its own boot ROM, and a card reaching into it
        // is written by DMA and read back as ROM. A CPU that claims nothing
        // there loses those 128 KB, which is the price of a machine that
        // starts on any of them.
        cpu_reserved_start = 017760000 - 0x20000;
        break;
#endif
    default:
        FATAL("Address width of %d bits invalid!", _addr_width);
    }
    addr_width = _addr_width;
    addr_space_byte_count = 2 * addr_space_word_count;
}

// verify user selected address width,
// address width is determined by PDP-11 CPU and cannot be guessed.
// Example: a 16 bit LSI operates in an 18 bit backplane,
// then QBOne must generate BS7 for addresses >= 160000
// but  addresses 0.. 777776 are valid.
void qunibus_c::assert_addr_width(void)
{
#if defined(QBUS)
    if (!addr_width) {
        FATAL("Select address width of CPU via global parameter\n(command line -aw 16/18/22)") ;
    }
#endif
}


/* return a 16 bit result, or TIMEOUT
 * result: 0 = timeout, else OK
 */
char *qunibus_c::data2text(unsigned val)
{
    char *buffer = rolling_text_buffers.get_next();
    if (val <= 0177777)
        sprintf(buffer, "%06o", val);
    else
        strcpy(buffer, "??????");
    return buffer;
}


/* return UNOBUS control as text: "DATI", DATO", ....
 */
char *qunibus_c::control2text(uint8_t control)
{
    char *buffer = rolling_text_buffers.get_next();
    switch (control) {
    case QUNIBUS_CYCLE_DATI:
        strcpy(buffer, "DATI");
        break;
    case QUNIBUS_CYCLE_DATIP:
        strcpy(buffer, "DATIP");
        break;
    case QUNIBUS_CYCLE_DATO:
        strcpy(buffer, "DATO");
        break;
    case QUNIBUS_CYCLE_DATOB:
        strcpy(buffer, "DATOB");
        break;
    default:
        strcpy(buffer, "???");
    }
    return buffer;
}

// multiple static buffers: many calls allowed per printf() !
char *qunibus_c::addr2text(unsigned addr)
{
    const char *iopagestr ;
    char *buffer = rolling_text_buffers.get_next();
    if ((addr & ~QUNIBUS_IOPAGE_ADDR_BITMASK)  >= iopage_start_addr)
        iopagestr = "io";
    else
        iopagestr = "";
    switch (addr_width) {
    case 16:
        sprintf(buffer, "%s%06o", iopagestr, addr & 0177777);
        break;
    case 18:
        sprintf(buffer, "%s%06o", iopagestr, addr & 0777777);
        break;
    case 22:
        sprintf(buffer, "%s%08o", iopagestr, addr & 017777777);
        break;
    default:
        FATAL("Address width of %d bits invalid!", addr_width);
    }
    return buffer;
}



// octal, or '<char>'
bool qunibus_c::parse_word(char *txt, uint16_t *word)
{
    *word = 0;
    if (!txt || *txt == 0)
        return false;

    if (*txt == '\'') {
        txt++;
        if (*txt)
            *word = *txt; // ASCII code of first char after ''
    } else
        *word = strtol(txt, NULL, 8); // octal literal
    return true;
}

// octal, trunc to 18 bit
bool qunibus_c::parse_addr(char *txt, uint32_t *addr)
{
    unsigned maxval = 0 ;
    *addr = strtol(txt, NULL, 8);
    switch (addr_width) {
    case 16:
        maxval = 0177777;
        break;
    case 18:
        maxval = 0777777;
        break;
    case 22:
        maxval = 017777777;
        break;
    default:
        FATAL("Address width of %d bits invalid!", addr_width);
    }

    if (*addr > maxval) {
        *addr = maxval;
        return false;
    }
    return true;
}

bool qunibus_c::parse_level(char *txt, uint8_t *level)
{
    *level = strtol(txt, NULL, 8);
    if (*level < 4 || *level > 7) {
        printf("Illegal interrupt level %u, must be 4..7.\n", *level);
        return false;
    }
    return true;
}

bool qunibus_c::parse_vector(char *txt, uint16_t max_vector, uint16_t *vector)
{
    *vector = strtol(txt, NULL, 8);
    if (*vector > max_vector) {
        printf("Illegal interrupt vector %06o, must be <= %06o.\n", (unsigned) *vector,
               (unsigned) max_vector);
        return false;
    } else if ((*vector & 3) != 0) {
        printf("Illegal interrupt vector %06o, must be multiple of 4.\n", *vector);
        return false;
    }
    return true;
}

bool qunibus_c::parse_slot(char *txt, uint8_t *priority_slot)
{
    *priority_slot = strtol(txt, NULL, 10);
    if (*priority_slot <= 0 || *priority_slot > 31) {
        printf("Illegal priority slot %u, must be 1..31.\n", *priority_slot);
        return false;
    }
    return true;
}


// Drive one of the bus signals the PRU raises on command: INIT, ACLO/DCLO,
// POK/DCOK, HALT. The id and the value are two fields of a payload that shares
// its union with every other ARM2PRU command, so they are filled under the
// mailbox lock and the request goes out before it is released - a power cycle
// from the web API and a CPU start on another thread otherwise interleave
// their fills and the PRU acts on the mixture.
static bool set_initializationsignal(uint8_t id, uint8_t val)
{
    mailbox_lock_c lock;
    mailbox->initializationsignal.id = id;
    mailbox->initializationsignal.val = val;
    return mailbox_execute_locked(ARM2PRU_INITALIZATIONSIGNAL_SET);
}

/* pulse INIT cycle for some milliseconds
 */
void qunibus_c::init()
{
    set_initializationsignal(INITIALIZATIONSIGNAL_INIT, 1);
#if defined(UNIBUS)
    timeout_c::wait_ms(10); // UNIBUS: PDP-11/70 = 10ms
#elif defined(QBUS)
    timeout_c::wait_us(10); // QBUS only 10us !
#endif
    set_initializationsignal(INITIALIZATIONSIGNAL_INIT, 0);
}

// return: bitmask with shortcount BG*/NPG IN_OUT signals
// Values see PRIORITY_ARBITRATION_BIT_*
// Fiddling with the BG*/NPG signal may crash running CPU, also
// the M9302 will generate a SACK.
// So CPU is stopped with a surrounding a power cycle
uint8_t qunibus_c::probe_grant_continuity(bool error_if_closed)
{
    uint8_t grant_mask = 0;
    // simulate POWER OFF
    powercycle(1);
#if 0
    // CPU should be stopped now, holding BG*/NPG lines active LOW = logic 0.
    // If the power vector 24 does something weird, we may have
    // BG*/NPG set and have malfunctions now.

    // Test algorithm is difficult to implement.

    // First, pull INIT low to disable fucntion of other cards

    // 3 cases:
    // 	1) Running CPU on BUS: set a BR, wait for BGIN
    //	(not tested here)
    //
    // 2) If HALTed CPU on bus: BG*/NPG held LOW
    // Need M9302 with SACK turnaround.
    // 	- set each BGOUT/NPGOUT  (assume IN  0 by CPU)
    //  - if M9302 responds with SACK:
    //      it sees a BG 1 => no jumper IN-OUT
    //		if no SACK: M9302 sees a "0" => jumper set

    // 3) If no CPU on bus: BG*/NPG pulled up
    //     set BG OUT = 0, if IN 0 -> jumper!

    // Set BG*_OUT/NPG_OUT bits at latch 0
    // and read back. The read-back is part of the command: the answer sits in
    // the same union another thread's payload would overwrite, so the lock is
    // held until it has been taken.
    {
        mailbox_lock_c lock;
        mailbox->buslatch.addr = 0;
        mailbox->buslatch.bitmask = PRIORITY_ARBITRATION_BIT_MASK;
        mailbox->buslatch.val = 0x00;// output 0 = against pullups
        mailbox_execute_locked(ARM2PRU_BUSLATCH_SET);

        // Read back BG*_IN/NPG_IN bits from latch 0
//	mailbox->buslatch.addr = 0;
//	mailbox_execute_locked(ARM2PRU_BUSLATCH_GET);
        grant_mask = ~ (mailbox->buslatch.val & PRIORITY_ARBITRATION_BIT_MASK);
    }
#endif
    // simulate POWER ON
    powercycle(2);

    if (grant_mask && error_if_closed) {
        printf("Error: GRANT IN-OUT closed on UNIBUS backplane:");
        if (grant_mask & PRIORITY_ARBITRATION_BIT_B4)
            printf(" BG4");
        if (grant_mask & PRIORITY_ARBITRATION_BIT_B5)
            printf(" BG5");
        if (grant_mask & PRIORITY_ARBITRATION_BIT_B6)
            printf(" BG6");
        if (grant_mask & PRIORITY_ARBITRATION_BIT_B7)
            printf(" BG7");
        if (grant_mask & PRIORITY_ARBITRATION_BIT_NP)
            printf(" NPG");
        printf(".\n");
        exit(1);
    }

    return grant_mask;
}

/* Simulate a power cycle
 * phase: 0x01 = only OFF, 0x02 = only ON, 0x03 = ON and OFF
 */
void qunibus_c::powercycle(int phase)
{
    const unsigned delay_ms = 200; // time between phases. 70ns for QBUS
#if defined(UNIBUS)
    /* Sequence:
     * 1. Line power fail -> ACLO active
     * 2. Power supply capacitors empty -> DCLO active
     * 3. Logic power OK -> DCLO inactive
     * 4. Line power back -> ACLO inactive
     *	 ACLO ist specified to go unasserted AFTER DCLO.
     *	 For example, M9312 works only on ACLO as startup condition.
     */
    if (phase & 0x01) { // Power Down
        set_initializationsignal(INITIALIZATIONSIGNAL_ACLO, 1);
        timeout_c::wait_ms(delay_ms);
        set_initializationsignal(INITIALIZATIONSIGNAL_DCLO, 1);
        timeout_c::wait_ms(delay_ms);
    }
    if (phase & 0x02) { // Power Up
        set_initializationsignal(INITIALIZATIONSIGNAL_DCLO, 0);
        timeout_c::wait_ms(delay_ms);
        // CPU generates INIT
        set_initializationsignal(INITIALIZATIONSIGNAL_ACLO, 0);
        timeout_c::wait_ms(delay_ms);
        // CPU executes power fail vector
    }
#elif defined(QBUS)
    if (phase & 0x01) { // Power Down
        // "If the ac voltage to a power supply drops below 75% of the
        // nominal voltage for one full line cycle (15 - 24 ms), BPOK H
        // is negated by the power supply. Once BPOK H is negated the
        // entire power down sequence must be completed.
        // A device that requested bus mastership before the power
        // failure, and has not become bus master, may maintain the
        // request u n til BINIT L is asserted or the request is
        // acknowledged (in which case regular bus protocol is followed)."
        set_initializationsignal(INITIALIZATIONSIGNAL_POK, 0);
        // "Processor software should execute a RESET Instruction 3 ms
        // minimun after the negation of BPOK H. This asserts BINIT L
        // for from 8 to 20 us. Processor software executes a HALT
        // instruction imnediately following the RESET instruction."

        //	"BDCOK H must be negated a minimum of 4 ms after the negation
        // of BPOK H. This 4 ms allots mass storage and similar devices
        // to protect themselves against erasures and erroneous writes
        // during a power failure.""
        timeout_c::wait_ms(delay_ms);
        set_initializationsignal(INITIALIZATIONSIGNAL_DCOK, 0);
        // "The Processor asserts BINIT L 1 us minimum after the negation of BDCOK H."
        // "Dc power must remain stable for a minimum of 5 us after the negation of BDCOK H."
        // "BDCOK H must remain negated for a minimnn of 3 ms."
        timeout_c::wait_ms(delay_ms);
    }
    if (phase & 0x02) { // Power Up
        // The DEC sequence asserts BDCOK first and BPOK ~70 ms later, and "the
        // assertion of BPOK H will cause a processor interrupt". Released as
        // BDCOK asserts, the CPU boots during that gap and takes the BPOK
        // interrupt through the power-fail vector mid-boot ("power fail in boot").
        // Assert BPOK first, while BINIT still holds the CPU, so it cold-starts
        // with power already up and the boot runs uninterrupted.
        set_initializationsignal(INITIALIZATIONSIGNAL_POK, 1);
        timeout_c::wait_ms(delay_ms);
        // "Power supply logic ... asserts BDCOK H 3 ms minimum after dc power is
        // restored." The processor negates BINIT after the assertion of BDCOK H
        // and starts from the power-up vector, with BPOK already asserted.
        set_initializationsignal(INITIALIZATIONSIGNAL_DCOK, 1);
        timeout_c::wait_ms(delay_ms);
    }
#endif
}

#if defined(QBUS)
// set state of QBUS  HALT line, like HALT toggle switch on QBUS front panels
void qunibus_c::set_halt(bool active)
{
    set_initializationsignal(INITIALIZATIONSIGNAL_HALT, active);
}
#endif


#if defined(UNIBUS)
void qunibus_c::set_address_overlay(uint32_t address_overlay)
{
    // address_overlay has a field of its own rather than a place in the
    // payload union, but it is still half of a command: filled here, acted on
    // by the request, so the two go together under the lock.
    mailbox_lock_c lock;
    mailbox->address_overlay = address_overlay;
    mailbox_execute_locked(ARM2PRU_ADDRESS_OVERLAY);
}

// check: UNIBUS ADDR lines manipulated by (M9312) overlay?
bool qunibus_c::is_address_overlay_active() {
    return (mailbox->address_overlay != 0);
}
#endif

// force CPU to be silent on BUS
// Only necessary on QBUS:
// even a HALTed CPU runs ODT and polls the SLU for user I/O.
void qunibus_c::set_cpu_bus_activity(bool active)
{
    UNUSED(active) ;
#if defined(QBUS)
    mailbox_execute(ARM2PRU_CPU_BUS_ACCESS, active);
#endif
}

void qunibus_c::set_arbitrator_active(bool active)
{
    if (active) {
        mailbox_execute(ARM2PRU_ARB_MODE_CLIENT);
    } else {
        mailbox_execute(ARM2PRU_ARB_MODE_NONE);
    }
    arbitrator_active = active;
}

bool qunibus_c::get_arbitrator_active(void)
{
    return arbitrator_active;
}

// do a DMA transaction with or without arbitration (arbitration_client)
// mailbox.dma.words already filled
// if result = timeout: =
// 0 = bus time, error address =  mailbox->dma.cur_addr
// 1 = all transfered
// A limit for time used by DMA can be compiled-in
bool qunibus_c::dma(bool blocking, uint8_t qunibus_cycle, uint32_t startaddr, uint16_t *buffer,
                    unsigned wordcount, bool share_bus, unsigned timeout_ms)
{
    int dma_bandwidth_percent = 50; // use 50% of time for DMA, rest for running PDP-11 CPU
    uint64_t dmatime_ns, totaltime_ns;
    // can access bus with DMA when there's a Bus Arbitrator
    assert(pru->emulating());

    // A transfer running past the end of the machine's address space is a
    // programming error to the adapter, which asserts - and an assert takes the
    // whole emulator down, machine and all, over an address somebody typed.
    // Refuse it here instead: the caller sees a transfer that did not happen,
    // which is also what it would have seen from a bus that did not answer.
    if ((uint64_t) startaddr + 2 * (uint64_t) wordcount > (uint64_t) addr_space_byte_count) {
        ERROR("DMA of %u words at %s runs past the end of the %u bit address space",
              wordcount, addr2text(startaddr), addr_width);
        return false;
    }

    timeout.start_ns(0); // no timeout, just running timer
    qunibusadapter->DMA(*dma_request, blocking, qunibus_cycle, startaddr, buffer, wordcount,
                        timeout_ms);

    dmatime_ns = timeout.elapsed_ns();
    // Wait before the next transaction, to leave the running PDP-11 the rest of
    // the bus. A single-word transfer the board makes for itself - a probe
    // before a card is placed - takes no bandwidth worth sharing, and the pause
    // is charged on what the cycle took: a cycle that waited seconds for the
    // machine to grant the bus would be followed by seconds of sleep.
    if (share_bus) {
        // calc required total time for DMA time + wait
        // 100% -> total = dma
        // 50% -> total = 2*dma
        // 25% -> total = 4* dma
        totaltime_ns = (dmatime_ns * 100) / dma_bandwidth_percent;
        // whole transaction requires totaltime, dma already done
        timeout.wait_ns(totaltime_ns - dmatime_ns);
    }

    return dma_request->success; // only useful if blocking
}

/* scan qunibus addresses ascending from 0.
 * Stop on error, return first invalid address
 * return 0: no memory found at all
 * arbitration_active: if 1, perform NPR/NPG/SACK resp. DMR/DMG/SACK arbitration before mem accesses
 * words[]: buffer for whole QBUS/UNIBUS address range, is filled with data
 *
 * no_grant, when given, tells the two zeroes apart: memory that starts at the
 * first address with nothing in it, and a backplane that granted the board
 * nothing at all. Both mean "nothing answered", but only the second is worth
 * reporting to whoever asked for the sweep.
 */
uint32_t qunibus_c::test_sizer(bool *no_grant)
{
    if (no_grant != nullptr)
        *no_grant = false;

    // The sweep is made a chunk at a time rather than as one transfer the
    // adapter splits, because each chunk is then bounded. An unbounded one
    // parks this thread for good on a backplane that never grants the bus - a
    // machine switched off, or one whose processor is not arbitrating - and
    // sizing memory is exactly what is asked before a card is placed in such a
    // machine. Issue #95: the sweep took the device layer's lock with it, and
    // nothing short of a service restart got it back.
    //
    // Bounding every chunk rather than only the first is deliberate: a machine
    // can stop arbitrating in the middle of a sweep - the operator powers it
    // down, or halts it - and the part still to walk is the expensive part.
    const unsigned chunk_timeout_ms = 2000;

    // The sweep ends on a bus timeout: that is the answer it went looking for,
    // not a device losing a transfer, so the adapter is told to expect it.
    dma_request->timeout_expected = true;

    timeout_c sweep_time;
    sweep_time.start_ns(0);
    uint64_t elapsed_ms = 0;
    uint32_t end_addr = 0;

    for (unsigned words_done = 0; words_done < addr_space_word_count; ) {
        unsigned chunk_words = addr_space_word_count - words_done;
        if (chunk_words > PRU_MAX_DMA_WORDCOUNT)
            chunk_words = PRU_MAX_DMA_WORDCOUNT;
        uint32_t addr = 2 * words_done;

        uint64_t before_ms = elapsed_ms;
        qunibusadapter->DMA(*dma_request, true, QUNIBUS_CYCLE_DATI, addr,
                            testwords + words_done, chunk_words, chunk_timeout_ms);
        elapsed_ms = sweep_time.elapsed_ms();

        if (dma_request->success) {
            end_addr = dma_request->qunibus_end_addr; // last address that answered
            words_done += chunk_words;
            continue;
        }
        // Nothing granted that chunk: it took the whole bound, where a slave
        // that simply did not answer ends the cycle in microseconds. No address
        // above can answer either, since it is the bus and not the address that
        // is silent.
        if (elapsed_ms - before_ms >= chunk_timeout_ms) {
            INFO("memory sizing stopped at %s after %llu ms: nothing is "
                 "arbitrating the bus, so nothing can answer",
                 addr2text(addr), (unsigned long long) elapsed_ms);
            dma_request->timeout_expected = false;
            if (no_grant != nullptr)
                *no_grant = true;
            return 0;
        }
        // A bus timeout, which is what the sweep is looking for: the address it
        // stopped at is the first that nothing implements.
        dma_request->timeout_expected = false;
        return dma_request->qunibus_end_addr;
    }

    // Everything answered, up to the top of the address space.
    dma_request->timeout_expected = false;
    return end_addr;
}

/* probe_range(): does anything on the bus answer inside [startaddr, endaddr]?
 *
 * DATIs both ends and one word every "step" bytes, and returns the lowest
 * address that answered, else QUNIBUS_PROBE_NONE. Used before claiming a range
 * for emulated memory: two slaves answering one cycle drive the bus against
 * each other, and the damage shows up somewhere else entirely.
 *
 * The board answers its own emulated ranges, so probe before claiming, not
 * after. A range that stays silent is not proven free — a card may decode
 * addresses it does not answer — so this can only refuse, never confirm.
 */
uint32_t qunibus_c::probe_range(uint32_t startaddr, uint32_t endaddr, uint32_t step)
{
    if (startaddr > endaddr || step < 2)
        return QUNIBUS_PROBE_NONE;

    // Every cycle here is expected to time out, and none of them shares the bus
    // with anything: the machine is not running the range being probed.
    dma_request->timeout_expected = true;
    timeout_c probe_time;
    probe_time.start_ns(0);
    unsigned cycles = 0;
    uint64_t first_cycle_ms = 0;
    uint32_t answered = QUNIBUS_PROBE_NONE;

    // Each cycle is bounded. An unbounded one parks this thread for good on a
    // backplane that never grants the bus - a machine switched off, or one
    // whose processor is not arbitrating - and that is the state a range is
    // probed in before a memory card is placed. A granted cycle answers or
    // times out on the bus in microseconds, so the bound costs a live machine
    // nothing; it is generous because the first grant after a power cycle is
    // the slow one.
    const unsigned probe_cycle_timeout_ms = 2000;

    uint64_t elapsed_ms = 0;
    for (uint32_t addr = startaddr; addr <= endaddr; addr += step) {
        cycles++;
        uint64_t before_ms = elapsed_ms;
        bool hit = dma(true, QUNIBUS_CYCLE_DATI, addr, &probe_word_buffer, 1,
                       /*share_bus*/false, probe_cycle_timeout_ms);
        elapsed_ms = probe_time.elapsed_ms();
        if (cycles == 1)
            first_cycle_ms = elapsed_ms;
        // Nothing granted that cycle: it took the whole bound, where a slave
        // that simply did not answer takes microseconds. No address in the
        // range can answer either, and walking it would pay the bound at every
        // step - minutes of them across a memory card's range, with the
        // adapter's operations_mutex held for all of it. Nothing answers, which
        // is what the caller asked. Every step is timed, not only the first: a
        // machine can stop arbitrating in the middle of a probe - the operator
        // powers it down, or halts it - and the walk that is left to do is the
        // expensive part.
        if (!hit && elapsed_ms - before_ms >= probe_cycle_timeout_ms) {
            INFO("probe of %s..%s stopped at %s after %u cycles: nothing is "
                 "arbitrating the bus, so nothing can answer",
                 addr2text(startaddr), addr2text(endaddr), addr2text(addr), cycles);
            dma_request->timeout_expected = false;
            return QUNIBUS_PROBE_NONE;
        }
        if (hit) {
            answered = addr;
            break;
        }
        if (endaddr - addr < step)
            break; // last step would wrap
    }
    // the end of the range need not fall on a step boundary
    if (answered == QUNIBUS_PROBE_NONE) {
        cycles++;
        if (dma(true, QUNIBUS_CYCLE_DATI, endaddr, &probe_word_buffer, 1,
                /*share_bus*/false, probe_cycle_timeout_ms))
            answered = endaddr;
    }

    // What the probe cost, in the terms it can be shortened by: how many cycles
    // the step asked for, and how long they took. The first cycle is called out
    // because it carries what the machine takes to grant the bus after a power
    // cycle, which the rest of the walk does not pay again.
    INFO("probed %s..%s in %u cycles, %llu ms (first cycle %llu ms)",
            addr2text(startaddr), addr2text(endaddr), cycles,
            (unsigned long long) probe_time.elapsed_ms(),
            (unsigned long long) first_cycle_ms);
    dma_request->timeout_expected = false;
    return answered;
}

/* probe_word(): DATI one address that is allowed not to answer.
 *
 * probe_range() asks whether anything is there; this asks what it says. The
 * caller wants both outcomes - a processor that puts its registers on the bus
 * and one that does not look alike until the cycle is made - so a timeout is
 * reported rather than logged, and the adapter is told to expect it so it does
 * not count as a bus error of the running machine.
 */
bool qunibus_c::probe_word(uint32_t addr, uint16_t *w, bool share_bus, unsigned timeout_ms)
{
    dma_request->timeout_expected = true;
    bool answered = dma(true, QUNIBUS_CYCLE_DATI, addr, w, 1, share_bus, timeout_ms);
    dma_request->timeout_expected = false;
    return answered;
}

/*
 * Test memory from 0 .. end_addr
 * mode = 1: fill every word with its address, then check endlessly,
 */

// write a subset of words[] with QBUS/UNIBUS DMA:
// all words from start_addr to including end_addr
//
// DMA blocksize can be choosen arbitrarily
void qunibus_c::mem_write(uint16_t *words, unsigned unibus_start_addr, unsigned unibus_end_addr,
                          bool *result_timeout, unsigned timeout_ms)
{
    unsigned wordcount = (unibus_end_addr - unibus_start_addr) / 2 + 1;
    uint16_t *buffer_start_addr = words + unibus_start_addr / 2;
    assert(pru->emulating());
    *result_timeout = !dma(true, QUNIBUS_CYCLE_DATO, unibus_start_addr, buffer_start_addr, wordcount,
                           /*share_bus*/true, timeout_ms);
    if (*result_timeout) {
        printf("\nWrite result_timeout @ %s\n", qunibus->addr2text(mailbox->dma.cur_addr));
        return;
    }
}

// Read a subset of words[] with QBUS/UNIBUS DMA
// all words from start_addr to including end_addr
// DMA blocksize can be choosen arbitrarily
// arbitration_active: if 1, perform NPR/NPG/SACK resp. DMR/DMG/SACK arbitration before mem accesses
void qunibus_c::mem_read(uint16_t *words, uint32_t unibus_start_addr, uint32_t unibus_end_addr,
                         bool *result_timeout, unsigned timeout_ms)
{
    unsigned wordcount = (unibus_end_addr - unibus_start_addr) / 2 + 1;
    uint16_t *buffer_start_addr = words + unibus_start_addr / 2;
    assert(pru->emulating());

    *result_timeout = !dma(true, QUNIBUS_CYCLE_DATI, unibus_start_addr, buffer_start_addr, wordcount,
                           /*share_bus*/true, timeout_ms);
    if (*result_timeout) {
        printf("\nRead result_timeout @ %s\n", qunibus->addr2text(mailbox->dma.cur_addr));
        return;
    }
}

// read or write
void qunibus_c::mem_access_random(uint8_t unibus_control, uint16_t *words,
                                  uint32_t unibus_start_addr, uint32_t unibus_end_addr, bool *result_timeout,
                                  uint32_t *block_counter)
{
    uint32_t block_unibus_start_addr, block_unibus_end_addr;
    // in average, make 16 sub transactions
    assert(pru->emulating());
    assert(unibus_control == QUNIBUS_CYCLE_DATI || unibus_control == QUNIBUS_CYCLE_DATO);
    block_unibus_start_addr = unibus_start_addr;
    // split transaction in random sized blocks
    uint32_t max_block_wordcount = (unibus_end_addr - unibus_start_addr + 2) / 2;

    do {
        uint16_t *block_buffer_start = words + block_unibus_start_addr / 2;
        uint32_t block_wordcount;
        do {
            block_wordcount = random32_log(max_block_wordcount);
        } while (block_wordcount < 1);
        assert(block_wordcount < max_block_wordcount);
        // wordcount limited by "words left to transfer"
        block_wordcount = std::min(block_wordcount,
                                   (unibus_end_addr - block_unibus_start_addr) / 2 + 1);
        block_unibus_end_addr = block_unibus_start_addr + 2 * block_wordcount - 2;
        assert(block_unibus_end_addr <= unibus_end_addr);
        (*block_counter) += 1;
        // printf("%06d: %5u words %06o-%06o\n", *block_counter, block_wordcount, block_unibus_start_addr, block_unibus_end_addr) ;
        *result_timeout = !dma(true, unibus_control, block_unibus_start_addr, block_buffer_start,
                               block_wordcount);
        if (*result_timeout) {
            printf("\n%s result_timeout @ %s\n", control2text(unibus_control),
                   qunibus->addr2text(mailbox->dma.cur_addr));
            return;
        }
        block_unibus_start_addr = block_unibus_end_addr + 2;
    } while (block_unibus_start_addr <= unibus_end_addr);
}

// print a "memory test mismatch" message
// uses "testwords[]"
void qunibus_c::test_mem_print_error(uint32_t mismatch_count, uint32_t start_addr,
                                     uint32_t end_addr, uint32_t cur_test_addr, uint16_t found_mem_val)
{
    uint16_t expected_mem_val = testwords[cur_test_addr / 2];
    // print bitwise error mask
    printf("\nMemory mismatch #%u at %s: expected %06o, found %06o, diff mask = %06o.  ",
           mismatch_count, qunibus->addr2text(cur_test_addr), expected_mem_val, found_mem_val,
           expected_mem_val ^ found_mem_val);

    // to analyze address errors: into which addresses should the test value have been written.
    int mem_val_found_count = 0;
    for (uint32_t addr = start_addr; addr < end_addr; addr += 2)
        if (testwords[addr / 2] == found_mem_val) {
            if (mem_val_found_count == 0)
                printf("\n  Found mem value %06o was written to addresses:", found_mem_val);
            printf(" %s", qunibus->addr2text(addr));
            mem_val_found_count++;
        }
    if (mem_val_found_count == 0)
        printf("\n Test value %06o was never written in this pass.", expected_mem_val);
}

// arbitration_active: if 1, perform NPR/NPG/SACK arbitration before mem accesses
bool qunibus_c::test_mem(uint32_t start_addr, uint32_t end_addr, unsigned mode)
{
#define MAX_ERROR_COUNT	8
    progress_c progress = progress_c(80);
    bool has_timeout = false, mismatch = false;
    unsigned mismatch_count = 0;
    uint32_t cur_test_addr;
    unsigned pass_count = 0, total_read_block_count = 0, total_write_block_count = 0;

    assert(pru->emulating());

    // Setup ^C catcher
    SIGINTcatchnext();
    switch (mode) {
    case 1: // single write, multiple read, "address" pattern
        /**** 1. Generate test values: only for even addresses
         */
        for (cur_test_addr = start_addr; cur_test_addr <= end_addr; cur_test_addr += 2)
            testwords[cur_test_addr / 2] =
                // even 18 bit address  => 17 bits significant => msb bit 17 as XOR
                ((cur_test_addr >> 1) & 0xffff) ^ (cur_test_addr >> 17);
        /**** 2. Write memory ****/
        progress.put("W");  //info : full memory write
        mem_write(testwords, start_addr, end_addr, &has_timeout);

        /**** 3. read until ^C ****/
        while (!SIGINTreceived && !has_timeout && !mismatch_count) {
            pass_count++;
            if (pass_count % 10 == 0)
                progress.putf(" %d ", pass_count);
            total_write_block_count++; // not randomized
            total_read_block_count++;
            progress.put("R");
            // read back into unibus_membuffer[]
            mem_read(membuffer->data.words, start_addr, end_addr, &has_timeout);
            // compare
            for (mismatch_count = 0, cur_test_addr = start_addr; cur_test_addr <= end_addr;
                    cur_test_addr += 2) {
                uint16_t cur_mem_val = membuffer->data.words[cur_test_addr / 2];
                mismatch = (testwords[cur_test_addr / 2] != cur_mem_val);
                if (mismatch && ++mismatch_count <= MAX_ERROR_COUNT) // print only first errors
                    test_mem_print_error(mismatch_count, start_addr, end_addr, cur_test_addr,
                                         cur_mem_val);
            }
        } // while
        break;

    case 2: // full write, full read
        /**** 1. Full write generate test values */
//		start_addr = 0;
//		end_addr = 076;
        while (!SIGINTreceived && !has_timeout && !mismatch_count) {
            pass_count++;
            if (pass_count % 10 == 0)
                progress.putf(" %d ", pass_count);

            for (cur_test_addr = start_addr; cur_test_addr <= end_addr; cur_test_addr += 2)
                testwords[cur_test_addr / 2] = random24() & 0xffff; // random
//				testwords[cur_test_addr / 2] = (cur_test_addr >> 1) & 0xffff; // linear

            progress.put("W");  //info : full memory write
            mem_access_random(QUNIBUS_CYCLE_DATO, testwords, start_addr, end_addr, &has_timeout,
                              &total_write_block_count);

            if (SIGINTreceived || has_timeout)
                break; // leave loop

            // first full read
            progress.put("R");  //info : full memory write
            // read back into unibus_membuffer[]
            mem_access_random(QUNIBUS_CYCLE_DATI, membuffer->data.words, start_addr, end_addr,
                              &has_timeout, &total_read_block_count);
            // compare
            for (mismatch_count = 0, cur_test_addr = start_addr; cur_test_addr <= end_addr;
                    cur_test_addr += 2) {
                uint16_t cur_mem_val = membuffer->data.words[cur_test_addr / 2];
                mismatch = (testwords[cur_test_addr / 2] != cur_mem_val);
                if (mismatch && ++mismatch_count <= MAX_ERROR_COUNT) // print only first errors
                    test_mem_print_error(mismatch_count, start_addr, end_addr, cur_test_addr,
                                         cur_mem_val);
            }
        } // while
        break;
    } // switch(mode)
    printf("\n");
    if (has_timeout || mismatch_count)
        printf("Stopped by error: %stimeout, %d mismatches\n", (has_timeout ? "" : "no "),
               mismatch_count);
    else
        printf("All OK! Total %d passes, split into %d block writes and %d block reads\n",
               pass_count, total_write_block_count, total_read_block_count);
    return !has_timeout && mismatch_count == 0;
}

