/* dzv11.cpp: DZV11/DZQ11 4-line asynchronous serial multiplexer

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see any device source header for the full text.

   See dzv11.hpp. The register model follows the DZV11 set from EK-DZV11-TM-001:
   char-at-a-time receive through a silo, a transmit scanner that presents TRDY
   for one enabled line at a time, and modem status driven by each line's TCP
   carrier. It passes the complete XXDP VDZAD3 (CVDZA) diagnostic with no errors.
*/

#include <cstring>
#include <cstdio>

#include "logger.hpp"
#include "timeout.hpp"
#include "qunibusadapter.hpp"
#include "qunibus.h"
#include "dzv11.hpp"

dzv11_c::dzv11_c() : qunibusdevice_c()
{
	set_workers_count(2); // 0 = receiver scan, 1 = transmitter scan

	name.value = "dzv11";
	type_name.value = "dzv11_c";
	log_label = "dzv11";

	set_default_bus_params(DZV11_ADDR, DZV11_SLOT, DZV11_VECTOR, DZV11_LEVEL);

	register_count = dz_idx_count;

	reg_csr = &(this->registers[dz_idx_csr]);
	strcpy(reg_csr->name, "CSR");
	reg_csr->active_on_dati = false; // status polled
	reg_csr->active_on_dato = true;
	reg_csr->reset_value = 0;
	// TM Table 3-2: read/write bits are MAINT(3), CLR(4), MSE(5), RIE(6),
	// SAE(12) and TIE(14). RDONE(7), TLINE(8-9), SA(13), TRDY(15) are read-only
	// (recomputed in update_csr_and_INTR); 00-02 and 10-11 read as 0.
	reg_csr->writable_bits = 0050170;

	reg_rbuf_lpr = &(this->registers[dz_idx_rbuf_lpr]);
	strcpy(reg_rbuf_lpr->name, "RBUF"); // read RBUF, write LPR
	reg_rbuf_lpr->active_on_dati = true;  // read pops the silo
	reg_rbuf_lpr->active_on_dato = true;  // write is LPR
	reg_rbuf_lpr->reset_value = 0;
	// LPR (write) is line(0-1), char length(3-4), stop(5), parity(6-7),
	// baud(8-11), receiver enable(12); the read side returns the silo entry.
	reg_rbuf_lpr->writable_bits = 0017777;

	reg_tcr = &(this->registers[dz_idx_tcr]);
	strcpy(reg_tcr->name, "TCR");
	reg_tcr->active_on_dati = false;
	reg_tcr->active_on_dato = true;
	reg_tcr->reset_value = 0;
	// TM 3.2.4: line-enable(0-3) and DTR(8-11) are read/write; 04-07 and 12-15
	// are unused and read as 0.
	reg_tcr->writable_bits = 0007417;

	reg_msr_tdr = &(this->registers[dz_idx_msr_tdr]);
	strcpy(reg_msr_tdr->name, "MSR"); // read MSR, write TDR
	reg_msr_tdr->active_on_dati = false; // modem status polled
	reg_msr_tdr->active_on_dato = true;  // write is TDR
	reg_msr_tdr->reset_value = 0;
	// TDR (write) is transmit data(0-7) and break(8-11); the read side returns
	// the modem status register (ring 0-3, carrier 8-11).
	reg_msr_tdr->writable_bits = 0007777;

	memset(rx_enabled, 0, sizeof rx_enabled);
	memset(tx_enabled, 0, sizeof tx_enabled);
	memset(line_open, 0, sizeof line_open);
	silo_alarm_count = 0;
	tdr_pending = false;
	tdr_char = 0;
	tdr_line = 0;
	csr_tie = csr_sae = csr_sa = csr_rie = csr_mse = csr_maint = false;
	csr_tline = 0;
	csr_trdy = false;

	// Usable defaults: every line listens on a distinct TCP port, so an enabled
	// device accepts a client without any per-line configuration. tcp_host stays
	// empty (used only for connect-out).
	for (unsigned i = 0; i < DZ_LINE_COUNT; i++) {
		tcp_role[i].value = "listen";
		tcp_port[i].value = DZV11_TCP_PORT_BASE + i;
	}
}

