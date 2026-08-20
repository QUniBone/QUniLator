/* delqa.cpp: DELQA (M7516) Qbus Ethernet controller

   Copyright (c) 2026, Hans Huebner
   MIT license, see delqa.hpp for the full text.

   Register file, reset/self-test, buffer descriptor list processing and
   interrupt logic of the DELQA.  The eth0 packet bridge attaches through
   transmit_packet() and the receive queue.

   The host manipulates the controller through DATO cycles on the
   BDL address registers, VAR and CSR.  Register semantics (bit masks,
   mode switching, self-test) execute directly in the register access
   callback; everything that needs bus DMA (walking the buffer
   descriptor lists) runs on the worker thread.

   Locking: controller state (CSR, VAR, receive queue, setup filter) is
   guarded by on_after_register_access_mutex.  The register access
   callback runs on the qunibusadapter event thread, which also completes
   DMA transfers - so the worker never holds the state mutex across a
   blocking DMA call.
*/

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fstream>
#include <string>

#include "logger.hpp"
#include "timeout.hpp"
#include "utils.hpp" // now_ms()
#include "gpios.hpp" // activity_leds
#include "qunibus.h"
#include "qunibusadapter.hpp"
#include "delqa.hpp"
#include "delqa_bootrom.h"


delqa_c::delqa_c() :
        qunibusdevice_c(),
        VAR_reg(nullptr),
        CSR_reg(nullptr),
        csr(0),
        var(0),
        deqna_mode(false),
        interrupt_asserted(false),
        read_queue_loss(0),
        pending_rbdl(false),
        pending_xbdl(false),
        pending_deliver(false),
        pending_bootrom(false),
        pending_wakeup(false),
        rbdl_ba(0),
        xbdl_ba(0),
        carrier_off_ms(0)
{
    set_workers_count(2); // 0: BDL engine, 1: eth0 bridge receiver

    name.value = "delqa";
    type_name.value = "DELQA";
    log_label = "delqa";

    // A raw socket on the uplink cannot reach the host it runs on, so the
    // controller gets an interface of its own: the veth the QBone package
    // creates and joins to the bridge that carries the uplink.
    interface.value = "veth-pdp";
    mac.value = ethernet_default_station_address("delqa");
    deqna_lock.value = false;
    activity_led.value = 3;

    // running state driven by traffic, not configuration
    activity_led.kind = parameter_c::PARAM_STATUS;
    activity_lamp.kind = parameter_c::PARAM_STATUS;

    memset(rbdl, 0, sizeof(rbdl));
    memset(xbdl, 0, sizeof(xbdl));
    memset(&setup, 0, sizeof(setup));

    // standard first Qbus Ethernet controller: 17774440, vector 120
    set_default_bus_params(0774440, 21, 0120, 4);

    // The DELQA has eight registers.  The first six read as the station
    // address PROM; writes to registers 2..5 set the receive and transmit
    // BDL start addresses.
    register_count = 8;

    for (unsigned i = 0; i < 6; i++) {
        qunibusdevice_register_t *reg = &(this->registers[i]);
        sprintf(reg->name, "SA%d", i);
        reg->active_on_dati = false;
        reg->active_on_dato = (i >= 2); // BDL address writes need processing
        reg->reset_value = 0xff00;
        reg->writable_bits = 0x0000;    // reads always deliver the PROM
    }
    // BDL address registers accept any value
    for (unsigned i = 2; i < 6; i++)
        this->registers[i].writable_bits = 0xffff;

    VAR_reg = &(this->registers[6]);
    strcpy(VAR_reg->name, "VAR");
    VAR_reg->active_on_dati = false;
    VAR_reg->active_on_dato = true;
    VAR_reg->reset_value = 0;
    VAR_reg->writable_bits = 0xffff;

    CSR_reg = &(this->registers[7]);
    strcpy(CSR_reg->name, "CSR");
    CSR_reg->active_on_dati = false;
    CSR_reg->active_on_dato = true;
    CSR_reg->reset_value = CSR_RL | CSR_XL;
    CSR_reg->writable_bits = 0xffff;

    parse_mac(mac.value);
}

delqa_c::~delqa_c()
{
}

// parse_mac(): accept 6 hex bytes separated by ':' or '-'.
// Returns false and leaves the station address untouched on bad input.
bool delqa_c::parse_mac(const std::string &text)
{
    unsigned bytes[6];
    if (sscanf(text.c_str(), "%x%*[:-]%x%*[:-]%x%*[:-]%x%*[:-]%x%*[:-]%x",
            &bytes[0], &bytes[1], &bytes[2], &bytes[3], &bytes[4], &bytes[5]) != 6)
        return false;
    for (unsigned i = 0; i < 6; i++)
        if (bytes[i] > 0xff)
            return false;
    if (bytes[0] & 0x01)
        return false; // station addresses are unicast
    for (unsigned i = 0; i < 6; i++)
        station_address[i] = bytes[i];
    make_prom_checksum();
    return true;
}

// make_prom_checksum(): checksum over the station address, as read from
// PROM bytes 0/1 in external loopback mode.  Algorithm documented in the
// DEC VAX Ethernet boot driver (xqbtdrivr.mar).
void delqa_c::make_prom_checksum(void)
{
    uint32_t checksum = 0;
    const uint32_t wmask = 0xffff;

    for (unsigned i = 0; i < 6; i += 2) {
        checksum <<= 1;
        if (checksum > wmask)
            checksum -= wmask;
        checksum += (station_address[i] << 8) | station_address[i + 1];
        if (checksum > wmask)
            checksum -= wmask;
    }
    if (checksum == wmask)
        checksum = 0;

    prom_checksum[0] = checksum & 0xff;
    prom_checksum[1] = (checksum >> 8) & 0xff;
}

// update_prom_registers(): expose the station address in the DATI values
// of registers 0..5.  In external loopback mode bytes 0/1 deliver the
// PROM checksum instead, which diagnostics use to verify the address.
void delqa_c::update_prom_registers(void)
{
    if (handle == 0)
        return; // not plugged into the bus

    for (unsigned i = 0; i < 6; i++) {
        uint8_t byte;
        if ((csr & CSR_EL) && i < 2)
            byte = prom_checksum[i];
        else
            byte = station_address[i];
        set_register_dati_value(&(this->registers[i]), 0xff00 | byte, __func__);
    }
}

bool delqa_c::on_param_changed(parameter_c *param)
{
    if (param == &priority_slot) {
        dma_request.set_priority_slot(priority_slot.new_value);
        intr_request.set_priority_slot(priority_slot.new_value);
    } else if (param == &intr_level) {
        intr_request.set_level(intr_level.new_value);
    } else if (param == &intr_vector) {
        return false; // the host sets the vector by writing VAR
    } else if (param == &mac) {
        if (!parse_mac(mac.new_value))
            return false;
        pthread_mutex_lock(&on_after_register_access_mutex);
        update_prom_registers();
        pthread_mutex_unlock(&on_after_register_access_mutex);
    } else if (param == &deqna_lock) {
        if (enabled.value)
            return false; // configuration switch, change while disabled
    }
    return qunibusdevice_c::on_param_changed(param);
}

// set_interrupt(): request an interrupt at the vector programmed into VAR.
// Caller holds on_after_register_access_mutex.
void delqa_c::set_interrupt(void)
{
    uint16_t vector = var & VAR_IV;
    if (vector == 0)
        return; // interrupts disabled until the host programs a vector
    interrupt_asserted = true;
    qunibusadapter->INTR(intr_request, NULL, 0);
}

