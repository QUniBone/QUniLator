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

   The range is the operator's to place, and must lie above whatever memory the
   machine already carries: two slaves answering one cycle drive the bus
   against each other. Claiming probes the range first and refuses when
   something answers.
*/
#ifndef _MEMORY_HPP_
#define _MEMORY_HPP_

#include <cstdint>

#include "device.hpp"
#include "parameter.hpp"

class memory_c: public device_c {
public:
	const char *category(void) const override { return "memory"; }

	memory_c();
	~memory_c();

	// The range the card answers, both addresses included. The defaults cover
	// everything below the I/O page, which is the whole machine on a backplane
	// that carries no memory of its own.
	parameter_unsigned_c startaddr = parameter_unsigned_c(this, "startaddr", "sa",
			/*readonly*/false, "", "%08o", "first address the card answers", 22, 8);
	parameter_unsigned_c endaddr = parameter_unsigned_c(this, "endaddr", "ea",
			/*readonly*/true, "", "%08o", "last address the card answers", 22, 8);
	// A range claimed over memory the machine already carries has two slaves
	// answering one cycle. Clear this only where the probe cannot work: a bus
	// with no arbitrator to grant the DMA it needs.
	parameter_bool_c probe = parameter_bool_c(this, "probe", "pr", /*readonly*/false,
			"check the range answers to nothing before claiming it");

	// card size in KB or MB ("256 KB", "2 MB"), from which endaddr is derived
	parameter_string_c size = parameter_string_c(this, "size", "sz", /*readonly*/false,
			"size of the range the card answers");

	bool on_param_changed(parameter_c *param) override;
	void on_power_changed(signal_edge_enum aclo_edge, signal_edge_enum dclo_edge) override;
	void on_init_changed(void) override;

private:
	// have the PRU answer the range out of DDR, or refuse and say why
	bool claim(uint32_t start, uint32_t end, std::string *reason = nullptr);
	void release(void);
	void update_size(void);
	bool parse_size_text(const std::string &text, uint32_t *size_bytes,
			std::string *normalized);
	uint32_t end_from_start_size(uint32_t start, uint32_t size_bytes);
};

#endif // _MEMORY_HPP_
