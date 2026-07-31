/* dhv11.cpp: DHV11/DHQ11 8-line async mux with DMA transmit

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see any device source header for the full text.

   See dhv11.hpp: the register/bit model follows EK-DHV11-TM-002 and passes DEC's
   CVDHA (VDHAE0) diagnostic under XXDP with no errors. Concurrency follows the
   DELQA rule: the transmit worker never holds state_mutex across a blocking DMA
   call.
*/

#include <cstring>
#include <cstdio>

#include "logger.hpp"
#include "timeout.hpp"
#include "qunibusadapter.hpp"
#include "qunibus.h"
#include "dhv11.hpp"

dhv11_c::dhv11_c() : qunibusdevice_c()
{
	set_workers_count(2); // 0 = receiver scan, 1 = transmit-DMA engine

	name.value = "dhv11";
	type_name.value = "dhv11_c";
	log_label = "dhv11";

	set_default_bus_params(DHV11_ADDR, DHV11_SLOT, DHV11_VECTOR, DHV11_LEVEL);

	register_count = dhv_idx_count;

	static const char *rnames[dhv_idx_count] = {
		"CSR", "RBUF", "LPR", "STAT", "LNCTRL", "TBUFAD1", "TBUFAD2", "TBUFCT"
	};
	// Per-register writable bits and read-back rules follow EK-DHV11-TM-002 Table
	// 3-1 and the bit definitions in §3.2.2. The register-access test writes a
	// pattern and reads it back masked to these bits, so the masks must be exact.
	for (unsigned i = 0; i < dhv_idx_count; i++) {
		reg[i] = &(this->registers[i]);
		strcpy(reg[i]->name, rnames[i]);
		reg[i]->active_on_dati = false;
		reg[i]->active_on_dato = true;
		reg[i]->reset_value = 0;
		reg[i]->writable_bits = 0xffff;
	}
	// CSR is active on read too: reading it clears TX.ACTION (§3.2.2.1).
	reg[dhv_idx_csr]->active_on_dati = true;
	reg[dhv_idx_csr]->writable_bits = DHV_CSR_WMASK;
	// base+2 is RBUF on read (pops the FIFO) and TXCHAR on write (a single
	// programmed character): active on both cycles.
	reg[dhv_idx_rbuf]->active_on_dati = true;
	reg[dhv_idx_rbuf]->writable_bits = DHV_TXCHAR_VALID | 0377;
	// STAT is read-only modem status kept current by the worker.
	reg[dhv_idx_stat]->active_on_dato = false;
	reg[dhv_idx_stat]->writable_bits = 0;

	memset(chan, 0, sizeof chan);
	csr_rx_ie = csr_tx_ie = csr_tx_act = false;
	csr_master_reset = csr_diag_fail = selftest_pending = selftest_skip = false;
	selftest_ticks = 0;
	selftest_polls = 0;
	selftest_target = DHV_SELFTEST_POLLS;
	poll_active_ticks = 0;
	csr_tx_line = 0;
	csr_channel = 0;

	memset(rx_lamp_until_ms, 0, sizeof rx_lamp_until_ms);
	memset(tx_lamp_until_ms, 0, sizeof tx_lamp_until_ms);
	for (unsigned i = 0; i < DHV_LINE_COUNT; i++) {
		rx_lamp[i].kind = parameter_c::PARAM_STATUS;
		tx_lamp[i].kind = parameter_c::PARAM_STATUS;
		dtr_lamp[i].kind = parameter_c::PARAM_STATUS;
		rts_lamp[i].kind = parameter_c::PARAM_STATUS;
		cd_lamp[i].kind = parameter_c::PARAM_STATUS;
		dsr_lamp[i].kind = parameter_c::PARAM_STATUS;
		cts_lamp[i].kind = parameter_c::PARAM_STATUS;
	}

	// Usable defaults: every line listens on a distinct TCP port, so an enabled
	// device accepts a client without any per-line configuration. tcp_host stays
	// empty (used only for connect-out).
	for (unsigned i = 0; i < DHV_LINE_COUNT; i++) {
		tcp_role[i].value = "listen";
		tcp_port[i].value = DHV11_TCP_PORT_BASE + i;
	}
}

