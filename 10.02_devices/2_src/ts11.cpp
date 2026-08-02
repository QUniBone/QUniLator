/* ts11.cpp: TS11/TSV05 magnetic tape subsystem implementation

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see ts11.hpp for the full text.

   The register interface is two words wide and carries no commands: the CPU
   builds a four-word command packet on a modulo-4 boundary in its own memory
   and writes the packet's address into TSDB. That write clears SSR and hands
   ownership of the command buffer to the controller, which fetches the packet
   by DMA, executes it, deposits a message packet holding the residual count and
   the five extended status registers, sets SSR and interrupts.

   Sources, followed for every register bit and every command mode:
     EK-OTS11-TM-003  TS11 Technical Manual, chapter 5
     EK-TSV05-UG-001  TSV05 User's Guide, section 3.3

   The transport is a SIMH .tap image through simh_tape_c, which supplies the
   records and tape marks the command set moves over.
*/

#include <string.h>
#include <string>

#include "logger.hpp"
#include "qunibus.h"
#include "qunibusadapter.hpp"
#include "qunibusdevice.hpp"
#include "utils.hpp"
#include "ts11.hpp"

// Command codes, EK-OTS11-TM-003 table 5-10
#define TS_CMD_READ                 001
#define TS_CMD_WRITE_CHARACTERISTICS 004
#define TS_CMD_WRITE                005
#define TS_CMD_WRITE_SUBSYSTEM_MEM  006
#define TS_CMD_POSITION             010
#define TS_CMD_FORMAT               011
#define TS_CMD_CONTROL              012
#define TS_CMD_INITIALIZE           013
#define TS_CMD_GET_STATUS           017

// Termination class codes, EK-OTS11-TM-003 table 5-13
#define TC_NORMAL                   0
#define TC_ATTENTION                1
#define TC_TAPE_STATUS_ALERT        2
#define TC_FUNCTION_REJECT          3
#define TC_RECOVERABLE_ONE_RECORD   4
#define TC_RECOVERABLE_NO_MOTION    5
#define TC_UNRECOVERABLE            6
#define TC_FATAL                    7

// Message type codes, EK-OTS11-TM-003 table 5-12
#define MSG_END                     020
#define MSG_FAIL                    021
#define MSG_ERROR                   022
#define MSG_ATTN                    023

// XST0 bits, EK-OTS11-TM-003 table 5-5
#define XST0_TMK    0x8000  // tape mark detected
#define XST0_RLS    0x4000  // record length short
#define XST0_LET    0x2000  // logical end of tape
#define XST0_RLL    0x1000  // record length long
#define XST0_WLE    0x0800  // write lock error
#define XST0_NEF    0x0400  // nonexecutable function
#define XST0_ILC    0x0200  // illegal command
#define XST0_ILA    0x0100  // illegal address
#define XST0_MOT    0x0080  // capstan moving
#define XST0_ONL    0x0040  // transport on-line
#define XST0_IE     0x0020  // interrupt enable of the last command
#define XST0_VCK    0x0010  // volume check
#define XST0_PED    0x0008  // phase encoded drive
#define XST0_WLK    0x0004  // write locked
#define XST0_BOT    0x0002  // beginning of tape
#define XST0_EOT    0x0001  // end of tape

// XST1 bits, EK-OTS11-TM-003 table 5-6
#define XST1_UNC    0x0002  // uncorrectable data

// XST2 bits, EK-OTS11-TM-003 table 5-7 / TSV05 UG figure 3-15
#define XST2_OPM    0x8000  // operation in progress

// XST3 bits, EK-OTS11-TM-003 table 5-8
#define XST3_OPI    0x0040  // operation incomplete
#define XST3_REV    0x0020  // reverse direction
#define XST3_DCK    0x0008  // density check
#define XST3_RIB    0x0001  // reverse into BOT

// TSDBX bits, TSV05 UG table 3-6. The register is a byte at the high half of
// the TSSR word, so its bits sit at 15:8 of the register's write flipflops.
#define TSDBX_BOOT  0x8000
#define TSDBX_P_MASK 0x0f00 // command pointer bits 21:18

// ---------------------------------------------------------------------------
// The transport.

ts05_tape_c::ts05_tape_c(storagecontroller_c *ctrl) :
    storagedrive_c(ctrl)
{
    set_workers_count(0);   // the controller drives the tape; no worker of its own
    log_label = "ts05";

    type_name.value = "TS05";
    type_name.readonly = true;

    // A 2400 foot reel at 1600 bpi. Settable, so a shorter reel can be
    // modelled: the transport reports EOT once the position passes it.
    capacity.value = TS05_CAPACITY;
    capacity.readonly = false;

    write_enable_ring.value = true;
    online_switch.value = true;
    write_protect_lamp.kind = parameter_c::PARAM_STATUS;
}

ts05_tape_c::~ts05_tape_c(void)
{
    _tape.close();
}

//
// on_param_changed():
//  The "image" parameter names the .tap file. Setting it mounts the reel;
//  clearing it takes the reel off the hub. A read/write mount creates the file
//  if it is absent, and falls back to a read-only mount when it cannot be
//  written, which reads to the host as a reel with no write-enable ring.
//
bool ts05_tape_c::on_param_changed(parameter_c *param)
{
    if (param == &image_filepath) {
        _tape.close();
        const std::string &path = image_filepath.new_value;
        if (!path.empty() && !_tape.open(path, false)) {
            ERROR("ts05: cannot open tape image '%s'", path.c_str());
            return false;
        }
        return true;
    }

    return device_c::on_param_changed(param);
}

//
// refresh_activity():
//  Runs on the lamp poll. Ages out the ACCESS lamp (base class) and tracks the
//  WRITE PROTECT lamp against the reel's current write-lock state.
//
void ts05_tape_c::refresh_activity(void)
{
    storagedrive_c::refresh_activity();
    write_protect_lamp.value = is_write_locked();
}

//
// on_power_changed() / on_init_changed():
//  Power-up runs the transport's automatic load sequence, which leaves the tape
//  at BOT. A bus INIT leaves the reel where it is: only a write to TSSR
//  (subsystem initialize) reaches the transport.
//
void ts05_tape_c::on_power_changed(signal_edge_enum aclo_edge, signal_edge_enum dclo_edge)
{
    UNUSED(aclo_edge);
    if (dclo_edge == SIGNAL_EDGE_RAISING)
        _tape.rewind();
}

void ts05_tape_c::on_init_changed(void)
{
}

// ---------------------------------------------------------------------------
// The controller.

