/* dispreg.cpp: console switch and display register at 777570

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see any device source header for the full text.

   See dispreg.hpp.
*/
#include <cstring>
#include <time.h>
#include <iostream>
#include "utils.hpp"
#include "logger.hpp"
#include "qunibus.h"
#include "dispreg.hpp"
#if defined(UNIBUS)
#include "qunibusadapter.hpp"
#include "cpu.hpp"
#endif

dispreg_c::dispreg_c() : qunibusdevice_c()
{
	name.value = "dispreg";
	type_name.value = "dispreg_c";
	log_label = "dsp";

	set_default_bus_params(DISPREG_ADDR, 0, 0, 0);
	// a register and nothing else: no interrupt, no transfer, no worker
	set_workers_count(0);

	register_count = 1;
	reg = &(this->registers[0]);
	strcpy(reg->name, "SR");
	reg->active_on_dati = false;  // DATI answers the switches from the read latch
	reg->active_on_dato = true;   // a write goes to the display
	reg->reset_value = 0;
	reg->writable_bits = 0177777;

	display.kind = parameter_c::PARAM_STATUS;
}

dispreg_c::~dispreg_c()
{
}

// Two slaves answering 777570 would drive the bus against each other, so the
// register goes in only where nothing answers there yet.
bool dispreg_c::on_before_install(void)
{
#if defined(UNIBUS)
	// An emulated PDP-11 answers 777570 itself, without a bus cycle, so the
	// probe below cannot see it.
	if (dynamic_cast<cpu_base_c *>(qunibusadapter->installed_cpu())) {
		ERROR("the emulated processor carries its own switch register at %s",
				qunibus->addr2text(base_addr.value));
		return false;
	}
#endif
	uint32_t answered = qunibus->probe_range(base_addr.value, base_addr.value);
	if (answered != QUNIBUS_PROBE_NONE) {
		ERROR("%s is answered by the machine already", qunibus->addr2text(answered));
		return false;
	}
	set_register_dati_value(reg, switches.value, __func__);
	return true;
}

void dispreg_c::on_after_register_access(qunibusdevice_register_t *device_reg,
		uint8_t unibus_control, DATO_ACCESS access)
{
	UNUSED(access);
	if (device_reg != reg || unibus_control != QUNIBUS_CYCLE_DATO)
		return;
	uint16_t value = get_register_dato_value(reg);
	display.value = value;
	if (logged_any && value == last_logged_value)
		return;
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	uint64_t us = (uint64_t) ts.tv_sec * 1000000u + (uint64_t) ts.tv_nsec / 1000u;
	if (logged_any && us - last_logged_us < CHANGE_SPACING_US)
		return;
	unsigned head = change_head.load(std::memory_order_relaxed);
	if (head - change_tail.load(std::memory_order_acquire) >= CHANGES) {
		changes_dropped.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	changes[head % CHANGES] = { us, value };
	change_head.store(head + 1, std::memory_order_release);
	last_logged_us = us;
	last_logged_value = value;
	logged_any = true;
}

unsigned dispreg_c::drain_changes(std::vector<dispreg_change_t> &out)
{
	unsigned tail = change_tail.load(std::memory_order_relaxed);
	unsigned head = change_head.load(std::memory_order_acquire);
	for (; tail != head; tail++)
		out.push_back(changes[tail % CHANGES]);
	change_tail.store(tail, std::memory_order_release);
	return changes_dropped.exchange(0, std::memory_order_relaxed);
}

bool dispreg_c::on_param_changed(parameter_c *param)
{
	if (param == &switches)
		set_register_dati_value(reg, switches.new_value, __func__);
	return qunibusdevice_c::on_param_changed(param);
}

void dispreg_c::on_power_changed(signal_edge_enum aclo_edge, signal_edge_enum dclo_edge)
{
	UNUSED(aclo_edge);
	UNUSED(dclo_edge);
	reset_unibus_registers();
	set_register_dati_value(reg, switches.value, __func__);
	display.value = 0;
}

// The display holds through INIT, as the lights of a console do.
void dispreg_c::on_init_changed(void)
{
}
