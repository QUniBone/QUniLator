/* dhv11.hpp: DHV11/DHQ11 8-line asynchronous serial multiplexer with DMA output

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see any device source header for the full text.

   The DHV11 (Q-bus) / DHQ11 is an 8-line async mux whose transmit path is DMA:
   the host programs a bus address (TBUFFAD1/TBUFFAD2) and a character count
   (TBUFFCT) for a selected channel, and the controller reads the character
   block straight out of PDP-11 memory and shifts it out. Receive is a shared
   FIFO read through RBUF. Each line's bytes are carried by a serial_tcp_line_c.

   IMPORTANT — fidelity note. No DHV11/DHQ11 manual (EK-DHV11-UG) is present in
   this repo, so the register layout and control-bit semantics below are modeled
   from the standard DHV11 register set (the eight word registers CSR, RBUF, LPR,
   STAT, LNCTRL, TBUFFAD1, TBUFFAD2, TBUFFCT) and are NOT yet validated under
   XXDP. The DMA transmit engine and the interrupt logic are structurally real
   (they issue genuine bus DMA and interrupts), but the exact trigger/status bit
   assignments must be reconciled against the manual before the CXY/DHV XXDP
   diagnostics can be expected to pass. Each such assumption is marked TODO.
*/
#ifndef _DHV11_HPP_
#define _DHV11_HPP_

#include <cstdint>
#include <deque>

#include "utils.hpp"
#include "qunibusdevice.hpp"
#include "parameter.hpp"
#include "serial_tcp_line.hpp"

#define DHV_LINE_COUNT 8

// DEC standard address for the first DHV11 (EK-DHV11-TM-002; the CVDHA
// diagnostic's built-in default CSR is 160460 = 0760460 18-bit).
#define DHV11_ADDR   0760460
// Two arbitration slots per mux (RCV at slot, XMT at slot+1); the two-instance
// pool spans slot..slot+3. Base 12 sits just above the DZV11 pool (4..11) and
// clear of uda (20) and delqa (21) on level 5 — a shared slot loses interrupts.
#define DHV11_SLOT   12
#define DHV11_VECTOR 0300   // RCV +0, XMT +4
#define DHV11_LEVEL  05

// Default TCP listen port base: line i listens on DHV11_TCP_PORT_BASE + i, so an
// enabled device accepts a client on a distinct port without per-line setup.
#define DHV11_TCP_PORT_BASE 4100

enum dhv_reg_index {
	dhv_idx_csr = 0,   // CSR      control/status
	dhv_idx_rbuf,      // RBUF     receive FIFO (read)
	dhv_idx_lpr,       // LPR      line parameter (write)
	dhv_idx_stat,      // STAT     line status (read)
	dhv_idx_lnctrl,    // LNCTRL   line control
	dhv_idx_tbuffad1,  // TBUFFAD1 transmit DMA address low
	dhv_idx_tbuffad2,  // TBUFFAD2 transmit DMA address high
	dhv_idx_tbuffct,   // TBUFFCT  transmit DMA character count
	dhv_idx_count
};

// CSR bits (EK-DHV11-TM-002 §3.2.2.1). <3:0> select the channel for indexed (M)
// registers; only 0..7 are used, so bit 3 is always 0.
#define DHV_CSR_CHAN_MASK    0000017 // <3:0> indirect address register (channel select)
#define DHV_CSR_MASTER_RESET 0000040 // 5  master reset (self-clearing after self-test)
#define DHV_CSR_RX_IE        0000100 // 6  receive interrupt enable
#define DHV_CSR_RX_DATA      0000200 // 7  received data available (RO)
#define DHV_CSR_TX_LINE_MASK 0007400 // <11:8> transmit line number (RO)
#define DHV_CSR_TX_LINE_SHIFT 8
#define DHV_CSR_TX_DMA_ERR   0010000 // 12 transmit DMA error (RO)
#define DHV_CSR_DIAG_FAIL    0020000 // 13 diagnostic fail (RO)
#define DHV_CSR_TX_IE        0040000 // 14 transmit interrupt enable
#define DHV_CSR_TX_ACT       0100000 // 15 transmitter action (RO, cleared when CSR is read)
// Host-writable CSR bits: channel <3:0>, master reset <5>, RXIE <6>, TXIE <14>.
#define DHV_CSR_WMASK        0040157

