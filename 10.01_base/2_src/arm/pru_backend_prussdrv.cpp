/* pru_backend_prussdrv.cpp: the PRUs through uio_pruss

 Copyright (c) 2026, Hans Huebner
 MIT license, see delqa.hpp for the full text.

 libprussdrv over the uio_pruss kernel driver, which is how this ran before
 there was a choice. The driver was removed from mainline in 6.10, so this
 backend works up to 6.6 and not beyond; see docs/plans/remoteproc-port-plan.md.

 Firmware here is raw instruction words written into instruction memory,
 with an explicit entry address. The ELF the caller also offers is ignored.
*/

#include <stdio.h>
#include <string.h>

#include "prussdrv.h"
#include <pruss_intc_mapping.h>

#include "logsource.hpp"
#include "logger.hpp"
#include "pru_backend.hpp"

class pru_backend_prussdrv_c: public pru_backend_c, public logsource_c {
public:
	pru_backend_prussdrv_c() {
		log_label = "PRUSS";
	}

	const char *name(void) override {
		return "prussdrv";
	}

	bool open(void) override {
		tpruss_intc_initdata intc = PRUSS_INTC_INITDATA;

		if (prussdrv_init() != 0) {
			ERROR("prussdrv_init() failed");
			return false;
		}
		if (prussdrv_open(PRU_EVTOUT_0) != 0) {
			ERROR("prussdrv_open() failed");
			return false;
		}
		if (prussdrv_pruintc_init(&intc) != 0) {
			ERROR("prussdrv_pruintc_init() failed");
			return false;
		}
		return true;
	}

	void close(void) override {
		if (prussdrv_exit() != 0)
			ERROR("prussdrv_exit() failed");
	}

	void *map_ram(enum pru_ram_e ram) override {
		void *addr = NULL;
		unsigned id;
		switch (ram) {
		case PRU_RAM_PRU0_DATA:
			id = PRUSS0_PRU0_DATARAM;
			break;
		case PRU_RAM_PRU1_DATA:
			id = PRUSS0_PRU1_DATARAM;
			break;
		default:
			id = PRUSS0_SHARED_DATARAM;
			break;
		}
		if (prussdrv_map_prumem(id, &addr)) {
			ERROR("prussdrv_map_prumem() failed");
			return NULL;
		}
		return addr;
	}

	bool map_host_memory(void **virt, size_t *len, uint32_t *phys) override {
		*virt = NULL;
		prussdrv_map_extmem(virt);
		if (*virt == NULL) {
			ERROR("prussdrv_map_extmem() failed");
			return false;
		}
		*len = prussdrv_extmem_size();
		*phys = prussdrv_get_phys_addr(*virt);
		return true;
	}

	bool load_and_start(unsigned pru_num, const uint32_t *code, size_t code_bytes,
			const uint8_t *elf, size_t elf_bytes, uint32_t entry) override {
		(void) elf;      // this backend loads instruction words, not an ELF
		(void) elf_bytes;
		// prussdrv takes the words by non-const pointer without writing them
		if (prussdrv_exec_code_at(pru_num, (unsigned int *) code, code_bytes, entry) != 0) {
			ERROR("prussdrv_exec_code_at(PRU%u) failed", pru_num);
			return false;
		}
		return true;
	}

	void halt(unsigned pru_num) override {
		if (prussdrv_pru_disable(pru_num) != 0)
			ERROR("prussdrv_pru_disable(%u) failed", pru_num);
	}

	int wait_event(unsigned timeout_us) override {
		return prussdrv_pru_wait_event_timeout(PRU_EVTOUT_0, timeout_us);
	}

	void clear_event(void) override {
		if (prussdrv_pru_clear_event(PRU_EVTOUT_0, PRU0_ARM_INTERRUPT))
			ERROR("prussdrv_pru_clear_event() failed");
	}
};

pru_backend_c *pru_backend_prussdrv(void) {
	// Built on first call, not at static initialization: a logsource_c
	// registers with the logger, which does not exist yet before main().
	static pru_backend_prussdrv_c backend;
	return &backend;
}