void delqa_c::clear_interrupt(void)
{
    interrupt_asserted = false;
    qunibusadapter->cancel_INTR(intr_request);
}

// update_csr(): change CSR bits and derive the interrupt request state
// from the IE/XI/RI edges this change produces.
// Caller holds on_after_register_access_mutex.
void delqa_c::update_csr(uint16_t set_bits, uint16_t clear_bits)
{
    uint16_t saved_csr = csr;

    csr = (csr | set_bits) & ~clear_bits;
    if (handle != 0)
        set_register_dati_value(CSR_reg, csr, __func__);

    // external loopback selects between PROM address and checksum bytes
    if ((saved_csr ^ csr) & CSR_EL)
        update_prom_registers();

    // The DEQNA/DELQA interrupt is level-sensitive: it stands asserted while
    // (XI | RI) & IE is true and a vector is programmed, and drops only when the
    // host clears the requesting bit (W1C) or IE. The QBUS adapter delivers one
    // vector per INTR() and forgets the request once the PRU grants it, so the
    // level must be re-presented whenever it is still high after a CSR change or
    // a completion — otherwise a second source that becomes ready while an
    // earlier interrupt is still pending is stranded with no vector of its own,
    // and the driver's ring reaping stalls the CPU at the device's BR level.
    bool want_intr = (csr & CSR_IE) && (csr & CSR_XIRI) && (var & VAR_IV);
    if (want_intr && !interrupt_asserted)
        set_interrupt();
    else if (want_intr && interrupt_asserted)
        qunibusadapter->INTR(intr_request, NULL, 0); // re-arm; deduped if still pending
    else if (!want_intr && interrupt_asserted)
        clear_interrupt();
}

// software_reset(): CSR SR transition 1 -> 0, bus INIT or power cycle.
// Caller holds on_after_register_access_mutex.
void delqa_c::software_reset(void)
{
    DEBUG("software reset");

    const uint16_t set_bits = CSR_XL | CSR_RL;
    update_csr(set_bits, ~set_bits);

    // transceiver reports power ok as long as the controller is running.
    // Carrier is a live signal, so it starts absent and returns with the
    // next transmission that reaches the port.
    update_csr(CSR_OK, CSR_CA);
    carrier_off_ms = 0;

    clear_interrupt();

    pending_rbdl = false;
    pending_xbdl = false;
    pending_deliver = false;

    // the internal receive FIFO survives a software reset: the MOP boot
    // firmware resets the controller after every received frame, and a
    // wire frame buffered behind the one being processed (a load response
    // behind a broadcast) must stay deliverable across that reset.
    // Reflections belong to the pre-reset loopback context and are dropped.
    for (auto it = read_queue.begin(); it != read_queue.end();) {
        if (it->type == packet_c::NORMAL) {
            it->used = 0;
            ++it;
        } else {
            it = read_queue.erase(it);
        }
    }
    read_queue_loss = 0;

    // The address filter survives: a software reset restarts the lists and
    // the state machine, and leaves the setup RAM holding the addresses the
    // host last programmed. The diagnostic resets and then loops a frame
    // back to an address it set up beforehand, expecting it recognised.
    // The filter modes are controller state rather than stored addresses,
    // so they go back to naming nothing until the next setup packet asks
    // for them again.
    setup.promiscuous = false;
    setup.all_multicast = false;
}

// power_on_reset(): DCLO cycle.  Full reset including VAR.
// Caller holds on_after_register_access_mutex.
void delqa_c::power_on_reset(void)
{
    deqna_mode = deqna_lock.value;
    var = deqna_mode ? 0 : VAR_MS;
    intr_request.set_vector(0);
    intr_vector.value = 0;
    if (handle != 0)
        set_register_dati_value(VAR_reg, var, __func__);

    // power cycling clears the setup RAM, so the filter starts empty and the
    // PROM station address is the only one the controller answers to
    memset(&setup, 0, sizeof(setup));

    software_reset();
    update_prom_registers();
}

// write_var(): host write to the Vector Address Register.
// Caller holds on_after_register_access_mutex.
void delqa_c::write_var(uint16_t data)
{
    uint16_t saved_var = var;

    if (deqna_lock.value) {
        // DEQNA-lock: only the vector and the identity bit respond
        var = data & (VAR_IV | VAR_ID);
    } else {
        var = data & VAR_RW_MASK;

        if ((saved_var ^ var) & VAR_MS) {
            if (~var & VAR_MS) {
                DEBUG("switching to DEQNA-lock mode");
                deqna_mode = true;
                var &= ~(VAR_OS | VAR_RS | VAR_ST);
            } else {
                DEBUG("switching to DELQA normal mode");
                deqna_mode = false;
            }
        }

        // self-test request: complete immediately and report the result
        if (var & VAR_RS) {
            var &= ~VAR_RS;
            var &= ~VAR_ST; // all status bits clear = success
            DEBUG("self test performed");
        }
    }

    uint16_t vector = data & VAR_IV;
    intr_request.set_vector(vector);
    intr_vector.value = vector;

    set_register_dati_value(VAR_reg, var, __func__);
}

// write_csr(): host write to the Control and Status Register.
// Caller holds on_after_register_access_mutex.
void delqa_c::write_csr(uint16_t data)
{
    uint16_t set_bits = data & CSR_RW_MASK;
    uint16_t clear_bits = ((data ^ CSR_RW_MASK) & CSR_RW_MASK) // RW bits written as 0
            | (data & CSR_W1_MASK)                             // write 1 to clear
            | ((data & CSR_XI) ? CSR_NI : 0);                  // clearing XI clears NI

    // reset controller when SR transitions to cleared
    if (csr & CSR_SR & ~data) {
        software_reset();
        return;
    }

    if (~csr & CSR_RE & data) {
        DEBUG("receiver enabled");
        // deliver already queued packets to the receive list
        if (!read_queue.empty()) {
            pending_deliver = true;
            wakeup_worker();
        }
        // TODO stage 3: start the eth0 bridge receiver
    }
    if (csr & CSR_RE & ~data)
        DEBUG("receiver disabled");

    update_csr(set_bits, clear_bits);

    // boot/diagnostic ROM load: BD and EL both set (CSR_BP). Deferred to the
    // worker because it walks the receive BDL with DMA.
    if ((csr & CSR_BOOT) == CSR_BOOT) {
        DEBUG("boot/diagnostic ROM requested");
        pending_bootrom = true;
        wakeup_worker();
    }
}

// wakeup_worker(): hand pending BDL work to the worker thread.
// Caller holds on_after_register_access_mutex.
void delqa_c::wakeup_worker(void)
{
    pending_wakeup = true;
    pthread_cond_signal(&on_after_register_access_cond);
}

// on_after_register_access(): DATO to one of the device registers.
// Runs on the qunibusadapter event thread: no DMA from here, descriptor
// processing is signalled to the worker.
void delqa_c::on_after_register_access(qunibusdevice_register_t *device_reg,
        uint8_t unibus_control, DATO_ACCESS access)
{
    UNUSED(access);

    if (unibus_control != QUNIBUS_CYCLE_DATO) {
        // DATI: no side effects, only a trace of the host polling CSR/VAR
        DEBUG("DATI %s = %06o", device_reg->name, device_reg->active_dati_flipflops);
        return;
    }

    uint16_t value = device_reg->active_dato_flipflops;
    DEBUG("DATO %s = %06o", device_reg->name, value);

    pthread_mutex_lock(&on_after_register_access_mutex);

    switch (device_reg->index) {
    case 2: // receive BDL start address, low word
        rbdl[0] = value;
        break;
    case 3: // receive BDL start address, high bits: starts reception
        rbdl[1] = value;
        update_csr(0, CSR_RL);
        pending_rbdl = true;
        wakeup_worker();
        break;
    case 4: // transmit BDL start address, low word
        xbdl[0] = value;
        break;
    case 5: // transmit BDL start address, high bits: starts transmission
        xbdl[1] = value;
        update_csr(0, CSR_XL);
        pending_xbdl = true;
        wakeup_worker();
        break;
    case 6:
        write_var(value);
        break;
    case 7:
        write_csr(value);
        break;
    default: // writes to the PROM registers are ignored
        break;
    }

    pthread_mutex_unlock(&on_after_register_access_mutex);
}

