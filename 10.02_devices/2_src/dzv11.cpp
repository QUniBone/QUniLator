/* dzv11.cpp: DZV11/DZQ11 4-line asynchronous serial multiplexer

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see any device source header for the full text.

   See dzv11.hpp. The register model follows the standard DZ11/DZV11 set:
   char-at-a-time receive through a silo, a transmit scanner that presents TRDY
   for one enabled line at a time, and modem status driven by each line's TCP
   carrier. No DZ11 manual is in this repo, so the model is not yet XXDP-checked.
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
	reg_csr->writable_bits = 0xffff;

	reg_rbuf_lpr = &(this->registers[dz_idx_rbuf_lpr]);
	strcpy(reg_rbuf_lpr->name, "RBUF"); // read RBUF, write LPR
	reg_rbuf_lpr->active_on_dati = true;  // read pops the silo
	reg_rbuf_lpr->active_on_dato = true;  // write is LPR
	reg_rbuf_lpr->reset_value = 0;
	reg_rbuf_lpr->writable_bits = 0xffff;

	reg_tcr = &(this->registers[dz_idx_tcr]);
	strcpy(reg_tcr->name, "TCR");
	reg_tcr->active_on_dati = false;
	reg_tcr->active_on_dato = true;
	reg_tcr->reset_value = 0;
	reg_tcr->writable_bits = 0xffff;

	reg_msr_tdr = &(this->registers[dz_idx_msr_tdr]);
	strcpy(reg_msr_tdr->name, "MSR"); // read MSR, write TDR
	reg_msr_tdr->active_on_dati = false; // modem status polled
	reg_msr_tdr->active_on_dato = true;  // write is TDR
	reg_msr_tdr->reset_value = 0;
	reg_msr_tdr->writable_bits = 0xffff;

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
	}
	return qunibusdevice_c::on_param_changed(param);
}

void dzv11_c::open_lines(void)
{
	for (unsigned i = 0; i < DZ_LINE_COUNT; i++) {
		line_open[i] = false;
		if (tcp_role[i].value.empty())
			continue;
		serial_tcp_line_c &ln = tcp_line[i];
		if (tcp_role[i].value == "listen")
			ln.role = serial_tcp_line_c::ROLE_LISTEN;
		else if (tcp_role[i].value == "connect")
			ln.role = serial_tcp_line_c::ROLE_CONNECT;
		else {
			WARNING("line %u: tcp_role must be listen/connect, got \"%s\"", i,
					tcp_role[i].value.c_str());
			continue;
		}
		ln.host = tcp_host[i].value;
		ln.port = (uint16_t) tcp_port[i].value;
		char lbl[32];
		snprintf(lbl, sizeof lbl, "dzv11.%u", i);
		ln.log_label = lbl;
		ln.verbose = true;
		if (ln.open())
			line_open[i] = true;
		else
			WARNING("line %u: cannot open TCP transport (port %u)", i,
					(unsigned) tcp_port[i].value);
	}
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
	open_lines();
	for (unsigned i = 0; i < DZ_LINE_COUNT; i++) {
		tcp_role[i].readonly = true;
		tcp_host[i].readonly = true;
		tcp_port[i].readonly = true;
	}
	return true;
}

void dzv11_c::on_after_uninstall(void)
{
	close_lines();
	for (unsigned i = 0; i < DZ_LINE_COUNT; i++) {
		tcp_role[i].readonly = false;
		tcp_host[i].readonly = false;
		tcp_port[i].readonly = false;
	}
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
void dzv11_c::set_rbuf_dati(void)
{
	uint16_t val = 0;
	if (!silo.empty())
		val = silo.front(); // already carries VALID + error + line + char
	set_register_dati_value(reg_rbuf_lpr, val, __func__);
}

// caller holds state_mutex
void dzv11_c::set_msr_dati(void)
{
	// carrier-detect per line = that line has a connected TCP client
	uint16_t val = 0;
	for (unsigned i = 0; i < DZ_LINE_COUNT; i++)
		if (line_open[i] && tcp_line[i].client_connected())
			val |= (1u << i);
	set_register_dati_value(reg_msr_tdr, val, __func__);
}

void dzv11_c::eval_csr_dato(void)
{
	uint16_t val = get_register_dato_value(reg_csr);
	csr_tie = val & DZ_CSR_TIE;
	csr_sae = val & DZ_CSR_SAE;
	csr_rie = val & DZ_CSR_RIE;
	csr_maint = val & DZ_CSR_MAINT;
	bool new_mse = val & DZ_CSR_MSE;

	if (val & DZ_CSR_CLR) {
		// master clear: silo, scanner, status
		silo.clear();
		silo_alarm_count = 0;
		csr_sa = false;
		csr_trdy = false;
		csr_tline = 0;
		tdr_pending = false;
		set_rbuf_dati();
	}
	csr_mse = new_mse;
}

void dzv11_c::eval_lpr_dato(void)
{
	uint16_t val = get_register_dato_value(reg_rbuf_lpr);
	unsigned line = val & DZ_LPR_LINE;
	if (line < DZ_LINE_COUNT)
		rx_enabled[line] = (val & DZ_LPR_RXON) != 0;
}

void dzv11_c::eval_tcr_dato(void)
{
	uint16_t val = get_register_dato_value(reg_tcr);
	for (unsigned i = 0; i < DZ_LINE_COUNT; i++)
		tx_enabled[i] = (val & (1u << i)) != 0;
	// DTR bits (8..11) drive modem control; not modelled on the TCP transport.
}

void dzv11_c::eval_tdr_dato(void)
{
	uint16_t val = get_register_dato_value(reg_msr_tdr);
	// the character is transmitted on the line the scanner currently presents
	tdr_char = val & DZ_TDR_DATA;
	tdr_line = csr_tline & 3;
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
			update_csr_and_INTR();
			pthread_cond_signal(&xmt_cond); // MSE/scan may have changed
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
			pthread_cond_signal(&xmt_cond);
		}
		break;
	case dz_idx_msr_tdr:
		if (unibus_control == QUNIBUS_CYCLE_DATO) {
			eval_tdr_dato();
			pthread_cond_signal(&xmt_cond);
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
	silo.clear();
	silo_alarm_count = 0;
	tdr_pending = false;
	rcvintr_request.edge_detect_reset();
	xmtintr_request.edge_detect_reset();
	set_rbuf_dati();
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
				uint16_t entry = DZ_RBUF_VALID | ((uint16_t) (i & 3) << DZ_RBUF_RLINE_SHIFT) | c;
				if (silo.size() >= 64) {
					// silo full: mark overrun on the newest kept entry
					entry |= DZ_RBUF_OVR;
					silo.pop_back();
				}
				silo.push_back(entry);
				if (++silo_alarm_count >= 16) {
					silo_alarm_count = 0;
					if (csr_sae)
						csr_sa = true;
				}
				set_rbuf_dati();
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
		if (!csr_mse) {
			pthread_cond_wait(&xmt_cond, &state_mutex);
			continue;
		}
		if (!csr_trdy) {
			if (select_next_tx_line()) {
				csr_trdy = true;
				update_csr_and_INTR();
			} else {
				// no line ready to transmit: wait for TCR/CSR change
				pthread_cond_wait(&xmt_cond, &state_mutex);
				continue;
			}
		}
		if (csr_trdy && tdr_pending) {
			uint8_t ch = tdr_char;
			uint8_t ln = tdr_line;
			bool maint = csr_maint;
			tdr_pending = false;
			csr_trdy = false; // character taken; scanner will re-present
			update_csr_and_INTR();
			if (maint) {
				// maintenance loopback: the transmitted char is delivered to
				// this line's receiver instead of the wire
				uint16_t entry = DZ_RBUF_VALID
						| ((uint16_t) (ln & 3) << DZ_RBUF_RLINE_SHIFT) | ch;
				silo.push_back(entry);
				set_rbuf_dati();
				update_csr_and_INTR();
			} else {
				pthread_mutex_unlock(&state_mutex);
				if (ln < DZ_LINE_COUNT && line_open[ln])
					tcp_line[ln].send(ch);
				pthread_mutex_lock(&state_mutex);
			}
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
