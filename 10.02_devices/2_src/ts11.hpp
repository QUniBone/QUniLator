/* ts11.hpp: TS11/TSV05 magnetic tape subsystem

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see any device source header for the full text.

   The TS11 presents two words of I/O page and drives everything else through
   packets in host memory: the CPU builds a four-word command packet, writes its
   address into TSDB, and the controller fetches the packet by DMA, executes it,
   and deposits a seven-word message packet carrying the residual count and the
   extended status registers. On the Q-bus the same programming model is the
   TSV05 (M7196 controller with a TS05/TU80 transport), which adds the TSDBX
   byte register for 22-bit command pointers and a tape boot.

   Modelled from:
     EK-OTS11-TM-003  TS11 Technical Manual, chapter 5 (registers, packets)
     EK-TSV05-UG-001  TSV05 User's Guide, section 3.3 (Q-bus programming)

   Registers (base 772520):
     +0  TSBA  read   bus address register, low 16 bits of the DMA address
     +0  TSDB  write  command pointer; a word write starts a command
     +2  TSSR  read   subsystem status
     +2  TSSR  write  subsystem initialize (word or low byte)
     +3  TSDBX write  command-pointer bits 21:18 and the boot bit (TSV05 only)
*/
#ifndef _TS11_HPP_
#define _TS11_HPP_

#include <stdint.h>
#include <vector>

#include "qunibusadapter.hpp"
#include "simh_tape.hpp"
#include "storagecontroller.hpp"

// A free interrupt slot (occupied: SLU 1/2, LTC 3, DZV11 4, KE11/RK11 10,
// RF11/DHV11 12, RL 15, RX 16/17, DEUNA 18, UDA50 20, DELQA 21, KW11P 22,
// TQK50/VCB01 24).
#define TS11_SLOT       26

#define TS11_ADDR       0772520
#define TS11_VECTOR     0224
#define TS11_LEVEL      5

// The largest record the controller transfers: a command packet carries a
// 16-bit byte count, and a count of 0 means 65536 bytes.
#define TS11_MAX_RECORD 65536u

// Capacity of a 2400 foot reel at 1600 bpi PE, the tape length past which the
// transport reports EOT. Settable per drive, so a shorter reel can be modelled.
#define TS05_CAPACITY   (40ull * 1024 * 1024)

// How long a packet or record transfer may take before it is called lost. A
// real transfer is microseconds; this is only there so a bus that has stopped
// answering cannot park the worker for good, which would take the device with
// it when a disable cancels the thread.
#define TS11_DMA_TIMEOUT_MS 2000

// Control microcode revision, reported in XST2<7:0> of the message a write
// characteristics command returns (EK-OTS11-TM-003 table 5-7 note).
#define TS11_MICROCODE_REVISION 5

class ts11_c;

/* The transport: one per controller, a SIMH .tap image through simh_tape_c.
   The reel carries a write-enable ring the operator can take out, and sits
   on-line or off-line as the transport's ON-LINE switch does. */
class ts05_tape_c: public storagedrive_c {
public:
    ts05_tape_c(storagecontroller_c *controller);
    ~ts05_tape_c(void) override;

    // A tape drives the frontend TapeWidget and carries removable media.
    const char *category(void) const override { return "tape"; }
    bool removable(void) const override { return true; }

    bool on_param_changed(parameter_c *param) override;

    // The write-enable ring in the back of the reel. Cleared, the tape is
    // write protected and a write command is refused with WLE.
    parameter_bool_c write_enable_ring = parameter_bool_c(this, "writering", "wr",
                                         /*readonly*/ false,
                                         "Write-enable ring fitted; clear to write protect the reel");

    // The transport's ON-LINE switch. A tape motion command reaches the
    // transport only while it is on-line; OFL in TSSR is its complement.
    parameter_bool_c online_switch = parameter_bool_c(this, "online", "onl",
                                     /*readonly*/ false,
                                     "Transport is on-line and accepts tape motion commands");

    // Front-panel WRITE PROTECT lamp, lit while the mounted reel is write
    // locked. Refreshed each lamp poll so the dashboard tracks it; the ACCESS
    // lamp (storagedrive_c) covers tape motion.
    parameter_bool_c write_protect_lamp = parameter_bool_c(this, "writeprotectlamp", "wpl",
                                          /*readonly*/ true, "State of WRITE PROTECT lamp");
    void refresh_activity(void) override;

    void on_power_changed(signal_edge_enum aclo_edge, signal_edge_enum dclo_edge) override;
    void on_init_changed(void) override;

    // The transport answers commands once a reel is mounted and the ON-LINE
    // switch is set.
    bool is_online(void) { return _tape.is_open() && online_switch.value; }
    bool is_write_locked(void) {
        return !_tape.is_open() || _tape.is_readonly() || !write_enable_ring.value;
    }
    // Tape position has reached the reel's length: the EOT reflective strip.
    bool at_eot(void) {
        return _tape.is_open() && capacity.value != 0 && _tape.position() >= capacity.value;
    }

    // The record layer the controller drives.
    simh_tape_c &tape(void) { return _tape; }

private:
    simh_tape_c _tape;
};