ts11_c::ts11_c(void) :
    storagecontroller_c()
{
    name.value = "ts";
    log_label = "ts";

    // The Q-bus M7196 carries TSDBX and the Extended Features switch; the
    // Unibus M7982 has neither, and reads every write to the status address as
    // a subsystem initialize.
#if defined(QBUS)
    type_name.value = "TSV05";
    _has_tsdbx = true;
    extended_features.value = (qunibus->addr_width == 22);
#else
    type_name.value = "TS11";
    _has_tsdbx = false;
    extended_features.value = false;
    extended_features.readonly = true;
#endif
    type_name.readonly = true;

    // base addr, priority slot, intr vector, intr level
    set_default_bus_params(TS11_ADDR, TS11_SLOT, TS11_VECTOR, TS11_LEVEL);

    register_count = 2;

    // TSBA when read, TSDB when written.
    busreg_TSDB = &(this->registers[0]);
    strcpy_s(busreg_TSDB->name, sizeof(busreg_TSDB->name), "TSBA/TSDB");
    busreg_TSDB->active_on_dati = false;    // TSBA reads back what the controller put there
    busreg_TSDB->active_on_dato = true;     // a word write starts a command
    busreg_TSDB->reset_value = 0;
    busreg_TSDB->writable_bits = 0xffff;

    // TSSR when read; a write is a subsystem initialize, or loads TSDBX.
    busreg_TSSR = &(this->registers[1]);
    strcpy_s(busreg_TSSR->name, sizeof(busreg_TSSR->name), "TSSR");
    busreg_TSSR->active_on_dati = false;    // polled in loops: keep it a plain read
    busreg_TSSR->active_on_dato = true;
    busreg_TSSR->reset_value = 0;
    busreg_TSSR->writable_bits = 0xffff;

    // one transport per controller
    drivecount = 1;
    ts05_tape_c *tape = new ts05_tape_c(this);
    tape->unitno.value = 0;
    tape->activity_led.value = 0;
    tape->name.value = name.value + "0";
    tape->log_label = tape->name.value;
    tape->parent = this;
    storagedrives.push_back(tape);

    _record.reserve(TS11_MAX_RECORD);
    reset_subsystem();
}

ts11_c::~ts11_c(void)
{
    for (unsigned i = 0; i < drivecount; i++)
        delete storagedrives[i];
    storagedrives.clear();
}

bool ts11_c::on_param_changed(parameter_c *param)
{
    if (param == &priority_slot) {
        dma_request.set_priority_slot(priority_slot.new_value);
        intr_request.set_priority_slot(priority_slot.new_value);
    } else if (param == &intr_level) {
        intr_request.set_level(intr_level.new_value);
    } else if (param == &intr_vector) {
        intr_request.set_vector(intr_vector.new_value);
    }

    return storagecontroller_c::on_param_changed(param);
}

//
// reset_subsystem():
//  A subsystem initialize, reached by a write to TSSR, by bus INIT and by
//  power-up. It resets the controller and the transport, runs the transport's
//  automatic load sequence (the tape returns to BOT while it is on-line), and
//  leaves the message buffer undefined: NBA is always set after initialization,
//  and so is volume check.
//
void ts11_c::reset_subsystem(void)
{
    _ssr = true;
    _nba = true;
    _rmr = false;
    _nxm = false;
    _sce = false;
    _fc = 0;
    _tc = TC_NORMAL;
    _tsba = 0;
    _tsdbx = 0;
    _maintenance = false;

    _msgbuf_addr = 0;
    _msgbuf_len = 0;
    _ess = _enb = _eai = _eri = false;
    _interrupt_enable = false;
    _msgbuf_owned = false;
    _attn_pending = false;
    _attn_class = 0;
    _volume_check = true;
    _high_speed = false;

    _pending_command = false;
    _pending_boot = false;
    _pending_attention = false;
    _command_pointer = 0;

    clear_command_status();

    if (drivecount > 0 && drive()->is_online())
        drive()->tape().rewind();
    _last_online = (drivecount > 0) && drive()->is_online();

    update_tssr();
}

//
// clear_command_status():
//  Every command clears the extended status error bits before it runs
//  (EK-OTS11-TM-003 5.3.4).
//
void ts11_c::clear_command_status(void)
{
    _xst0_errors = 0;
    _xst1 = 0;
    _xst2 = 0;
    _xst3 = 0;
    _xst4 = 0;
    _rbpcr = 0;
    _suppress_message = false;
}

// ---------------------------------------------------------------------------
// Register interface.

//
// tssr_value():
//  Assembles TSSR from the controller state. SC reports that the last command
//  ended other than cleanly: a nonzero termination class, or one of the status
//  register's own error bits.
//
uint16_t ts11_c::tssr_value(void)
{
    uint16_t v = 0;
    bool sc = (_tc != TC_NORMAL) || _rmr || _nxm || _sce;
    if (sc)
        v |= 0x8000;                        // SC
    if (_sce)
        v |= 0x2000;                        // SCE (SPE on the TS11)
    if (_rmr)
        v |= 0x1000;                        // RMR
    if (_nxm)
        v |= 0x0800;                        // NXM
    if (_nba)
        v |= 0x0400;                        // NBA
    v |= (uint16_t) (((_tsba >> 16) & 0x03) << 8);  // A17, A16
    if (_ssr)
        v |= 0x0080;                        // SSR
    if (drivecount > 0 && !drive()->is_online())
        v |= 0x0040;                        // OFL
    v |= (uint16_t) ((_fc & 0x03) << 4);    // fatal class
    v |= (uint16_t) ((_tc & 0x07) << 1);    // termination class
    return v;
}

//
// update_tssr():
//  Publishes TSSR and the low 16 bits of TSBA for the next DATI.
//
void ts11_c::update_tssr(void)
{
    set_register_dati_value(busreg_TSDB, (uint16_t) (_tsba & 0xffff), __func__);
    set_register_dati_value(busreg_TSSR, tssr_value(), __func__);
}

//
// update_tssr_and_interrupt():
//  Publishes TSSR and raises the interrupt if the last command packet enabled
//  it. The status value travels with the interrupt so the host cannot read a
//  TSSR that predates the completion it is being interrupted for.
//
void ts11_c::update_tssr_and_interrupt(void)
{
    set_register_dati_value(busreg_TSDB, (uint16_t) (_tsba & 0xffff), __func__);
    uint16_t v = tssr_value();
    if (_interrupt_enable)
        qunibusadapter->INTR(intr_request, busreg_TSSR, v);
    else
        set_register_dati_value(busreg_TSSR, v, __func__);
}

