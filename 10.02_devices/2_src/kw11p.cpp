/* kw11p.cpp: KW11-P programmable real-time clock

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see kw11p.hpp for the full text.

   Register and interrupt model from DEC-11-HPWB-D (KW11-P manual). The counter
   is not stepped in software; its value is computed from wall-clock time and the
   selected rate, and the underflow/overflow interrupt is scheduled at the
   computed instant. See kw11p.hpp for the CSR bit assignments.
*/

#include <cstring>

#include "utils.hpp"
#include "logger.hpp"
#include "timeout.hpp"
#include "qunibusadapter.hpp"
#include "qunibusdevice.hpp"
#include "qunibus.h"
#include "kw11p.hpp"

// worker idle/responsiveness cap: while running, the worker never sleeps longer
// than this, so a stop or reprogram takes effect promptly and the CTR readback
// stays fresh. A shorter interval than this is honoured exactly, so events fire
// on time down to sub-millisecond periods.
#define KW11P_WORKER_CAP_NS  (20 * MILLION)

kw11p_c::kw11p_c() : qunibusdevice_c()
{
	name.value = "kw11p";
	type_name.value = "kw11p_c";
	log_label = "kw11p";

	set_default_bus_params(KW11P_ADDR, KW11P_SLOT, KW11P_VECTOR, KW11P_LEVEL);

	register_count = kw11p_idx_count;

	reg_csr = &(this->registers[kw11p_idx_csr]);
	strcpy(reg_csr->name, "CSR");
	reg_csr->active_on_dati = true;  // read clears ERROR (section 3.4)
	reg_csr->active_on_dato = true;  // write starts/stops and reprograms
	reg_csr->reset_value = 0;
	reg_csr->writable_bits = KW11P_CSR_WRITABLE;

	reg_buf = &(this->registers[kw11p_idx_buf]);
	strcpy(reg_buf->name, "BUF");
	reg_buf->active_on_dati = false; // holds the preset, polled
	reg_buf->active_on_dato = true;  // write loads buffer and counter
	reg_buf->reset_value = 0;
	reg_buf->writable_bits = 0177777;

	reg_ctr = &(this->registers[kw11p_idx_ctr]);
	strcpy(reg_ctr->name, "CTR");
	reg_ctr->active_on_dati = false; // counter output, maintained by worker
	reg_ctr->active_on_dato = false; // counter is read-only on the bus
	reg_ctr->reset_value = 0;
	reg_ctr->writable_bits = 0;

	line_frequency.value = 60;

	running = false;
	rate = KW11P_RATE_100KHZ;
	mode_repeat = false;
	count_up = false;
	intr_enable = false;
	done = false;
	error = false;
	buf = 0;
	count_start = 0;
	counter_frozen = 0;
	run_start_ns = 0;
	intr_pending = false;
}

kw11p_c::~kw11p_c()
{
}

bool kw11p_c::on_param_changed(parameter_c *param)
{
	if (param == &line_frequency) {
		if (line_frequency.new_value != 50 && line_frequency.new_value != 60)
			WARNING("KW11-P non-standard line frequency %d, regular 50 or 60",
					line_frequency.new_value);
	} else if (param == &priority_slot) {
		intr_request.set_priority_slot(priority_slot.new_value);
	} else if (param == &intr_level) {
		intr_request.set_level(intr_level.new_value);
	} else if (param == &intr_vector) {
		intr_request.set_vector(intr_vector.new_value);
	}
	return qunibusdevice_c::on_param_changed(param);
}

// clock period for the selected rate; 0 for the external rate (no internal clock)
int64_t kw11p_c::tick_period_ns(void)
{
	switch (rate) {
	case KW11P_RATE_100KHZ:
		return 10000;              // 100 kHz -> 10 us
	case KW11P_RATE_10KHZ:
		return 100000;             // 10 kHz -> 100 us
	case KW11P_RATE_LINE: {
		unsigned f = line_frequency.value ? (unsigned) line_frequency.value : 60;
		return BILLION / f;        // 50/60 Hz
	}
	default:
		return 0;                  // external: driven by a signal we do not have
	}
}

// number of clock counts from the loaded value to the underflow/overflow event.
// Count down underflows on reaching zero; count up overflows on the wrap past
// all-ones. A zero load spans the full 16-bit range (section 2.2, 3.5).
uint32_t kw11p_c::ticks_to_event(void)
{
	if (count_up)
		return 0x10000u - count_start;
	return count_start ? count_start : 0x10000u;
}