// worker(): instance 0 executes the DMA-heavy work items (BDL walks),
// instance 1 receives packets from the host Ethernet.
void delqa_c::worker(unsigned instance)
{
    if (instance == 1) {
        bridge_worker();
        return;
    }

    worker_init_realtime_priority(rt_device);

    while (!workers_terminate) {
        pthread_mutex_lock(&on_after_register_access_mutex);
        // bounded wait: workers_stop() expects cooperative exit within 100ms
        while (!pending_wakeup && !workers_terminate) {
            struct timespec abstime;
            clock_gettime(CLOCK_REALTIME, &abstime);
            // carrier drops on its own deadline, well inside the idle tick
            unsigned wait_ms = 50;
            if (carrier_off_ms) {
                uint64_t now = now_ms();
                wait_ms = carrier_off_ms > now ? (unsigned) (carrier_off_ms - now) : 0;
                if (wait_ms > 50)
                    wait_ms = 50;
            }
            abstime.tv_nsec += wait_ms * 1000000;
            if (abstime.tv_nsec >= 1000000000) {
                abstime.tv_sec += 1;
                abstime.tv_nsec -= 1000000000;
            }
            pthread_cond_timedwait(&on_after_register_access_cond,
                    &on_after_register_access_mutex, &abstime);
            if (carrier_off_ms && now_ms() >= carrier_off_ms) {
                update_csr(0, CSR_CA);
                carrier_off_ms = 0;
            }
        }
        if (workers_terminate) {
            pthread_mutex_unlock(&on_after_register_access_mutex);
            break;
        }
        pending_wakeup = false;

        bool do_rbdl = pending_rbdl;
        bool do_xbdl = pending_xbdl;
        bool do_deliver = pending_deliver;
        bool do_bootrom = pending_bootrom;
        pending_rbdl = false;
        pending_xbdl = false;
        pending_deliver = false;
        pending_bootrom = false;
        if (carrier_off_ms && now_ms() >= carrier_off_ms) {
            update_csr(0, CSR_CA);
            carrier_off_ms = 0;
        }
        bool queued = !read_queue.empty();
        pthread_mutex_unlock(&on_after_register_access_mutex);

        if (do_rbdl)
            dispatch_rbdl();
        if (do_xbdl)
            process_xbdl();
        if (do_bootrom)
            process_bootrom();
        if (do_deliver && queued)
            process_rbdl();
    }
}

// ---------------------------------------------------------------------------
// DMA engine.  All transfers run blocking on the worker thread.

bool delqa_c::dma_read_words(uint32_t addr, uint16_t *buffer, unsigned wordcount)
{
    qunibusadapter->DMA(dma_request, true, QUNIBUS_CYCLE_DATI, addr, buffer, wordcount);
    return dma_request.success;
}

bool delqa_c::dma_write_words(uint32_t addr, const uint16_t *buffer, unsigned wordcount)
{
    qunibusadapter->DMA(dma_request, true, QUNIBUS_CYCLE_DATO, addr,
            const_cast<uint16_t *>(buffer), wordcount);
    return dma_request.success;
}

// dma_read_bytes(): read a byte buffer from bus memory, tolerating an odd
// start address and odd length.
bool delqa_c::dma_read_bytes(uint32_t addr, uint8_t *buffer, unsigned len)
{
    if (len == 0)
        return true;

    if (addr & 1) {
        // leading odd byte is the high byte of the containing word
        uint16_t word;
        if (!dma_read_words(addr & ~1u, &word, 1))
            return false;
        *buffer++ = word >> 8;
        addr++;
        len--;
    }

    unsigned wordcount = len / 2;
    if (wordcount) {
        std::vector<uint16_t> tmp(wordcount);
        if (!dma_read_words(addr, tmp.data(), wordcount))
            return false;
        memcpy(buffer, tmp.data(), wordcount * 2); // bus and ARM are little-endian
        addr += wordcount * 2;
        buffer += wordcount * 2;
        len -= wordcount * 2;
    }

    if (len) {
        // trailing odd byte is the low byte of the following word
        uint16_t word;
        if (!dma_read_words(addr, &word, 1))
            return false;
        *buffer = word & 0xff;
    }
    return true;
}

// dma_write_bytes(): write a byte buffer to bus memory, tolerating an odd
// start address and odd length.  Odd boundary bytes are merged into their
// containing word with a read-modify-write.
bool delqa_c::dma_write_bytes(uint32_t addr, const uint8_t *buffer, unsigned len)
{
    if (len == 0)
        return true;

    if (addr & 1) {
        uint16_t word;
        if (!dma_read_words(addr & ~1u, &word, 1))
            return false;
        word = (word & 0x00ff) | (buffer[0] << 8);
        if (!dma_write_words(addr & ~1u, &word, 1))
            return false;
        addr++;
        buffer++;
        len--;
    }

    unsigned wordcount = len / 2;
    if (wordcount) {
        std::vector<uint16_t> tmp(wordcount);
        memcpy(tmp.data(), buffer, wordcount * 2);
        if (!dma_write_words(addr, tmp.data(), wordcount))
            return false;
        addr += wordcount * 2;
        buffer += wordcount * 2;
        len -= wordcount * 2;
    }

    if (len) {
        uint16_t word;
        if (!dma_read_words(addr, &word, 1))
            return false;
        word = (word & 0xff00) | buffer[0];
        if (!dma_write_words(addr, &word, 1))
            return false;
    }
    return true;
}

// nxm_error(): a DMA transfer timed out.  Report nonexistent memory and
// stop list processing, as the real controller does.
void delqa_c::nxm_error(void)
{
    WARNING("bus timeout while accessing host memory");
    pthread_mutex_lock(&on_after_register_access_mutex);
    update_csr(CSR_NI | CSR_XI | CSR_XL | CSR_RL, 0);
    pthread_mutex_unlock(&on_after_register_access_mutex);
}

// ---------------------------------------------------------------------------
// Buffer descriptor list processing.
//
// A descriptor is 6 words:
//   word 0  flag (written to 0xffff when the controller uses the descriptor)
//   word 1  address bits 21:16 and V/C/E/S/L/H descriptor bits
//   word 2  address bits 15:0
//   word 3  buffer length, negative word count
//   word 4  status word 1
//   word 5  status word 2

// dispatch_rbdl(): host wrote a new receive BDL address.
void delqa_c::dispatch_rbdl(void)
{
    rbdl_ba = ((rbdl[1] & 0x3f) << 16) | (rbdl[0] & ~1);
    DEBUG("receive BDL at %08o", rbdl_ba);

    // probe the first descriptor
    uint16_t head[2];
    if (!dma_read_words(rbdl_ba, head, 2)) {
        nxm_error();
        return;
    }

    // Walk the list whether or not a packet is waiting: arming it hands the
    // descriptors to the controller, which marks them as it goes and reports
    // an empty list back through RL.
    process_rbdl();
}