//
// on_after_register_access():
//  Runs on the qunibusadapter event thread with the bus held: it only latches
//  state and hands the command to the worker, which does the DMA.
//
void ts11_c::on_after_register_access(qunibusdevice_register_t *device_reg,
                                      uint8_t unibus_control, DATO_ACCESS access)
{
    if (unibus_control != QUNIBUS_CYCLE_DATO)
        return;     // both registers are read as plain storage

    uint16_t value = device_reg->active_dato_flipflops;

    if (device_reg == busreg_TSSR) {
        if (_has_tsdbx && access == DATO_BYTEH) {
            // TSDBX (TSV05 UG 3.3.2.4): the command pointer's high bits and the
            // boot request. Writing it leaves SSR alone, so the host loads
            // TSDBX first and TSDB second; a load while the subsystem is busy
            // is refused and changes nothing.
            if (!_ssr) {
                _rmr = true;
                update_tssr();
                return;
            }
            _tsdbx = value & 0xff00;
            update_tssr();
            return;
        }
        // Any other write to the status address is a subsystem initialize
        // (EK-OTS11-TM-003 5.1.3 note).
        reset_subsystem();
        return;
    }

    // TSDB. A byte write is a maintenance data wraparound into TSBA, and puts
    // the controller in maintenance mode until the next subsystem initialize
    // (TSV05 UG 3.3.2.1). A word write is the command pointer.
    if (access == DATO_BYTEH) {
        uint16_t hi = (uint16_t) (value >> 8);
        _tsba = (uint32_t) ((hi << 8) | hi) | ((uint32_t) (hi & 0x03) << 16);
        _maintenance = true;
        _nba = true;
        _ssr = true;
        update_tssr();
        return;
    }
    if (access == DATO_BYTEL) {
        uint16_t lo = (uint16_t) (value & 0x00ff);
        _tsba = (uint32_t) ((lo << 8) | lo);
        _maintenance = true;
        _nba = true;
        _ssr = true;
        update_tssr();
        return;
    }

    if (_maintenance) {
        // A word write in maintenance mode supplies the wraparound address
        // rather than a command pointer.
        _tsba = value;
        _ssr = true;
        update_tssr();
        return;
    }

    if (!_ssr) {
        // The subsystem is busy or is putting out an attention message: the
        // write is refused and the command is never seen.
        _rmr = true;
        update_tssr();
        return;
    }

    // TSDB<15:2> are command pointer bits 15:2, TSDB<1:0> are bits 17:16, and
    // bits 1:0 of the pointer are zero: the packet sits on a modulo-4 boundary.
    uint32_t pointer = (uint32_t) (value & 0xfffc) | ((uint32_t) (value & 0x0003) << 16);
    bool boot = false;
    if (_has_tsdbx) {
        if (extended_features.value)
            pointer |= (uint32_t) ((_tsdbx & TSDBX_P_MASK) >> 8) << 18;
        boot = (_tsdbx & TSDBX_BOOT) != 0;
        _tsdbx = 0;     // consumed by this command pointer
    }

    // Loading a command pointer clears the status register's own error bits
    // (EK-OTS11-TM-003 5.3.4); the termination and fatal class stand until the
    // command ends.
    _rmr = false;
    _nxm = false;
    _sce = false;

    _tsba = pointer;
    _command_pointer = pointer;
    _ssr = false;
    update_tssr();

    pthread_mutex_lock(&on_after_register_access_mutex);
    if (boot)
        _pending_boot = true;
    else
        _pending_command = true;
    pthread_cond_signal(&on_after_register_access_cond);
    pthread_mutex_unlock(&on_after_register_access_mutex);
}

void ts11_c::worker_wake(void)
{
    pthread_mutex_lock(&on_after_register_access_mutex);
    pthread_cond_signal(&on_after_register_access_cond);
    pthread_mutex_unlock(&on_after_register_access_mutex);
}

void ts11_c::on_power_changed(signal_edge_enum aclo_edge, signal_edge_enum dclo_edge)
{
    storagecontroller_c::on_power_changed(aclo_edge, dclo_edge);
    if (dclo_edge == SIGNAL_EDGE_RAISING)
        reset_subsystem();
}

void ts11_c::on_init_changed(void)
{
    storagecontroller_c::on_init_changed();
    if (!init_asserted)     // falling edge of INIT
        reset_subsystem();
}

void ts11_c::on_drive_status_changed(storagedrive_c *changed)
{
    UNUSED(changed);
    check_transport_status();
}

//
// check_transport_status():
//  Watches the transport for a change of on-line state, which is a volume
//  check and an attention condition. The host sees it in OFL in TSSR straight
//  away, and in an attention message once it hands the controller the message
//  buffer. Run from the worker's idle tick as well as from the drive's own
//  notification, so a reel mounted or a switch thrown from the web interface
//  reaches the host without a command in flight.
//
void ts11_c::check_transport_status(void)
{
    bool online = drive()->is_online();
    if (online == _last_online)
        return;
    _last_online = online;
    _volume_check = true;
    update_tssr();
    raise_attention(0);     // class 0: on- or off-line
}

//
// raise_attention():
//  Queues an attention condition. The controller can only put out the message
//  once it owns the message buffer, which happens on the next command carrying
//  ACK or after a message buffer release command.
//
void ts11_c::raise_attention(unsigned class_code)
{
    _attn_pending = true;
    _attn_class = class_code;

    if (!_msgbuf_owned || _nba || !_ssr)
        return;

    // The controller holds the message buffer, so the attention goes out at
    // once: SSR drops, the message is deposited, SSR comes back up and the
    // interrupt follows if attention interrupts are enabled.
    pthread_mutex_lock(&on_after_register_access_mutex);
    _pending_attention = true;
    pthread_cond_signal(&on_after_register_access_cond);
    pthread_mutex_unlock(&on_after_register_access_mutex);
}

// ---------------------------------------------------------------------------
// DMA.

//
// dma_read_words() / dma_write_words():
//  One DMA transfer. TSBA counts up by two per word transferred and holds the
//  address past the transfer when it ends, which is what the host reads back
//  at the base address (EK-OTS11-TM-003 5.1.1).
//
bool ts11_c::dma_read_words(uint32_t addr, uint16_t *buffer, unsigned wordcount)
{
    if (wordcount == 0)
        return true;
    if (addr + 2 * wordcount > 2 * qunibus->addr_space_word_count) {
        _tsba = addr;
        return false;
    }
    qunibusadapter->DMA(dma_request, true, QUNIBUS_CYCLE_DATI, addr, buffer,
                        wordcount, TS11_DMA_TIMEOUT_MS);
    _tsba = dma_request.success ? addr + 2 * wordcount : dma_request.qunibus_end_addr;
    return dma_request.success;
}

