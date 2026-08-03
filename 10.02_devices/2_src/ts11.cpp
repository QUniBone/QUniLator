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
// The revision level field, XST2<7:0>: after a write characteristics command it
// carries the two configuration switches and the microcode revision, and after
// every other command the number of the selected transport (TSV05 UG table 3-9).
#define XST2_RL_EF  0x0080  // Extended Features Enable switch
#define XST2_RL_BUF 0x0040  // Buffering Enable switch

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
    // TSBA keeps what it held: "not cleared on power up, subsystem INIT, or
    // bus initialize" (EK-OTS11-TM-003 5.1.1; TSV05 UG 3.3.2.1 item 5), which
    // is what the maintenance wraparound tests read back.
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
    _bot_strip = false;
    _bot_blank = false;
    _rewinding = false;
    _unit_select = 0;
    _high_speed = false;

    _pending_command = false;
    _pending_boot = false;
    _pending_attention = false;
    _command_pointer = 0;

    clear_command_status();

    // "if the ON-LINE switch is on, the drive performs an auto-load sequence
    // and positions the tape at BOT" (EK-OTS11-TM-003 5.3.4). The worker owns
    // the transport, so it performs that; a reset arrives on the thread
    // serving the register write, and two threads in one image file is not a
    // thing to do.
    _pending_autoload = (drivecount > 0) && drive()->is_online();
    _last_online = (drivecount > 0) && drive()->is_online();
    // A command the worker is finishing belongs to the subsystem as it was.
    _reset_generation++;

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
    // The selected transport is off-line, and one that is not in the
    // subsystem at all answers the same way.
    if (drivecount == 0 || _unit_select != 0 || !drive()->is_online())
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
        // "If SSR is clear, an error (RMR) occurs, but the transfer is still
        // executed and completed" (EK-OTS11-TM-003 5.1.2.2).
        if (!_ssr)
            _rmr = true;
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

    if (wordcount > TS11_MAX_RECORD / 2 + 2)
        return false;
    if (!dma_read_words(word_lo, _dma_span, wordcount))
        return false;

    const uint8_t *bytes = (const uint8_t *) _dma_span;   // bus and ARM are little-endian
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

    if (wordcount > TS11_MAX_RECORD / 2 + 2)
        return false;
    memset(_dma_span, 0, 2 * (size_t) wordcount);
    std::vector<bool> touched(2 * wordcount, false);
    uint8_t *bytes = (uint8_t *) _dma_span;
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
        if (!dma_read_words(word_lo + 2 * w, &_dma_word, 1))
            return false;
        uint16_t old = _dma_word;
        if (!touched[2 * w])
            bytes[2 * w] = (uint8_t) (old & 0xff);
        if (!touched[2 * w + 1])
            bytes[2 * w + 1] = (uint8_t) (old >> 8);
    }

    return dma_write_words(word_lo, _dma_span, wordcount);
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
    // "Tape is moving ... the transport is asserting Formatter Busy or
    // Rewinding status" (EK-TSV05-UG-001 table 3-11).
    if (_rewinding)
        v |= XST0_MOT;
    // On the blank stretch erases laid down past the marker, the head is
    // beyond BOT even though the image position has not moved.
    if (d->tape().at_bot() && d->erased_footage() == 0 && !_bot_blank)
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
    uint16_t *m = _dma_message;

    if (_high_speed)
        _xst4 |= 0x8000;    // HSP: the transport runs at 100 ips

    m[0] = (uint16_t) ((ack ? 0x8000 : 0)
                       | ((class_code & 0x0f) << 8)
                       | (message_type & 0x1f));
    m[1] = (uint16_t) ((words - 2) * 2);    // bytes of data following this word
    m[2] = _rbpcr;
    m[3] = build_xst0();
    m[4] = _xst1;
    m[5] = (uint16_t) (_xst2 | (_rewinding ? XST2_OPM : 0));
    m[6] = _xst3;
    m[7] = _xst4;

    DEBUG("message type=%02o xst0=%06o xst1=%06o xst3=%06o rbpcr=%o",
          message_type, m[3], m[4], m[6], m[2]);
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
    // A subsystem initialize that arrived while this command was running has
    // already put the subsystem where it belongs; the command it halted says
    // nothing more (EK-OTS11-TM-003 5.3.4).
    if (_reset_generation != _command_generation)
        return;

    _tc = tc;

    DEBUG("terminate tc=%u xst0err=%06o xst1=%06o xst3=%06o obj=%u%s",
          tc, _xst0_errors, _xst1, _xst3,
          drivecount > 0 ? (unsigned) drive()->tape().object_position() : 0,
          drivecount > 0 && drive()->tape().at_bot() ? " BOT" : "");

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

        // The auto-load a subsystem initialize asks for, on the thread that
        // owns the transport.
        if (_pending_autoload) {
            _pending_autoload = false;
            if (drivecount > 0 && drive()->is_online()) {
                drive()->rewind();
                _bot_strip = false;
                _bot_blank = false;
                _rewinding = false;
            }
        }

        check_transport_status();

        _command_generation = _reset_generation;
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
            if (_eai && _interrupt_enable)
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