class ts11_c: public storagecontroller_c {
public:
    ts11_c(void);
    ~ts11_c(void) override;

    // TSBA on read / TSDB on write, offset +0
    qunibusdevice_register_t *busreg_TSDB;
    // TSSR on read / subsystem initialize on write, offset +2. A byte write to
    // its high byte loads TSDBX on the TSV05.
    qunibusdevice_register_t *busreg_TSSR;

    // The Extended Features switch on the M7196: 22-bit command pointers and
    // data pointers, the fifth characteristics word, and XST4. Present only on
    // the Q-bus board, which is the only one that carries TSDBX.
    parameter_bool_c extended_features = parameter_bool_c(this, "extfeatures", "ef",
                                         /*readonly*/ false,
                                         "Extended Features switch: 22-bit addressing and XST4");

    bool on_param_changed(parameter_c *param) override;

    void on_after_register_access(qunibusdevice_register_t *device_reg,
                                  uint8_t unibus_control, DATO_ACCESS access) override;
    void on_power_changed(signal_edge_enum aclo_edge, signal_edge_enum dclo_edge) override;
    void on_init_changed(void) override;
    void on_drive_status_changed(storagedrive_c *drive) override;

    void worker(unsigned instance) override;
    void worker_wake(void) override;

private:
    dma_request_c dma_request = dma_request_c(this);
    intr_request_c intr_request = intr_request_c(this);

    // true on the Q-bus M7196, which decodes a byte write to TSSR<15:8> as
    // TSDBX. The Unibus M7982 has no such register and reads any write to the
    // status address as a subsystem initialize.
    bool _has_tsdbx;

    ts05_tape_c *drive(void) { return (ts05_tape_c *) storagedrives[0]; }

    /*** TSSR state ***/
    volatile bool _ssr;             // subsystem ready
    volatile bool _nba;             // need buffer address
    volatile bool _rmr;             // register modification refused
    volatile bool _nxm;             // nonexistent memory
    volatile bool _sce;             // sanity check error (SPE on the TS11)
    volatile uint8_t _fc;           // fatal class
    volatile uint8_t _tc;           // termination class
    volatile uint32_t _tsba;        // 22-bit bus address register
    volatile uint16_t _tsdbx;       // extended data buffer, cleared by TSDB
    volatile bool _maintenance;     // entered by a byte write to TSDB

    /*** characteristics, from the write characteristics command ***/
    uint32_t _msgbuf_addr;
    uint16_t _msgbuf_len;
    bool _ess, _enb, _eai, _eri;
    bool _high_speed;               // transport runs at 100 ips, reported in XST4
    bool _interrupt_enable;         // IE of the last command packet
    bool _msgbuf_owned;             // controller owns the message buffer
    bool _attn_pending;
    unsigned _attn_class;
    bool _last_online;              // to detect the transport's status changes

    /*** per-command status ***/
    uint16_t _xst0_errors;          // TMK/RLS/LET/RLL/WLE/NEF/ILC/ILA
    uint16_t _xst1, _xst2, _xst3, _xst4;
    uint16_t _rbpcr;
    bool _volume_check;
    bool _suppress_message;         // this termination deposits no message packet

    /*** work handed from the register access to the worker ***/
    volatile bool _pending_command;
    volatile bool _pending_boot;
    volatile bool _pending_attention;
    volatile uint32_t _command_pointer;

    // staging for one record between the tape and host memory
    std::vector<uint8_t> _record;

    void reset_subsystem(void);
    void clear_command_status(void);
    void check_transport_status(void);

    uint16_t tssr_value(void);
    void update_tssr(void);
    void update_tssr_and_interrupt(void);

    bool extended_active(void) { return _has_tsdbx && extended_features.value; }

    bool dma_read_words(uint32_t addr, uint16_t *buffer, unsigned wordcount);
    bool dma_write_words(uint32_t addr, const uint16_t *buffer, unsigned wordcount);
    bool dma_fetch_bytes(uint32_t addr, uint8_t *data, uint32_t len, bool swap_bytes);
    bool dma_store_bytes(uint32_t addr, const uint8_t *data, uint32_t len, bool swap_bytes);

    uint16_t build_xst0(void);
    void send_message(unsigned message_type, unsigned class_code, bool ack);
    void terminate(unsigned tc);
    void raise_attention(unsigned class_code);

    void execute_command(void);
    void execute_boot(void);
    bool packet_address(uint16_t low, uint16_t high, uint32_t *addr);

    unsigned command_read(unsigned mode, bool opp, bool swb, uint32_t addr, uint32_t count);
    unsigned command_write_characteristics(uint16_t low, uint16_t high, uint16_t extent);
    unsigned command_write(unsigned mode, bool swb, uint32_t addr, uint32_t count);
    unsigned command_position(unsigned mode, uint32_t count);
    unsigned command_format(unsigned mode);
    unsigned command_control(unsigned mode);

    unsigned transfer_record(uint32_t addr, uint32_t count, bool swb, bool reverse);
    unsigned space_one(bool reverse);
};

#endif /* _TS11_HPP_ */