bool ts11_c::dma_write_words(uint32_t addr, const uint16_t *buffer, unsigned wordcount)
{
    if (wordcount == 0)
        return true;
    if (addr + 2 * wordcount > 2 * qunibus->addr_space_word_count) {
        _tsba = addr;
        return false;
    }
    qunibusadapter->DMA(dma_request, true, QUNIBUS_CYCLE_DATO, addr,
                        const_cast<uint16_t *>(buffer), wordcount,
                        TS11_DMA_TIMEOUT_MS);
    _tsba = dma_request.success ? addr + 2 * wordcount : dma_request.qunibus_end_addr;
    return dma_request.success;
}

//
// byte_address():
//  Where byte k of a record sits in host memory. With SWB clear the bytes run
//  up consecutive addresses, the first byte of a word in bits 7:0; with SWB set
//  the two halves of every word exchange places, so the first byte of a word is
//  bits 15:8 (TSV05 UG figure 3-19, TS11 TM figures 5-6 and 5-7).
//
static inline uint32_t byte_address(uint32_t base, uint32_t k, bool swap_bytes)
{
    uint32_t a = base + k;
    return swap_bytes ? (a ^ 1) : a;
}

//
// dma_fetch_bytes():
//  Reads a record's worth of bytes out of host memory. The word span covering
//  the byte addresses is read in one transfer and the bytes are picked out of
//  it, so a byte count or a buffer address that is odd costs no extra cycles.
//
bool ts11_c::dma_fetch_bytes(uint32_t addr, uint8_t *data, uint32_t len, bool swap_bytes)
{
    if (len == 0)
        return true;

    uint32_t lo = byte_address(addr, 0, swap_bytes);
    uint32_t hi = lo;
    for (uint32_t k = 1; k < len; k++) {
        uint32_t a = byte_address(addr, k, swap_bytes);
        if (a < lo)
            lo = a;
        if (a > hi)
            hi = a;
    }
    uint32_t word_lo = lo & ~1u;
    uint32_t word_hi = hi | 1u;
    unsigned wordcount = (unsigned) ((word_hi + 1 - word_lo) / 2);

    std::vector<uint16_t> span(wordcount);
    if (!dma_read_words(word_lo, span.data(), wordcount))
        return false;

    const uint8_t *bytes = (const uint8_t *) span.data();   // bus and ARM are little-endian
    for (uint32_t k = 0; k < len; k++)
        data[k] = bytes[byte_address(addr, k, swap_bytes) - word_lo];
    return true;
}

//
// dma_store_bytes():
//  Lays a record into host memory. The word span covering the byte addresses is
//  assembled locally; words the record only partly fills are read back first so
//  the bytes around it survive, and the whole span then goes out in one
//  transfer.
//
bool ts11_c::dma_store_bytes(uint32_t addr, const uint8_t *data, uint32_t len, bool swap_bytes)
{
    if (len == 0)
        return true;

    uint32_t lo = byte_address(addr, 0, swap_bytes);
    uint32_t hi = lo;
    for (uint32_t k = 1; k < len; k++) {
        uint32_t a = byte_address(addr, k, swap_bytes);
        if (a < lo)
            lo = a;
        if (a > hi)
            hi = a;
    }
    uint32_t word_lo = lo & ~1u;
    uint32_t word_hi = hi | 1u;
    unsigned wordcount = (unsigned) ((word_hi + 1 - word_lo) / 2);

    std::vector<uint16_t> span(wordcount, 0);
    std::vector<bool> touched(2 * wordcount, false);
    uint8_t *bytes = (uint8_t *) span.data();
    for (uint32_t k = 0; k < len; k++) {
        uint32_t off = byte_address(addr, k, swap_bytes) - word_lo;
        bytes[off] = data[k];
        touched[off] = true;
    }

    // Read back any word the record leaves half empty, so the untouched byte
    // keeps the value the host put there.
    for (unsigned w = 0; w < wordcount; w++) {
        if (touched[2 * w] && touched[2 * w + 1])
            continue;
        uint16_t old;
        if (!dma_read_words(word_lo + 2 * w, &old, 1))
            return false;
        if (!touched[2 * w])
            bytes[2 * w] = (uint8_t) (old & 0xff);
        if (!touched[2 * w + 1])
            bytes[2 * w + 1] = (uint8_t) (old >> 8);
    }

    return dma_write_words(word_lo, span.data(), wordcount);
}

// ---------------------------------------------------------------------------
// Status and message packets.

//
// build_xst0():
//  Extended status register 0: the error bits this command produced, over the
//  transport's standing state.
//
uint16_t ts11_c::build_xst0(void)
{
    ts05_tape_c *d = drive();
    uint16_t v = _xst0_errors;
    if (d->is_online())
        v |= XST0_ONL;
    if (_interrupt_enable)
        v |= XST0_IE;
    if (_volume_check)
        v |= XST0_VCK;
    v |= XST0_PED;                  // the transport reads and writes 1600 bpi PE only
    if (d->is_write_locked())
        v |= XST0_WLK;
    if (d->tape().at_bot())
        v |= XST0_BOT;
    if (d->at_eot())
        v |= XST0_EOT;
    return v;
}

//
// send_message():
//  Deposits the message packet: header, data field length, the residual count
//  and the extended status registers. With Extended Features on and a message
//  buffer of at least 16 bytes, XST4 follows as an eighth word.
//
void ts11_c::send_message(unsigned message_type, unsigned class_code, bool ack)
{
    unsigned words = (extended_active() && _msgbuf_len >= 16) ? 8 : 7;
    uint16_t m[8];

    if (_high_speed)
        _xst4 |= 0x8000;    // HSP: the transport runs at 100 ips

    m[0] = (uint16_t) ((ack ? 0x8000 : 0)
                       | ((class_code & 0x0f) << 8)
                       | (message_type & 0x1f));
    m[1] = (uint16_t) ((words - 2) * 2);    // bytes of data following this word
    m[2] = _rbpcr;
    m[3] = build_xst0();
    m[4] = _xst1;
    m[5] = _xst2;
    m[6] = _xst3;
    m[7] = _xst4;

    if (!dma_write_words(_msgbuf_addr, m, words))
        _nxm = true;
}