// RBUF bits (§3.2.2.2). A read of base+2 pops the receive FIFO.
#define DHV_RBUF_DATA_VALID  0100000 // 15 data valid (FIFO not empty)
#define DHV_RBUF_OVERRUN     0040000 // 14 overrun error
#define DHV_RBUF_FRAME       0020000 // 13 framing error
#define DHV_RBUF_PARITY      0010000 // 12 parity error
#define DHV_RBUF_DIAG        0070000 // <14:12>=111 marks a diagnostic/modem-status code
#define DHV_RBUF_LINE_SHIFT  8       // <11:8> receive line number (or diag sequence)

// TXCHAR (write base+2) bits (§3.2.2.3): single-character programmed transmit.
#define DHV_TXCHAR_VALID     0100000 // 15 transmit data valid

// LNCTRL bits (§3.2.2.6): per selected channel.
#define DHV_LNCTRL_DMA_ABORT 0000001 // 0  transmit DMA abort
#define DHV_LNCTRL_IAUTO     0000002 // 1  incoming auto-flow
#define DHV_LNCTRL_RX_ENABLE 0000004 // 2  receiver enable
#define DHV_LNCTRL_BREAK     0000010 // 3  break
#define DHV_LNCTRL_OAUTO     0000020 // 4  outgoing auto-flow
#define DHV_LNCTRL_FORCE_XOFF 0000040 // 5 force X-OFF
#define DHV_LNCTRL_MAINT_MASK 0000300 // <7:6> maintenance mode
#define DHV_LNCTRL_MAINT_SHIFT 6
#define DHV_LNCTRL_LINK_TYPE 0000400 // 8  link type (modem)
#define DHV_LNCTRL_DTR       0001000 // 9  data terminal ready
#define DHV_LNCTRL_RTS       0010000 // 12 request to send

// TBUFFAD2 bits (§3.2.2.8): DMA address high, transmit enable and DMA start.
#define DHV_TBUFFAD2_ADDR_HI 0000077 // <5:0> bus address bits 21..16
#define DHV_TBUFFAD2_DMA_START 0000200 // 7  transmit DMA start (self-clearing)
#define DHV_TBUFFAD2_TX_ENA  0100000 // 15 transmitter enable (set by master reset)

// STAT bits (§3.2.2.5): modem status in the high byte. Bit 8 reads 0 on a DHV11
// (1 would mean a DHU11); it lets software tell the two apart.
#define DHV_STAT_DHU11       0000400 // 8  0 = DHV11, 1 = DHU11
#define DHV_STAT_CTS         0004000 // 11 clear to send
#define DHV_STAT_DCD         0010000 // 12 data carrier detected
#define DHV_STAT_RI          0020000 // 13 ring indicator
#define DHV_STAT_DSR         0100000 // 15 data set ready

// Maintenance modes (LNCTRL <7:6>).
#define DHV_MAINT_NORMAL 0
#define DHV_MAINT_ECHO   1  // received data retransmitted and also placed in FIFO
#define DHV_MAINT_LOCAL  2  // DUART output looped to input; RX.ENA ignored
#define DHV_MAINT_REMOTE 3  // received data retransmitted, not placed in FIFO

// LPR reset default (§3.3.1): 9600 baud TX and RX (<15:12>=<11:8>=1101), 8 data
// bits (<4:3>=11), one stop bit, parity disabled.
#define DHV_LPR_RESET 0156430

// Self-test diagnostic codes (§3.3.10). After a healthy reset the FIFO holds six
// null codes and two ROM-version codes. A code word carries DATA.VALID, the
// diagnostic marker <14:12>=111, the sequence number in <11:8>, and the status
// byte in <7:0>. A null code is 201(8); a ROM-version byte has D7=0, D0=1, the
// version in D6..D2 and the processor number (0/1) in D1.
#define DHV_SELFTEST_NULL    0201 // filler after a normal (run) self-test
#define DHV_SELFTEST_SKIP    0203 // filler after a skipped self-test (§3.3.10.3)
#define DHV_SELFTEST_ROM1    0001 // ROM version 0, PROC1 (D7=0, D6..D2=0, D1=0, D0=1)
#define DHV_SELFTEST_ROM2    0003 // ROM version 0, PROC2 (D1=1, D0=1)
// The skip-self-test pattern the host writes throughout the control registers to
// bypass the ~2.5 s self-test (§3.3.10.3).
#define DHV_SELFTEST_SKIP_PATTERN 0052525