// printf helpers for the MAC addresses in the receive traces
#define DELQA_MAC6(m) (m)[0], (m)[1], (m)[2], (m)[3], (m)[4], (m)[5]
#define DELQA_MACF "%02x:%02x:%02x:%02x:%02x:%02x"

// process_rbdl(): deliver queued packets into the receive buffer list.
void delqa_c::process_rbdl(void)
{
    uint32_t start_ba = rbdl_ba;
    unsigned dcount = 0;

    pthread_mutex_lock(&on_after_register_access_mutex);
    bool list_valid = !(csr & CSR_RL);
    size_t qsize = read_queue.size();
    pthread_mutex_unlock(&on_after_register_access_mutex);
    DEBUG("process_rbdl: ba=%08o list_valid=%d queue=%zu", rbdl_ba, list_valid, qsize);
    if (!list_valid)
        return;

    // RI (a buffer was used) and RL (the list ran dry) are raised together at
    // the end so the host never observes RI without RL - the self-test checks
    // both in one read and the receive-completion is atomic on real hardware.
    bool ri_pending = false;
    bool rl_reached = false;

    while (true) {
        uint16_t bd[6];

        // descriptor flag and address bits
        if (!dma_read_words(rbdl_ba, bd, 2)) {
            nxm_error();
            return;
        }

        // stop when a circular list has been walked completely
        if (dcount && rbdl_ba == start_ba) {
            DEBUG("processed all %d receive descriptors", dcount);
            break;
        }
        // a list that closes anywhere but on its head never reaches the check
        // above, so bound the walk and report the receiver stopped
        if (dcount > MAX_DESCRIPTORS) {
            WARNING("receive BDL at %08o does not terminate, stopped after %d descriptors",
                    start_ba, dcount);
            rl_reached = true;
            break;
        }
        dcount++;

        // mark descriptor used
        uint16_t flag_word = 0xffff;
        if (!dma_write_words(rbdl_ba, &flag_word, 1)) {
            nxm_error();
            return;
        }

        if (!(bd[1] & DSC_V)) {
            // end of list: the receive list is now empty
            rl_reached = true;
            break;
        }

        if (bd[1] & DSC_C) {
            // explicit chain to another descriptor
            if (!dma_read_words(rbdl_ba + 4, &bd[2], 1)) {
                nxm_error();
                return;
            }
            rbdl_ba = ((bd[1] & 0x3f) << 16) | bd[2];
            continue;
        }

        pthread_mutex_lock(&on_after_register_access_mutex);
        bool queue_empty = read_queue.empty();
        pthread_mutex_unlock(&on_after_register_access_mutex);
        if (queue_empty)
            break;

        // address, length and status words
        if (!dma_read_words(rbdl_ba + 4, &bd[2], 4)) {
            nxm_error();
            return;
        }
        DEBUG("rx descriptor @%08o: %06o %06o %06o %06o %06o %06o",
                rbdl_ba, bd[0], bd[1], bd[2], bd[3], bd[4], bd[5]);

        uint32_t address = ((bd[1] & 0x3f) << 16) | bd[2];
        uint16_t w_length = (uint16_t) (~bd[3] + 1); // negative word count
        unsigned b_length = w_length * 2;
        if (bd[1] & DSC_H) {
            b_length -= 1;
            address += 1;
        }
        if (bd[1] & DSC_L)
            b_length -= 1;

        // take the packet at the queue head; the queue is only consumed by
        // this thread, so working on a copy of the metadata is safe
        pthread_mutex_lock(&on_after_register_access_mutex);
        packet_c &item = read_queue.front();

        unsigned rbl = item.data.size();
        unsigned offset = item.used;

        if (offset == 0) {
            // pad runts (loopback packets from host diagnostics) to legal size
            if (item.type == packet_c::NORMAL && rbl < ETH_MIN_PACKET) {
                item.data.resize(ETH_MIN_PACKET, 0);
                rbl = ETH_MIN_PACKET;
            }
            // trim giants (never the boot ROM or loopback packets)
            if (item.type == packet_c::NORMAL && rbl > MAX_RCV_PACKET) {
                item.data.resize(MAX_RCV_PACKET);
                rbl = MAX_RCV_PACKET;
            }
        }
        rbl -= offset;
        if (rbl > b_length)
            rbl = b_length;
        item.used += rbl;

        // copy the segment so DMA can run without the state mutex
        std::vector<uint8_t> segment(item.data.begin() + offset,
                item.data.begin() + offset + rbl);
        enum packet_c::type_e type = item.type;
        unsigned packet_len = item.data.size();
        unsigned used = item.used;
        bool loss = read_queue_loss != 0;
        read_queue_loss = 0;
        uint16_t csr_now = csr;
        // the address logic answers about the frame's destination, which the
        // filter can change once this thread drops the state mutex
        bool dst_recognised = item.data.size() >= 6
                && address_recognised(item.data.data());
        uint8_t filter_station[6];
        memcpy(filter_station, active_physical_address(), 6);
        pthread_mutex_unlock(&on_after_register_access_mutex);

        if (!dma_write_bytes(address, segment.data(), rbl)) {
            nxm_error();
            return;
        }

        // build status words
        uint16_t st1 = 0;
        uint16_t st2;
        switch (type) {
        case packet_c::SETUP:
            st1 = 0x2700; // ESETUP plus RBL bits 10:8
            break;
        case packet_c::LOOPBACK:
            // A looped packet never went on the wire, so it carries none of
            // the wire's faults. Runt reports the one failure the loopback
            // path can have of its own: the receiver is on, the frame is
            // too short to be a datagram, and the address logic did not
            // answer to its destination, so the loopback never completed.
            // Internal loopback carries six-byte packets and nothing else
            // (EK-DELQA-UG-002 section 3.6.5), which is why the frames that
            // reach this test are all that length.
            // RBL is the received byte length of the whole packet, not of
            // the buffer it ended up in, so a packet split down a list
            // reports its full length on the segment that completes it.
            rbl = packet_len;
            // Bit 07 is reserved and comes back set on a looped packet; a
            // normal one carries the whole reserved field set instead.
            st1 = (rbl & 0x0700) | 0x0080;
            // The frame is what is too short, not the segment it landed in:
            // a long frame split down a list of small buffers is no runt.
            if ((csr_now & CSR_RE) && packet_len < ETH_MIN_PACKET
                    && !dst_recognised)
                st1 |= RST_RUNT;
            if (csr_now & CSR_EL)
                st1 |= RST_ESETUP;
            if (segment.size() >= 6)
                DEBUG("loopback rsw: len=%u rbl=%u csr=%06o RE=%d IL=%d EL=%d "
                        "dst=" DELQA_MACF " phys=" DELQA_MACF " known=%d -> st1=%06o",
                        packet_len, rbl, csr_now, !!(csr_now & CSR_RE),
                        !!(csr_now & CSR_IL), !!(csr_now & CSR_EL),
                        DELQA_MAC6(segment.data()), DELQA_MAC6(filter_station),
                        (int) dst_recognised, st1);
            break;
        case packet_c::NORMAL:
            rbl = packet_len - ETH_MIN_PACKET; // RBL field counts payload beyond 60
            st1 = (rbl & 0x0700) | 0x00f8;     // reserved bits set to 1
            break;
        case packet_c::BOOTROM:
            // Bit 15 marks the segment used; bit 14 is added below only when
            // another segment follows.
            st1 = 0x8000 | (rbl & 0x0700);
            break;
        }
        // "Not last segment" says a segment follows, for a boot ROM load as
        // for any other packet: the host reads it to decide whether to keep
        // walking the receive list.
        if (used < packet_len)
            st1 |= RST_LASTNOT;
        st2 = ((rbl & 0xff) << 8) | (rbl & 0xff);
        if (type == packet_c::BOOTROM && used < packet_len)
            st2 |= RST_LASTNOT; // the boot ROM reads the marker in word 2 too
        if (loss)
            st1 |= RST_OVERFLOW;
        // length error: giant packet, or over-long external loopback
        if ((!(csr_now & CSR_EL)
                && (rbl + (type == packet_c::NORMAL ? ETH_MIN_PACKET : 0)) > ETH_MAX_PACKET)
                || ((csr_now & CSR_EL) && type == packet_c::LOOPBACK && rbl >= LONG_PACKET))
            st1 |= RST_LASTERR;

        // A segment that is not the last of its packet reports only that it
        // was used and that more follows - bits 15:14 both set. The packet's
        // own status belongs to the segment that completes it, so nothing
        // below those bits applies yet and both words read all ones. The
        // boot ROM load keeps its own marker, which its firmware reads.
        if (used < packet_len && type != packet_c::BOOTROM) {
            st1 = 0xffff;
            st2 = 0xffff;
        }

        uint16_t st[2] = { st1, st2 };
        if (!dma_write_words(rbdl_ba + 8, st, 2)) {
            nxm_error();
            return;
        }

        DEBUG("delivered %s packet segment: bd=%08o addr=%08o len=%d st1=%06o st2=%06o",
                type == packet_c::SETUP ? "setup" :
                type == packet_c::LOOPBACK ? "loopback" :
                type == packet_c::BOOTROM ? "bootrom" : "normal",
                rbdl_ba, address, rbl, st1, st2);
        if (type == packet_c::NORMAL && segment.size() >= 2) {
            char hex[3 * 40 + 1];
            unsigned n = segment.size() < 40 ? segment.size() : 40;
            for (unsigned i = 0; i < n; i++)
                sprintf(hex + 3 * i, "%02x ", segment[i]);
            DEBUG("  segment head: %s", hex);
        }


        // a receive buffer was used; the interrupt is raised after the walk
        pthread_mutex_lock(&on_after_register_access_mutex);
        if (used >= packet_len)
            read_queue.pop_front();
        ri_pending = true;
        pthread_mutex_unlock(&on_after_register_access_mutex);

        rbdl_ba += 12; // implicit chain to the next descriptor
    }

    // raise RL and RI together so the host sees a consistent completion
    if (ri_pending || rl_reached) {
        pthread_mutex_lock(&on_after_register_access_mutex);
        update_csr((rl_reached ? CSR_RL : 0) | (ri_pending ? CSR_RI : 0), 0);
        pthread_mutex_unlock(&on_after_register_access_mutex);
    }
}