//
// terminate():
//  Ends a command: the message packet goes out if the controller owns the
//  message buffer, ownership of both buffers returns to the CPU, SSR comes back
//  up and the interrupt follows.
//
void ts11_c::terminate(unsigned tc)
{
    _tc = tc;

    if (_msgbuf_owned && !_nba && !_suppress_message) {
        unsigned message_type, class_code = 0;
        switch (tc) {
        case TC_NORMAL:
        case TC_TAPE_STATUS_ALERT:
            message_type = MSG_END;
            break;
        case TC_ATTENTION:
            message_type = MSG_ATTN;
            break;
        case TC_FUNCTION_REJECT:
            message_type = MSG_FAIL;
            // class 2 is a write lock error or a nonexecutable function, class
            // 1 covers illegal command, illegal address and need buffer address
            class_code = (_xst0_errors & (XST0_WLE | XST0_NEF)) ? 2 : 1;
            break;
        default:
            message_type = MSG_ERROR;
            break;
        }
        send_message(message_type, class_code, true);
    }
    if (_msgbuf_owned && !_suppress_message)
        _msgbuf_owned = false;

    _ssr = true;
    update_tssr_and_interrupt();
}

// ---------------------------------------------------------------------------
// Command execution.

void ts11_c::worker(unsigned instance)
{
    UNUSED(instance);
    worker_init_realtime_priority(rt_device);

    while (!workers_terminate) {
        pthread_mutex_lock(&on_after_register_access_mutex);
        while (!_pending_command && !_pending_boot && !_pending_attention
               && !workers_terminate) {
            struct timespec abstime;
            clock_gettime(CLOCK_REALTIME, &abstime);
            // bounded wait: workers_stop() expects a cooperative exit
            abstime.tv_nsec += 50 * 1000000;
            if (abstime.tv_nsec >= 1000000000) {
                abstime.tv_sec += 1;
                abstime.tv_nsec -= 1000000000;
            }
            pthread_cond_timedwait(&on_after_register_access_cond,
                                   &on_after_register_access_mutex, &abstime);
        }
        bool do_command = _pending_command;
        bool do_boot = _pending_boot;
        bool do_attention = _pending_attention;
        _pending_command = false;
        _pending_boot = false;
        _pending_attention = false;
        pthread_mutex_unlock(&on_after_register_access_mutex);

        if (workers_terminate)
            break;
        if (init_asserted)
            continue;

        check_transport_status();

        if (do_boot)
            execute_boot();
        else if (do_command)
            execute_command();
        else if (do_attention && _attn_pending && _msgbuf_owned && !_nba) {
            // An attention while the controller holds the message buffer: SSR
            // drops for the length of the message and comes back up after it.
            clear_command_status();
            _attn_pending = false;
            _ssr = false;
            update_tssr();
            _tc = TC_ATTENTION;
            send_message(MSG_ATTN, _attn_class, false);
            _msgbuf_owned = false;
            _ssr = true;
            if (_eai)
                update_tssr_and_interrupt();
            else
                update_tssr();
        }
    }
}

//
// packet_address():
//  Turns the two address words of a command packet into a bus address. With
//  Extended Features off only bits 17:16 may be given, and the address is 18
//  bits; with it on the low six bits of the high word carry bits 21:16. Any
//  bit set outside that field is an illegal address.
//
bool ts11_c::packet_address(uint16_t low, uint16_t high, uint32_t *addr)
{
    if (extended_active()) {
        if (high & 0xffc0)
            return false;
        *addr = (uint32_t) low | ((uint32_t) (high & 0x003f) << 16);
    } else {
        if (high & 0xfffc)
            return false;
        *addr = (uint32_t) low | ((uint32_t) (high & 0x0003) << 16);
    }
    return true;
}

void ts11_c::execute_command(void)
{
    uint16_t pkt[4];

    clear_command_status();

    // All four words of the packet are fetched even when the command uses only
    // one (EK-OTS11-TM-003 5.2.2 note).
    if (!dma_read_words(_command_pointer, pkt, 4)) {
        _nxm = true;
        terminate(TC_RECOVERABLE_NO_MOTION);
        return;
    }

    uint16_t header = pkt[0];
    bool ack = (header & 0x8000) != 0;
    bool cvc = (header & 0x4000) != 0;
    bool opp = (header & 0x2000) != 0;
    bool swb = (header & 0x1000) != 0;
    unsigned mode = (header >> 8) & 0x0f;
    unsigned format = (header >> 5) & 0x07;
    unsigned command = header & 0x1f;

    if (ack)
        _msgbuf_owned = true;

    // An attention queued while the CPU held the message buffer goes out ahead
    // of the command. The message carries no ACK, so the CPU keeps the command
    // buffer and knows to reissue; volume check and interrupt enable stay as
    // they were (TSV05 UG 3.3.3.2).
    if (_attn_pending && _msgbuf_owned && !_nba) {
        _attn_pending = false;
        _tc = TC_ATTENTION;
        send_message(MSG_ATTN, _attn_class, false);
        _msgbuf_owned = false;
        _ssr = true;
        update_tssr_and_interrupt();
        return;
    }

    _interrupt_enable = (format & 0x04) != 0;
    if (cvc)
        _volume_check = false;

    // The format field takes only 000 and 100: a one-word header with the
    // interrupt disabled or enabled.
    if (format & 0x03) {
        _xst0_errors |= XST0_ILC;
        terminate(TC_FUNCTION_REJECT);
        return;
    }

    // Without a message buffer the controller can report nothing, so every
    // command but write characteristics is refused (TSV05 UG table 3-5, NBA).
    if (_nba && command != TS_CMD_WRITE_CHARACTERISTICS) {
        _xst0_errors |= XST0_NEF;
        terminate(TC_FUNCTION_REJECT);
        return;
    }

    // Commands that move tape need the transport on-line and the volume check
    // cleared (EK-OTS11-TM-003 table 5-5, NEF).
    bool motion_command = (command == TS_CMD_READ || command == TS_CMD_WRITE
                           || command == TS_CMD_POSITION || command == TS_CMD_FORMAT);
    if (motion_command) {
        if (!drive()->is_online() || _volume_check) {
            _xst0_errors |= XST0_NEF;
            terminate(TC_FUNCTION_REJECT);
            return;
        }
    }

    unsigned tc = TC_NORMAL;
    uint32_t addr = 0;
    uint32_t count = pkt[3];
    if (count == 0)
        count = TS11_MAX_RECORD;    // a byte count of 0 asks for 65536 bytes

    switch (command) {
    case TS_CMD_GET_STATUS:
        // The extended status registers travel in the message packet, which
        // every command deposits; get status asks for that and nothing else.
        break;

    case TS_CMD_INITIALIZE:
        // With no microdiagnostic error outstanding the drive initialize is a
        // no-op that updates the message, as get status does.
        break;

    case TS_CMD_READ:
        if (!packet_address(pkt[1], pkt[2], &addr)) {
            _xst0_errors |= XST0_ILA;
            tc = TC_FUNCTION_REJECT;
            break;
        }
        tc = command_read(mode, opp, swb, addr, count);
        break;

    case TS_CMD_WRITE:
        if (!packet_address(pkt[1], pkt[2], &addr)) {
            _xst0_errors |= XST0_ILA;
            tc = TC_FUNCTION_REJECT;
            break;
        }
        tc = command_write(mode, swb, addr, count);
        break;

    case TS_CMD_WRITE_CHARACTERISTICS:
        if (mode != 0) {
            _xst0_errors |= XST0_ILC;
            tc = TC_FUNCTION_REJECT;
            break;
        }
        tc = command_write_characteristics(pkt[1], pkt[2], pkt[3]);
        break;

    case TS_CMD_POSITION:
        tc = command_position(mode, pkt[1]);
        break;

    case TS_CMD_FORMAT:
        tc = command_format(mode);
        break;

    case TS_CMD_CONTROL:
        tc = command_control(mode);
        return;     // control commands answer for themselves

    default:
        // Write subsystem memory loads test functions into the controller's
        // microcode store, which this model does not carry.
        _xst0_errors |= XST0_ILC;
        tc = TC_FUNCTION_REJECT;
        break;
    }

    terminate(tc);
}

