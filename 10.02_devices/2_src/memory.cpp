/* memory.cpp: the machine's memory, served out of the board's DDR

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see memory.hpp for the description of the device.
*/

#include <stdio.h>

#include "logger.hpp"
#include "qunibus.h"
#include "ddrmem.h"
#include "memory.hpp"

memory_c::memory_c() :
		device_c()
{
	// a card of memory cells: nothing to run, nothing to time
	set_workers_count(0);

	name.value = "MEM";
#if defined(QBUS)
	type_name.value = "MSV11";
#else
	type_name.value = "MS11";
#endif
	log_label = "mem";

	size.kind = parameter_c::PARAM_STATUS;

	startaddr.value = 0;
	// everything below the I/O page: the whole machine, on a backplane that
	// carries no memory of its own
	endaddr.value = qunibus->iopage_start_addr ? qunibus->iopage_start_addr - 2 : 0;
	probe.value = true;
	update_size(/*claimed*/false);
}

memory_c::~memory_c()
{
	if (enabled.value)
		release();
}

// The claimed size, as a card is described: "2040 KB", "4 MB". Called with the
// enabled state the change is settling on: a parameter's value is committed
// after on_param_changed() has accepted it, so enabled.value still holds the
// state being left.
void memory_c::update_size(bool claimed)
{
	char buf[32];
	if (!claimed || startaddr.value > endaddr.value) {
		size.set("none");
		return;
	}
	uint32_t kb = (endaddr.value - startaddr.value + 2) / 1024;
	if (kb >= 1024 && kb % 1024 == 0)
		snprintf(buf, sizeof buf, "%u MB", kb / 1024);
	else
		snprintf(buf, sizeof buf, "%u KB", kb);
	size.set(buf);
}

// claim(): have the PRU answer [start, end] out of DDR.
// Refuses rather than claiming a range that would collide, so a card is never
// installed against memory the machine already answers.
bool memory_c::claim(uint32_t start, uint32_t end)
{
	if (start > end) {
		ERROR("start address %s is above end address %s", qunibus->addr2text(start),
				qunibus->addr2text(end));
		return false;
	}
	if (start % 2 || end % 2) {
		ERROR("range %s..%s is not word aligned", qunibus->addr2text(start),
				qunibus->addr2text(end));
		return false;
	}
	if (end >= qunibus->iopage_start_addr) {
		ERROR("range %s..%s reaches the I/O page at %s", qunibus->addr2text(start),
				qunibus->addr2text(end), qunibus->addr2text(qunibus->iopage_start_addr));
		return false;
	}

	if (probe.value) {
		uint32_t answered = qunibus->probe_range(start, end);
		if (answered != QUNIBUS_PROBE_NONE) {
			ERROR("%s is answered by the machine already; a card claimed over it "
					"would drive the bus against what answers there",
					qunibus->addr2text(answered));
			return false;
		}
	}

	if (!ddrmem->set_range(DDRMEM_RANGE_MEMORY, start, end)) {
		ERROR("cannot serve memory at %s..%s", qunibus->addr2text(start),
				qunibus->addr2text(end));
		return false;
	}

	// The DDR reservation holds whatever the last run left in it, and a machine
	// must not boot on that.
	ddrmem->clear_range(start, end);
	INFO("memory at %s..%s", qunibus->addr2text(start), qunibus->addr2text(end));
	return true;
}

void memory_c::release(void)
{
	// start > end disables the range
	ddrmem->set_range(DDRMEM_RANGE_MEMORY, 0xffffffff, 0);
}

bool memory_c::on_param_changed(parameter_c *param)
{
	if (param == &enabled) {
		if (enabled.new_value) {
			if (!claim(startaddr.value, endaddr.value))
				return false;
		} else
			release();
		if (!device_c::on_param_changed(param))
			return false;
		update_size(enabled.new_value);
		return true;
	}
	if ((param == &startaddr || param == &endaddr) && enabled.value) {
		// a card is re-strapped out of the backplane
		ERROR("disable the card before moving its range");
		return false;
	}
	return device_c::on_param_changed(param);
}

// Memory keeps its contents across INIT, and across the DCLO/ACLO sequence the
// board drives for a power cycle: a program loaded into it and started from the
// console survives the restart that starts it.
void memory_c::on_power_changed(signal_edge_enum aclo_edge, signal_edge_enum dclo_edge)
{
	UNUSED(aclo_edge);
	UNUSED(dclo_edge);
}

void memory_c::on_init_changed(void)
{
}