dhv11_c::~dhv11_c()
{
}

bool dhv11_c::on_param_changed(parameter_c *param)
{
	if (param == &priority_slot) {
		rcvintr_request.set_priority_slot(priority_slot.new_value);
		xmtintr_request.set_priority_slot(priority_slot.new_value + 1);
		dma_request.set_priority_slot(priority_slot.new_value);
	} else if (param == &intr_vector) {
		rcvintr_request.set_vector(intr_vector.new_value);
		xmtintr_request.set_vector(intr_vector.new_value + 4);
	} else if (param == &intr_level) {
		rcvintr_request.set_level(intr_level.new_value);
		xmtintr_request.set_level(intr_level.new_value);
	} else {
		// A live tcp_role/tcp_host/tcp_port edit re-opens just that one line with
		// the new transport config, leaving the other lines and the bus
		// registration untouched. on_param_changed runs before the parameter's
		// value is committed, so the changed field's new_value is used and the
		// two unchanged fields keep their current value.
		for (unsigned i = 0; i < DHV_LINE_COUNT; i++) {
			if (param != &tcp_role[i] && param != &tcp_host[i] && param != &tcp_port[i])
				continue;
			if (enabled.value) {
				std::string role = tcp_role[i].value;
				std::string host = tcp_host[i].value;
				uint16_t port = (uint16_t) tcp_port[i].value;
				if (param == &tcp_role[i])
					role = tcp_role[i].new_value;
				else if (param == &tcp_host[i])
					host = tcp_host[i].new_value;
				else
					port = (uint16_t) tcp_port[i].new_value;
				open_line(i, role, host, port);
			}
			break;
		}
	}
	return qunibusdevice_c::on_param_changed(param);
}

// (Re)open one line's transport. The channel's open flag is flipped under
// state_mutex so the workers stop touching the line, but the blocking socket
// close()/open() runs outside the lock — the same "never hold state_mutex across
// blocking I/O" rule the DMA transmit path follows.
void dhv11_c::open_line(unsigned i, const std::string &role, const std::string &host,
		uint16_t port)
{
	pthread_mutex_lock(&state_mutex);
	bool was_open = chan[i].open;
	chan[i].open = false;
	pthread_mutex_unlock(&state_mutex);
	if (was_open)
		tcp_line[i].close(); // drops any client connected under the old config

	if (role.empty())
		return; // an empty tcp_role leaves the line closed

	serial_tcp_line_c &ln = tcp_line[i];
	if (role == "listen")
		ln.role = serial_tcp_line_c::ROLE_LISTEN;
	else if (role == "connect")
		ln.role = serial_tcp_line_c::ROLE_CONNECT;
	else if (role == "websocket")
		ln.role = serial_tcp_line_c::ROLE_WEBSOCKET;
	else {
		WARNING("line %u: tcp_role must be listen/connect/websocket, got \"%s\"", i, role.c_str());
		return;
	}
	ln.host = host;
	ln.port = port;
	char lbl[32];
	snprintf(lbl, sizeof lbl, "dhv11.%u", i);
	ln.log_label = lbl;
	ln.verbose = true;
	if (ln.open()) {
		pthread_mutex_lock(&state_mutex);
		chan[i].open = true;
		pthread_mutex_unlock(&state_mutex);
	} else {
		WARNING("line %u: cannot open TCP transport (port %u)", i, (unsigned) port);
	}
}

void dhv11_c::open_lines(void)
{
	for (unsigned i = 0; i < DHV_LINE_COUNT; i++)
		open_line(i, tcp_role[i].value, tcp_host[i].value, (uint16_t) tcp_port[i].value);
}

void dhv11_c::close_lines(void)
{
	for (unsigned i = 0; i < DHV_LINE_COUNT; i++)
		if (chan[i].open) {
			tcp_line[i].close();
			chan[i].open = false;
		}
}

bool dhv11_c::on_before_install(void)
{
	// The TCP transport params stay writable while installed so a line's port or
	// role can be retuned live (see on_param_changed); they do not touch the bus
	// registration. The base class locks the bus params (base_addr/slot/vector/
	// level) on enable.
	open_lines();
	return true;
}

