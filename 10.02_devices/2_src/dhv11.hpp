/* dhv11.hpp: DHV11/DHQ11 8-line asynchronous serial multiplexer with DMA output

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see any device source header for the full text.

   The DHV11 (Q-bus) / DHQ11 is an 8-line async mux whose transmit path is DMA:
   the host programs a bus address (TBUFFAD1/TBUFFAD2) and a character count
   (TBUFFCT) for a selected channel, and the controller reads the character
   block straight out of PDP-11 memory and shifts it out. Receive is a shared
   FIFO read through RBUF. Each line's bytes are carried by a serial_tcp_line_c.

   The register layout and control-bit semantics follow EK-DHV11-TM-002 (the
   eight word registers CSR, RBUF, LPR, STAT, LNCTRL, TBUFFAD1, TBUFFAD2,
   TBUFFCT). The model passes DEC's CVDHA (VDHAE0) "DHV11-M FUNC TST PART 1"
   diagnostic under XXDP with no errors — register access, the master-reset
   self-test timing (see the self-test note below), the ROM-version self-test
   codes, TX.ENA gating, the level-4 interrupt, and the Background Monitor
   Program report. The DMA transmit engine and the interrupt logic issue genuine
   bus DMA and interrupts.
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
// clear of uda (20) and delqa (21) — a shared slot loses interrupts.
#define DHV11_SLOT   12
#define DHV11_VECTOR 0300   // RCV +0, XMT +4
// The DHV11 is a Level 4 interrupt device (EK-DHV11-TM-002: "the DHV11 is a Level
// 4 device"); CVDHA's interrupt-BR-level test checks the request line it asserts.
#define DHV11_LEVEL  04

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

// LPR DIAG field (<2:1>): writing 01 requests a Background Monitor Program report
// on the selected channel (§3.3.10.4). The DHV11 pushes a BMP status code to the
// FIFO and clears DIAG when the check completes.
#define DHV_LPR_DIAG_MASK    0000006 // <2:1> DIAG field
#define DHV_LPR_DIAG_REQUEST 0000002 // <2:1> = 01: request a BMP report
#define DHV_BMP_RUNNING      0305    // BMP code: DHV11 running (307 = defective)

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
// the self-test takes up to ~2.5 s; a requested skip completes quickly).
//
// The completion is measured two ways, so both a polling diagnostic and a
// timer-driven OS driver see correct behaviour:
//
//   * Poll-count model (the diagnostic path). CVDHA does not time the reset by
//     wall clock — it counts how many times its poll helper reads the CSR before
//     CSR<5> reads 0, and requires that count to fall inside a window (Test 2/14
//     want the bit held for >= 500 and < 3000 of its outer poll iterations; the
//     skip tests want a skipped reset cleared between 10 and 14 iterations, with
//     DIAG.FAIL dropping alongside CSR<5>). Each outer iteration is a fixed
//     number of CSR reads (its inner constant, calibrated on the host CPU
//     against the line clock). We count the CSR reads the host
//     issues during the hold and clear CSR<5> after DHV_SELFTEST_POLLS (or
//     DHV_SKIP_POLLS for a requested skip), placing the diagnostic's outer-
//     iteration count squarely inside its window. Counting the same reads the
//     diagnostic counts makes this independent of QBus speed — a wall-clock hold
//     cannot, because the poll helper's reads cross the slow ARM-in-the-loop
//     QBus while its iteration constant is calibrated on fast CPU/memory loops.
//
//   * Inactivity fallback (the non-polling-host path). When the host is not
//     actively polling the CSR, CSR<5> instead clears after a wall-clock hold —
//     ~2.5 s for a full self-test, ~20 ms once a skip is requested (§3.3.10.3).
//     The fallback countdown is frozen while the host is actively polling, so it
//     never cuts the poll-count model short.
//
// DHV_CVDHA_POLLS_PER_ITER is CVDHA's inner poll constant on this 11/73 (its
// @#2376 cell). The two poll counts are placed near the centre of each window in
// units of that constant.
#define DHV_CVDHA_POLLS_PER_ITER 220u
#define DHV_SELFTEST_POLLS (800u * DHV_CVDHA_POLLS_PER_ITER) // ~800 iters: window [500,3000)
#define DHV_SKIP_POLLS     (12u  * DHV_CVDHA_POLLS_PER_ITER) // ~12 iters:  window [10,14]

#define DHV_SELFTEST_FALLBACK_TICKS 5000 // ~2.5 s, the manual's self-test ceiling
#define DHV_SKIP_FALLBACK_TICKS     40   // ~20 ms once a skip is requested (§3.3.10.3)
// A CSR read marks the host "actively polling" for this many worker ticks (~2 ms),
// which freezes the inactivity fallback while a diagnostic poll loop is running.
#define DHV_POLL_ACTIVE_TICKS 4

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

	// Per-line signal lamps for the dashboard, one set per line. The DHV11
	// carries full modem control, so its panel shows more than the DZV11's:
	// rx/tx pulse with traffic (held briefly by refresh_activity), dtr and rts
	// follow the LNCTRL bits the guest drives, and cd/dsr/cts follow carrier —
	// a connected TCP client, which the STAT register reports on all three.
	// Nothing rings RI, so it has no lamp. Names end in "lamp" so webevents'
	// lamp poll picks up the direct value assignments the device threads make.
	parameter_bool_c rx_lamp[DHV_LINE_COUNT] = {
		parameter_bool_c(this, "rx0lamp", "rxl0", true, "line 0 receive activity"),
		parameter_bool_c(this, "rx1lamp", "rxl1", true, "line 1 receive activity"),
		parameter_bool_c(this, "rx2lamp", "rxl2", true, "line 2 receive activity"),
		parameter_bool_c(this, "rx3lamp", "rxl3", true, "line 3 receive activity"),
		parameter_bool_c(this, "rx4lamp", "rxl4", true, "line 4 receive activity"),
		parameter_bool_c(this, "rx5lamp", "rxl5", true, "line 5 receive activity"),
		parameter_bool_c(this, "rx6lamp", "rxl6", true, "line 6 receive activity"),
		parameter_bool_c(this, "rx7lamp", "rxl7", true, "line 7 receive activity"),
	};
	parameter_bool_c tx_lamp[DHV_LINE_COUNT] = {
		parameter_bool_c(this, "tx0lamp", "txl0", true, "line 0 transmit activity"),
		parameter_bool_c(this, "tx1lamp", "txl1", true, "line 1 transmit activity"),
		parameter_bool_c(this, "tx2lamp", "txl2", true, "line 2 transmit activity"),
		parameter_bool_c(this, "tx3lamp", "txl3", true, "line 3 transmit activity"),
		parameter_bool_c(this, "tx4lamp", "txl4", true, "line 4 transmit activity"),
		parameter_bool_c(this, "tx5lamp", "txl5", true, "line 5 transmit activity"),
		parameter_bool_c(this, "tx6lamp", "txl6", true, "line 6 transmit activity"),
		parameter_bool_c(this, "tx7lamp", "txl7", true, "line 7 transmit activity"),
	};
	parameter_bool_c dtr_lamp[DHV_LINE_COUNT] = {
		parameter_bool_c(this, "dtr0lamp", "dtl0", true, "line 0 data terminal ready"),
		parameter_bool_c(this, "dtr1lamp", "dtl1", true, "line 1 data terminal ready"),
		parameter_bool_c(this, "dtr2lamp", "dtl2", true, "line 2 data terminal ready"),
		parameter_bool_c(this, "dtr3lamp", "dtl3", true, "line 3 data terminal ready"),
		parameter_bool_c(this, "dtr4lamp", "dtl4", true, "line 4 data terminal ready"),
		parameter_bool_c(this, "dtr5lamp", "dtl5", true, "line 5 data terminal ready"),
		parameter_bool_c(this, "dtr6lamp", "dtl6", true, "line 6 data terminal ready"),
		parameter_bool_c(this, "dtr7lamp", "dtl7", true, "line 7 data terminal ready"),
	};
	parameter_bool_c rts_lamp[DHV_LINE_COUNT] = {
		parameter_bool_c(this, "rts0lamp", "rtl0", true, "line 0 request to send"),
		parameter_bool_c(this, "rts1lamp", "rtl1", true, "line 1 request to send"),
		parameter_bool_c(this, "rts2lamp", "rtl2", true, "line 2 request to send"),
		parameter_bool_c(this, "rts3lamp", "rtl3", true, "line 3 request to send"),
		parameter_bool_c(this, "rts4lamp", "rtl4", true, "line 4 request to send"),
		parameter_bool_c(this, "rts5lamp", "rtl5", true, "line 5 request to send"),
		parameter_bool_c(this, "rts6lamp", "rtl6", true, "line 6 request to send"),
		parameter_bool_c(this, "rts7lamp", "rtl7", true, "line 7 request to send"),
	};
	parameter_bool_c cd_lamp[DHV_LINE_COUNT] = {
		parameter_bool_c(this, "cd0lamp", "cdl0", true, "line 0 carrier detect"),
		parameter_bool_c(this, "cd1lamp", "cdl1", true, "line 1 carrier detect"),
		parameter_bool_c(this, "cd2lamp", "cdl2", true, "line 2 carrier detect"),
		parameter_bool_c(this, "cd3lamp", "cdl3", true, "line 3 carrier detect"),
		parameter_bool_c(this, "cd4lamp", "cdl4", true, "line 4 carrier detect"),
		parameter_bool_c(this, "cd5lamp", "cdl5", true, "line 5 carrier detect"),
		parameter_bool_c(this, "cd6lamp", "cdl6", true, "line 6 carrier detect"),
		parameter_bool_c(this, "cd7lamp", "cdl7", true, "line 7 carrier detect"),
	};
	parameter_bool_c dsr_lamp[DHV_LINE_COUNT] = {
		parameter_bool_c(this, "dsr0lamp", "dsl0", true, "line 0 data set ready"),
		parameter_bool_c(this, "dsr1lamp", "dsl1", true, "line 1 data set ready"),
		parameter_bool_c(this, "dsr2lamp", "dsl2", true, "line 2 data set ready"),
		parameter_bool_c(this, "dsr3lamp", "dsl3", true, "line 3 data set ready"),
		parameter_bool_c(this, "dsr4lamp", "dsl4", true, "line 4 data set ready"),
		parameter_bool_c(this, "dsr5lamp", "dsl5", true, "line 5 data set ready"),
		parameter_bool_c(this, "dsr6lamp", "dsl6", true, "line 6 data set ready"),
		parameter_bool_c(this, "dsr7lamp", "dsl7", true, "line 7 data set ready"),
	};
	parameter_bool_c cts_lamp[DHV_LINE_COUNT] = {
		parameter_bool_c(this, "cts0lamp", "ctl0", true, "line 0 clear to send"),
		parameter_bool_c(this, "cts1lamp", "ctl1", true, "line 1 clear to send"),
		parameter_bool_c(this, "cts2lamp", "ctl2", true, "line 2 clear to send"),
		parameter_bool_c(this, "cts3lamp", "ctl3", true, "line 3 clear to send"),
		parameter_bool_c(this, "cts4lamp", "ctl4", true, "line 4 clear to send"),
		parameter_bool_c(this, "cts5lamp", "ctl5", true, "line 5 clear to send"),
		parameter_bool_c(this, "cts6lamp", "ctl6", true, "line 6 clear to send"),
		parameter_bool_c(this, "cts7lamp", "ctl7", true, "line 7 clear to send"),
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
	void refresh_activity(void) override; // expire the rx/tx pulse lamps

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
	uint32_t selftest_polls;  // CSR reads counted since the master reset was issued
	uint32_t selftest_target; // poll count at which CSR<5> clears (skip lowers it)
	int selftest_ticks;    // inactivity-fallback ticks remaining (frozen while polling)
	int poll_active_ticks; // >0 while the host is actively polling the CSR

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

	// rx/tx activity lamps pulse per byte and are held for activity_lamp_on_time_ms
	// so a poll between bursts still sees them; refresh_activity clears them.
	uint64_t rx_lamp_until_ms[DHV_LINE_COUNT];
	uint64_t tx_lamp_until_ms[DHV_LINE_COUNT];
	void note_rx_activity(unsigned line);
	void note_tx_activity(unsigned line);
	// modem-control and carrier lamps, from the LNCTRL bits and each line's
	// TCP carrier; the receive scan refreshes them alongside the STAT register
	void refresh_signal_lamps(void); // caller holds state_mutex

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