// process_xbdl(): walk the transmit buffer descriptor list, assembling
// packets and handing them to the wire, the loopback path or setup
// processing.
void delqa_c::process_xbdl(void)
{
    xbdl_ba = ((xbdl[1] & 0x3f) << 16) | (xbdl[0] & ~1);
    DEBUG("transmit BDL at %08o", xbdl_ba);

    write_buffer.clear();
    bool delivered = false; // loopback/setup packets queued for reception
    // A completed transmit raises XI, but the host reads XI and XL together:
    // the list is only known to be empty once the terminating descriptor has
    // been read, so hold the interrupt until that is known and raise both at
    // once. Reporting XI first shows the host a transmitter that has finished
    // a packet yet still claims work outstanding.
    bool pending_xi = false;
    uint32_t start_ba = xbdl_ba;
    unsigned dcount = 0;

    while (true) {
        uint16_t bd[6];

        // A transmit list ends with an invalid descriptor. One that chains
        // back on itself instead never reaches that end, so abandon the walk
        // and report the transmitter idle - the list is malformed, the host
        // memory behind it is not.
        if (dcount && (xbdl_ba == start_ba || dcount > MAX_DESCRIPTORS)) {
            WARNING("transmit BDL at %08o does not terminate, stopped after %d descriptors",
                    start_ba, dcount);
            pthread_mutex_lock(&on_after_register_access_mutex);
            update_csr(CSR_XL | (pending_xi ? CSR_XI : 0), 0);
            pthread_mutex_unlock(&on_after_register_access_mutex);
            pending_xi = false;
            break;
        }
        dcount++;

        if (!dma_read_words(xbdl_ba, bd, 6)) {
            nxm_error();
            return;
        }
        uint16_t flag_word = 0xffff;
        if (!dma_write_words(xbdl_ba, &flag_word, 1)) {
            nxm_error();
            return;
        }

        uint32_t address = ((bd[1] & 0x3f) << 16) | bd[2];

        if (bd[1] & DSC_C) {
            // explicit chain
            xbdl_ba = address;
            continue;
        }

        if (!(bd[1] & DSC_V)) {
            // end of list
            pthread_mutex_lock(&on_after_register_access_mutex);
            update_csr(CSR_XL | (pending_xi ? CSR_XI : 0), 0);
            pthread_mutex_unlock(&on_after_register_access_mutex);
            pending_xi = false;
            break;
        }
        if (pending_xi) {
            // another packet follows, so the list is not empty: report the
            // completed one on its own
            pthread_mutex_lock(&on_after_register_access_mutex);
            update_csr(CSR_XI, 0);
            pthread_mutex_unlock(&on_after_register_access_mutex);
            pending_xi = false;
        }

        uint16_t w_length = (uint16_t) (~bd[3] + 1);
        unsigned b_length = w_length * 2;
        if (bd[1] & DSC_H) {
            b_length -= 1;
            address += 1;
        }
        if (bd[1] & DSC_L)
            b_length -= 1;

        size_t offset = write_buffer.size();
        write_buffer.resize(offset + b_length);
        if (!dma_read_bytes(address, &write_buffer[offset], b_length)) {
            nxm_error();
            return;
        }

        if (bd[1] & DSC_E) {
            // end of message: dispose of the assembled packet
            pthread_mutex_lock(&on_after_register_access_mutex);
            bool loopback = (~csr & CSR_IL) || (csr & CSR_EL);
            bool list_ready = !(csr & CSR_RL) || (csr & CSR_EL);
            // Anything but an internal loop drives the transceiver port, and
            // a connector there returns it as carrier. Internal loopback
            // never leaves the chip, so it raises none.
            if (loopback_connector.value && (csr & CSR_IL)) {
                update_csr(CSR_CA, 0);
                carrier_off_ms = now_ms() + CARRIER_MS;
            }
            pthread_mutex_unlock(&on_after_register_access_mutex);

            DEBUG("xmt end: len=%zu setup=%d loopback=%d(IL=%d EL=%d) dst=%02x:%02x:%02x:%02x:%02x:%02x",
                    write_buffer.size(), !!(bd[1] & DSC_S), loopback, !!(csr & CSR_IL), !!(csr & CSR_EL),
                    write_buffer.size()>5?write_buffer[0]:0, write_buffer.size()>5?write_buffer[1]:0,
                    write_buffer.size()>5?write_buffer[2]:0, write_buffer.size()>5?write_buffer[3]:0,
                    write_buffer.size()>5?write_buffer[4]:0, write_buffer.size()>5?write_buffer[5]:0);
            uint16_t st[2];
            if (loopback || (bd[1] & DSC_S)) {
                if (bd[1] & DSC_S) {
                    // setup packet: load the filter, reflect it to the host
                    process_setup(bd[3]);
                    enqueue_packet(packet_c::SETUP, write_buffer);
                    st[0] = 0x200c; // DELQA setup transmit status
                    st[1] = 0x0860;
                } else {
                    // Loopback packet. A reflection needs a receive buffer to
                    // land in as it comes back; with the list empty it is
                    // lost, and the controller keeps no backlog of it. The
                    // diagnostic transmits a run of loopback frames with no
                    // list armed and then arms one, expecting the reflection
                    // that follows rather than the first of the run.
                    // External loopback rides the transceiver and arrives
                    // late enough for a list armed meanwhile to catch it.
                    if (list_ready) {
                        enqueue_packet(packet_c::LOOPBACK, write_buffer);
                        delivered = true;
                    } else {
                        // a reflection with nowhere to go is a lost packet,
                        // and the next receive that does land says so
                        pthread_mutex_lock(&on_after_register_access_mutex);
                        read_queue_loss++;
                        pthread_mutex_unlock(&on_after_register_access_mutex);
                    }
                    // A loop never reaches the wire, so it reports what a
                    // setup packet does: bits 15:14 clear for "last segment,
                    // sent without errors", no carrier loss, no abort and a
                    // collision count of zero. The remaining bits are
                    // reserved, and these are the values the diagnostic
                    // reads back from the hardware.
                    st[0] = 0x200c;
                    st[1] = 0x0860;
                }
                if (bd[1] & DSC_S)
                    delivered = true;
            } else {
                // hand the packet to the wire
                bool ok = transmit_packet(write_buffer);
                if (ok) {
                    st[0] = 0; // used, last segment, no errors
                } else {
                    WARNING("packet transmission failed");
                    st[0] = XMT_LASTERR;
                }
                st[1] = (100 + write_buffer.size() * 8) & 0x03ff; // plausible TDR
            }

            if (!dma_write_words(xbdl_ba + 8, st, 2)) {
                nxm_error();
                return;
            }

            write_buffer.clear();

            // transmission complete; the interrupt goes out with XL once
            // the end of the list is known
            pending_xi = true;
        } else {
            // A segment that only carries part of a message reports "used,
            // not the last segment" - status word 1 bits 15:14 both set
            // (EK-DELQA-UG-002 section 3.4.3.5). No transmission happened on
            // this buffer alone, so nothing below those bits applies and the
            // controller leaves both words all ones.
            uint16_t st[2] = { 0xffff, 0xffff };
            if (!dma_write_words(xbdl_ba + 8, st, 2)) {
                nxm_error();
                return;
            }
        }

        xbdl_ba += 12;
    }

    // hand reflected setup/loopback packets to the receive list
    if (delivered) {
        pthread_mutex_lock(&on_after_register_access_mutex);
        bool queued = !read_queue.empty();
        bool list_valid = !(csr & CSR_RL);
        pthread_mutex_unlock(&on_after_register_access_mutex);
        if (queued && list_valid)
            process_rbdl();
    }
}