void dhv11_c::on_after_uninstall(void)
{
	close_lines();
}

// -------------------------------------------------------------------------

bool dhv11_c::get_rcv_intr_level(void)
{
	return csr_rx_ie && !rx_fifo.empty();
}

bool dhv11_c::get_xmt_intr_level(void)
{
	return csr_tx_ie && csr_tx_act;
}

// caller holds state_mutex
void dhv11_c::update_csr_and_INTR(void)
{
	uint16_t val = (csr_tx_act ? DHV_CSR_TX_ACT : 0)
			| (csr_tx_ie ? DHV_CSR_TX_IE : 0)
			| (csr_diag_fail ? DHV_CSR_DIAG_FAIL : 0)
			| (((uint16_t) csr_tx_line << DHV_CSR_TX_LINE_SHIFT) & DHV_CSR_TX_LINE_MASK)
			| (rx_fifo.empty() ? 0 : DHV_CSR_RX_DATA)
			| (csr_rx_ie ? DHV_CSR_RX_IE : 0)
			| (csr_master_reset ? DHV_CSR_MASTER_RESET : 0)
			| (csr_channel & DHV_CSR_CHAN_MASK);
	set_register_dati_value(reg[dhv_idx_csr], val, __func__);

	switch (rcvintr_request.edge_detect(get_rcv_intr_level())) {
	case intr_request_c::INTERRUPT_EDGE_RAISING:
		qunibusadapter->INTR(rcvintr_request, NULL, 0);
		break;
	case intr_request_c::INTERRUPT_EDGE_FALLING:
		qunibusadapter->cancel_INTR(rcvintr_request);
		break;
	default:
		break;
	}
	switch (xmtintr_request.edge_detect(get_xmt_intr_level())) {
	case intr_request_c::INTERRUPT_EDGE_RAISING:
		qunibusadapter->INTR(xmtintr_request, NULL, 0);
		break;
	case intr_request_c::INTERRUPT_EDGE_FALLING:
		qunibusadapter->cancel_INTR(xmtintr_request);
		break;
	default:
		break;
	}
}

// caller holds state_mutex
void dhv11_c::set_rbuf_dati(void)
{
	uint16_t val = rx_fifo.empty() ? 0 : rx_fifo.front();
	set_register_dati_value(reg[dhv_idx_rbuf], val, __func__);
}

// caller holds state_mutex: push one RBUF word and refresh RX status
void dhv11_c::rx_fifo_push(uint16_t entry)
{
	if (rx_fifo.size() >= 256) {
		entry |= DHV_RBUF_OVERRUN;
		rx_fifo.pop_back();
	}
	rx_fifo.push_back(entry);
	set_rbuf_dati();
}

// caller holds state_mutex: raise TX.ACTION reporting the given line (§3.2.2.1)
void dhv11_c::tx_report(unsigned line)
{
	csr_tx_line = line & 7;
	csr_tx_act = true;
}

// caller holds state_mutex: STAT (base+6) reports modem status in its high byte.
// Bit 8 reads 0 to identify the part as a DHV11 (§3.2.2.5); a connected TCP peer
// stands in for carrier and data-set-ready on the selected channel.
void dhv11_c::set_stat_dati(void)
{
	uint16_t val = 0;
	unsigned c = csr_channel & DHV_CSR_CHAN_MASK;
	if (c < DHV_LINE_COUNT && chan[c].open && tcp_line[c].client_connected())
		val |= DHV_STAT_DCD | DHV_STAT_DSR | DHV_STAT_CTS;
	set_register_dati_value(reg[dhv_idx_stat], val, __func__);
}