//
// command_read():
//  Read next, read previous and the two reread modes. A reread pairs a space
//  with a read so the same record is presented again; OPP exchanges the order
//  of the two (TSV05 UG 3.3.4.3).
//
unsigned ts11_c::command_read(unsigned mode, bool opp, bool swb, uint32_t addr, uint32_t count)
{
    simh_tape_c &t = drive()->tape();
    unsigned tc;

    switch (mode) {
    case 0:     // read next (forward)
        return transfer_record(addr, count, swb, false);

    case 1:     // read previous (reverse)
        if (t.at_bot()) {
            _xst0_errors |= XST0_NEF;
            return TC_FUNCTION_REJECT;
        }
        return transfer_record(addr, count, swb, true);

    case 2:     // reread previous
        if (t.at_bot()) {
            _xst0_errors |= XST0_NEF;
            return TC_FUNCTION_REJECT;
        }
        if (opp) {
            // read reverse, space forward
            tc = transfer_record(addr, count, swb, true);
            if (tc == TC_NORMAL || tc == TC_TAPE_STATUS_ALERT)
                space_one(false);
            return tc;
        }
        // space reverse, read forward
        tc = space_one(true);
        if (tc != TC_NORMAL)
            return tc;
        return transfer_record(addr, count, swb, false);

    case 3:     // reread next
        if (opp) {
            // read forward, space reverse
            tc = transfer_record(addr, count, swb, false);
            if (tc == TC_NORMAL || tc == TC_TAPE_STATUS_ALERT)
                space_one(true);
            return tc;
        }
        // space forward, read reverse
        tc = space_one(false);
        if (tc != TC_NORMAL)
            return tc;
        return transfer_record(addr, count, swb, true);

    default:
        _xst0_errors |= XST0_ILC;
        return TC_FUNCTION_REJECT;
    }
}

//
// space_one():
//  Moves over a single object. Returns TC_NORMAL when that was a data record,
//  and otherwise the termination class the object it met calls for, with the
//  status bits that go with it set.
//
unsigned ts11_c::space_one(bool reverse)
{
    simh_tape_c &t = drive()->tape();
    uint32_t reclen = 0;

    if (reverse)
        _xst3 |= XST3_REV;

    switch (reverse ? t.space_reverse(&reclen) : t.space_forward(&reclen)) {
    case simh_tape_c::R_RECORD:
        return TC_NORMAL;
    case simh_tape_c::R_TAPE_MARK:
        _xst0_errors |= XST0_TMK | XST0_RLS;
        return TC_TAPE_STATUS_ALERT;
    case simh_tape_c::R_BEGIN_OF_TAPE:
        _xst3 |= XST3_RIB;
        return TC_TAPE_STATUS_ALERT;
    default:
        _xst3 |= XST3_OPI;
        return TC_UNRECOVERABLE;
    }
}

//
// transfer_record():
//  Moves one record between the tape and host memory. A read of known length
//  is expected: a record shorter than the byte count sets RLS and leaves the
//  shortfall in the residual count, a longer one sets RLL and delivers only the
//  bytes asked for. Reading reverse delivers the record's tail and stores it
//  from the top of the buffer downwards, so the bytes land in memory in the
//  order they were written (TSV05 UG 3.3.4.3).
//
unsigned ts11_c::transfer_record(uint32_t addr, uint32_t count, bool swb, bool reverse)
{
    simh_tape_c &t = drive()->tape();
    simh_tape_c::result_t r;
    uint32_t reclen = 0;

    if (reverse)
        _xst3 |= XST3_REV;

    // the staging buffer is filled by the read; nothing beyond it is used
    _record.resize(TS11_MAX_RECORD);

    if (reverse) {
        // Step back over the record, read it forward into the staging buffer,
        // then step back again: the head ends up in front of the record, where
        // a reverse read leaves it.
        r = t.space_reverse(&reclen);
        if (r == simh_tape_c::R_TAPE_MARK) {
            _xst0_errors |= XST0_TMK | XST0_RLS;
            _rbpcr = (uint16_t) count;
            return TC_TAPE_STATUS_ALERT;
        }
        if (r == simh_tape_c::R_BEGIN_OF_TAPE) {
            _xst3 |= XST3_RIB;
            _rbpcr = (uint16_t) count;
            return TC_TAPE_STATUS_ALERT;
        }
        if (r != simh_tape_c::R_RECORD) {
            _xst3 |= XST3_OPI;
            return TC_UNRECOVERABLE;
        }
        r = t.read_forward(_record.data(), TS11_MAX_RECORD, &reclen);
        t.space_reverse(nullptr);
        if (r != simh_tape_c::R_RECORD) {
            _xst3 |= XST3_OPI;
            return TC_UNRECOVERABLE;
        }
    } else {
        r = t.read_forward(_record.data(), TS11_MAX_RECORD, &reclen);
        if (r == simh_tape_c::R_TAPE_MARK) {
            _xst0_errors |= XST0_TMK | XST0_RLS;
            _rbpcr = (uint16_t) count;
            return TC_TAPE_STATUS_ALERT;
        }
        if (r != simh_tape_c::R_RECORD) {
            // Running off the end of the recorded tape without finding data is
            // an operation incomplete, and the position is no longer known.
            _xst3 |= XST3_OPI;
            return TC_UNRECOVERABLE;
        }
    }

    // The staging buffer holds at most one full-length TS11 record; a longer
    // one on the image reads as a record too long for the transfer.
    uint32_t captured = reclen < TS11_MAX_RECORD ? reclen : TS11_MAX_RECORD;
    uint32_t n = count < captured ? count : captured;
    const uint8_t *src = _record.data();
    uint32_t dest = addr;
    if (reverse) {
        src += captured - n;    // the last bytes written are the first read back
        dest += count - n;      // stored from the top of the buffer downwards
    }
    if (n > 0 && !dma_store_bytes(dest, src, n, swb)) {
        _nxm = true;
        return TC_RECOVERABLE_ONE_RECORD;
    }

    if (reclen < count) {
        _xst0_errors |= XST0_RLS;
        _rbpcr = (uint16_t) (count - reclen);
        return TC_TAPE_STATUS_ALERT;
    }
    if (reclen > count) {
        _xst0_errors |= XST0_RLL;
        return TC_TAPE_STATUS_ALERT;
    }
    return TC_NORMAL;
}

