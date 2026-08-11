/* delqa.hpp: DELQA (M7516) Qbus Ethernet controller

   Copyright (c) 2026, Hans Huebner

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
   THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
   IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
   CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

   Emulation of the DEC DELQA Ethernet adapter, with an optional DEQNA-lock
   mode for operating systems that only support the DEQNA register model
   (RSTS/E, older RSX releases).  Register semantics follow the DELQA
   User's Guide (EK-DELQA-UG); the SIMH pdp11_xq module served as the
   reference for many behavioral details.

   The controller is a standard 8-word Qbus device:
     base+0..base+12  station address PROM (read) / BDL start addresses (write)
     base+14          Vector Address Register (VAR)
     base+16          Control and Status Register (CSR)

   Packet transport to the host Ethernet ("shared eth0") is handled by a
   separate bridge object; this class implements the bus-visible register
   file, buffer descriptor list processing and interrupt generation.
*/
#ifndef _DELQA_HPP_
#define _DELQA_HPP_

#include <stdint.h>
#include <pthread.h>
#include <string>
#include <vector>
#include <deque>

#include "parameter.hpp"
#include "qunibusdevice.hpp"
#include "ether_bridge.hpp"

class delqa_c: public qunibusdevice_c {
public:
    delqa_c();
    virtual ~delqa_c();

    const char *category(void) const override { return "network"; }

    bool on_param_changed(parameter_c *param) override;

    void worker(unsigned instance) override;

    void on_after_register_access(qunibusdevice_register_t *device_reg,
            uint8_t unibus_control, DATO_ACCESS access) override;

    void on_power_changed(signal_edge_enum aclo_edge, signal_edge_enum dclo_edge) override;
    void on_init_changed(void) override;
    void refresh_activity(void) override;

    // host network interface the packet bridge attaches to
    parameter_string_c interface = parameter_string_c(this, "interface", "if", false,
            "host Ethernet interface shared with the emulated controller");

    // station address, stored in the address PROM
    parameter_string_c mac = parameter_string_c(this, "mac", "mac", false,
            "station address, 6 hex bytes separated by \":\" or \"-\"");

    // configuration switch: present the DEQNA register model to the host
    // Index into the shared activity LED bank. Drives claim indexes by unit
    // number, so 3 keeps the controller clear of a single-drive system.
    parameter_unsigned_c activity_led = parameter_unsigned_c(this, "activityled", "al", /*readonly*/
                                        false, "", "%d", "Number of LED to used for activity display.", 8, 10);

    // Lit by traffic in either direction, held on past the frame so a
    // periodic sampler sees it.
    parameter_bool_c activity_lamp = parameter_bool_c(this, "activitylamp", "actl", /*readonly*/
                                     true, "State of activity lamp");

    parameter_bool_c deqna_lock = parameter_bool_c(this, "deqna_lock", "dl", false,
            "DEQNA-lock mode: emulate a DEQNA (for RSTS/E and other DEQNA-only drivers)");

    // A loopback connector plugged into the transceiver port, as DEC's
    // diagnostics ask for: what the controller transmits comes straight back
    // to its own receiver, the network is disconnected, and the returning
    // signal shows as carrier.
    parameter_bool_c loopback_connector = parameter_bool_c(this, "loopback", "lb", false,
            "loopback connector installed on the transceiver port");

    dma_request_c dma_request{this};
    intr_request_c intr_request{this};

private:
    // --- CSR bits (base+16) ---
    static const uint16_t CSR_RI = 0x8000; // receive interrupt request [W1C]
    static const uint16_t CSR_PE = 0x4000; // parity error in host memory [RO]
    static const uint16_t CSR_CA = 0x2000; // carrier from receiver enabled [RO]
    static const uint16_t CSR_OK = 0x1000; // Ethernet transceiver power ok [RO]
    static const uint16_t CSR_SE = 0x0400; // sanity timer enable [RW]
    static const uint16_t CSR_EL = 0x0200; // external loopback [RW]
    static const uint16_t CSR_IL = 0x0100; // internal loopback (0 = loop) [RW]
    static const uint16_t CSR_XI = 0x0080; // transmit interrupt request [W1C]
    static const uint16_t CSR_IE = 0x0040; // interrupt enable [RW]
    static const uint16_t CSR_RL = 0x0020; // receive list invalid/empty [RO]
    static const uint16_t CSR_XL = 0x0010; // transmit list invalid/empty [RO]
    static const uint16_t CSR_BD = 0x0008; // boot/diagnostic ROM load [RW]
    static const uint16_t CSR_NI = 0x0004; // nonexistent memory timeout [RO]
    static const uint16_t CSR_SR = 0x0002; // software reset [RW]
    static const uint16_t CSR_RE = 0x0001; // receiver enable [RW]