// process_setup(): a setup packet in write_buffer carries the address
// filter and controller options.  Format per DELQA User's Guide: MAC
// address n, byte j lives at offset (n+1) + 8*j for the first seven
// addresses, at (n+0101) + 8*j for the second seven.
void delqa_c::process_setup(uint16_t descriptor_count)
{
    const std::vector<uint8_t> &buf = write_buffer;

    {
        char hex[3 * 130 + 1];
        unsigned n = buf.size() < 130 ? buf.size() : 130;
        for (unsigned i = 0; i < n; i++)
            sprintf(hex + 3 * i, "%02x ", buf[i]);
        DEBUG("setup frame bytes: %s", hex);
    }

    pthread_mutex_lock(&on_after_register_access_mutex);

    memset(setup.macs, 0, sizeof(setup.macs));
    for (unsigned i = 0; i < 7; i++)
        for (unsigned j = 0; j < 6; j++) {
            unsigned lo = (i + 001) + (j * 8);
            if (lo < buf.size())
                setup.macs[i][j] = buf[lo];
            unsigned hi = (i + 0101) + (j * 8);
            if (hi < buf.size())
                setup.macs[i + 7][j] = buf[hi];
        }

    // The address filter modes ride in the setup packet's length: the table
    // occupies 128 bytes and the host appends up to three more, bit 00
    // all-multicast and bit 01 promiscuous.  A plain 128-byte setup names
    // neither, which is how a mode is turned back off, so every setup
    // reloads both.
    unsigned len = buf.size();
    setup.all_multicast = !!(len & 0x0001);
    setup.promiscuous = !!(len & 0x0002);

    // sanity timer period select, carried by a long setup only
    if (len > 128) {
        static const unsigned quarter_secs[8] = {
            1, 4, 16, 64,               // 1/4s, 1s, 4s, 16s
            1 * 240, 4 * 240, 16 * 240, 64 * 240 // 1, 4, 16, 64 minutes
        };
        setup.sanity_quarter_secs = quarter_secs[(len & 0x0070) >> 4];
    }

    setup.valid = true;

    INFO("setup packet: %d bytes, count=%06o, promiscuous=%d, all_multicast=%d, station=%02x:%02x:%02x:%02x:%02x:%02x",
            (int) buf.size(), descriptor_count, setup.promiscuous, setup.all_multicast,
            setup.macs[0][0], setup.macs[0][1], setup.macs[0][2],
            setup.macs[0][3], setup.macs[0][4], setup.macs[0][5]);
    for (unsigned i = 0; i < FILTER_MAX; i++) {
        bool nonzero = false;
        for (unsigned j = 0; j < 6; j++)
            if (setup.macs[i][j])
                nonzero = true;
        if (nonzero)
            DEBUG("  filter[%d] = %02x:%02x:%02x:%02x:%02x:%02x", i,
                    setup.macs[i][0], setup.macs[i][1], setup.macs[i][2],
                    setup.macs[i][3], setup.macs[i][4], setup.macs[i][5]);
    }

    pthread_mutex_unlock(&on_after_register_access_mutex);

    // TODO stage 3: reprogram the eth0 bridge receive filter
}

// process_bootrom(): deliver the on-board boot/diagnostic ROM into host
// memory. Requested through CSR<BD,EL>. The host has already set up a
// receive BDL of two 2 KB buffers; the 4 KB ROM image is queued as a single
// loopback packet and delivered through the normal receive path, which
// splits it across the two buffers and sets the receive status words the
// citizenship test expects. After this the host executes the loaded code,
// which runs a MOP network boot client.
void delqa_c::process_bootrom(void)
{
    DEBUG("delivering boot ROM (%u bytes) to receive BDL at %08o",
            (unsigned) sizeof(delqa_bootrom), rbdl_ba);

    // the ROM words are in PDP-11 memory order; on this little-endian host a
    // byte copy of the array already has low-byte-first layout
    std::vector<uint8_t> rom(sizeof(delqa_bootrom));
    memcpy(rom.data(), delqa_bootrom, sizeof(delqa_bootrom));

    enqueue_packet(packet_c::BOOTROM, rom);
    process_rbdl();
}