// caller holds state_mutex: the dashboard's modem-signal lamps. STAT reports
// only the channel the CSR selects, so the panel reads each line's carrier and
// LNCTRL directly — the same conditions, for all eight lines at once.
void dhv11_c::refresh_signal_lamps(void)
{
	for (unsigned i = 0; i < DHV_LINE_COUNT; i++) {
		bool carrier = chan[i].open && tcp_line[i].client_connected();
		cd_lamp[i].value = carrier;
		dsr_lamp[i].value = carrier;
		cts_lamp[i].value = carrier;
		dtr_lamp[i].value = (chan[i].lnctrl & DHV_LNCTRL_DTR) != 0;
		rts_lamp[i].value = (chan[i].lnctrl & DHV_LNCTRL_RTS) != 0;
	}
}

// -------------------------------------------------------------------------
// activity lamps

// A byte moves in microseconds; hold the lamp for activity_lamp_on_time_ms so a
// poll between bursts still catches it. refresh_activity() clears it once the
// hold elapses. Called from the device threads by direct value assignment, the
// same path the webevents lamp poll watches.
void dhv11_c::note_rx_activity(unsigned line)
{
	rx_lamp_until_ms[line] = now_ms() + activity_lamp_on_time_ms;
	rx_lamp[line].value = true;
}

void dhv11_c::note_tx_activity(unsigned line)
{
	tx_lamp_until_ms[line] = now_ms() + activity_lamp_on_time_ms;
	tx_lamp[line].value = true;
}

void dhv11_c::refresh_activity(void)
{
	uint64_t now = now_ms();
	for (unsigned i = 0; i < DHV_LINE_COUNT; i++) {
		if (rx_lamp[i].value && now >= rx_lamp_until_ms[i])
			rx_lamp[i].value = false;
		if (tx_lamp[i].value && now >= tx_lamp_until_ms[i])
			tx_lamp[i].value = false;
	}
}

// caller holds state_mutex: load the eight self-test result codes into the FIFO
// (§3.3.10): six null codes then the two ROM-version codes, each carrying
// DATA.VALID, the diagnostic marker <14:12>=111 and its sequence number <11:8>.
void dhv11_c::load_selftest_codes(void)
{
	// Six filler codes then the two ROM-version codes; the filler is 203 when the
	// self-test was skipped, 201 when it ran (§3.3.10). The self-test reports
	// PROC2's ROM version (D1=1) before PROC1's (D1=0): the codes are read in
	// sequence order and the version-number test requires PROC2 to appear first.
	uint8_t filler = selftest_skip ? DHV_SELFTEST_SKIP : DHV_SELFTEST_NULL;
	uint8_t code[8] = {
		filler, filler, filler, filler, filler, filler,
		DHV_SELFTEST_ROM2, DHV_SELFTEST_ROM1
	};
	rx_fifo.clear();
	for (unsigned seq = 0; seq < 8; seq++)
		rx_fifo.push_back(DHV_RBUF_DATA_VALID | DHV_RBUF_DIAG
				| ((uint16_t) seq << DHV_RBUF_LINE_SHIFT) | code[seq]);
	set_rbuf_dati();
}

// caller holds state_mutex: finish a running master reset — load the self-test
// result codes, report success (DIAG.FAIL clear) and drop CSR<5>.
void dhv11_c::complete_selftest(void)
{
	if (!csr_master_reset)
		return;
	DEBUG("self-test complete after %u CSR polls (skip=%d, target=%u)",
			(unsigned) selftest_polls, (int) selftest_skip, (unsigned) selftest_target);
	// The self-test leaves every channel at its power-up defaults regardless of
	// any skip-pattern writes the host made during the reset (§3.3.1).
	for (unsigned i = 0; i < DHV_LINE_COUNT; i++)
		set_channel_defaults(i);
	load_selftest_codes();
	csr_diag_fail = false;
	csr_master_reset = false;
	selftest_pending = false;
	refresh_indexed_dati(csr_channel & DHV_CSR_CHAN_MASK);
}

// caller holds state_mutex: a channel's post-reset state (§3.3.1): 9600 baud, 8
// data bits, one stop, no parity, receiver disabled, transmitter enabled, no
// maintenance/modem control, DMA idle.
void dhv11_c::set_channel_defaults(unsigned c)
{
	chan[c].lpr = DHV_LPR_RESET;
	chan[c].lnctrl = 0;
	chan[c].tbuffad1 = 0;
	chan[c].tbuffad2 = DHV_TBUFFAD2_TX_ENA; // TX.ENA set by master reset
	chan[c].tbuffct = 0;
	chan[c].dma_addr = 0;
	chan[c].dma_count = 0;
	chan[c].dma_pending = false;
}