// Master reset holds CSR<5> set while the self-test runs, then clears it (§3.3.1:
// the self-test takes up to ~2.5 s; a requested skip completes quickly). The
// hold is timed on the worker, which ticks about every 500 us.
//
// NOTE — a QBone limitation, not a fidelity gap. CVDHA times the reset by
// counting how many times the host polls the CSR, a count calibrated to a real
// DHV11's register-access speed. Every register access here crosses the
// ARM-in-the-loop QBus and is far slower, so the diagnostic's self-test-timing
// sub-tests (its Test 2 onward) cannot all be satisfied: a hold long enough to
// clear one check's lower iteration bound overruns another's upper bound. The
// hold below is set from the manual's self-test duration, not reverse-engineered
// from those windows. Detection and the register-access test (which do not
// depend on this timing) still validate the register model.
#define DHV_SELFTEST_TICKS 3000 // ~1.5 s, within the manual's self-test range
#define DHV_SKIP_TICKS     40   // ~20 ms once a skip is requested (§3.3.10.3)

class dhv11_c: public qunibusdevice_c {
public:
	const char *category(void) const override { return "serial"; }

	dhv11_c();
	~dhv11_c();

	serial_tcp_line_c tcp_line[DHV_LINE_COUNT];

	parameter_string_c   tcp_role[DHV_LINE_COUNT] = {
		parameter_string_c(this, "tcp_role0", "r0", false, "line 0 TCP role: \"\"/listen/connect"),
		parameter_string_c(this, "tcp_role1", "r1", false, "line 1 TCP role: \"\"/listen/connect"),
		parameter_string_c(this, "tcp_role2", "r2", false, "line 2 TCP role: \"\"/listen/connect"),
		parameter_string_c(this, "tcp_role3", "r3", false, "line 3 TCP role: \"\"/listen/connect"),
		parameter_string_c(this, "tcp_role4", "r4", false, "line 4 TCP role: \"\"/listen/connect"),
		parameter_string_c(this, "tcp_role5", "r5", false, "line 5 TCP role: \"\"/listen/connect"),
		parameter_string_c(this, "tcp_role6", "r6", false, "line 6 TCP role: \"\"/listen/connect"),
		parameter_string_c(this, "tcp_role7", "r7", false, "line 7 TCP role: \"\"/listen/connect"),
	};
	parameter_string_c   tcp_host[DHV_LINE_COUNT] = {
		parameter_string_c(this, "tcp_host0", "h0", false, "line 0 connect-out host"),
		parameter_string_c(this, "tcp_host1", "h1", false, "line 1 connect-out host"),
		parameter_string_c(this, "tcp_host2", "h2", false, "line 2 connect-out host"),
		parameter_string_c(this, "tcp_host3", "h3", false, "line 3 connect-out host"),
		parameter_string_c(this, "tcp_host4", "h4", false, "line 4 connect-out host"),
		parameter_string_c(this, "tcp_host5", "h5", false, "line 5 connect-out host"),
		parameter_string_c(this, "tcp_host6", "h6", false, "line 6 connect-out host"),
		parameter_string_c(this, "tcp_host7", "h7", false, "line 7 connect-out host"),
	};
	parameter_unsigned_c tcp_port[DHV_LINE_COUNT] = {
		parameter_unsigned_c(this, "tcp_port0", "p0", false, "", "%d", "line 0 TCP port", 16, 10),
		parameter_unsigned_c(this, "tcp_port1", "p1", false, "", "%d", "line 1 TCP port", 16, 10),
		parameter_unsigned_c(this, "tcp_port2", "p2", false, "", "%d", "line 2 TCP port", 16, 10),
		parameter_unsigned_c(this, "tcp_port3", "p3", false, "", "%d", "line 3 TCP port", 16, 10),
		parameter_unsigned_c(this, "tcp_port4", "p4", false, "", "%d", "line 4 TCP port", 16, 10),
		parameter_unsigned_c(this, "tcp_port5", "p5", false, "", "%d", "line 5 TCP port", 16, 10),
		parameter_unsigned_c(this, "tcp_port6", "p6", false, "", "%d", "line 6 TCP port", 16, 10),
		parameter_unsigned_c(this, "tcp_port7", "p7", false, "", "%d", "line 7 TCP port", 16, 10),
	};