//
// command_write_characteristics():
//  Tells the controller where the message buffer is and how long it is, and
//  loads the characteristics mode byte. It is the one command that answers
//  while NBA is set, since it is what clears NBA.
//
unsigned ts11_c::command_write_characteristics(uint16_t low, uint16_t high, uint16_t extent)
{
    uint32_t addr;

    // The characteristics data sits on a word boundary; an odd address or a
    // high-order word outside the address field is refused, and the refusal
    // deposits no message (TSV05 UG 3.3.4.4).
    if ((low & 1) || !packet_address(low, high, &addr)) {
        _xst0_errors |= XST0_ILA;
        _nba = true;
        _suppress_message = true;
        return TC_FUNCTION_REJECT;
    }

    // At least three data words are needed to name the message buffer.
    if (extent < 6) {
        _xst0_errors |= XST0_ILA;
        _nba = true;
        return TC_FUNCTION_REJECT;
    }

    unsigned wanted = extent;
    unsigned limit = extended_active() ? 10 : 8;
    if (wanted > limit)
        wanted = limit;
    unsigned words = (wanted + 1) / 2;

    uint16_t data[5] = { 0, 0, 0, 0, 0 };
    if (!dma_read_words(addr, data, words)) {
        _nxm = true;
        _nba = true;
        return TC_RECOVERABLE_NO_MOTION;
    }

    uint32_t msgbuf;
    if ((data[0] & 1) || !packet_address(data[0], data[1], &msgbuf) || data[2] < 14) {
        _xst0_errors |= XST0_ILA;
        _nba = true;
        return TC_FUNCTION_REJECT;
    }

    _msgbuf_addr = msgbuf;
    _msgbuf_len = data[2];

    // A byte count under 7 leaves the characteristics mode byte unfetched, and
    // the values the controller already holds stand.
    if (extent >= 7) {
        _ess = (data[3] & 0x0080) != 0;
        _enb = (data[3] & 0x0040) != 0;
        _eai = (data[3] & 0x0020) != 0;
        _eri = (data[3] & 0x0010) != 0;
    }
    // Likewise a count under 10 leaves the extended control word unfetched.
    if (extended_active() && extent >= 10)
        _high_speed = (data[4] & 0x0020) != 0;

    _nba = false;

    // The message this command returns carries the control microcode revision
    // in the low byte of XST2 (EK-OTS11-TM-003 table 5-7 note).
    _xst2 |= TS11_MICROCODE_REVISION;
    return TC_NORMAL;
}

//
// command_write():
//  Write data and write data retry. A retry backs over the record just written,
//  erases, and writes it again; on an image the erase leaves no trace, because
//  writing a record makes it the end of the recorded tape.
//
unsigned ts11_c::command_write(unsigned mode, bool swb, uint32_t addr, uint32_t count)
{
    ts05_tape_c *d = drive();
    simh_tape_c &t = d->tape();

    if (mode != 0 && mode != 2) {
        _xst0_errors |= XST0_ILC;
        return TC_FUNCTION_REJECT;
    }
    if (d->is_write_locked()) {
        _xst0_errors |= XST0_WLE | XST0_NEF;
        return TC_FUNCTION_REJECT;
    }
    if (mode == 2 && t.at_bot()) {
        _xst0_errors |= XST0_NEF;
        return TC_FUNCTION_REJECT;
    }

    _record.resize(count);
    if (!dma_fetch_bytes(addr, _record.data(), count, swb)) {
        _nxm = true;
        return TC_RECOVERABLE_NO_MOTION;
    }

    if (mode == 2) {
        unsigned tc = space_one(true);
        if (tc != TC_NORMAL)
            return tc;
    }

    if (t.write_record(_record.data(), count) != simh_tape_c::R_RECORD) {
        _xst1 |= XST1_UNC;
        return TC_UNRECOVERABLE;
    }

    // A write at or past the EOT marker completes, and reports the marker.
    if (d->at_eot())
        return TC_TAPE_STATUS_ALERT;
    return TC_NORMAL;
}

