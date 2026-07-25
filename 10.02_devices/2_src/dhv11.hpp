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

// DEC floating defaults for the first DHV11
#define DHV11_ADDR   0760440
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

// CSR bits (modeled — TODO verify against EK-DHV11-UG)
#define DHV_CSR_TX_ACT   0100000  // 15 a channel's DMA transmit completed / needs service
#define DHV_CSR_TX_IE    0040000  // 14 transmit interrupt enable
#define DHV_CSR_RX_DATA  0000200  // 7  receive FIFO not empty
#define DHV_CSR_RX_IE    0000100  // 6  receive interrupt enable
#define DHV_CSR_MASTER_RESET 0010000 // 12 master reset (self-clearing)
#define DHV_CSR_CHAN_MASK 0000007 // 2..0 channel select for indexed registers

// RBUF bits (modeled)
#define DHV_RBUF_VALID   0100000  // 15 data valid
#define DHV_RBUF_OVR     0040000  // 14 overrun
#define DHV_RBUF_FRAME   0020000  // 13 framing error
#define DHV_RBUF_PAR     0010000  // 12 parity error
#define DHV_RBUF_LINE_SHIFT 8     // 10..8 source line number

// LNCTRL bits (modeled): per selected channel
#define DHV_LNCTRL_TX_ENABLE 0000001 // 0 transmitter enable
#define DHV_LNCTRL_RX_ENABLE 0000010 // 3 receiver enable

// TBUFFAD2: <5:0> = bus address bits 21..16; <15> abort (modeled)
#define DHV_TBUFFAD2_ADDR_HI 0000077
#define DHV_TBUFFAD2_ABORT   0100000

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
	uint8_t csr_channel; // selected channel for indexed registers

	// per-channel transmit-DMA state
	struct tx_channel_t {
		bool enabled;
		bool rx_enabled;
		bool open;         // TCP transport running
		uint32_t dma_addr; // 22-bit bus address
		uint16_t dma_count;// characters remaining to DMA
		bool dma_pending;  // a transmit DMA has been requested
	};
	tx_channel_t chan[DHV_LINE_COUNT];

	// receive FIFO of RBUF words
	std::deque<uint16_t> rx_fifo;

	bool get_rcv_intr_level(void);
	bool get_xmt_intr_level(void);
	void update_csr_and_INTR(void); // caller holds state_mutex
	void set_rbuf_dati(void);
	void set_stat_dati(void);

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