// enqueue_packet(): queue a packet for delivery through the receive list.
void delqa_c::enqueue_packet(enum packet_c::type_e type, const std::vector<uint8_t> &data)
{
    pthread_mutex_lock(&on_after_register_access_mutex);
    if (type == packet_c::NORMAL) {
        // the onboard buffer holds a handful of frames; while the host is
        // not consuming (receiver off for long spans) old frames fall out
        unsigned normal_count = 0;
        for (auto &p : read_queue)
            if (p.type == packet_c::NORMAL)
                normal_count++;
        if (normal_count >= 4) {
            for (auto it = read_queue.begin(); it != read_queue.end(); ++it)
                if (it->type == packet_c::NORMAL) {
                    read_queue.erase(it);
                    break;
                }
        }
    }
    if (read_queue.size() >= READ_QUEUE_MAX) {
        read_queue_loss++;
        // drop the oldest normal packet to make room, reflected packets stay
        for (auto it = read_queue.begin(); it != read_queue.end(); ++it)
            if (it->type == packet_c::NORMAL) {
                read_queue.erase(it);
                break;
            }
        if (read_queue.size() >= READ_QUEUE_MAX) {
            pthread_mutex_unlock(&on_after_register_access_mutex);
            return;
        }
    }
    packet_c item;
    item.type = type;
    item.data = data;
    item.used = 0;
    read_queue.push_back(item);
    DEBUG("enqueued type=%d len=%zu queue=%zu", (int) type, data.size(), read_queue.size());
    // deliver immediately: the self-test firmware polls for the reflected
    // frame with a ~38ms timeout, so delivery must not wait for an
    // unrelated worker wakeup
    pending_deliver = true;
    wakeup_worker();
    pthread_mutex_unlock(&on_after_register_access_mutex);
}

// ---------------------------------------------------------------------------
// eth0 bridge.  A raw AF_PACKET socket in promiscuous mode shares the host
// interface: the DELQA station address is distinct from the host MAC, and
// the software filter below decides which frames reach the emulated
// controller.

// active_physical_address(): the one physical address the receive logic
// recognises - the first setup entry with a clear multicast bit, or the
// PROM station address when the table names none (EK-DELQA-UG-002 section
// 3.6.2.4).  Call with on_after_register_access_mutex held.
const uint8_t *delqa_c::active_physical_address(void)
{
    if (setup.valid)
        for (unsigned i = 0; i < FILTER_MAX; i++)
            if (!(setup.macs[i][0] & 0x01))
                return setup.macs[i];
    return station_address;
}

// address_recognised(): does the address filter answer to this destination?
// The filter holds the station address and the fourteen setup entries, and
// matches any of them exactly, multicast bit or not; promiscuous and
// all-multicast widen it.  This is what the loopback path asks, and it is
// wider than normal-mode datagram reception, where only one physical
// address is active (see bridge_accept).  Call with
// on_after_register_access_mutex held.
bool delqa_c::address_recognised(const uint8_t *dst)
{
    static const uint8_t broadcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

    if (!setup.valid)
        // before the first setup packet: the PROM station address and broadcast
        return memcmp(dst, station_address, 6) == 0
                || memcmp(dst, broadcast, 6) == 0;
    if (setup.promiscuous)
        return true;
    if ((dst[0] & 0x01) && setup.all_multicast)
        return true;
    if (memcmp(dst, station_address, 6) == 0)
        return true;
    for (unsigned i = 0; i < FILTER_MAX; i++)
        if (memcmp(dst, setup.macs[i], 6) == 0)
            return true;
    return false;
}

// bridge_accept(): software address filter, mirroring what the DELQA
// receive logic does in hardware.
bool delqa_c::bridge_accept(const uint8_t *frame, unsigned len)
{
    if (len < 12)
        return false;
    const uint8_t *dst = frame;
    const uint8_t *src = frame + 6;

    // Trace station-addressed (unicast) frames: these carry the MOP replies
    // whose delivery decides the boot, and they are rare enough to log.
    const bool trace = !(dst[0] & 0x01);

    // Fast reject without the mutex: a busy network floods this path, and
    // taking on_after_register_access_mutex per frame priority-inverts against
    // the BDL worker delivering loopback reflections, blowing the firmware's
    // receive-poll timeout. A stale read of csr here is harmless - the frame
    // is re-checked under the lock before it is ever queued. In internal
    // (IL=0) or external (EL) loopback, or with the receiver off, no wire
    // frame is wanted.
    // a connector in the transceiver port disconnects the network
    if (loopback_connector.value)
        return false;

    uint16_t csr_snapshot = csr;
    if ((~csr_snapshot & CSR_IL) || (csr_snapshot & CSR_EL)) {
        if (trace)
            DEBUG("rxfilter drop(loopback) dst=" DELQA_MACF " src=" DELQA_MACF " csr=%06o",
                 DELQA_MAC6(dst), DELQA_MAC6(src), csr_snapshot);
        return false;
    }

    // unicast traffic for other stations floods a busy network; reject it
    // before the mutex. The unlocked reads race a concurrent setup update
    // only for the microseconds a new filter is being stored, and accepted
    // frames are re-checked against the filter under the lock below.
    if (!(dst[0] & 0x01) && !setup.promiscuous) {
        bool ours = memcmp(dst, station_address, 6) == 0;
        for (unsigned i = 0; i < FILTER_MAX && !ours; i++)
            if (memcmp(dst, setup.macs[i], 6) == 0)
                ours = true;
        if (!ours) {
            if (trace)
                DEBUG("rxfilter drop(not-ours) dst=" DELQA_MACF " station=" DELQA_MACF,
                     DELQA_MAC6(dst), DELQA_MAC6(station_address));
            return false;
        }
    }

    pthread_mutex_lock(&on_after_register_access_mutex);

    // re-check under the lock now that we intend to inspect the filter.
    // RE only gates delivery to the host: the controller's onboard buffer
    // keeps receiving, so a reply arriving while the host briefly holds
    // RE off (the boot firmware does between transmit and re-arm) is
    // delivered once the receiver is re-enabled.
    bool loopback = (~csr & CSR_IL) || (csr & CSR_EL);

    bool accept = false;
    const uint8_t *phys_trace = station_address; // for the rxfilter trace
    bool own_trace = false;
    if (!loopback) {
        if (setup.valid) {
            // The boot ROM relies on the PROM fallback: its setup table
            // carries the station address with the multicast bit set in
            // every entry (7f:...), keeping the PROM address active for
            // the MOP load replies.
            const uint8_t *phys = active_physical_address();

            // never receive our own transmissions back: drop frames whose
            // source matches the active physical address or one of the
            // programmed unicast addresses
            bool own = memcmp(src, phys, 6) == 0;
            for (unsigned i = 0; i < FILTER_MAX && !own; i++)
                if (!(setup.macs[i][0] & 0x01)
                        && memcmp(src, setup.macs[i], 6) == 0)
                    own = true;
            phys_trace = phys;
            own_trace = own;

            // Datagram reception is narrower than the address filter: one
            // physical address is active, and a unicast table entry that is
            // not it does not receive.
            if (!own) {
                if (setup.promiscuous) {
                    accept = true;
                } else if ((dst[0] & 0x01) && setup.all_multicast) {
                    accept = true;
                } else if (memcmp(dst, phys, 6) == 0) {
                    accept = true;
                } else if (dst[0] & 0x01) {
                    // multicast entries (including broadcast, if enabled
                    // in the table) match exactly
                    for (unsigned i = 0; i < FILTER_MAX && !accept; i++)
                        if (memcmp(dst, setup.macs[i], 6) == 0)
                            accept = true;
                }
            }
        } else {
            static const uint8_t broadcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
            // before the first setup packet: the PROM station address
            // and broadcast
            if (memcmp(src, station_address, 6) != 0
                    && (memcmp(dst, station_address, 6) == 0
                            || memcmp(dst, broadcast, 6) == 0))
                accept = true;
        }
    }

    pthread_mutex_unlock(&on_after_register_access_mutex);
    if (trace)
        DEBUG("rxfilter %s dst=" DELQA_MACF " src=" DELQA_MACF " phys=" DELQA_MACF
             " setup=%d promisc=%d own=%d csr=%06o",
             accept ? "ACCEPT" : "drop", DELQA_MAC6(dst), DELQA_MAC6(src),
             DELQA_MAC6(phys_trace), (int) setup.valid, (int) setup.promiscuous,
             (int) own_trace, csr);
    return accept;
}

