/* memory.hpp: the machine's memory, served out of the board's DDR

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see any device source header for the full text.

   A memory card for a machine whose backplane does not carry one, or does not
   carry enough. The card answers DATI/DATO/DATOB for one address range out of
   the board's DDR reservation; the PRU serves those cycles itself, so the
   machine sees memory at bus speed and the ARM never takes part.

   The card holds no registers and appears nowhere in the I/O page: an MSV11
   is decoded by address alone, and an operating system finds the top of memory
   by probing for a bus timeout. Parity and the CSR at 772100 are not modeled.

   The card is placed the way a card is described: a start address and a size.
   The end address follows from the two and is reported, not set.

   The range is the operator's to place, and must lie above whatever memory the
   machine already carries: two slaves answering one cycle drive the bus
   against each other. Claiming probes the range first and refuses when
   something answers, and a refused placement leaves the card answering the
   range it had. Placing a card is re-strapping it: the range it takes up comes
   up cleared, as a card just plugged in does.
*/
#ifndef _MEMORY_HPP_
#define _MEMORY_HPP_

#include <cstdint>
#include <string>

#include "device.hpp"
#include "parameter.hpp"

class memory_c: public device_c {
public:
	// Sampled by the status poll: light a lamp when the PRU's count for its
	// direction has moved, and put it out once the dwell has passed.
	void refresh_activity(void) override;

private:
	uint32_t last_read_count = 0;      // PRU counts at the last sample
	uint32_t last_write_count = 0;
	uint64_t read_lamp_until_ms = 0;   // each lamp stays lit until then
	uint64_t write_lamp_until_ms = 0;
public:

	const char *category(void) const override { return "memory"; }

	memory_c();
	~memory_c();

	// Where the card sits and how much it carries. The defaults cover
	// everything below the I/O page, which is the whole machine on a backplane
	// that carries no memory of its own.
	parameter_unsigned_c startaddr = parameter_unsigned_c(this, "startaddr", "sa",
			/*readonly*/false, "", "%08o", "first address the card answers", 22, 8);
	// "256 KB", "2 MB", or a plain count of bytes
	parameter_string_c size = parameter_string_c(this, "size", "sz", /*readonly*/false,
			"how much memory the card carries");
	// the last address the placement reaches, as the card reports it
	parameter_unsigned_c endaddr = parameter_unsigned_c(this, "endaddr", "ea",
			/*readonly*/true, "", "%08o", "last address the card answers", 22, 8);
	// A range claimed over memory the machine already carries has two slaves
	// answering one cycle. Clear this only where the probe cannot work: a bus
	// with no arbitrator to grant the DMA it needs.
	// Lit while the machine is reading or writing the card, one lamp per
	// direction. The PRU answers this card's range out of DDR on its own, so
	// the only thing the ARM sees of a transfer is the counts the PRU keeps;
	// a lamp is its count having moved since the last look, held on long
	// enough for a periodic sampler to catch a burst rather than miss it
	// between two.
	parameter_bool_c read_lamp = parameter_bool_c(this, "readlamp", "rdl",
			/*readonly*/true, "machine is reading the card");
	parameter_bool_c write_lamp = parameter_bool_c(this, "writelamp", "wrl",
			/*readonly*/true, "machine is writing the card");

	parameter_bool_c probe = parameter_bool_c(this, "probe", "pr", /*readonly*/false,
			"check the range answers to nothing before claiming it");

	// Place the card at a start address with a size, in one step: the three
	// parameters end up describing one range, and a refused placement changes
	// nothing. This is what a stored configuration names the card's range with.
	bool place_at(uint32_t start, uint32_t bytes);
	// the same, with the size written as a card is described
	bool place_at(uint32_t start, const std::string &sizespec);

	bool on_param_changed(parameter_c *param) override;
	void on_power_changed(signal_edge_enum aclo_edge, signal_edge_enum dclo_edge) override;
	void on_init_changed(void) override;

private:
	// how much the card carries; the size parameter is this as a card is
	// described, and the end address follows from it and the start address
	uint32_t range_bytes;
	// set while a placement writes the parameters that describe it, so the
	// derived values are taken as given rather than as a placement of their own
	bool placing;

	// the last address a start address and a size reach, or a refusal saying
	// what a card cannot answer about the span
	bool range_of(uint32_t start, uint32_t bytes, uint32_t *end);
	// true when the range clears the memory the CPU module answers itself
	bool reaches_no_further_than_the_cpu_allows(uint32_t start, uint32_t end);
	// move the card, re-claiming out of DDR when it is in the machine
	bool place(uint32_t start, uint32_t bytes);
	// have the PRU answer the range out of DDR, or refuse and say why
	bool claim(uint32_t start, uint32_t end, bool putting_the_card_in = false);
	void release(void);
	// a byte count as a card is described ("256 KB", "2 MB"), and back
	static std::string size_text(uint32_t bytes);
	bool parse_size(const std::string &text, uint32_t *bytes);
};

#endif // _MEMORY_HPP_
