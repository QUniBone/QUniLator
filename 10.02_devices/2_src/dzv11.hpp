/* dzv11.hpp: DZ11 family asynchronous serial multiplexer

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see any device source header for the full text.

   One character-at-a-time async mux with the classic DZ11 register interface: a
   control/status register, a receiver silo, a per-line parameter register, a
   transmit-control register, a modem-status register and a transmit-data
   register, at four word addresses whose read and write functions differ.

   The bus decides how many lines the board has. The Unibus DZ11 is an eight-line
   board and its operating systems expect eight — VMS's autoconfigure creates
   TTA0 through TTA7 from one CSR at 760100. The Q-bus DZV11/DZQ11 is the
   four-line board. The register layout is the same either way; the line number
   fields, the per-line enable/DTR bytes and the modem-status bytes are as wide
   as the board has lines.

   Each line's byte stream is carried by a serial_tcp_line_c (Telnet/RFC2217
   over TCP). The transport models an auto-answer modem: a connecting client
   rings the line (ring indicator, briefly) and presents carrier, and the guest
   dropping Data Terminal Ready hangs the client up. The register model is built
   from EK-DZV11-TM-001 and passes the complete XXDP VDZAD3 (CVDZA) diagnostic
   with no errors.
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

#if defined(UNIBUS)
#define DZ_LINE_COUNT 8         // DZ11
#else
#define DZ_LINE_COUNT 4         // DZV11 / DZQ11
#endif

// the line number as the CSR's TLINE and the RBUF's RLINE field carry it
#define DZ_LINE_MASK (DZ_LINE_COUNT - 1)
// the per-line byte of the TCR, the TDR's break field and the MSR's two halves
#define DZ_LINE_BITS ((1u << DZ_LINE_COUNT) - 1)

// DEC floating defaults for the first DZV11
#define DZV11_ADDR   0760100
// Each mux uses two arbitration slots (RCV at slot, XMT at slot+1), and the
// pool steps four instances by two, so a DZV11 pool spans slot..slot+7. Base 4
// keeps all four instances clear of the console DL11 (1,2), the KW11 (3), and
// the disk/network controllers (uda 20, delqa 21) that share level 5; a slot
// shared with another device's request drops one device's interrupts.
#define DZV11_SLOT   4
#define DZV11_VECTOR 0300   // RCV +0, XMT +4
#define DZV11_LEVEL  04     // DZV11 asserts BIRQ4 (TM: priority 200 = level 4)

// Default TCP listen port base: line i listens on DZV11_TCP_PORT_BASE + i, so an
// enabled device accepts a client on a distinct port without per-line setup.
#define DZV11_TCP_PORT_BASE 4000

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
#define DZ_CSR_TLINE_SHIFT 8   // 10..8 transmitter scan line number
#define DZ_CSR_TLINE  (DZ_LINE_MASK << DZ_CSR_TLINE_SHIFT)
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
#define DZ_RBUF_RLINE_SHIFT 8  // 10..8 receive line number

// LPR bits (write at base+2)
#define DZ_LPR_LINE   0000007  // 2..0 line number
#define DZ_LPR_PENABLE 0000100 // 6 parity enable
#define DZ_LPR_ODD    0000200  // 7 odd parity (with PENABLE)
#define DZ_LPR_RXON   0010000  // 12 receiver enable for the line

// TCR bits: one transmit-enable bit per line from 0 up, one DTR bit per line
// from 8 up
#define DZ_TCR_LINE_ENABLE_MASK DZ_LINE_BITS
#define DZ_TCR_DTR_SHIFT 8

// TDR: 7..0 transmit data, one break-control bit per line from 8 up
#define DZ_TDR_DATA   0000377
#define DZ_TDR_BREAK_SHIFT 8   // forces a line's output to space

// MSR bits (read at base+6): one ring bit per line from 0 up, one carrier-detect
// bit per line from 8 up. The driver takes carrier from the high byte to gate a
// line's open().
#define DZ_MSR_RI_SHIFT 0
#define DZ_MSR_CD_SHIFT 8

// How long a connecting client rings before the modem stops ringing, in ms. A
// guest that answers by raising Data Terminal Ready ends the ring at once; one
// that does no modem control simply sees carrier and the ring lapses.
#define DZ_RING_MS 4000

class dzv11_c: public qunibusdevice_c {
public:
	const char *category(void) const override { return "serial"; }

	dzv11_c();
	~dzv11_c();

	// one TCP transport per line; a connected client is that line's carrier
	serial_tcp_line_c tcp_line[DZ_LINE_COUNT];

	// A line that listens only while the guest holds it open. With this set, a
	// line's TCP port is bound while the guest asserts Data Terminal Ready on it
	// and given up when DTR falls, so a caller reaches a line the guest is
	// actually ready for rather than one that will not answer. Off by default,
	// which is a line that listens whenever the device is enabled.
	parameter_bool_c dtr_listen = parameter_bool_c(this, "dtr_listen", "dtrl", false,
			"listen on a line's TCP port only while the guest asserts DTR");

	// Per-line TCP configuration and the dashboard's signal lamps. The lines past
	// the fourth exist only on the eight-line board, so each list runs to four
	// and the Unibus build adds the rest.
	parameter_string_c   tcp_role[DZ_LINE_COUNT] = {
		parameter_string_c(this, "tcp_role0", "r0", false, "line 0 TCP role: \"\"/listen/connect"),
		parameter_string_c(this, "tcp_role1", "r1", false, "line 1 TCP role: \"\"/listen/connect"),
		parameter_string_c(this, "tcp_role2", "r2", false, "line 2 TCP role: \"\"/listen/connect"),
		parameter_string_c(this, "tcp_role3", "r3", false, "line 3 TCP role: \"\"/listen/connect"),
#if DZ_LINE_COUNT > 4
		parameter_string_c(this, "tcp_role4", "r4", false, "line 4 TCP role: \"\"/listen/connect"),
		parameter_string_c(this, "tcp_role5", "r5", false, "line 5 TCP role: \"\"/listen/connect"),
		parameter_string_c(this, "tcp_role6", "r6", false, "line 6 TCP role: \"\"/listen/connect"),
		parameter_string_c(this, "tcp_role7", "r7", false, "line 7 TCP role: \"\"/listen/connect"),
#endif
	};
	parameter_string_c   tcp_host[DZ_LINE_COUNT] = {
		parameter_string_c(this, "tcp_host0", "h0", false, "line 0 connect-out host"),
		parameter_string_c(this, "tcp_host1", "h1", false, "line 1 connect-out host"),
		parameter_string_c(this, "tcp_host2", "h2", false, "line 2 connect-out host"),
		parameter_string_c(this, "tcp_host3", "h3", false, "line 3 connect-out host"),
#if DZ_LINE_COUNT > 4
		parameter_string_c(this, "tcp_host4", "h4", false, "line 4 connect-out host"),
		parameter_string_c(this, "tcp_host5", "h5", false, "line 5 connect-out host"),
		parameter_string_c(this, "tcp_host6", "h6", false, "line 6 connect-out host"),
		parameter_string_c(this, "tcp_host7", "h7", false, "line 7 connect-out host"),
#endif
	};
	parameter_unsigned_c tcp_port[DZ_LINE_COUNT] = {
		parameter_unsigned_c(this, "tcp_port0", "p0", false, "", "%d", "line 0 TCP port", 16, 10),
		parameter_unsigned_c(this, "tcp_port1", "p1", false, "", "%d", "line 1 TCP port", 16, 10),
		parameter_unsigned_c(this, "tcp_port2", "p2", false, "", "%d", "line 2 TCP port", 16, 10),
		parameter_unsigned_c(this, "tcp_port3", "p3", false, "", "%d", "line 3 TCP port", 16, 10),
#if DZ_LINE_COUNT > 4
		parameter_unsigned_c(this, "tcp_port4", "p4", false, "", "%d", "line 4 TCP port", 16, 10),
		parameter_unsigned_c(this, "tcp_port5", "p5", false, "", "%d", "line 5 TCP port", 16, 10),
		parameter_unsigned_c(this, "tcp_port6", "p6", false, "", "%d", "line 6 TCP port", 16, 10),
		parameter_unsigned_c(this, "tcp_port7", "p7", false, "", "%d", "line 7 TCP port", 16, 10),
#endif
	};

	// Per-line signal lamps for the dashboard, one set per line. rx/tx pulse with
	// traffic (held briefly by refresh_activity); dtr follows the TCR DTR bit the
	// guest drives; cd follows carrier (a connected TCP client) and ri the ring
	// a connecting client raises. Names end in "lamp" so webevents' lamp poll
	// picks up the direct value assignments the device threads make. The board
	// carries no RTS/CTS/DSR, so those signals have no lamp here.
	parameter_bool_c rx_lamp[DZ_LINE_COUNT] = {
		parameter_bool_c(this, "rx0lamp", "rxl0", true, "line 0 receive activity"),
		parameter_bool_c(this, "rx1lamp", "rxl1", true, "line 1 receive activity"),
		parameter_bool_c(this, "rx2lamp", "rxl2", true, "line 2 receive activity"),
		parameter_bool_c(this, "rx3lamp", "rxl3", true, "line 3 receive activity"),
#if DZ_LINE_COUNT > 4
		parameter_bool_c(this, "rx4lamp", "rxl4", true, "line 4 receive activity"),
		parameter_bool_c(this, "rx5lamp", "rxl5", true, "line 5 receive activity"),
		parameter_bool_c(this, "rx6lamp", "rxl6", true, "line 6 receive activity"),
		parameter_bool_c(this, "rx7lamp", "rxl7", true, "line 7 receive activity"),
#endif
	};
	parameter_bool_c tx_lamp[DZ_LINE_COUNT] = {
		parameter_bool_c(this, "tx0lamp", "txl0", true, "line 0 transmit activity"),
		parameter_bool_c(this, "tx1lamp", "txl1", true, "line 1 transmit activity"),
		parameter_bool_c(this, "tx2lamp", "txl2", true, "line 2 transmit activity"),
		parameter_bool_c(this, "tx3lamp", "txl3", true, "line 3 transmit activity"),
#if DZ_LINE_COUNT > 4
		parameter_bool_c(this, "tx4lamp", "txl4", true, "line 4 transmit activity"),
		parameter_bool_c(this, "tx5lamp", "txl5", true, "line 5 transmit activity"),
		parameter_bool_c(this, "tx6lamp", "txl6", true, "line 6 transmit activity"),
		parameter_bool_c(this, "tx7lamp", "txl7", true, "line 7 transmit activity"),
#endif
	};
	parameter_bool_c dtr_lamp[DZ_LINE_COUNT] = {
		parameter_bool_c(this, "dtr0lamp", "dtl0", true, "line 0 data terminal ready"),
		parameter_bool_c(this, "dtr1lamp", "dtl1", true, "line 1 data terminal ready"),
		parameter_bool_c(this, "dtr2lamp", "dtl2", true, "line 2 data terminal ready"),
		parameter_bool_c(this, "dtr3lamp", "dtl3", true, "line 3 data terminal ready"),
#if DZ_LINE_COUNT > 4
		parameter_bool_c(this, "dtr4lamp", "dtl4", true, "line 4 data terminal ready"),
		parameter_bool_c(this, "dtr5lamp", "dtl5", true, "line 5 data terminal ready"),
		parameter_bool_c(this, "dtr6lamp", "dtl6", true, "line 6 data terminal ready"),
		parameter_bool_c(this, "dtr7lamp", "dtl7", true, "line 7 data terminal ready"),
#endif
	};
	parameter_bool_c cd_lamp[DZ_LINE_COUNT] = {
		parameter_bool_c(this, "cd0lamp", "cdl0", true, "line 0 carrier detect"),
		parameter_bool_c(this, "cd1lamp", "cdl1", true, "line 1 carrier detect"),
		parameter_bool_c(this, "cd2lamp", "cdl2", true, "line 2 carrier detect"),
		parameter_bool_c(this, "cd3lamp", "cdl3", true, "line 3 carrier detect"),
#if DZ_LINE_COUNT > 4
		parameter_bool_c(this, "cd4lamp", "cdl4", true, "line 4 carrier detect"),
		parameter_bool_c(this, "cd5lamp", "cdl5", true, "line 5 carrier detect"),
		parameter_bool_c(this, "cd6lamp", "cdl6", true, "line 6 carrier detect"),
		parameter_bool_c(this, "cd7lamp", "cdl7", true, "line 7 carrier detect"),
#endif
	};
	parameter_bool_c ri_lamp[DZ_LINE_COUNT] = {
		parameter_bool_c(this, "ri0lamp", "ril0", true, "line 0 ring indicator"),
		parameter_bool_c(this, "ri1lamp", "ril1", true, "line 1 ring indicator"),
		parameter_bool_c(this, "ri2lamp", "ril2", true, "line 2 ring indicator"),
		parameter_bool_c(this, "ri3lamp", "ril3", true, "line 3 ring indicator"),
#if DZ_LINE_COUNT > 4
		parameter_bool_c(this, "ri4lamp", "ril4", true, "line 4 ring indicator"),
		parameter_bool_c(this, "ri5lamp", "ril5", true, "line 5 ring indicator"),
		parameter_bool_c(this, "ri6lamp", "ril6", true, "line 6 ring indicator"),
		parameter_bool_c(this, "ri7lamp", "ril7", true, "line 7 ring indicator"),
#endif
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
	bool line_dtr[DZ_LINE_COUNT];     // TCR DTR bit last written by the guest

	// The auto-answer modem in front of each line. A client arriving on an
	// unanswered line rings it: the ring indicator stands until the guest raises
	// Data Terminal Ready (it has answered) or the ring lapses. VMS's dialup
	// terminal driver waits for that ring before it raises DTR, and hangs up by
	// dropping it again; a guest that does no modem control ignores the ring and
	// works off carrier alone, as before.
	bool line_carrier[DZ_LINE_COUNT]; // carrier as of the last modem-status scan
	uint64_t ring_until_ms[DZ_LINE_COUNT]; // 0 = not ringing

	// rx/tx activity lamps pulse per byte and are held for activity_lamp_on_time_ms
	// so a poll between bursts still sees them; refresh_activity clears them.
	uint64_t rx_lamp_until_ms[DZ_LINE_COUNT];
	uint64_t tx_lamp_until_ms[DZ_LINE_COUNT];
	void note_rx_activity(unsigned line);
	void note_tx_activity(unsigned line);

	// per-line format, from the LPR; drives received parity/framing status
	bool parity_enable[DZ_LINE_COUNT]; // LPR 06
	bool odd_parity[DZ_LINE_COUNT];    // LPR 07 (with parity_enable)
	uint8_t tx_break;                  // TDR 08..11: lines forced to space

	// receiver silo (FIFO of RBUF words)
	std::deque<uint16_t> silo;
	unsigned silo_alarm_count;        // chars since last silo-alarm

	// last MSR value written to the register. The receiver scan refreshes modem
	// status every 500 us; carrier only moves when a client connects or drops,
	// so the register is rewritten (and logged) only on a real change. 0xffff is
	// an impossible MSR, so the first refresh always writes the true value.
	uint16_t msr_dati_value = 0xffff;

	bool get_rcv_intr_level(void);
	bool get_xmt_intr_level(void);
	void update_csr_and_INTR(void);   // recompute CSR DATI value, raise/cancel INTR
	void eval_csr_dato(void);
	void eval_lpr_dato(void);
	void eval_tcr_dato(void);
	void eval_tdr_dato(void);
	void set_rbuf_dati(void);         // top-of-silo -> RBUF read value
	void silo_push(uint8_t line, uint8_t ch, uint16_t err_bits); // enqueue a rx char
	void set_msr_dati(void);          // modem status from TCP carrier
	bool select_next_tx_line(void);   // scanner: pick a ready enabled line
	void scan_tx_trdy(void);          // advance TRDY state (present/drop) now

	// Tell each line's transport whether it may take a caller: with dtr_listen
	// set that follows the guest's DTR bit, otherwise the line always may.
	void refresh_listen_gates(void);

	std::string open_lines(void);
	void close_lines(void);
	// (re)configure and (re)open one line's TCP transport with the given role/
	// host/port. Closes any transport already running on the line first; an empty
	// role leaves it closed. Used both at install and for a live tcp_* edit.
	bool open_line(unsigned i, const std::string &role, const std::string &host,
			uint16_t port);

	// transmit handoff between register callback and worker
	bool tdr_pending;
	uint8_t tdr_char;
	uint8_t tdr_line;

	void worker_rcv(void);
	void worker_xmt(void);
};

#endif // _DZV11_HPP_