//
// command_position():
//  Space records, skip tape marks and rewind. Spacing counts the objects it
//  moves over and leaves what it did not reach in the residual count.
//
unsigned ts11_c::command_position(unsigned mode, uint32_t count)
{
    simh_tape_c &t = drive()->tape();
    bool reverse = (mode == 1 || mode == 3);

    if (mode == 4) {
        t.rewind();
        return TC_NORMAL;
    }
    if (mode > 4) {
        _xst0_errors |= XST0_ILC;
        return TC_FUNCTION_REJECT;
    }
    if (reverse) {
        _xst3 |= XST3_REV;
        if (t.at_bot()) {
            _xst0_errors |= XST0_NEF;
            return TC_FUNCTION_REJECT;
        }
    }

    bool skip_marks = (mode == 2 || mode == 3);
    bool started_at_bot = t.at_bot();
    bool previous_was_mark = false;
    bool first_object = true;
    unsigned tc = TC_NORMAL;
    uint32_t left = count;

    while (left > 0) {
        uint32_t reclen = 0;
        simh_tape_c::result_t r = reverse ? t.space_reverse(&reclen) : t.space_forward(&reclen);

        if (r == simh_tape_c::R_BEGIN_OF_TAPE) {
            _xst3 |= XST3_RIB;
            tc = TC_TAPE_STATUS_ALERT;
            break;
        }
        if (r != simh_tape_c::R_RECORD && r != simh_tape_c::R_TAPE_MARK) {
            _xst3 |= XST3_OPI;
            tc = TC_UNRECOVERABLE;
            break;
        }

        if (!skip_marks) {
            // Space records: the tape mark counts as one object and ends the
            // operation with a tape status alert.
            left--;
            if (r == simh_tape_c::R_TAPE_MARK) {
                _xst0_errors |= XST0_TMK;
                tc = TC_TAPE_STATUS_ALERT;
                break;
            }
            continue;
        }

        // Skip tape marks: only tape marks count down.
        if (r == simh_tape_c::R_TAPE_MARK) {
            left--;
            _xst0_errors |= XST0_TMK;
            if (_ess) {
                // Two tape marks with no data between them are the logical end
                // of tape, and so is a tape mark as the first object off BOT
                // when ENB is set as well.
                bool double_mark = previous_was_mark;
                bool first_off_bot = first_object && started_at_bot && !reverse && _enb;
                if (double_mark || first_off_bot) {
                    _xst0_errors |= XST0_LET;
                    tc = TC_TAPE_STATUS_ALERT;
                    first_object = false;
                    break;
                }
            }
            previous_was_mark = true;
        } else {
            previous_was_mark = false;
        }
        first_object = false;
    }

    _rbpcr = (uint16_t) left;
    if (left != 0)
        _xst0_errors |= XST0_RLS;
    return tc;
}

//
// command_format():
//  Write tape mark, erase, and write tape mark retry. An erase lays down blank
//  tape, which a .tap image does not record; the tape mark it precedes is what
//  the host reads back.
//
unsigned ts11_c::command_format(unsigned mode)
{
    ts05_tape_c *d = drive();
    simh_tape_c &t = d->tape();

    if (mode > 2) {
        _xst0_errors |= XST0_ILC;
        return TC_FUNCTION_REJECT;
    }
    if (d->is_write_locked()) {
        _xst0_errors |= XST0_WLE | XST0_NEF;
        return TC_FUNCTION_REJECT;
    }
    if (mode == 2 && t.at_bot()) {
        _xst0_errors |= XST0_NEF;
        return TC_FUNCTION_REJECT;
    }

    if (mode == 1)  // erase: 3.75 inches of blank tape, no record written
        return d->at_eot() ? TC_TAPE_STATUS_ALERT : TC_NORMAL;

    if (mode == 2) {
        unsigned tc = space_one(true);
        if (tc != TC_NORMAL)
            return tc;
    }

    if (t.write_tape_mark() != simh_tape_c::R_RECORD) {
        _xst1 |= XST1_UNC;
        return TC_UNRECOVERABLE;
    }
    _xst0_errors |= XST0_TMK;

    if (d->at_eot())
        return TC_TAPE_STATUS_ALERT;
    return TC_NORMAL;
}

//
// command_control():
//  Message buffer release, rewind and unload, the no-op that stands in for the
//  TS11's clean tape, and the TSV05's rewind with immediate interrupt. A
//  control command terminates at once and answers for itself: message buffer
//  release keeps the buffer, so the controller reports the outcome from it.
//
unsigned ts11_c::command_control(unsigned mode)
{
    ts05_tape_c *d = drive();

    switch (mode) {
    case 0:     // message buffer release
        // The controller keeps the message buffer so it can report an attention
        // while the host is busy elsewhere. No message goes out; only SSR comes
        // back up, and the interrupt follows when ERI is set.
        _tc = TC_NORMAL;
        _ssr = true;
        if (_eri && _interrupt_enable)
            update_tssr_and_interrupt();
        else
            update_tssr();
        // An attention that was waiting for the buffer can go out now.
        if (_attn_pending && _msgbuf_owned && !_nba)
            raise_attention(_attn_class);
        return TC_NORMAL;

    case 1:     // rewind and unload
        d->tape().rewind();
        d->online_switch.set(false);
        terminate(TC_NORMAL);
        return TC_NORMAL;

    case 2:     // no-op; the TS11 cleans tape here
        terminate(TC_NORMAL);
        return TC_NORMAL;

    case 4:     // rewind with immediate interrupt
        if (!extended_active()) {
            _xst0_errors |= XST0_ILC;
            terminate(TC_FUNCTION_REJECT);
            return TC_FUNCTION_REJECT;
        }
        d->tape().rewind();
        terminate(TC_NORMAL);
        return TC_NORMAL;

    default:
        _xst0_errors |= XST0_ILC;
        terminate(TC_FUNCTION_REJECT);
        return TC_FUNCTION_REJECT;
    }
}

//
// execute_boot():
//  The boot bit in TSDBX (TSV05 UG table 3-6): rewind to BOT, skip the first
//  record, and load the first 512 bytes of the second record into memory from
//  location 0. SSR stays clear until the sequence finishes.
//
void ts11_c::execute_boot(void)
{
    ts05_tape_c *d = drive();
    simh_tape_c &t = d->tape();
    simh_tape_c::result_t r;
    uint32_t reclen = 0;

    clear_command_status();
    _interrupt_enable = false;

    if (!d->is_online()) {
        _xst0_errors |= XST0_NEF;
        _tc = TC_FUNCTION_REJECT;
        _ssr = true;
        update_tssr();
        return;
    }

    t.rewind();
    r = t.space_forward(&reclen);
    if (r != simh_tape_c::R_RECORD) {
        _xst3 |= XST3_OPI;
        _tc = TC_UNRECOVERABLE;
        _ssr = true;
        update_tssr();
        return;
    }

    _record.assign(512, 0);   // a short boot record leaves the rest of the block zero
    r = t.read_forward(_record.data(), 512, &reclen);
    if (r != simh_tape_c::R_RECORD) {
        _xst3 |= XST3_OPI;
        _tc = TC_UNRECOVERABLE;
        _ssr = true;
        update_tssr();
        return;
    }
    if (!dma_store_bytes(0, _record.data(), 512, false)) {
        _nxm = true;
        _tc = TC_RECOVERABLE_ONE_RECORD;
        _ssr = true;
        update_tssr();
        return;
    }

    _tc = TC_NORMAL;
    _ssr = true;
    update_tssr();
}