// caller holds state_mutex: set the read-back value of each indexed (M) register
// from the selected channel's stored word, so a read of base+n after a write of
// base+n (same channel) returns what was written (§3.2.1).
void dhv11_c::refresh_indexed_dati(unsigned c)
{
	set_register_dati_value(reg[dhv_idx_lpr], chan[c].lpr, __func__);
	set_register_dati_value(reg[dhv_idx_lnctrl], chan[c].lnctrl, __func__);
	set_register_dati_value(reg[dhv_idx_tbuffad1], chan[c].tbuffad1, __func__);
	set_register_dati_value(reg[dhv_idx_tbuffad2], chan[c].tbuffad2, __func__);
	set_register_dati_value(reg[dhv_idx_tbuffct], chan[c].tbuffct, __func__);
	set_stat_dati();
}

// -------------------------------------------------------------------------

void dhv11_c::on_after_register_access(qunibusdevice_register_t *device_reg,
		uint8_t unibus_control, DATO_ACCESS access)
{
	UNUSED(access);
	if (qunibusadapter->line_INIT)
		return;

	pthread_mutex_lock(&state_mutex);
	bool is_dato = (unibus_control == QUNIBUS_CYCLE_DATO);
	unsigned c = csr_channel & DHV_CSR_CHAN_MASK;

	// Any bus access while a master reset is in progress means the host is driving
	// the DHV11 (polling the CSR, or writing the skip pattern through the control
	// registers), so freeze the inactivity fallback. Only CSR reads count as
	// self-test polls (below); this keeps the fallback from firing in the gap
	// between the reset and the first poll — e.g. during the skip-writer burst.
	if (csr_master_reset)
		poll_active_ticks = DHV_POLL_ACTIVE_TICKS;

	// §3.3.10.3: writing the skip pattern to a control register while a master
	// reset is in progress asks the DHV11 to bypass the self-test, so it finishes
	// quickly instead of running the full sequence.
	if (is_dato && csr_master_reset && !selftest_skip && device_reg->index != dhv_idx_csr
			&& get_register_dato_value(device_reg) == DHV_SELFTEST_SKIP_PATTERN) {
		selftest_skip = true;
		selftest_target = DHV_SKIP_POLLS;
		if (selftest_ticks > DHV_SKIP_FALLBACK_TICKS)
			selftest_ticks = DHV_SKIP_FALLBACK_TICKS;
	}

	switch (device_reg->index) {
	case dhv_idx_csr:
		if (is_dato) {
			uint16_t v = get_register_dato_value(reg[dhv_idx_csr]);
			csr_rx_ie = v & DHV_CSR_RX_IE;
			csr_tx_ie = v & DHV_CSR_TX_IE;
			csr_channel = v & DHV_CSR_CHAN_MASK;
			if ((v & DHV_CSR_MASTER_RESET) && !csr_master_reset) {
				// §3.3.1: reset every channel to its power-up state and start the
				// self-test. CSR<5> stays set — and the FIFO stays empty — until the
				// host has polled the CSR DHV_SELFTEST_POLLS times (or the inactivity
				// fallback elapses), at which point complete_selftest() loads the
				// codes and drops the bit.
				csr_master_reset = true;
				csr_diag_fail = true; // valid only after the test clears CSR<5>
				csr_tx_act = false;
				csr_tx_line = 0;
				for (unsigned i = 0; i < DHV_LINE_COUNT; i++)
					set_channel_defaults(i);
				rx_fifo.clear();
				set_rbuf_dati();
				selftest_polls = 0;
				selftest_target = DHV_SELFTEST_POLLS;
				selftest_ticks = DHV_SELFTEST_FALLBACK_TICKS;
				poll_active_ticks = 0;
				selftest_pending = true;
				selftest_skip = false;
			}
			refresh_indexed_dati(csr_channel & DHV_CSR_CHAN_MASK);
			update_csr_and_INTR();
		} else {
			// §3.2.2.1: a CSR read clears TX.ACTION.
			if (csr_master_reset) {
				// A CSR read during a master reset is a self-test poll: count it and
				// complete the self-test once the host has polled enough times that
				// CVDHA's outer-iteration count lands inside its window (see dhv11.hpp).
				if (++selftest_polls >= selftest_target) {
					complete_selftest();
					update_csr_and_INTR();
				}
			}
			if (csr_tx_act) {
				csr_tx_act = false;
				update_csr_and_INTR();
			}
		}
		break;
	case dhv_idx_rbuf:
		if (is_dato) {
			// TXCHAR: a single programmed character (§3.2.2.3).
			uint16_t v = get_register_dato_value(reg[dhv_idx_rbuf]);
			if (v & DHV_TXCHAR_VALID) {
				uint8_t ch = v & 0377;
				uint8_t m = chan_maint(c);
				if (m == DHV_MAINT_LOCAL || m == DHV_MAINT_REMOTE
						|| m == DHV_MAINT_ECHO) {
					// Maintenance loopback wraps the character back to the FIFO. LOCAL
					// ignores RX.ENA but TX.ENA still gates transmission (§3.2.2.6); echo
					// and remote need the receiver enabled.
					bool loop = (m == DHV_MAINT_LOCAL)
							? chan_tx_ena(c) : chan_rx_enabled(c);
					if (loop)
						rx_fifo_push(DHV_RBUF_DATA_VALID
								| ((uint16_t) (c & 7) << DHV_RBUF_LINE_SHIFT) | ch);
				} else if (chan_tx_ena(c) && chan[c].open) {
					tcp_line[c].send(ch);
					note_tx_activity(c);
				}
				tx_report(c);
				update_csr_and_INTR();
			}
		} else { // read pops the FIFO
			if (!rx_fifo.empty())
				rx_fifo.pop_front();
			set_rbuf_dati();
			update_csr_and_INTR();
		}
		break;
	case dhv_idx_lpr:
		if (is_dato) {
			uint16_t v = get_register_dato_value(reg[dhv_idx_lpr]);
			// A DIAG (LPR<2:1>) = 01 request runs the Background Monitor Program
			// report for this channel: push its status code to the FIFO (305 =
			// running) with the error markers set and the requesting line in <11:8>,
			// then clear the DIAG field (§3.3.10.4).
			if ((v & DHV_LPR_DIAG_MASK) == DHV_LPR_DIAG_REQUEST) {
				v &= ~DHV_LPR_DIAG_MASK;
				rx_fifo_push(DHV_RBUF_DATA_VALID | DHV_RBUF_DIAG
						| ((uint16_t) (c & 7) << DHV_RBUF_LINE_SHIFT) | DHV_BMP_RUNNING);
				update_csr_and_INTR();
			}
			chan[c].lpr = v;
			set_register_dati_value(reg[dhv_idx_lpr], chan[c].lpr, __func__);
		}
		break;
	case dhv_idx_lnctrl:
		if (is_dato) {
			chan[c].lnctrl = get_register_dato_value(reg[dhv_idx_lnctrl]);
			set_register_dati_value(reg[dhv_idx_lnctrl], chan[c].lnctrl, __func__);
		}
		break;
	case dhv_idx_tbuffad1:
		if (is_dato) {
			chan[c].tbuffad1 = get_register_dato_value(reg[dhv_idx_tbuffad1]);
			set_register_dati_value(reg[dhv_idx_tbuffad1], chan[c].tbuffad1, __func__);
		}
		break;
	case dhv_idx_tbuffad2:
		if (is_dato) {
			uint16_t v = get_register_dato_value(reg[dhv_idx_tbuffad2]);
			chan[c].tbuffad2 = v;
			// §3.2.2.8: setting TX.DMA.START launches the transfer; the DHV11
			// clears the bit before returning TX.ACTION, so it reads back 0.
			if ((v & DHV_TBUFFAD2_DMA_START) && (v & DHV_TBUFFAD2_TX_ENA)
					&& chan[c].tbuffct > 0) {
				chan[c].dma_addr = ((uint32_t) (v & DHV_TBUFFAD2_ADDR_HI) << 16)
						| chan[c].tbuffad1;
				chan[c].dma_count = chan[c].tbuffct;
				chan[c].dma_pending = true;
				pthread_cond_signal(&xmt_cond);
			}
			chan[c].tbuffad2 &= ~DHV_TBUFFAD2_DMA_START;
			set_register_dati_value(reg[dhv_idx_tbuffad2], chan[c].tbuffad2, __func__);
		}
		break;
	case dhv_idx_tbuffct:
		if (is_dato) {
			chan[c].tbuffct = get_register_dato_value(reg[dhv_idx_tbuffct]);
			set_register_dati_value(reg[dhv_idx_tbuffct], chan[c].tbuffct, __func__);
		}
		break;
	default:
		break;
	}
	pthread_mutex_unlock(&state_mutex);
}