    static const uint16_t CSR_RW_MASK = CSR_SE | CSR_EL | CSR_IL | CSR_IE | CSR_BD
            | CSR_SR | CSR_RE;      // 0x074B
    static const uint16_t CSR_W1_MASK = CSR_RI | CSR_XI;   // write 1 to clear
    static const uint16_t CSR_XIRI = CSR_RI | CSR_XI;      // interrupt requests
    static const uint16_t CSR_BOOT = CSR_EL | CSR_BD;      // boot/diagnostic ROM request

    // --- VAR bits (base+14) ---
    static const uint16_t VAR_MS = 0x8000; // mode select: 1 = DELQA normal [RW]
    static const uint16_t VAR_OS = 0x4000; // option switch setting [RO]
    static const uint16_t VAR_RS = 0x2000; // request self-test [RW]
    static const uint16_t VAR_S3 = 0x1000; // self-test status [RO]
    static const uint16_t VAR_S2 = 0x0800; // self-test status [RO]
    static const uint16_t VAR_S1 = 0x0400; // self-test status [RO]
    static const uint16_t VAR_ST = VAR_S3 | VAR_S2 | VAR_S1;
    static const uint16_t VAR_IV = 0x03FC; // interrupt vector [RW]
    static const uint16_t VAR_ID = 0x0001; // identity test bit: 1 = DELQA [RW]

    static const uint16_t VAR_RW_MASK = VAR_MS | VAR_RS | VAR_IV | VAR_ID; // 0xA3FD

    // --- buffer descriptor bits ---
    static const uint16_t DSC_V = 0x8000; // descriptor valid
    static const uint16_t DSC_C = 0x4000; // chain to address in this descriptor
    static const uint16_t DSC_E = 0x2000; // end of message [transmit]
    static const uint16_t DSC_S = 0x1000; // setup packet [transmit]
    static const uint16_t DSC_L = 0x0080; // low byte termination [odd length]
    static const uint16_t DSC_H = 0x0040; // high byte start [odd address]

    // --- transmit status word 1 (EK-DELQA-UG-002 section 3.4.3.5) ---
    // 15:14 pair, as for a receive: 10 initialized by the host, 11 used but
    // not the last segment, 00 last segment sent without errors, 01 last
    // segment sent with errors.  Then 12 Loss of carrier, 10 STE
    // (DEQNA-lock only), 09 Abort on excessive collisions, 07:04 Count of
    // collisions, and the rest reserved.  Status word 2 carries the TDR
    // count in 09:00 and reserves the rest.
    static const uint16_t XMT_LASTNOT = 0xC000; // used, not the last segment
    static const uint16_t XMT_LASTERR = 0x4000; // used, last segment, errors
    static const uint16_t XMT_LOSS    = 0x1000; // carrier lost while sending
    static const uint16_t XMT_ABORT   = 0x0200; // aborted, excessive collisions

    // --- receive status word 1 ---
    static const uint16_t RST_LASTNOT  = 0xC000; // used, not last segment
    static const uint16_t RST_LASTERR  = 0x4000; // used, last segment, errors
    static const uint16_t RST_ESETUP   = 0x2000; // setup or loopback packet
    static const uint16_t RST_RUNT     = 0x4800; // runt packet
    static const uint16_t RST_OVERFLOW = 0x0001; // receiver overflow, packets lost

    // Ethernet frame limits
    static const unsigned ETH_MIN_PACKET = 60;
    static const unsigned ETH_MAX_PACKET = 1518;
    static const unsigned MAX_RCV_PACKET = 1600;
    static const unsigned LONG_PACKET = 0x0600;  // loopback length error limit

    static const unsigned FILTER_MAX = 14; // MAC address filter slots

    // How far a buffer descriptor list is walked before the walk is abandoned.
    // A list that chains back on itself is walked forever otherwise; comparing
    // against the address the walk started from catches a chain closing on the
    // head, and this bounds the shapes that close anywhere else. Far above any
    // list a driver builds: a maximum-length packet needs a handful of
    // segments, and a receive ring holds tens of buffers.
    static const unsigned MAX_DESCRIPTORS = 256;