int64_t kw11p_c::event_period_ns(void)
{
	return (int64_t) ticks_to_event() * tick_period_ns();
}

// counter value derived from time elapsed since the run began
uint16_t kw11p_c::current_counter(void)
{
	int64_t period = tick_period_ns();
	if (!running || period <= 0)
		return counter_frozen;
	int64_t elapsed = the_flexi_timeout_controller->world_now_ns() - run_start_ns;
	if (elapsed < 0)
		elapsed = 0;
	uint32_t ticks = (uint32_t) (elapsed / period);
	if (count_up)
		return (uint16_t) ((count_start + ticks) & 0xffff);
	return (uint16_t) ((count_start - ticks) & 0xffff);
}

uint16_t kw11p_c::compute_csr_value(void)
{
	uint16_t v = 0;
	if (running)
		v |= KW11P_CSR_RUN;
	v |= (uint16_t) ((rate << KW11P_CSR_RATE_SHIFT) & KW11P_CSR_RATE);
	if (mode_repeat)
		v |= KW11P_CSR_MODE;
	if (count_up)
		v |= KW11P_CSR_UPCOUNT;
	if (intr_enable)
		v |= KW11P_CSR_INTR_ENB;
	if (done)
		v |= KW11P_CSR_DONE;
	if (error)
		v |= KW11P_CSR_ERROR;
	// FIX (bit 5) is write-only and reads as 0
	return v;
}

void kw11p_c::update_csr_register(void)
{
	set_register_dati_value(reg_csr, compute_csr_value(), __func__);
}

// handle a counter underflow (count down) or overflow (count up). Called with
// state_mutex held, from the worker on a scheduled event and from single_step().
void kw11p_c::fire_event(void)
{
	// second event before the previous interrupt was serviced -> ERROR (3.6.2)
	if (mode_repeat && intr_pending)
		error = true;
	done = true;

	if (mode_repeat) {
		// reload the counter from the buffer and keep running (section 3.5)
		count_start = buf;
		counter_frozen = buf;
		run_start_ns = the_flexi_timeout_controller->world_now_ns();
		set_register_dati_value(reg_ctr, counter_frozen, __func__);
	} else {
		// single-interrupt: stop the clock, clear counter and buffer (section 3.5)
		running = false;
		buf = 0;
		count_start = 0;
		counter_frozen = 0;
		set_register_dati_value(reg_ctr, 0, __func__);
		set_register_dati_value(reg_buf, 0, __func__);
	}

	uint16_t csrval = compute_csr_value();
	if (intr_enable) {
		intr_pending = true;
		qunibusadapter->INTR(intr_request, reg_csr, csrval);
	} else {
		set_register_dati_value(reg_csr, csrval, __func__);
	}
}

// FIX: advance the counter by exactly one count in the selected direction, as a
// maintenance aid (section 3.3.4). Used with RUN cleared.
void kw11p_c::single_step(void)
{
	bool event;
	if (count_up) {
		event = (counter_frozen == 0xffff);
		counter_frozen = (uint16_t) ((counter_frozen + 1) & 0xffff);
	} else {
		event = (counter_frozen == 0);
		counter_frozen = (uint16_t) ((counter_frozen - 1) & 0xffff);
	}
	count_start = counter_frozen;
	set_register_dati_value(reg_ctr, counter_frozen, __func__);
	if (event) {
		done = true;
		if (intr_enable) {
			intr_pending = true;
			qunibusadapter->INTR(intr_request, reg_csr, compute_csr_value());
			return;
		}
	}
	update_csr_register();
}