void dhv11_c::on_power_changed(signal_edge_enum aclo_edge, signal_edge_enum dclo_edge)
{
	UNUSED(aclo_edge);
	UNUSED(dclo_edge);
	reset();
}

void dhv11_c::on_init_changed(void)
{
	if (init_asserted)
		reset();
}

void dhv11_c::reset(void)
{
	pthread_mutex_lock(&state_mutex);
	reset_unibus_registers();
	// BINIT clears the interrupt enables and, like a host master reset, runs the
	// self-test and initialization sequence (§3.3.1). Completed synchronously
	// here: the bus reset has no host handshake to observe CSR<5> in transit.
	csr_rx_ie = csr_tx_ie = csr_tx_act = false;
	csr_master_reset = false;
	csr_diag_fail = false;
	csr_tx_line = 0;
	csr_channel = 0;
	selftest_pending = false;
	selftest_skip = false;
	selftest_ticks = 0;
	selftest_polls = 0;
	selftest_target = DHV_SELFTEST_POLLS;
	poll_active_ticks = 0;
	for (unsigned i = 0; i < DHV_LINE_COUNT; i++) {
		bool was_open = tcp_line[i].is_open();
		set_channel_defaults(i);
		chan[i].open = was_open; // TCP lines survive a bus INIT
		// BINIT clears LNCTRL, so the modem-control lines it drives fall with it;
		// the carrier lamps are recomputed from live carrier below.
		rx_lamp[i].value = false;
		tx_lamp[i].value = false;
	}
	refresh_signal_lamps();
	load_selftest_codes();
	rcvintr_request.edge_detect_reset();
	xmtintr_request.edge_detect_reset();
	refresh_indexed_dati(0);
	update_csr_and_INTR();
	pthread_mutex_unlock(&state_mutex);
}