    // a packet waiting for delivery through the receive BDL
    struct packet_c {
        enum type_e {
            SETUP,    // reflected setup packet
            LOOPBACK, // internally or externally looped transmit packet
            NORMAL,   // packet received from the wire
            BOOTROM   // on-board boot/diagnostic ROM image
        } type;
        std::vector<uint8_t> data;
        unsigned used; // bytes already delivered into receive buffers
    };

    // register shortcuts; PROM bytes occupy registers 0..5
    qunibusdevice_register_t *VAR_reg;
    qunibusdevice_register_t *CSR_reg;

    // controller state, guarded by on_after_register_access_mutex
    uint16_t csr;
    uint16_t var;
    bool deqna_mode;      // presenting the DEQNA register model right now
    bool interrupt_asserted;

    uint8_t station_address[6];
    uint8_t prom_checksum[2];

    // buffer descriptor list start addresses, as written by the host
    uint16_t rbdl[2];
    uint16_t xbdl[2];

    // packets waiting for receive buffers; guarded by the state mutex
    std::deque<packet_c> read_queue;
    unsigned read_queue_loss;
    static const unsigned READ_QUEUE_MAX = 32;

    // address filter, loaded by setup packets; guarded by the state mutex
    struct {
        bool valid;
        uint8_t macs[FILTER_MAX][6];
        bool promiscuous;
        bool all_multicast;
        unsigned sanity_quarter_secs;
    } setup;

    // work items for the worker thread
    bool pending_rbdl;    // host wrote a new receive BDL address
    bool pending_xbdl;    // host wrote a new transmit BDL address
    bool pending_deliver; // packets queued for an already-dispatched list
    bool pending_bootrom; // host requested the boot/diagnostic ROM load
    bool pending_wakeup;  // condition variable predicate

    // BDL walk state, owned by the worker thread
    uint32_t rbdl_ba;     // current receive descriptor address
    uint32_t xbdl_ba;     // current transmit descriptor address
    std::vector<uint8_t> write_buffer; // transmit packet assembly

    // Carrier is a passing signal, not a level: it comes on when a frame
    // reaches the transceiver port and drops once the frame is through.
    // A maximum-length frame occupies 10Mbit Ethernet for about 1.2ms, and
    // the diagnostic watches for both edges around a transmission.
    static const unsigned CARRIER_MS = 2;
    uint64_t carrier_off_ms;           // 0: no carrier on the port

    bool parse_mac(const std::string &text);
    void make_prom_checksum(void);
    void update_prom_registers(void);
    void update_csr(uint16_t set_bits, uint16_t clear_bits);
    void set_interrupt(void);
    void clear_interrupt(void);
    void write_csr(uint16_t data);
    void write_var(uint16_t data);
    void software_reset(void);
    void power_on_reset(void);
    void wakeup_worker(void);

    // DMA engine; worker thread only, called without the state mutex
    bool dma_read_words(uint32_t addr, uint16_t *buffer, unsigned wordcount);
    bool dma_write_words(uint32_t addr, const uint16_t *buffer, unsigned wordcount);
    bool dma_read_bytes(uint32_t addr, uint8_t *buffer, unsigned len);
    bool dma_write_bytes(uint32_t addr, const uint8_t *buffer, unsigned len);
    void nxm_error(void);

    // BDL processing; worker thread only
    void dispatch_rbdl(void);
    void process_rbdl(void);
    void process_xbdl(void);
    void process_setup(uint16_t descriptor_count);
    void process_bootrom(void);
    void enqueue_packet(enum packet_c::type_e type, const std::vector<uint8_t> &data);

    // eth0 bridge: an AF_PACKET raw socket in promiscuous mode shares the
    // host interface.  Runs on worker instance 1.
    ether_bridge_c bridge = ether_bridge_c("delqa");
    void bridge_worker(void);
    // Pulse the activity LED and lamp; called for traffic in either direction.
    void note_activity(void);
    uint64_t activity_lamp_until_ms = 0; // activity_lamp stays lit until then

    bool bridge_accept(const uint8_t *frame, unsigned len);
    const uint8_t *active_physical_address(void);
    bool address_recognised(const uint8_t *dst);
    void bridge_receive(const uint8_t *frame, unsigned len);
    bool transmit_packet(const std::vector<uint8_t> &data);
};

#endif // _DELQA_HPP_
