/* dzv11.hpp: DZV11/DZQ11 4-line asynchronous serial multiplexer

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see any device source header for the full text.

   The DZV11 (Q-bus) / DZQ11 is a 4-line character-at-a-time async mux with the
   classic DZ11 register interface: a control/status register, a receiver silo,
   a per-line parameter register, a transmit-control register, a modem-status
   register and a transmit-data register, at four word addresses whose read and
   write functions differ.

   Each line's byte stream is carried by a serial_tcp_line_c (Telnet/RFC2217
   over TCP); a connected TCP client presents as carrier-detect in the modem
   status register. No DZ11 manual is in this repo, so the register model is
   built from the standard DZ11/DZV11 register set and is not yet XXDP-validated.
*/
#ifndef _DZV11_HPP_
#define _DZV11_HPP_

#include <cstdint>
#include <deque>

#include "utils.hpp"
#include "qunibusdevice.hpp"
#include "parameter.hpp"
#include "rs232adapter.hpp"
#include "serial_tcp_line.hpp"

#define DZ_LINE_COUNT 4

// DEC floating defaults for the first DZV11
#define DZV11_ADDR   0760100
#define DZV11_SLOT   20
#define DZV11_VECTOR 0300   // RCV +0, XMT +4
#define DZV11_LEVEL  05

// register indices (each a word; read and write meanings differ)
enum dz_reg_index {
	dz_idx_csr = 0,      // CSR       (r/w)
	dz_idx_rbuf_lpr,     // RBUF (r) / LPR (w)
	dz_idx_tcr,          // TCR       (r/w)
	dz_idx_msr_tdr,      // MSR  (r) / TDR (w)
	dz_idx_count
};

// CSR bits
#define DZ_CSR_TRDY   0100000  // 15 transmitter ready
#define DZ_CSR_TIE    0040000  // 14 transmitter interrupt enable
#define DZ_CSR_SA     0020000  // 13 silo alarm
#define DZ_CSR_SAE    0010000  // 12 silo alarm enable
#define DZ_CSR_TLINE  0001400  // 10..8 transmitter scan line number
#define DZ_CSR_TLINE_SHIFT 8
#define DZ_CSR_RDONE  0000200  // 7 receiver done (silo not empty)
#define DZ_CSR_RIE    0000100  // 6 receiver interrupt enable
#define DZ_CSR_MSE    0000040  // 5 master scan enable
#define DZ_CSR_CLR    0000020  // 4 master clear (self-clearing)
#define DZ_CSR_MAINT  0000010  // 3 maintenance loopback

// RBUF bits (read at base+2)
#define DZ_RBUF_VALID 0100000  // 15 data valid
#define DZ_RBUF_OVR   0040000  // 14 overrun error
#define DZ_RBUF_FRAME 0020000  // 13 framing error
#define DZ_RBUF_PAR   0010000  // 12 parity error
#define DZ_RBUF_RLINE_SHIFT 8  // 9..8 receive line number

// LPR bits (write at base+2)
#define DZ_LPR_LINE   0000007  // 2..0 line number
#define DZ_LPR_RXON   0010000  // 12 receiver enable for the line

// TCR bits: 0..3 = transmit enable per line, 8..11 = DTR per line
#define DZ_TCR_LINE_ENABLE_MASK 0000017
#define DZ_TCR_DTR_SHIFT 8

// TDR: 7..0 transmit data, 8..11 break control per line
#define DZ_TDR_DATA   0000377

class dzv11_c: public qunibusdevice_c {
public:
	const char *category(void) const override { return "serial"; }

	dzv11_c();
	~dzv11_c();

	// one TCP transport per line; a connected client is that line's carrier
	serial_tcp_line_c tcp_line[DZ_LINE_COUNT];

	// per-line TCP configuration parameters
	parameter_string_c   tcp_role[DZ_LINE_COUNT] = {
		parameter_string_c(this, "tcp_role0", "r0", false, "line 0 TCP role: \"\"/listen/connect"),
		parameter_string_c(this, "tcp_role1", "r1", false, "line 1 TCP role: \"\"/listen/connect"),
		parameter_string_c(this, "tcp_role2", "r2", false, "line 2 TCP role: \"\"/listen/connect"),
		parameter_string_c(this, "tcp_role3", "r3", false, "line 3 TCP role: \"\"/listen/connect"),
	};
	parameter_string_c   tcp_host[DZ_LINE_COUNT] = {
		parameter_string_c(this, "tcp_host0", "h0", false, "line 0 connect-out host"),
		parameter_string_c(this, "tcp_host1", "h1", false, "line 1 connect-out host"),
		parameter_string_c(this, "tcp_host2", "h2", false, "line 2 connect-out host"),
		parameter_string_c(this, "tcp_host3", "h3", false, "line 3 connect-out host"),
	};
	parameter_unsigned_c tcp_port[DZ_LINE_COUNT] = {
		parameter_unsigned_c(this, "tcp_port0", "p0", false, "", "%d", "line 0 TCP port", 16, 10),
		parameter_unsigned_c(this, "tcp_port1", "p1", false, "", "%d", "line 1 TCP port", 16, 10),
		parameter_unsigned_c(this, "tcp_port2", "p2", false, "", "%d", "line 2 TCP port", 16, 10),
		parameter_unsigned_c(this, "tcp_port3", "p3", false, "", "%d", "line 3 TCP port", 16, 10),
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
	qunibusdevice_register_t *reg_csr;
	qunibusdevice_register_t *reg_rbuf_lpr;
	qunibusdevice_register_t *reg_tcr;
	qunibusdevice_register_t *reg_msr_tdr;

	// two interrupts of the same level, slot and slot+1 (RCV, XMT)
	intr_request_c rcvintr_request = intr_request_c(this);
	intr_request_c xmtintr_request = intr_request_c(this);

	pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
	pthread_cond_t  xmt_cond = PTHREAD_COND_INITIALIZER;

	// CSR state
	bool csr_tie;      // transmitter interrupt enable
	bool csr_sae;      // silo alarm enable
	bool csr_sa;       // silo alarm
	bool csr_rie;      // receiver interrupt enable
	bool csr_mse;      // master scan enable
	bool csr_maint;    // maintenance loopback
	uint8_t csr_tline; // transmitter scan line number
	bool csr_trdy;     // transmitter ready

	// per-line enables
	bool rx_enabled[DZ_LINE_COUNT];   // LPR RX ON
	bool tx_enabled[DZ_LINE_COUNT];   // TCR line enable
	bool line_open[DZ_LINE_COUNT];    // this line's TCP transport is running

	// receiver silo (FIFO of RBUF words)
	std::deque<uint16_t> silo;
	unsigned silo_alarm_count;        // chars since last silo-alarm

	bool get_rcv_intr_level(void);
	bool get_xmt_intr_level(void);
	void update_csr_and_INTR(void);   // recompute CSR DATI value, raise/cancel INTR
	void eval_csr_dato(void);
	void eval_lpr_dato(void);
	void eval_tcr_dato(void);
	void eval_tdr_dato(void);
	void set_rbuf_dati(void);         // top-of-silo -> RBUF read value
	void set_msr_dati(void);          // modem status from TCP carrier
	bool select_next_tx_line(void);   // scanner: pick a ready enabled line

	void open_lines(void);
	void close_lines(void);

	// transmit handoff between register callback and worker
	bool tdr_pending;
	uint8_t tdr_char;
	uint8_t tdr_line;

	void worker_rcv(void);
	void worker_xmt(void);
};

#endif // _DZV11_HPP_