// -------------------------------------------------------------------------
// DMA helper (blocking, on the transmit worker thread)

bool dhv11_c::dma_read_words(uint32_t addr, uint16_t *buffer, unsigned wordcount)
{
	qunibusadapter->DMA(dma_request, true, QUNIBUS_CYCLE_DATI, addr, buffer, wordcount);
	return dma_request.success;
}

// -------------------------------------------------------------------------
// workers

void dhv11_c::worker_wake(void)
{
	pthread_mutex_lock(&state_mutex);
	pthread_cond_signal(&xmt_cond);
	pthread_mutex_unlock(&state_mutex);
}

void dhv11_c::worker_rcv(void)
{
	timeout_c timeout;
	worker_init_realtime_priority(rt_device);

	while (!workers_terminate) {
		timeout.wait_us(500);
		if (qunibusadapter->line_INIT)
			continue;

		pthread_mutex_lock(&state_mutex);
		if (selftest_pending) {
			// While the host is actively polling the CSR the poll-count model in
			// on_after_register_access completes the self-test; freeze this
			// wall-clock fallback so it never cuts that model short. When no poll
			// has arrived recently (a timer-driven host, or none at all), the
			// fallback clears CSR<5> after the manual's self-test duration.
			if (poll_active_ticks > 0)
				poll_active_ticks--;
			else if (--selftest_ticks <= 0) {
				DEBUG("self-test inactivity fallback fired after %u CSR polls (skip=%d)",
						(unsigned) selftest_polls, (int) selftest_skip);
				complete_selftest();
				update_csr_and_INTR();
			}
		}
		set_stat_dati();
		refresh_signal_lamps();
		pthread_mutex_unlock(&state_mutex);

		for (unsigned i = 0; i < DHV_LINE_COUNT; i++) {
			if (!chan[i].open || !chan_rx_enabled(i))
				continue;
			uint8_t m = chan_maint(i);
			if (m == DHV_MAINT_LOCAL || m == DHV_MAINT_REMOTE)
				continue; // wire data is ignored while looping back
			uint8_t ch;
			while (tcp_line[i].poll_rcv(&ch)) {
				note_rx_activity(i);
				pthread_mutex_lock(&state_mutex);
				rx_fifo_push(DHV_RBUF_DATA_VALID
						| ((uint16_t) (i & 7) << DHV_RBUF_LINE_SHIFT) | ch);
				update_csr_and_INTR();
				bool echo = (chan_maint(i) == DHV_MAINT_ECHO) && chan[i].open;
				pthread_mutex_unlock(&state_mutex);
				if (echo)
					tcp_line[i].send(ch); // automatic echo retransmits the character
			}
		}
	}
}