void kw11p_c::on_after_register_access(qunibusdevice_register_t *device_reg,
		uint8_t unibus_control, DATO_ACCESS access)
{
	UNUSED(access);

	pthread_mutex_lock(&state_mutex);

	switch (device_reg->index) {

	case kw11p_idx_csr:
		if (unibus_control == QUNIBUS_CYCLE_DATO) {
			uint16_t raw = device_reg->active_dato_flipflops;

			rate = (raw & KW11P_CSR_RATE) >> KW11P_CSR_RATE_SHIFT;
			mode_repeat = !!(raw & KW11P_CSR_MODE);
			count_up = !!(raw & KW11P_CSR_UPCOUNT);
			bool new_intr_enable = !!(raw & KW11P_CSR_INTR_ENB);
			bool new_run = !!(raw & KW11P_CSR_RUN);

			// addressing the status register clears ERROR (section 3.4)
			error = false;
			intr_enable = new_intr_enable;

			if (new_run && !running) {
				// RUN 0->1 clocks DONE clear and starts counting (section 3.6.2)
				done = false;
				intr_pending = false;
				count_start = counter_frozen;
				run_start_ns = the_flexi_timeout_controller->world_now_ns();
				running = true;
			} else if (!new_run && running) {
				counter_frozen = current_counter();
				running = false;
				set_register_dati_value(reg_ctr, counter_frozen, __func__);
			}

			if (raw & KW11P_CSR_FIX)
				single_step();

			if (!intr_enable) {
				intr_pending = false;
				qunibusadapter->cancel_INTR(intr_request);
			}

			update_csr_register();
		} else {
			// DATI: reading the status register clears ERROR and services the
			// pending interrupt (section 3.4, 3.6.2)
			error = false;
			intr_pending = false;
			update_csr_register();
		}
		break;

	case kw11p_idx_buf:
		if (unibus_control == QUNIBUS_CYCLE_DATO) {
			// writing the buffer loads both buffer and counter (section 3.5).
			// The buffer has no bus read drivers: a DATI returns data only from
			// the counter or the CSR (section 3.2), so BUF always reads as 0 and
			// its DATI value is never loaded from the written count.
			buf = device_reg->active_dato_flipflops;
			count_start = buf;
			counter_frozen = buf;
			if (running)
				run_start_ns = the_flexi_timeout_controller->world_now_ns();
			set_register_dati_value(reg_ctr, buf, __func__);
		}
		break;

	default:
		break;
	}

	pthread_mutex_unlock(&state_mutex);
	worker_wake();
}

void kw11p_c::on_power_changed(signal_edge_enum aclo_edge, signal_edge_enum dclo_edge)
{
	UNUSED(aclo_edge);
	UNUSED(dclo_edge);
	reset();
}

void kw11p_c::on_init_changed(void)
{
	if (init_asserted)
		reset();
}

void kw11p_c::reset(void)
{
	pthread_mutex_lock(&state_mutex);
	reset_unibus_registers();
	running = false;
	rate = KW11P_RATE_100KHZ;
	mode_repeat = false;
	count_up = false;
	intr_enable = false;
	done = false;
	error = false;
	buf = 0;
	count_start = 0;
	counter_frozen = 0;
	run_start_ns = 0;
	intr_pending = false;
	intr_request.edge_detect_reset();
	qunibusadapter->cancel_INTR(intr_request);
	set_register_dati_value(reg_csr, 0, __func__);
	set_register_dati_value(reg_buf, 0, __func__);
	set_register_dati_value(reg_ctr, 0, __func__);
	pthread_mutex_unlock(&state_mutex);
}

void kw11p_c::worker_wake(void)
{
	// the worker re-reads state on its own short cadence; nothing to signal
}

void kw11p_c::worker(unsigned instance)
{
	UNUSED(instance);
	timeout_c timeout;

	worker_init_realtime_priority(rt_device);

	while (!workers_terminate) {
		int64_t wait_ns = KW11P_WORKER_CAP_NS;

		pthread_mutex_lock(&state_mutex);
		int64_t period = tick_period_ns();
		if (running && period > 0) {
			int64_t event_ns = event_period_ns();
			int64_t now = the_flexi_timeout_controller->world_now_ns();
			int64_t elapsed = now - run_start_ns;
			if (event_ns > 0 && elapsed >= event_ns) {
				fire_event();
				wait_ns = 0; // re-evaluate immediately
			} else {
				set_register_dati_value(reg_ctr, current_counter(), __func__);
				int64_t remaining = event_ns - elapsed;
				if (remaining < 0)
					remaining = 0;
				wait_ns = (remaining < KW11P_WORKER_CAP_NS) ?
						remaining : KW11P_WORKER_CAP_NS;
			}
		}
		pthread_mutex_unlock(&state_mutex);

		if (wait_ns > 0)
			timeout.wait_ns(wait_ns);
	}
}
