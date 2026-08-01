/* memory.cpp: the machine's memory, served out of the board's DDR

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see memory.hpp for the description of the device.
*/

#include <stdio.h>
#include <cctype>
#include <cstring>
#include <limits>

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

	startaddr.value = 0;
	// everything below the I/O page: the whole machine, on a backplane that
	// carries no memory of its own
	endaddr.value = qunibus->iopage_start_addr ? qunibus->iopage_start_addr - 2 : 0;
	probe.value = true;
	update_size();
}

memory_c::~memory_c()
{
	if (enabled.value)
		release();
}

// The configured size, as a card is described: "2040 KB", "4 MB".
void memory_c::update_size(void)
{
	char buf[32];
	if (startaddr.value > endaddr.value) {
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

bool memory_c::parse_size_text(const std::string &text, uint32_t *size_bytes,
		std::string *normalized)
{
	char unit[3] = { 0, 0, 0 };
	unsigned long count;
	char tail[2];
	if (sscanf(text.c_str(), " %lu %2s %1s", &count, unit, tail) != 2)
		return false;
	for (unsigned i = 0; unit[i]; ++i)
		unit[i] = (char) toupper((unsigned char) unit[i]);
	uint64_t bytes = 0;
	if (!strcmp(unit, "KB"))
		bytes = (uint64_t) count * 1024ULL;
	else if (!strcmp(unit, "MB"))
		bytes = (uint64_t) count * 1024ULL * 1024ULL;
	else
		return false;
	if (bytes < 2 || bytes > std::numeric_limits<uint32_t>::max() || (bytes % 2))
		return false;
	*size_bytes = (uint32_t) bytes;
	char buf[32];
	if ((bytes % (1024ULL * 1024ULL)) == 0)
		snprintf(buf, sizeof buf, "%lu MB", (unsigned long) (bytes / (1024ULL * 1024ULL)));
	else
		snprintf(buf, sizeof buf, "%lu KB", (unsigned long) (bytes / 1024ULL));
	*normalized = buf;
	return true;
}

uint32_t memory_c::end_from_start_size(uint32_t start, uint32_t size_bytes)
{
	uint64_t end = (uint64_t) start + size_bytes - 2ULL;
	if (end > std::numeric_limits<uint32_t>::max())
		throw bad_parameter_parse("size extends past the address range");
	return (uint32_t) end;
}

// claim(): have the PRU answer [start, end] out of DDR.
// Refuses rather than claiming a range that would collide, so a card is never
// installed against memory the machine already answers.
bool memory_c::claim(uint32_t start, uint32_t end, std::string *reason)
{
	auto fail = [&](const std::string &msg) {
		if (reason != nullptr)
			*reason = msg;
		return false;
	};
	if (start > end) {
		ERROR("start address %s is above end address %s", qunibus->addr2text(start),
				qunibus->addr2text(end));
		return fail("start address is above end address");
	}
	if (start % 2 || end % 2) {
		ERROR("range %s..%s is not word aligned", qunibus->addr2text(start),
				qunibus->addr2text(end));
		return fail("range is not word aligned");
	}
	if (end >= qunibus->iopage_start_addr) {
		ERROR("range %s..%s reaches the I/O page at %s", qunibus->addr2text(start),
				qunibus->addr2text(end), qunibus->addr2text(qunibus->iopage_start_addr));
		return fail("range reaches the I/O page");
	}

	if (probe.value) {
		uint32_t answered = qunibus->probe_range(start, end);
		if (answered != QUNIBUS_PROBE_NONE) {
			ERROR("%s is answered by the machine already; a card claimed over it "
					"would drive the bus against what answers there",
					qunibus->addr2text(answered));
			return fail(std::string("address ") + qunibus->addr2text(answered)
					+ " is answered by the machine already");
		}
	}

	if (!ddrmem->set_range(DDRMEM_RANGE_MEMORY, start, end)) {
		ERROR("cannot serve memory at %s..%s", qunibus->addr2text(start),
				qunibus->addr2text(end));
		return fail("cannot serve memory at the selected range");
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
		update_size();
		return true;
	}
	if (param == &size || param == &startaddr) {
		uint32_t size_bytes;
		std::string normalized_size;
		if (param == &size) {
			if (!parse_size_text(size.new_value, &size_bytes, &normalized_size))
				throw bad_parameter_parse("size must be \"<n> KB\" or \"<n> MB\"");
			size.new_value = normalized_size;
		} else {
			size_bytes = endaddr.value - startaddr.value + 2;
		}
		uint32_t new_start = (param == &startaddr) ? startaddr.new_value : startaddr.value;
		uint32_t new_end = end_from_start_size(new_start, size_bytes);
		if (enabled.value) {
			std::string reason;
			uint32_t old_start = startaddr.value, old_end = endaddr.value;
			release();
			if (!claim(new_start, new_end, &reason)) {
				claim(old_start, old_end);
				throw bad_parameter_check(reason.empty()
						? "cannot move memory to the requested placement" : reason);
			}
		}
		endaddr.value = new_end;
		if (param == &startaddr)
			update_size();
		return true;
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