dzv11_c::~dzv11_c()
{
}

bool dzv11_c::on_param_changed(parameter_c *param)
{
	if (param == &priority_slot) {
		rcvintr_request.set_priority_slot(priority_slot.new_value);
		xmtintr_request.set_priority_slot(priority_slot.new_value + 1);
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
		for (unsigned i = 0; i < DZ_LINE_COUNT; i++) {
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

// (Re)open one line's transport. The open flag is flipped under state_mutex so
// the workers stop touching the line, but the blocking socket close()/open()
// runs outside the lock — the same "never hold state_mutex across blocking I/O"
// rule the transmit path follows.
void dzv11_c::open_line(unsigned i, const std::string &role, const std::string &host,
		uint16_t port)
{
	pthread_mutex_lock(&state_mutex);
	bool was_open = line_open[i];
	line_open[i] = false;
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
	else {
		WARNING("line %u: tcp_role must be listen/connect, got \"%s\"", i, role.c_str());
		return;
	}
	ln.host = host;
	ln.port = port;
	char lbl[32];
	snprintf(lbl, sizeof lbl, "dzv11.%u", i);
	ln.log_label = lbl;
	ln.verbose = true;
	if (ln.open()) {
		pthread_mutex_lock(&state_mutex);
		line_open[i] = true;
		pthread_mutex_unlock(&state_mutex);
	} else {
		WARNING("line %u: cannot open TCP transport (port %u)", i, (unsigned) port);
	}
}

void dzv11_c::open_lines(void)
{
	for (unsigned i = 0; i < DZ_LINE_COUNT; i++)
		open_line(i, tcp_role[i].value, tcp_host[i].value, (uint16_t) tcp_port[i].value);
}

void dzv11_c::close_lines(void)
{
	for (unsigned i = 0; i < DZ_LINE_COUNT; i++)
		if (line_open[i]) {
			tcp_line[i].close();
			line_open[i] = false;
		}
}

bool dzv11_c::on_before_install(void)
{
	// The TCP transport params stay writable while installed so a line's port or
	// role can be retuned live (see on_param_changed); they do not touch the bus
	// registration. The base class locks the bus params (base_addr/slot/vector/
	// level) on enable.
	open_lines();
	return true;
}

void dzv11_c::on_after_uninstall(void)
{
	close_lines();
}

// -------------------------------------------------------------------------
// CSR / interrupt evaluation

bool dzv11_c::get_rcv_intr_level(void)
{
	return csr_rie && !silo.empty();
}

bool dzv11_c::get_xmt_intr_level(void)
{
	return csr_tie && csr_trdy;
}

// caller holds state_mutex
void dzv11_c::update_csr_and_INTR(void)
{
	bool rdone = !silo.empty();
	uint16_t val = (csr_trdy ? DZ_CSR_TRDY : 0) | (csr_tie ? DZ_CSR_TIE : 0)
			| (csr_sa ? DZ_CSR_SA : 0) | (csr_sae ? DZ_CSR_SAE : 0)
			| ((uint16_t) (csr_tline & 7) << DZ_CSR_TLINE_SHIFT)
			| (rdone ? DZ_CSR_RDONE : 0) | (csr_rie ? DZ_CSR_RIE : 0)
			| (csr_mse ? DZ_CSR_MSE : 0) | (csr_maint ? DZ_CSR_MAINT : 0);
	set_register_dati_value(reg_csr, val, __func__);

	// The DZV11 arbitrates its two interrupts internally with the receiver silo
	// above the transmitter (TM 3.2.1: the receiver channel has interrupt
	// priority). While the receiver interrupt condition stands, the transmitter
	// interrupt is held off; it is presented once the CPU drains the silo and the
	// receiver condition clears. This is what lets a program that enables TIE with
	// Transmitter Ready already set, then enables RIE, take the receiver interrupt
	// first: the transmitter interrupt does not reach the CPU ahead of it.
	bool rcv_level = get_rcv_intr_level();
	bool xmt_level = get_xmt_intr_level() && !rcv_level;

	intr_request_c::interrupt_edge_enum rcv_edge = rcvintr_request.edge_detect(rcv_level);
	intr_request_c::interrupt_edge_enum xmt_edge = xmtintr_request.edge_detect(xmt_level);

	// Retract before asserting: a falling edge frees the level's arbitration slot,
	// so when the transmitter interrupt yields to the receiver, cancelling the
	// transmitter request lets the receiver request activate at once.
	if (rcv_edge == intr_request_c::INTERRUPT_EDGE_FALLING)
		qunibusadapter->cancel_INTR(rcvintr_request);
	if (xmt_edge == intr_request_c::INTERRUPT_EDGE_FALLING)
		qunibusadapter->cancel_INTR(xmtintr_request);
	if (rcv_edge == intr_request_c::INTERRUPT_EDGE_RAISING)
		qunibusadapter->INTR(rcvintr_request, NULL, 0);
	if (xmt_edge == intr_request_c::INTERRUPT_EDGE_RAISING)
		qunibusadapter->INTR(xmtintr_request, NULL, 0);
}

// caller holds state_mutex
void dzv11_c::set_rbuf_dati(void)
{
	uint16_t val = 0;
	if (!silo.empty())
		val = silo.front(); // already carries VALID + error + line + char
	set_register_dati_value(reg_rbuf_lpr, val, __func__);
}

// caller holds state_mutex; enqueue a received character into the silo FIFO with
// its line number and any error bits (TM Table 3-3). A full silo flags Overrun,
// and every 16th entry raises the Silo Alarm when armed (SAE).
void dzv11_c::silo_push(uint8_t line, uint8_t ch, uint16_t err_bits)
{
	uint16_t entry = DZ_RBUF_VALID
			| ((uint16_t) (line & 3) << DZ_RBUF_RLINE_SHIFT) | err_bits | ch;
	if (silo.size() >= 64) {
		entry |= DZ_RBUF_OVR; // silo full: mark overrun on the newest kept entry
		silo.pop_back();
	}
	silo.push_back(entry);
	if (++silo_alarm_count >= 16) {
		silo_alarm_count = 0;
		if (csr_sae)
			csr_sa = true;
	}
	set_rbuf_dati();
}

// caller holds state_mutex
void dzv11_c::set_msr_dati(void)
{
	// carrier-detect per line = that line has a connected TCP client. The
	// driver reads carrier from the MSR high byte to release a line's open(),
	// so a connected client asserts CD there; without it getty blocks forever.
	uint16_t val = 0;
	for (unsigned i = 0; i < DZ_LINE_COUNT; i++)
		if (line_open[i] && tcp_line[i].client_connected())
			val |= (1u << (DZ_MSR_CD_SHIFT + i));
	if (val == msr_dati_value)
		return;  // carrier unchanged: skip the register write and its log line
	msr_dati_value = val;
	set_register_dati_value(reg_msr_tdr, val, __func__);
}

void dzv11_c::eval_csr_dato(void)
{
	uint16_t val = get_register_dato_value(reg_csr);

	if (val & DZ_CSR_CLR) {
		// Master Clear (CSR 04) generates "initialize": TM 3.2.1 — "All bits in
		// the CSR are cleared by ... setting device Master Clear", along with
		// the silos, UARTs and the transmitter line-enables (TCR low byte).
		// The other bits in this same write do not survive it. RBUF bits 00-14,
		// the TCR high byte (DTR), and the MSR are preserved (handled by BINIT
		// in reset(); not touched here).
		csr_tie = csr_sae = csr_rie = csr_maint = csr_mse = false;
		csr_sa = false;
		csr_trdy = false;
		csr_tline = 0;
		tdr_pending = false;
		silo.clear();
		silo_alarm_count = 0;
		memset(tx_enabled, 0, sizeof tx_enabled);
		tx_break = 0;  // TDR/break register cleared by Master Clear (TM 3.2.6)
		// clear the TCR line-enables in the read-back; the DTR high byte survives
		set_register_dati_value(reg_tcr,
				reg_tcr->pru_iopage_register->value & 0007400, __func__);
		set_rbuf_dati();
		return;
	}

	csr_tie = val & DZ_CSR_TIE;
	csr_sae = val & DZ_CSR_SAE;
	csr_rie = val & DZ_CSR_RIE;
	csr_maint = val & DZ_CSR_MAINT;
	bool was_mse = csr_mse;
	csr_mse = val & DZ_CSR_MSE;
	if (was_mse && !csr_mse) {
		// TM 3.2.1 (CSR 05): clearing MSE clears the received-character silos
		// (and with them Receiver Done / the Silo Alarm).
		silo.clear();
		silo_alarm_count = 0;
		csr_sa = false;
		set_rbuf_dati();
	}
}

void dzv11_c::eval_lpr_dato(void)
{
	uint16_t val = get_register_dato_value(reg_rbuf_lpr);
	unsigned line = val & DZ_LPR_LINE;
	if (line < DZ_LINE_COUNT) {
		rx_enabled[line] = (val & DZ_LPR_RXON) != 0;
		parity_enable[line] = (val & DZ_LPR_PENABLE) != 0;
		odd_parity[line] = (val & DZ_LPR_ODD) != 0;
	}
}

void dzv11_c::eval_tcr_dato(void)
{
	uint16_t val = get_register_dato_value(reg_tcr);
	for (unsigned i = 0; i < DZ_LINE_COUNT; i++)
		tx_enabled[i] = (val & (1u << i)) != 0;
	// TCR line-enable (00-03) and DTR (08-11) are read/write (TM Table 3-4), so
	// the register reads back what was written. val is already masked to those
	// bits by writable_bits. DTR does not drive the TCP transport.
	set_register_dati_value(reg_tcr, val, __func__);
}

void dzv11_c::eval_tdr_dato(void)
{
	uint16_t val = get_register_dato_value(reg_msr_tdr);
	uint8_t ch = val & DZ_TDR_DATA;
	uint8_t line = csr_tline & 3;

	// TDR high byte (08-11) is the break control register (TM 3.2.6): a set bit
	// forces that line's output to space until cleared.
	uint8_t new_break = (uint8_t) ((val >> DZ_TDR_BREAK_SHIFT) & 0017);
	uint8_t rising_break = new_break & ~tx_break;
	tx_break = new_break;

	if (csr_maint) {
		// Maintenance wrap-around (TM 5.4.11): the transmitter output is routed
		// back to the line's receiver. The receiver only assembles a character
		// into the silo when it is enabled (LPR 12); a disabled receiver drops
		// the wrapped data just as it drops line data.
		if (rising_break) {
			// A break holds the line at space, so the receiver frames a null
			// character with a framing error (TM RBUF 13), plus a parity error
			// when odd parity is enabled (the all-zero frame's parity bit fails
			// the odd check). Report it on each line just put into break. The
			// break register is independent of the transmitter scanner, so it
			// does not consume Transmitter Ready.
			for (unsigned i = 0; i < DZ_LINE_COUNT; i++)
				if ((rising_break & (1u << i)) && rx_enabled[i]) {
					uint16_t err = DZ_RBUF_FRAME;
					if (parity_enable[i] && odd_parity[i])
						err |= DZ_RBUF_PAR;
					silo_push((uint8_t) i, 0, err);
				}
			return;
		}
		// A same-line wrap has matching format: the character returns cleanly.
		// Deliver it in step with the TDR load so a program polling Receiver
		// Done right after sees it; the transmitter took the character, so TRDY
		// drops and the scanner re-presents it for the next one.
		if (rx_enabled[line])
			silo_push(line, ch, 0);
		csr_trdy = false;
		return;
	}
	tdr_char = ch;
	tdr_line = line;
	tdr_pending = true;
}

// caller holds state_mutex; pick the next enabled+open line for the scanner
bool dzv11_c::select_next_tx_line(void)
{
	for (unsigned n = 0; n < DZ_LINE_COUNT; n++) {
		uint8_t i = (uint8_t) ((csr_tline + 1 + n) % DZ_LINE_COUNT);
		if (tx_enabled[i]) {
			csr_tline = i;
			return true;
		}
	}
	return false;
}

// caller holds state_mutex; advance the transmit-ready (TRDY) state. TRDY drops
// when the scanner is off or the line it was presented for was disabled without
// a character being loaded, and appears for the next ready line otherwise. The
// hardware scanner presents TRDY within ~1.4 us of a line becoming ready, so
// this runs synchronously from the register-write path as well as from
// worker_xmt: a diagnostic's tight "enable line, poll TRDY" loop reads TRDY
// before its next access, which the worker thread's scheduling latency misses.
void dzv11_c::scan_tx_trdy(void)
{
	if (!csr_mse) {
		if (csr_trdy) {
			csr_trdy = false;
			update_csr_and_INTR();
		}
		return;
	}
	if (csr_trdy && !tdr_pending && !tx_enabled[csr_tline]) {
		csr_trdy = false;
		update_csr_and_INTR();
	}
	if (!csr_trdy && select_next_tx_line()) {
		csr_trdy = true;
		update_csr_and_INTR();
	}
}

// -------------------------------------------------------------------------
// register access

void dzv11_c::on_after_register_access(qunibusdevice_register_t *device_reg,
		uint8_t unibus_control, DATO_ACCESS access)
{
	UNUSED(access);
	if (qunibusadapter->line_INIT)
		return;

	pthread_mutex_lock(&state_mutex);
	switch (device_reg->index) {
	case dz_idx_csr:
		if (unibus_control == QUNIBUS_CYCLE_DATO) {
			eval_csr_dato();
			scan_tx_trdy();  // MSE just changed: present or drop TRDY now
			update_csr_and_INTR();
			pthread_cond_signal(&xmt_cond);
		}
		break;
	case dz_idx_rbuf_lpr:
		if (unibus_control == QUNIBUS_CYCLE_DATO) {
			eval_lpr_dato();
		} else { // DATI: the CPU consumed the top-of-silo entry
			if (!silo.empty())
				silo.pop_front();
			csr_sa = false;
			silo_alarm_count = 0;
			set_rbuf_dati();
			update_csr_and_INTR();
		}
		break;
	case dz_idx_tcr:
		if (unibus_control == QUNIBUS_CYCLE_DATO) {
			eval_tcr_dato();
			scan_tx_trdy();  // a newly enabled line becomes ready at once
			pthread_cond_signal(&xmt_cond);
		}
		break;
	case dz_idx_msr_tdr:
		if (unibus_control == QUNIBUS_CYCLE_DATO) {
			eval_tdr_dato();     // maint loopback delivers synchronously
			scan_tx_trdy();      // re-present TRDY for the next character
			update_csr_and_INTR();
			pthread_cond_signal(&xmt_cond); // wake worker for a wire (non-maint) send
		}
		break;
	default:
		break;
	}
	pthread_mutex_unlock(&state_mutex);
}

void dzv11_c::on_power_changed(signal_edge_enum aclo_edge, signal_edge_enum dclo_edge)
{
	UNUSED(aclo_edge);
	UNUSED(dclo_edge);
	reset();
}

void dzv11_c::on_init_changed(void)
{
	if (init_asserted)
		reset();
}

void dzv11_c::reset(void)
{
	pthread_mutex_lock(&state_mutex);
	reset_unibus_registers();
	csr_tie = csr_sae = csr_sa = csr_rie = csr_mse = csr_maint = false;
	csr_tline = 0;
	csr_trdy = false;
	memset(rx_enabled, 0, sizeof rx_enabled);
	memset(tx_enabled, 0, sizeof tx_enabled);
	memset(parity_enable, 0, sizeof parity_enable);
	memset(odd_parity, 0, sizeof odd_parity);
	tx_break = 0;  // TDR/break register cleared by BINIT (TM 3.2.6)
	silo.clear();
	silo_alarm_count = 0;
	tdr_pending = false;
	rcvintr_request.edge_detect_reset();
	xmtintr_request.edge_detect_reset();
	set_rbuf_dati();
	msr_dati_value = 0xffff;  // register was just zeroed; force a re-write
	set_msr_dati();
	update_csr_and_INTR();
	pthread_mutex_unlock(&state_mutex);
}

// -------------------------------------------------------------------------
// workers

void dzv11_c::worker_wake(void)
{
	pthread_mutex_lock(&state_mutex);
	pthread_cond_signal(&xmt_cond);
	pthread_mutex_unlock(&state_mutex);
}

// receiver scan: poll every rx-enabled, open line for a byte
void dzv11_c::worker_rcv(void)
{
	timeout_c timeout;
	worker_init_realtime_priority(rt_device);

	while (!workers_terminate) {
		timeout.wait_us(500);
		if (qunibusadapter->line_INIT)
			continue;

		pthread_mutex_lock(&state_mutex);
		bool scanning = csr_mse;
		set_msr_dati(); // refresh modem/carrier status while we hold the lock
		pthread_mutex_unlock(&state_mutex);
		if (!scanning)
			continue;

		for (unsigned i = 0; i < DZ_LINE_COUNT; i++) {
			if (!rx_enabled[i] || !line_open[i])
				continue;
			uint8_t c;
			while (tcp_line[i].poll_rcv(&c)) {
				pthread_mutex_lock(&state_mutex);
				silo_push((uint8_t) i, c, 0);
				update_csr_and_INTR();
				pthread_mutex_unlock(&state_mutex);
			}
		}
	}
}

// transmitter scan: present TRDY for one ready line, transmit the CPU's char
void dzv11_c::worker_xmt(void)
{
	worker_init_realtime_priority(rt_device);

	pthread_mutex_lock(&state_mutex);
	while (!workers_terminate) {
		// present TRDY for a ready line, or drop it if the scanner went off or
		// the presented line was disabled. The register-write path calls this
		// too; here it catches a client (dis)connecting between accesses.
		scan_tx_trdy();
		if (!csr_trdy) {
			// scanner off, or no enabled line: wait for a CSR/TCR change
			pthread_cond_wait(&xmt_cond, &state_mutex);
			continue;
		}
		if (tdr_pending) {
			// wire (non-maint) transmit; maint loopback is delivered
			// synchronously in eval_tdr_dato and never sets tdr_pending.
			uint8_t ch = tdr_char;
			uint8_t ln = tdr_line;
			tdr_pending = false;
			csr_trdy = false; // character taken; scanner will re-present
			update_csr_and_INTR();
			pthread_mutex_unlock(&state_mutex);
			if (ln < DZ_LINE_COUNT && line_open[ln])
				tcp_line[ln].send(ch);
			pthread_mutex_lock(&state_mutex);
			continue;
		}
		// TRDY presented, waiting for the CPU to write TDR
		pthread_cond_wait(&xmt_cond, &state_mutex);
	}
	pthread_mutex_unlock(&state_mutex);
}

void dzv11_c::worker(unsigned instance)
{
	if (instance == 0)
		worker_rcv();
	else
		worker_xmt();
}
