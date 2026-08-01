/* memory.cpp: the machine's memory, served out of the board's DDR

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see memory.hpp for the description of the device.
*/

#include <stdio.h>
#include <stdlib.h>
#include <cctype>
#include <cstring>

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

	// the placement reports where it reaches; the operator gives the start
	// address and the size
	endaddr.kind = parameter_c::PARAM_STATUS;

	placing = false;
	startaddr.value = 0;
	// everything below the I/O page: the whole machine, on a backplane that
	// carries no memory of its own
	range_bytes = qunibus->iopage_start_addr;
	endaddr.value = range_bytes ? range_bytes - 2 : 0;
	size.value = size_text(range_bytes);
	probe.value = true;
}

memory_c::~memory_c()
{
	if (enabled.value)
		release();
}

// A byte count as a card is described: "2 MB", "2040 KB". A span of odd
// kilobytes is spelled out in bytes, since no round unit names it.
std::string memory_c::size_text(uint32_t bytes)
{
	char buf[32];
	if (bytes >= 1024 * 1024 && bytes % (1024 * 1024) == 0)
		snprintf(buf, sizeof buf, "%u MB", bytes / (1024 * 1024));
	else if (bytes >= 1024 && bytes % 1024 == 0)
		snprintf(buf, sizeof buf, "%u KB", bytes / 1024);
	else
		snprintf(buf, sizeof buf, "%u bytes", bytes);
	return std::string(buf);
}

// A size as the operator writes it: a decimal count followed by KB, MB or
// nothing, in either case, with or without a space. A bare count is bytes.
bool memory_c::parse_size(const std::string &text, uint32_t *bytes)
{
	const char *s = text.c_str();
	char *endptr;
	while (isspace((unsigned char) *s))
		s++;
	unsigned long long n = strtoull(s, &endptr, 10);
	if (endptr == s) {
		ERROR("\"%s\" does not name a size", text.c_str());
		return false;
	}
	while (isspace((unsigned char) *endptr))
		endptr++;
	unsigned long long unit = 1;
	if (!strcasecmp(endptr, "k") || !strcasecmp(endptr, "kb"))
		unit = 1024;
	else if (!strcasecmp(endptr, "m") || !strcasecmp(endptr, "mb"))
		unit = 1024 * 1024;
	else if (*endptr && strcasecmp(endptr, "b") && strcasecmp(endptr, "byte")
			&& strcasecmp(endptr, "bytes")) {
		ERROR("\"%s\" is not a unit of memory; write KB, MB or a count of bytes",
				endptr);
		return false;
	}
	unsigned long long total = n * unit;
	if (n != 0 && (total / unit != n || total > qunibus->addr_space_byte_count)) {
		ERROR("%s is more memory than the %u-bit address space holds", text.c_str(),
				qunibus->addr_width);
		return false;
	}
	*bytes = (uint32_t) total;
	return true;
}

// The last address a card of "bytes" placed at "start" answers. Refuses a span
// a card cannot answer, and says which part of it is impossible.
bool memory_c::range_of(uint32_t start, uint32_t bytes, uint32_t *end)
{
	if (bytes == 0) {
		ERROR("a card of no size answers nothing");
		return false;
	}
	if (start % 2 || bytes % 2) {
		ERROR("%s at %s is not a whole number of words", size_text(bytes).c_str(),
				qunibus->addr2text(start));
		return false;
	}
	if (start >= qunibus->iopage_start_addr) {
		ERROR("start address %s is inside the I/O page at %s",
				qunibus->addr2text(start),
				qunibus->addr2text(qunibus->iopage_start_addr));
		return false;
	}
	if (bytes > qunibus->iopage_start_addr - start) {
		ERROR("%s at %s reaches the I/O page at %s", size_text(bytes).c_str(),
				qunibus->addr2text(start),
				qunibus->addr2text(qunibus->iopage_start_addr));
		return false;
	}
	*end = start + bytes - 2;
	return true;
}

// claim(): have the PRU answer [start, end] out of DDR.
// Refuses rather than claiming a range that would collide, so a card is never
// installed against memory the machine already answers.
bool memory_c::claim(uint32_t start, uint32_t end)
{
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

// Move the card to "bytes" of memory at "start". A card in the machine gives up
// the range it answers and takes the new one, so the operator re-straps it
// where it stands; a range the machine refuses leaves the card answering what
// it answered before.
bool memory_c::place(uint32_t start, uint32_t bytes)
{
	uint32_t end;
	if (!range_of(start, bytes, &end))
		return false;
	if (enabled.value && (start != startaddr.value || end != endaddr.value)) {
		uint32_t was_start = startaddr.value, was_end = endaddr.value;
		release();
		if (!claim(start, end)) {
			ddrmem->set_range(DDRMEM_RANGE_MEMORY, was_start, was_end);
			return false;
		}
	}
	range_bytes = bytes;
	// the reported placement follows the card, and the change reaches the
	// interfaces watching it
	placing = true;
	endaddr.set(end);
	placing = false;
	return true;
}

bool memory_c::place_at(uint32_t start, uint32_t bytes)
{
	if (!place(start, bytes))
		return false;
	placing = true;
	startaddr.set(start);
	size.set(size_text(bytes));
	placing = false;
	return true;
}

bool memory_c::place_at(uint32_t start, const std::string &sizespec)
{
	uint32_t bytes;
	if (!parse_size(sizespec, &bytes))
		return false;
	return place_at(start, bytes);
}

bool memory_c::on_param_changed(parameter_c *param)
{
	if (param == &enabled) {
		if (enabled.new_value) {
			if (!claim(startaddr.value, endaddr.value))
				return false;
		} else
			release();
		return device_c::on_param_changed(param);
	}
	if (placing)
		return device_c::on_param_changed(param);
	if (param == &startaddr)
		return place(startaddr.new_value, range_bytes);
	if (param == &size) {
		uint32_t bytes;
		if (!parse_size(size.new_value, &bytes))
			return false;
		if (!place(startaddr.value, bytes))
			return false;
		size.new_value = size_text(bytes); // as a card is described
		return true;
	}
	if (param == &endaddr) {
		ERROR("the end address follows from the start address and the size");
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