//
// command_mode_legal():
//  Whether a command code and mode field name a command at all
//  (EK-TSV05-UG-001 table 3-16). Control mode 4 is named here because the
//  decode knows it; whether the board carries the Extended Features that
//  execute it is the command's own business.
//
static bool command_mode_legal(unsigned command, unsigned mode)
{
    switch (command) {
    case TS_CMD_READ:                   return mode <= 3;
    case TS_CMD_WRITE_CHARACTERISTICS:  return mode == 0;
    case TS_CMD_WRITE:                  return mode == 0 || mode == 2;
    case TS_CMD_WRITE_SUBSYSTEM_MEM:    return mode == 0;
    case TS_CMD_POSITION:               return mode <= 4;
    case TS_CMD_FORMAT:                 return mode <= 2;
    case TS_CMD_CONTROL:                return mode <= 2 || mode == 4;
    case TS_CMD_INITIALIZE:             return mode == 0;
    case TS_CMD_GET_STATUS:             return mode == 0;
    default:                            return false;
    }
}

void ts11_c::execute_command(void)
{
    uint16_t *pkt = _dma_packet;

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
    //
    // Write characteristics and write subsystem memory run regardless: they
    // are how the software names the message buffer the attention would be
    // deposited in, so holding them back on a pending attention would leave no
    // way out of it (TSV05 UG 3.3.3.2).
    bool names_the_buffer = (command == TS_CMD_WRITE_CHARACTERISTICS
            || command == TS_CMD_WRITE_SUBSYSTEM_MEM);
    if (_attn_pending && _msgbuf_owned && !_nba && !names_the_buffer) {
        _attn_pending = false;
        _tc = TC_ATTENTION;
        send_message(MSG_ATTN, _attn_class, false);
        _msgbuf_owned = false;
        _ssr = true;
        // An attention interrupts when the message buffer release that armed
        // it enabled interrupts and attention interrupts are enabled
        // (EK-OTS11-TM-003 table 5-11 EAI; TSV05 UG 3.3.3.2).
        if (_eai)
            update_tssr_and_interrupt();
        else
            update_tssr();
        return;
    }

    _interrupt_enable = (format & 0x04) != 0;
    if (cvc)
        _volume_check = false;

    // one line per command, with the tape position it starts from: the record
    // a diagnostic's expectations diverge from the model at is read off here
    DEBUG("command %02o mode %o pkt=%06o,%06o,%06o ack=%d cvc=%d opp=%d swb=%d obj=%u%s",
          command, mode, pkt[1], pkt[2], pkt[3], ack, cvc, opp, swb,
          drivecount > 0 ? (unsigned) drive()->tape().object_position() : 0,
          drivecount > 0 && drive()->tape().at_bot() ? " BOT" : "");

    // The format field takes only 000 and 100: a one-word header with the
    // interrupt disabled or enabled.
    if (format & 0x03) {
        _xst0_errors |= XST0_ILC;
        terminate(TC_FUNCTION_REJECT);
        return;
    }

    // What the packet asks for is decoded before the transport is consulted,
    // so an illegal command is named as one whatever else stands in its way:
    // the NEF a volume check or an off-line transport adds stands beside ILC
    // rather than in place of it (CVTSC TST 004 SUB 010).
    bool illegal = !command_mode_legal(command, mode);
    if (illegal)
        _xst0_errors |= XST0_ILC;

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

    // A rewind may still be running: tape motion waits for the reel to reach
    // BOT, anything else answers over it and takes the rewind's own end with
    // it once that has passed.
    if (drivecount > 0)
        settle_rewind(motion_command);

    if (motion_command) {
        if (_unit_select != 0 || !drive()->is_online() || _volume_check) {
            _xst0_errors |= XST0_NEF;
            terminate(TC_FUNCTION_REJECT);
            return;
        }
    }

    // Every command but write characteristics answers with the number of the
    // selected transport in the low bits of XST2 (TSV05 UG table 3-9).
    if (command != TS_CMD_WRITE_CHARACTERISTICS)
        _xst2 |= (uint16_t) (_unit_select & 0x07);

    if (illegal) {
        terminate(TC_FUNCTION_REJECT);
        return;
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
        // no-op that updates the message, as get status does - but it always
        // returns the unit selection to transport 0 (EK-TSV05-UG-001 3.3.4.9,
        // table 3-18).
        _unit_select = 0;
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

    // Where the motion left the tape: reverse motion that ended on the BOT
    // marker stopped ON it, anything else - forward motion, a rewind, a write,
    // reverse ending mid-tape - leaves the strip state behind. An erase keeps
    // the blank-stretch state it just established. A refused command moved
    // nothing and changes nothing.
    if (motion_command && tc != TC_FUNCTION_REJECT && drivecount > 0) {
        _bot_strip = (_xst3 & XST3_REV) != 0 && drive()->tape().at_bot();
        if (!(command == TS_CMD_FORMAT && mode == 1))
            _bot_blank = false;
    }

    terminate(tc);
}

// The worse of two termination classes: a command built from two motions - a
// retry's space and its rewrite, a reread's space and its read - runs both
// halves and reports the graver outcome, with the status bits of both standing
// (the TS11 microcode combines its sub-operations the same way).
static inline unsigned tc_worse(unsigned a, unsigned b)
{
    return a > b ? a : b;
}

//
// start_rewind():
//  Sets the reel turning for a rewind with immediate interrupt, which answers
//  "at the start of the rewind rather than when the tape reaches BOT"
//  (EK-TSV05-UG-001 3.3.4.8). The tape reaches BOT when the wind-back time
//  for the footage behind it has passed; until then the transport reports
//  motion, and the image still holds the position the reel is winding back
//  from. The ordinary rewind answers on arrival instead, so the host sees the
//  turning reel only as the time the command takes, and that one moves the
//  image at once.
//
void ts11_c::start_rewind(void)
{
    _rewind_done_ns = timeout_c::abstime_ns() + drive()->rewind_time_us() * 1000;
    _rewinding = true;
}

//
// settle_rewind():
//  Brings a rewind that is still running to its end. A command that moves
//  tape waits for the reel: "if a transport is rewinding and another tape
//  motion command is issued to it, the controller will wait until the tape
//  has been rewound to BOT before proceeding" (EK-TSV05-UG-001 3.3.4.8).
//  Anything else - a get status, a message buffer release - answers over the
//  turning reel, and only collects the rewind once its time has passed.
//
void ts11_c::settle_rewind(bool wait)
{
    if (!_rewinding)
        return;

    while (timeout_c::abstime_ns() < _rewind_done_ns) {
        if (!wait)
            return;
        // A subsystem initialize that arrives meanwhile owns the transport.
        if (_reset_generation != _command_generation)
            return;
        timeout_c::wait_ms(1);
    }

    drive()->rewind();
    _bot_strip = false;
    _bot_blank = false;
    _rewinding = false;
}

//
// at_load_point():
//  True when the tape rests at the settled load point, where a reverse command
//  is nonexecutable: at BOT on the image with no erased footage in front of
//  the head, and the head neither stopped on the marker nor sits on the blank
//  an erase just laid down.
//
bool ts11_c::at_load_point(void)
{
    return drive()->tape().at_bot() && drive()->erased_footage() == 0
           && !_bot_strip && !_bot_blank;
}

//
// command_read():
//  Read next, read previous and the two reread modes. A reread pairs a space
//  with a read so the same record is presented again; OPP exchanges the order
//  of the two (TSV05 UG 3.3.4.3).
//
unsigned ts11_c::command_read(unsigned mode, bool opp, bool swb, uint32_t addr, uint32_t count)
{
    unsigned tc;

    switch (mode) {
    case 0:     // read next (forward)
        return transfer_record(addr, count, swb, false);

    case 1:     // read previous (reverse)
        if (at_load_point()) {
            // a reverse command issued at the settled load point is refused
            // (EK-OTS11-TM-003 table 5-6, BOT: TC3 at BOT on a reverse command);
            // stopped ON the marker after reversing into it, the motion runs
            // and halts there with RIB instead
            _xst0_errors |= XST0_NEF;
            _xst3 |= XST3_RIB;
            return TC_FUNCTION_REJECT;
        }
        return transfer_record(addr, count, swb, true);

    case 2:     // reread previous
        if (at_load_point()) {
            _xst0_errors |= XST0_NEF;
            _xst3 |= XST3_RIB;
            return TC_FUNCTION_REJECT;
        }
        if (opp) {
            // read reverse, space forward: both halves run, worse outcome wins
            tc = transfer_record(addr, count, swb, true);
            return tc_worse(tc, space_one(false));
        }
        // space reverse, read forward
        tc = space_one(true);
        return tc_worse(tc, transfer_record(addr, count, swb, false));

    case 3:     // reread next
        if (opp) {
            // read forward, space reverse
            tc = transfer_record(addr, count, swb, false);
            return tc_worse(tc, space_one(true));
        }
        // space forward, read reverse
        tc = space_one(false);
        return tc_worse(tc, transfer_record(addr, count, swb, true));

    default:                // the decode has already refused any other mode
        return TC_FUNCTION_REJECT;
    }
}

//
// reverse_met_bot():
//  Reverse motion that found no object where the image answers BOT. Across a
//  gap's length of blank the motion halts on the BOT marker with RIB and the
//  tape status alert; in a longer blank stretch the transport's gap shutdown
//  stops it a shutdown's length further back, still deep in the blank and
//  short of the marker, and the command ends operation incomplete with the
//  position lost - the next reverse from there gives up the same way (CVTSD
//  TST 004 SUB 001 reads back over one erase gap and alerts, SUB 003 issues
//  motion after motion over a fully erased tape and expects OPI on each).
//
unsigned ts11_c::reverse_met_bot(void)
{
    ts05_tape_c *d = drive();

    if (d->erased_footage() > TS05_BLANK_SHUTDOWN_BYTES) {
        d->blank_backed(TS05_BLANK_SHUTDOWN_BYTES);
        _xst3 |= XST3_OPI;
        return TC_UNRECOVERABLE;
    }
    d->rewind();    // the head rests on the marker; the short blank lies ahead
    _xst3 |= XST3_RIB;
    _xst0_errors |= XST0_BOT;
    return TC_TAPE_STATUS_ALERT;
}

//
// retry_backspace():
//  The reverse motion of a write or write-tape-mark retry. The object it
//  crosses - record or tape mark - is the one being rewritten, so crossing it
//  is the operation itself and reports nothing. Motion that meets the BOT
//  marker halts there with RIB and a tape status alert, and the retry's
//  rewrite does not run (CVTSD TST 001: the record ahead survives).
//
unsigned ts11_c::retry_backspace(void)
{
    simh_tape_c &t = drive()->tape();
    uint32_t reclen = 0;

    _xst3 |= XST3_REV;
    switch (t.space_reverse(&reclen)) {
    case simh_tape_c::R_RECORD:
    case simh_tape_c::R_TAPE_MARK:
        return TC_NORMAL;
    case simh_tape_c::R_BEGIN_OF_TAPE:
        return reverse_met_bot();
    default:
        _xst3 |= XST3_OPI;
        return TC_UNRECOVERABLE;
    }
}

//
// space_one():
//  Moves over a single object. Returns TC_NORMAL when that was a data record,
//  and otherwise the termination class the object it met calls for, with the
//  status bits that go with it set. Reverse motion that meets the BOT marker
//  halts there and reports RIB (EK-OTS11-TM-003 table 5-8): a tape status
//  alert, not a refusal - the command was in progress when it met the marker.
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
        _xst0_errors |= XST0_RLS;
        return reverse_met_bot();
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
            // Meeting BOT while searching for the record reports through
            // reverse_met_bot(); a record found and read reports by its
            // length alone, even when the head stops at the marker (CVTSD
            // TST 004 SUB 002 reads the first record and completes normally).
            _xst0_errors |= XST0_RLS;
            _rbpcr = (uint16_t) count;
            return reverse_met_bot();
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
    DEBUG("read%s %u of %u bytes to %06o swb=%d: %02x %02x %02x %02x",
          reverse ? " rev" : "", n, reclen, dest, swb,
          n > 0 ? src[0] : 0, n > 1 ? src[1] : 0,
          n > 2 ? src[2] : 0, n > 3 ? src[3] : 0);

    if (n > 0 && !dma_store_bytes(dest, src, n, swb)) {
        _nxm = true;
        return TC_RECOVERABLE_ONE_RECORD;
    }

    unsigned tc = TC_NORMAL;
    if (reclen < count) {
        _xst0_errors |= XST0_RLS;
        _rbpcr = (uint16_t) (count - reclen);
        tc = TC_TAPE_STATUS_ALERT;
    } else if (reclen > count) {
        _xst0_errors |= XST0_RLL;
        tc = TC_TAPE_STATUS_ALERT;
    }
    return tc;
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

    uint16_t *data = _dma_chars;
    memset(data, 0, sizeof _dma_chars);
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
    if (extended_active() && extent >= 10) {
        _high_speed = (data[4] & 0x0020) != 0;
        // Bits 2-0 select "a transport for subsequent tape operations"
        // (EK-TSV05-UG-001 table 3-18). The subsystem carries one, so any
        // other number selects a transport that is not there.
        _unit_select = data[4] & 0x0007;
    }

    _nba = false;

    // The message this command returns carries the module's configuration in
    // the low byte of XST2: the Extended Features switch, the Buffering Enable
    // switch, and the control microcode revision. This controller writes every
    // record straight to tape, so its buffering switch reads off.
    _xst2 |= TS11_MICROCODE_REVISION;
    if (extended_active())
        _xst2 |= XST2_RL_EF;
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

    if (d->is_write_locked()) {
        _xst0_errors |= XST0_WLE | XST0_NEF;
        return TC_FUNCTION_REJECT;
    }

    // A retry issued at the settled load point has nothing to back over.
    if (mode == 2 && at_load_point()) {
        _xst0_errors |= XST0_NEF;
        _xst3 |= XST3_RIB;
        return TC_FUNCTION_REJECT;
    }

    _record.resize(count);
    if (!dma_fetch_bytes(addr, _record.data(), count, swb)) {
        _nxm = true;
        return TC_RECOVERABLE_NO_MOTION;
    }

    DEBUG("write %u bytes from %06o swb=%d: %02x %02x %02x %02x", count, addr, swb,
          count > 0 ? _record[0] : 0, count > 1 ? _record[1] : 0,
          count > 2 ? _record[2] : 0, count > 3 ? _record[3] : 0);

    if (mode == 2) {
        unsigned tc = retry_backspace();
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
        drive()->rewind();
        _bot_strip = false;
        _bot_blank = false;
        return TC_NORMAL;
    }
    if (reverse) {
        _xst3 |= XST3_REV;
        if (at_load_point()) {
            // refused at the settled load point, like every reverse command;
            // stopped ON the marker it runs and halts there with RIB
            _xst0_errors |= XST0_NEF;
            _xst3 |= XST3_RIB;
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
            tc = reverse_met_bot();
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

    if (d->is_write_locked()) {
        _xst0_errors |= XST0_WLE | XST0_NEF;
        return TC_FUNCTION_REJECT;
    }

    if (mode == 1) {
        // Erase: 3.75 inches of blank tape laid down moving forward, no
        // record written. The blank overwrites what lay ahead, so the image
        // ends here, the way a write makes itself the end of the recorded
        // tape. Issued inside the BOT strip it carries the head past the
        // marker onto the blank stretch, so BOT stops reporting and a reverse
        // command runs from here - finding only erased tape, it halts at the
        // marker with RIB (CVTSD TST 004 SUB 001). The footage counts toward
        // the EOT strip, which stops an erase loop with the alert.
        if (t.at_bot()) {
            _bot_strip = false;
            _bot_blank = true;
        }
        t.erase_here();
        d->erase_gap();
        return d->at_eot() ? TC_TAPE_STATUS_ALERT : TC_NORMAL;
    }

    // A retry issued at the settled load point has nothing to back over.
    if (mode == 2 && at_load_point()) {
        _xst0_errors |= XST0_NEF;
        _xst3 |= XST3_RIB;
        return TC_FUNCTION_REJECT;
    }

    if (mode == 2) {
        unsigned tc = retry_backspace();
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
        d->rewind();
        _bot_strip = false;
        _bot_blank = false;
        _rewinding = false;
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
        // The answer goes out with the reel still turning, so the message it
        // deposits reports the motion and the operation in progress.
        start_rewind();
        terminate(TC_NORMAL);
        return TC_NORMAL;

    default:                // the decode has already refused any other mode
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

    d->rewind();
    _bot_strip = false;
    _bot_blank = false;
    _rewinding = false;
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