void dhv11_c::worker_xmt(void)
{
	worker_init_realtime_priority(rt_device);
	uint16_t dma_buf[4096];

	pthread_mutex_lock(&state_mutex);
	while (!workers_terminate) {
		// find a channel with a pending transmit DMA
		int line = -1;
		for (unsigned i = 0; i < DHV_LINE_COUNT; i++)
			if (chan[i].dma_pending && chan[i].dma_count > 0) {
				line = (int) i;
				break;
			}
		if (line < 0) {
			pthread_cond_wait(&xmt_cond, &state_mutex);
			continue;
		}

		// snapshot the request, then run the DMA without the state lock
		uint32_t addr = chan[line].dma_addr;
		uint16_t count = chan[line].dma_count;
		chan[line].dma_pending = false;
		bool open = chan[line].open;
		uint8_t maint = chan_maint(line);
		bool rx_ena = chan_rx_enabled(line);
		pthread_mutex_unlock(&state_mutex);

		unsigned words = (count + 1) / 2;
		if (words > 4096)
			words = 4096; // one DMA burst; TODO chain longer buffers per manual
		bool ok = dma_read_words(addr & ~1u, dma_buf, words);
		bool loopback = (maint == DHV_MAINT_LOCAL || maint == DHV_MAINT_REMOTE);
		unsigned n = count;
		if (n > words * 2)
			n = words * 2;
		const uint8_t *bytes = (const uint8_t *) dma_buf; // low byte first per word
		// wire output runs without the state lock (the blocking-I/O rule); the
		// maintenance loopback wraps the same bytes back to the FIFO under lock.
		if (ok && !loopback && open) {
			for (unsigned k = 0; k < n; k++)
				tcp_line[line].send(bytes[k]);
			if (n > 0)
				note_tx_activity(line);
		}

		pthread_mutex_lock(&state_mutex);
		if (ok && loopback)
			for (unsigned k = 0; k < n; k++)
				// remote loopback still needs the receiver enabled; local does not.
				if (maint == DHV_MAINT_LOCAL || rx_ena)
					rx_fifo_push(DHV_RBUF_DATA_VALID
							| ((uint16_t) (line & 7) << DHV_RBUF_LINE_SHIFT)
							| bytes[k]);
		chan[line].dma_count = 0;
		chan[line].tbuffct = 0; // whole buffer sent: nothing left to transfer
		chan[line].dma_addr = (addr + count) & 0x3fffff;
		set_register_dati_value(reg[dhv_idx_tbuffct], 0, __func__);
		tx_report(line); // transmit complete: raise TX.ACTION for this line
		update_csr_and_INTR();
	}
	pthread_mutex_unlock(&state_mutex);
}

void dhv11_c::worker(unsigned instance)
{
	if (instance == 0)
		worker_rcv();
	else
		worker_xmt();
}
