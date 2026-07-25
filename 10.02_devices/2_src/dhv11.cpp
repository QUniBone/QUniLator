/* dhv11.cpp: DHV11/DHQ11 8-line async mux with DMA transmit

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see any device source header for the full text.

   See dhv11.hpp — including the fidelity note: the register/bit model is built
   from the standard DHV11 register set (no EK-DHV11-UG in the repo) and is not
   yet XXDP-validated. The DMA transmit engine and interrupt paths are real; the
   trigger/status bit assignments are the modeled parts to reconcile with the
   manual. Concurrency follows the DELQA rule: the transmit worker never holds
   state_mutex across a blocking DMA call.
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
	for (unsigned i = 0; i < dhv_idx_count; i++) {
		reg[i] = &(this->registers[i]);
		strcpy(reg[i]->name, rnames[i]);
		reg[i]->active_on_dati = false;
		reg[i]->active_on_dato = true;
		reg[i]->reset_value = 0;
		reg[i]->writable_bits = 0xffff;
	}
	// RBUF read pops the receive FIFO; it is read-only (a writable register
	// active on DATI but passive on DATO is rejected by register_device).
	reg[dhv_idx_rbuf]->active_on_dati = true;
	reg[dhv_idx_rbuf]->active_on_dato = false;
	reg[dhv_idx_rbuf]->writable_bits = 0;
	// STAT is a read-only status word kept current by the worker
	reg[dhv_idx_stat]->active_on_dato = false;
	reg[dhv_idx_stat]->writable_bits = 0;

	memset(chan, 0, sizeof chan);
	csr_rx_ie = csr_tx_ie = csr_tx_act = false;
	csr_channel = 0;
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
	}
	return qunibusdevice_c::on_param_changed(param);
}

void dhv11_c::open_lines(void)
{
	for (unsigned i = 0; i < DHV_LINE_COUNT; i++) {
		chan[i].open = false;
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
		snprintf(lbl, sizeof lbl, "dhv11.%u", i);
		ln.log_label = lbl;
		ln.verbose = true;
		if (ln.open())
			chan[i].open = true;
		else
			WARNING("line %u: cannot open TCP transport (port %u)", i,
					(unsigned) tcp_port[i].value);
	}
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
	open_lines();
	for (unsigned i = 0; i < DHV_LINE_COUNT; i++) {
		tcp_role[i].readonly = true;
		tcp_host[i].readonly = true;
		tcp_port[i].readonly = true;
	}
	return true;
}

void dhv11_c::on_after_uninstall(void)
{
	close_lines();
	for (unsigned i = 0; i < DHV_LINE_COUNT; i++) {
		tcp_role[i].readonly = false;
		tcp_host[i].readonly = false;
		tcp_port[i].readonly = false;
	}
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
	uint16_t val = (csr_tx_act ? DHV_CSR_TX_ACT : 0) | (csr_tx_ie ? DHV_CSR_TX_IE : 0)
			| (rx_fifo.empty() ? 0 : DHV_CSR_RX_DATA) | (csr_rx_ie ? DHV_CSR_RX_IE : 0)
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

// caller holds state_mutex: STAT reports carrier per selected channel (modeled)
void dhv11_c::set_stat_dati(void)
{
	uint16_t val = 0;
	unsigned c = csr_channel & DHV_CSR_CHAN_MASK;
	if (chan[c].open && tcp_line[c].client_connected())
		val |= 1; // TODO map to the real STAT carrier-detect bit
	set_register_dati_value(reg[dhv_idx_stat], val, __func__);
}

// -------------------------------------------------------------------------

void dhv11_c::on_after_register_access(qunibusdevice_register_t *device_reg,
		uint8_t unibus_control, DATO_ACCESS access)
{
	UNUSED(access);
	if (qunibusadapter->line_INIT)
		return;

	pthread_mutex_lock(&state_mutex);
	unsigned c = csr_channel & DHV_CSR_CHAN_MASK;

	switch (device_reg->index) {
	case dhv_idx_csr:
		if (unibus_control == QUNIBUS_CYCLE_DATO) {
			uint16_t v = get_register_dato_value(reg[dhv_idx_csr]);
			csr_rx_ie = v & DHV_CSR_RX_IE;
			csr_tx_ie = v & DHV_CSR_TX_IE;
			csr_channel = v & DHV_CSR_CHAN_MASK;
			if (v & DHV_CSR_MASTER_RESET) {
				rx_fifo.clear();
				csr_tx_act = false;
				memset(chan, 0, sizeof chan);
				// note: master reset drops all transmit state; TCP lines stay open
				set_rbuf_dati();
			}
			update_csr_and_INTR();
			set_stat_dati();
		}
		break;
	case dhv_idx_rbuf:
		if (unibus_control != QUNIBUS_CYCLE_DATO) { // read pops the FIFO
			if (!rx_fifo.empty())
				rx_fifo.pop_front();
			set_rbuf_dati();
			update_csr_and_INTR();
		}
		break;
	case dhv_idx_lpr:
		// line parameters (baud/bits/parity) are cosmetic on a TCP transport;
		// accepted and ignored. TODO honor RX-enable if encoded here per manual.
		break;
	case dhv_idx_lnctrl:
		if (unibus_control == QUNIBUS_CYCLE_DATO) {
			uint16_t v = get_register_dato_value(reg[dhv_idx_lnctrl]);
			chan[c].enabled = v & DHV_LNCTRL_TX_ENABLE;
			chan[c].rx_enabled = v & DHV_LNCTRL_RX_ENABLE;
		}
		break;
	case dhv_idx_tbuffad1:
		if (unibus_control == QUNIBUS_CYCLE_DATO)
			chan[c].dma_addr = (chan[c].dma_addr & 0xffff0000u)
					| get_register_dato_value(reg[dhv_idx_tbuffad1]);
		break;
	case dhv_idx_tbuffad2:
		if (unibus_control == QUNIBUS_CYCLE_DATO) {
			uint16_t v = get_register_dato_value(reg[dhv_idx_tbuffad2]);
			chan[c].dma_addr = (chan[c].dma_addr & 0x0000ffffu)
					| ((uint32_t) (v & DHV_TBUFFAD2_ADDR_HI) << 16);
			if (v & DHV_TBUFFAD2_ABORT) {
				chan[c].dma_pending = false;
				chan[c].dma_count = 0;
			}
		}
		break;
	case dhv_idx_tbuffct:
		if (unibus_control == QUNIBUS_CYCLE_DATO) {
			// writing the count with the channel enabled launches the DMA
			// transmit. TODO confirm the exact start trigger against the manual.
			chan[c].dma_count = get_register_dato_value(reg[dhv_idx_tbuffct]);
			if (chan[c].enabled && chan[c].dma_count > 0) {
				chan[c].dma_pending = true;
				pthread_cond_signal(&xmt_cond);
			}
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
	csr_rx_ie = csr_tx_ie = csr_tx_act = false;
	csr_channel = 0;
	memset(chan, 0, sizeof chan);
	// preserve which TCP lines are open across a bus INIT
	for (unsigned i = 0; i < DHV_LINE_COUNT; i++)
		chan[i].open = tcp_line[i].is_open();
	rx_fifo.clear();
	rcvintr_request.edge_detect_reset();
	xmtintr_request.edge_detect_reset();
	set_rbuf_dati();
	set_stat_dati();
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
		set_stat_dati();
		pthread_mutex_unlock(&state_mutex);

		for (unsigned i = 0; i < DHV_LINE_COUNT; i++) {
			if (!chan[i].open || !chan[i].rx_enabled)
				continue;
			uint8_t ch;
			while (tcp_line[i].poll_rcv(&ch)) {
				pthread_mutex_lock(&state_mutex);
				uint16_t entry = DHV_RBUF_VALID
						| ((uint16_t) (i & 7) << DHV_RBUF_LINE_SHIFT) | ch;
				if (rx_fifo.size() >= 256) {
					entry |= DHV_RBUF_OVR;
					rx_fifo.pop_back();
				}
				rx_fifo.push_back(entry);
				set_rbuf_dati();
				update_csr_and_INTR();
				pthread_mutex_unlock(&state_mutex);
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
		pthread_mutex_unlock(&state_mutex);

		unsigned words = (count + 1) / 2;
		if (words > 4096)
			words = 4096; // one DMA burst; TODO chain longer buffers per manual
		bool ok = dma_read_words(addr & ~1u, dma_buf, words);
		if (ok && open) {
			// bus/ARM are both little-endian: low byte first in each word
			const uint8_t *bytes = (const uint8_t *) dma_buf;
			unsigned n = count;
			if (n > words * 2)
				n = words * 2;
			for (unsigned k = 0; k < n; k++)
				tcp_line[line].send(bytes[k]);
		}

		pthread_mutex_lock(&state_mutex);
		chan[line].dma_count = 0;
		chan[line].dma_addr = (addr + count) & 0x3fffff;
		csr_tx_act = true; // transmit complete: request service
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