// bridge_receive(): queue a frame from the wire for delivery.
void delqa_c::bridge_receive(const uint8_t *frame, unsigned len)
{
    // the kernel can loop several copies of a locally transmitted frame
    // to the tap in quick succession; identical frames within 50ms are
    // copies (MOP retransmissions are spaced at least 100ms apart)
    static uint8_t last_head[64];
    static unsigned last_len = 0;
    static struct timespec last_ts;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long dus = (now.tv_sec - last_ts.tv_sec) * 1000000L
            + (now.tv_nsec - last_ts.tv_nsec) / 1000;
    unsigned headlen = len < sizeof(last_head) ? len : sizeof(last_head);
    if (len == last_len && dus >= 0 && dus < 50000
            && memcmp(frame, last_head, headlen) == 0)
        return;
    last_len = len;
    memcpy(last_head, frame, headlen);
    last_ts = now;

    // Counted here rather than at the socket: what reaches this point is what
    // the station accepted and the duplicate filter kept, which is the traffic
    // the guest will see.
    rx_frames.add(1);
    rx_bytes.add(len);

    std::vector<uint8_t> data(frame, frame + len);
    enqueue_packet(packet_c::NORMAL, data);

    pthread_mutex_lock(&on_after_register_access_mutex);
    if (!(csr & CSR_RL)) {
        pending_deliver = true;
        wakeup_worker();
    }
    pthread_mutex_unlock(&on_after_register_access_mutex);
}

// bridge_worker(): receive loop on worker instance 1.
void delqa_c::bridge_worker(void)
{
    timeout_c timeout;
    uint8_t frame[2048];

    // the bridge briefly holds on_after_register_access_mutex per accepted
    // frame; at time-share priority a busy network preempts it mid-section
    // and the real-time BDL worker blocks behind the held mutex
    worker_init_realtime_priority(rt_device);

    while (!workers_terminate) {
        if (!bridge.is_open()) {
            if (!bridge.open(interface.value, station_address)) {
                // the interface may appear later; retry in small steps so
                // workers_stop() sees a quick exit
                for (unsigned i = 0; i < 40 && !workers_terminate; i++)
                    timeout.wait_ms(50);
                continue;
            }
        }

        // Keep draining the socket while the receiver is off: the controller's
        // onboard buffer keeps receiving, and the boot firmware holds RE down
        // between transmitting a MOP request and re-arming the receiver -
        // exactly when the reply arrives. Frames read here are queued by
        // bridge_receive and delivered once RE returns. The kernel address
        // filter installed by the bridge keeps this cheap enough not to
        // starve the BDL worker on this single-core board.
        // In loopback no wire frame is wanted at all.
        uint16_t c = csr;
        if ((~c & CSR_IL) || (c & CSR_EL)) {
            timeout.wait_ms(20);
            continue;
        }

        // short timeout: workers_stop() expects cooperative exit within 100ms
        bool outgoing;
        int len = bridge.receive(frame, sizeof(frame), 50, &outgoing);
        if (len == 0)
            continue; // nothing arrived: re-check workers_terminate
        if (len < 0) {
            bridge.close();
            continue;
        }

        // trace every frame addressed to the emulated station as it leaves
        // the socket, before any filtering, to locate where MOP replies die
        if (len >= 12 && memcmp(frame, station_address, 6) == 0)
            DEBUG("rxloop to-station outgoing=%d src=%02x:%02x:%02x:%02x:%02x:%02x len=%d",
                 (int) outgoing, frame[6], frame[7], frame[8], frame[9],
                 frame[10], frame[11], len);

        // Our own transmissions come back on the raw socket as outgoing
        // frames - never receive them (on real Ethernet a station does not
        // hear itself). Other outgoing frames pass: they are the
        // BeagleBone's own traffic, which the PDP-11 can talk to over the
        // shared interface.
        if (outgoing && len >= 12 && memcmp(frame + 6, station_address, 6) == 0)
            continue;

        if (!bridge_accept(frame, len))
            continue;

        note_activity();
        bridge_receive(frame, len);
    }

    bridge.close();
}

// note_activity(): a frame moved in one direction or the other.
void delqa_c::note_activity(void)
{
    // A frame is gone in microseconds; hold the lamp long enough for the next
    // sample to catch it. Independent of the GPIO below, so the UI shows
    // traffic even when no LED is assigned.
    activity_lamp_until_ms = now_ms() + activity_lamp_on_time_ms;
    activity_lamp.value = true;
    if (activity_led.value < 4)
        gpios->activity_leds.set(activity_led.value, true);
}

void delqa_c::refresh_activity(void)
{
    if (activity_lamp.value && now_ms() >= activity_lamp_until_ms)
        activity_lamp.value = false;
}

// transmit_packet(): hand a packet to the host Ethernet.
bool delqa_c::transmit_packet(const std::vector<uint8_t> &data)
{
    // A loopback connector disconnects the network and returns the frame to
    // the receiver. The returning signal is carrier, which is what the
    // diagnostic watches for while it runs its external loopback tests.
    if (loopback_connector.value) {
        std::vector<uint8_t> frame(data);
        if (frame.size() < ETH_MIN_PACKET)
            frame.resize(ETH_MIN_PACKET, 0);
        pthread_mutex_lock(&on_after_register_access_mutex);
        update_csr(CSR_CA, 0);
        carrier_off_ms = now_ms() + CARRIER_MS;
        pthread_mutex_unlock(&on_after_register_access_mutex);
        enqueue_packet(packet_c::NORMAL, frame);
        DEBUG("loopback connector: returned %d bytes", (int) frame.size());
        // The board transmitted it, so it counts; the copy coming back does not
        // pass bridge_receive() and so is not counted again as received.
        tx_frames.add(1);
        tx_bytes.add(data.size());
        note_activity();
        return true;
    }

    if (!bridge.is_open())
        return false; // no carrier

    if (!bridge.send(data.data(), data.size()))
        return false;

    DEBUG("bridge: transmitted %d bytes", (int) data.size());
    tx_frames.add(1);
    tx_bytes.add(data.size());
    note_activity();
    return true;
}

// on_power_changed(): DCLO cycle resets the controller.
void delqa_c::on_power_changed(signal_edge_enum aclo_edge, signal_edge_enum dclo_edge)
{
    UNUSED(aclo_edge);

    if (dclo_edge == SIGNAL_EDGE_RAISING) {
        pthread_mutex_lock(&on_after_register_access_mutex);
        power_on_reset();
        pthread_mutex_unlock(&on_after_register_access_mutex);
    }
}

// on_init_changed(): bus INIT resets the controller.
void delqa_c::on_init_changed(void)
{
    if (init_asserted) {
        pthread_mutex_lock(&on_after_register_access_mutex);
        power_on_reset();
        pthread_mutex_unlock(&on_after_register_access_mutex);
    }
}