	void reset(void);

	bool on_before_install(void) override;
	void on_after_uninstall(void) override;

	void worker(unsigned instance) override;
	void worker_wake(void) override;

	void on_after_register_access(qunibusdevice_register_t *device_reg,
			uint8_t unibus_control, DATO_ACCESS access) override;
	bool on_param_changed(parameter_c *param) override;
	void on_power_changed(signal_edge_enum aclo_edge, signal_edge_enum dclo_edge) override;
	void on_init_changed(void) override;

private:
	qunibusdevice_register_t *reg[dhv_idx_count];

	intr_request_c rcvintr_request = intr_request_c(this);
	intr_request_c xmtintr_request = intr_request_c(this);
	dma_request_c  dma_request = dma_request_c(this);

	pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
	pthread_cond_t  xmt_cond = PTHREAD_COND_INITIALIZER;

	// CSR state
	bool csr_rx_ie;
	bool csr_tx_ie;
	bool csr_tx_act;
	bool csr_master_reset; // <5> set until the self-test completes
	bool csr_diag_fail;    // <13> self-test detected a fault
	uint8_t csr_tx_line;   // <11:8> line reported in the last TX.ACTION
	uint8_t csr_channel;   // <3:0> selected channel for indexed registers
	bool selftest_pending; // a master reset is running the self-test
	bool selftest_skip;    // the host wrote the skip-self-test pattern this reset
	int selftest_ticks;    // worker ticks remaining until the self-test completes

	// per-channel line state. The raw last-written word of each read/write indexed
	// register is kept so the register-access test reads back what it wrote; the
	// transmit-DMA engine decodes the address/count/enable from them.
	struct tx_channel_t {
		bool open;         // TCP transport running
		uint16_t lpr;      // LPR: line parameters
		uint16_t lnctrl;   // LNCTRL: RX enable, maintenance mode, modem control
		uint16_t tbuffad1; // TBUFFAD1: DMA address bits 15..0
		uint16_t tbuffad2; // TBUFFAD2: DMA address bits 21..16, TX.ENA, DMA start
		uint16_t tbuffct;  // TBUFFCT: DMA character count
		uint32_t dma_addr; // 22-bit bus address latched for the running transfer
		uint16_t dma_count;// characters remaining to DMA
		bool dma_pending;  // the worker still has to run the requested DMA
	};
	tx_channel_t chan[DHV_LINE_COUNT];

	bool chan_rx_enabled(unsigned c) const { return chan[c].lnctrl & DHV_LNCTRL_RX_ENABLE; }
	uint8_t chan_maint(unsigned c) const {
		return (chan[c].lnctrl & DHV_LNCTRL_MAINT_MASK) >> DHV_LNCTRL_MAINT_SHIFT;
	}
	bool chan_tx_ena(unsigned c) const { return chan[c].tbuffad2 & DHV_TBUFFAD2_TX_ENA; }
	void set_channel_defaults(unsigned c); // caller holds state_mutex
	void refresh_indexed_dati(unsigned c); // caller holds state_mutex

	// receive FIFO of RBUF words
	std::deque<uint16_t> rx_fifo;

	bool get_rcv_intr_level(void);
	bool get_xmt_intr_level(void);
	void update_csr_and_INTR(void); // caller holds state_mutex
	void set_rbuf_dati(void);
	void set_stat_dati(void);
	void load_selftest_codes(void); // caller holds state_mutex
	void complete_selftest(void);   // caller holds state_mutex: finish a master reset
	void rx_fifo_push(uint16_t entry); // caller holds state_mutex
	void tx_report(unsigned line);  // caller holds state_mutex: raise TX.ACTION

	void open_lines(void);
	void close_lines(void);
	// (re)configure and (re)open one line's TCP transport with the given role/
	// host/port. Closes any transport already running on the line first; an empty
	// role leaves it closed. Used both at install and for a live tcp_* edit.
	void open_line(unsigned i, const std::string &role, const std::string &host,
			uint16_t port);

	bool dma_read_words(uint32_t addr, uint16_t *buffer, unsigned wordcount);

	void worker_rcv(void);
	void worker_xmt(void);
};

#endif // _DHV11_HPP_
