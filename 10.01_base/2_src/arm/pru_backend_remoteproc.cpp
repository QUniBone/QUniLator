/* pru_backend_remoteproc.cpp: the PRUs through remoteproc

 Copyright (c) 2026, Hans Huebner
 MIT license, see delqa.hpp for the full text.

 uio_pruss was removed from mainline in 6.10, so from that kernel on the PRUs
 are reached through remoteproc instead. The four jobs come from four
 different places here, where libprussdrv had them all:

   firmware      remoteproc, driven through its sysfs files
   PRU memories  /dev/mem, at the addresses the AM335x manual gives
   host memory   a reserved-memory region named in the device tree
   PRU events    a uio_pdrv_genirq device, also from the device tree

 The last two need device tree nodes that the cape overlay has to provide;
 see docs/plans/remoteproc-port-plan.md. Without them this backend still loads and runs
 firmware, and says plainly what is missing rather than failing obscurely.

 The firmware is written out from the executable's own copy at startup, so
 the binary stays self-contained: the file remoteproc reads is one this
 process just created from an array linked into it.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <string>

#include "logsource.hpp"
#include "logger.hpp"
#include "pru_backend.hpp"

// AM335x PRU-ICSS, from the technical reference manual. The data memories
// carry the mailbox and the device register descriptors; the cores
// themselves are only named to recognise their remoteproc devices.
#define PRUSS_PRU0_DATARAM_PA	0x4a300000
#define PRUSS_PRU1_DATARAM_PA	0x4a302000
#define PRUSS_SHARED_RAM_PA	0x4a310000
#define PRUSS_DATARAM_SIZE	0x2000	// 8K each
#define PRUSS_SHARED_RAM_SIZE	0x3000	// 12K

// The PRU-ICSS Industrial Ethernet peripheral's free-running counter, which
// the PRU firmware enables and samples as it raises an event. The PRU's own
// CYCLE register would be the obvious choice and is not usable: it saturates
// at 0xffffffff instead of wrapping, so it stops telling the time after 21
// seconds.
#define PRUSS_IEP_PA		0x4a32e000
#define PRU_IEP_TMR_CNT_OFFSET	0x0c

// The remoteproc devices appear named after the core's address.
#define PRU0_CORE_NAME		"4a334000.pru"
#define PRU1_CORE_NAME		"4a338000.pru"

#define REMOTEPROC_DIR		"/sys/class/remoteproc"
#define FIRMWARE_DIR		"/lib/firmware"
// The reserved-memory node the cape overlay must declare for the emulated
// machine's memory.
#define HOST_MEMORY_NODE	"/proc/device-tree/reserved-memory"
#define HOST_MEMORY_NAME	"qbone-ddr"

class pru_backend_remoteproc_c: public pru_backend_c, public logsource_c {
private:
	std::string rproc[2];	// sysfs directory per PRU
	int mem_fd;		// /dev/mem, held open for the mappings
	int event_fd;		// uio device for PRU events
	volatile uint32_t *cycle_reg;	// PRU1's CYCLE counter, mapped on demand

	// sysfs is a pile of one-line files; these two do all the talking.
	bool write_file(const std::string &path, const std::string &value) {
		int fd = ::open(path.c_str(), O_WRONLY);
		if (fd < 0) {
			ERROR("cannot write %s: %s", path.c_str(), strerror(errno));
			return false;
		}
		ssize_t n = ::write(fd, value.c_str(), value.size());
		::close(fd);
		if (n != (ssize_t) value.size()) {
			ERROR("short write to %s: %s", path.c_str(), strerror(errno));
			return false;
		}
		return true;
	}

	std::string read_file(const std::string &path) {
		char buf[256] = { 0 };
		int fd = ::open(path.c_str(), O_RDONLY);
		if (fd < 0)
			return "";
		ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
		::close(fd);
		if (n <= 0)
			return "";
		while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' '))
			buf[--n] = 0;
		return std::string(buf);
	}

	// The remoteproc instance whose underlying device carries this name.
	std::string find_rproc(const char *core_name) {
		DIR *dir = opendir(REMOTEPROC_DIR);
		if (dir == NULL)
			return "";
		struct dirent *entry;
		std::string found;
		while ((entry = readdir(dir)) != NULL) {
			if (strncmp(entry->d_name, "remoteproc", 10) != 0)
				continue;
			std::string dirpath = std::string(REMOTEPROC_DIR) + "/" + entry->d_name;
			char link[512];
			ssize_t n = readlink((dirpath + "/device").c_str(), link, sizeof(link) - 1);
			if (n <= 0)
				continue;
			link[n] = 0;
			const char *base = strrchr(link, '/');
			base = base ? base + 1 : link;
			if (strcmp(base, core_name) == 0) {
				found = dirpath;
				break;
			}
		}
		closedir(dir);
		return found;
	}

public:
	pru_backend_remoteproc_c(): mem_fd(-1), event_fd(-1), cycle_reg(NULL) {
		log_label = "PRURP";
	}

	const char *name(void) override {
		return "remoteproc";
	}

	bool open(void) override {
		rproc[0] = find_rproc(PRU0_CORE_NAME);
		rproc[1] = find_rproc(PRU1_CORE_NAME);
		if (rproc[0].empty() || rproc[1].empty()) {
			// Not an error: on a kernel with uio_pruss the other backend
			// takes over, and the caller reports if neither can run.
			INFO("no PRU remoteproc devices; the cape overlay must enable "
					"the PRU-ICSS and the pru_rproc driver must be loaded");
			return false;
		}
		mem_fd = ::open("/dev/mem", O_RDWR | O_SYNC);
		if (mem_fd < 0) {
			ERROR("cannot open /dev/mem: %s", strerror(errno));
			return false;
		}
		INFO("PRU0 at %s, PRU1 at %s", rproc[0].c_str(), rproc[1].c_str());
		return true;
	}

	void close(void) override {
		halt(0);
		halt(1);
		if (mem_fd >= 0) {
			::close(mem_fd);
			mem_fd = -1;
		}
		if (event_fd >= 0) {
			::close(event_fd);
			event_fd = -1;
		}
	}

	void *map_ram(enum pru_ram_e ram) override {
		off_t pa;
		size_t len;
		switch (ram) {
		case PRU_RAM_PRU0_DATA:
			pa = PRUSS_PRU0_DATARAM_PA;
			len = PRUSS_DATARAM_SIZE;
			break;
		case PRU_RAM_PRU1_DATA:
			pa = PRUSS_PRU1_DATARAM_PA;
			len = PRUSS_DATARAM_SIZE;
			break;
		default:
			pa = PRUSS_SHARED_RAM_PA;
			len = PRUSS_SHARED_RAM_SIZE;
			break;
		}
		void *addr = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd, pa);
		if (addr == MAP_FAILED) {
			ERROR("cannot map PRU memory at 0x%08lx: %s", (unsigned long) pa,
					strerror(errno));
			return NULL;
		}
		return addr;
	}

	// The emulated machine's memory. uio_pruss carved this out itself; with
	// remoteproc it is a reserved-memory region the device tree declares, so
	// that the kernel keeps its hands off it and the PRUs can be told where
	// it is.
	bool map_host_memory(void **virt, size_t *len, uint32_t *phys) override {
		// The node carries a unit address, "qbone-ddr@9f000000", so it is
		// found by scanning rather than by opening a known path.
		std::string node;
		DIR *dir = opendir(HOST_MEMORY_NODE);
		if (dir) {
			struct dirent *de;
			size_t namelen = strlen(HOST_MEMORY_NAME);
			while ((de = readdir(dir)) != NULL)
				if (!strncmp(de->d_name, HOST_MEMORY_NAME, namelen)
						&& (de->d_name[namelen] == '\0' || de->d_name[namelen] == '@')) {
					node = std::string(HOST_MEMORY_NODE) + "/" + de->d_name + "/reg";
					break;
				}
			closedir(dir);
		}
		int fd = node.empty() ? -1 : ::open(node.c_str(), O_RDONLY);
		if (fd < 0) {
			ERROR("no reserved memory named \"%s\" under %s: the cape overlay\n"
					"  must declare it for the emulated machine's memory",
					HOST_MEMORY_NAME, HOST_MEMORY_NODE);
			return false;
		}
		// two big-endian cells: address then size
		uint8_t reg[8];
		ssize_t n = ::read(fd, reg, sizeof(reg));
		::close(fd);
		if (n != sizeof(reg)) {
			ERROR("%s is %d bytes, expected 8", node.c_str(), (int) n);
			return false;
		}
		uint32_t pa = (reg[0] << 24) | (reg[1] << 16) | (reg[2] << 8) | reg[3];
		uint32_t size = (reg[4] << 24) | (reg[5] << 16) | (reg[6] << 8) | reg[7];

		void *addr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd, pa);
		if (addr == MAP_FAILED) {
			ERROR("cannot map host memory at 0x%08x: %s", pa, strerror(errno));
			return false;
		}
		*virt = addr;
		*len = size;
		*phys = pa;
		INFO("host memory: %u bytes at 0x%08x", size, pa);
		return true;
	}

	// remoteproc loads an ELF from where the kernel looks for firmware, so
	// the image travels from the array linked into this executable, through
	// a file, to the core.
	bool load_and_start(unsigned pru_num, const uint32_t *code, size_t code_bytes,
			const uint8_t *elf, size_t elf_bytes, uint32_t entry) override {
		(void) code;        // this backend loads an ELF, not instruction words
		(void) code_bytes;
		(void) entry;       // the ELF carries its own entry point

		if (pru_num > 1 || rproc[pru_num].empty()) {
			ERROR("no remoteproc device for PRU%u", pru_num);
			return false;
		}
		if (elf == NULL || elf_bytes == 0) {
			ERROR("PRU%u has no ELF image: remoteproc cannot load instruction "
					"words", pru_num);
			return false;
		}

		char fwname[64];
		snprintf(fwname, sizeof(fwname), "qbone-pru%u.elf", pru_num);
		std::string fwpath = std::string(FIRMWARE_DIR) + "/" + fwname;

		int fd = ::open(fwpath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd < 0) {
			ERROR("cannot write %s: %s", fwpath.c_str(), strerror(errno));
			return false;
		}
		ssize_t n = ::write(fd, elf, elf_bytes);
		::close(fd);
		if (n != (ssize_t) elf_bytes) {
			ERROR("short write of %s", fwpath.c_str());
			return false;
		}

		// A core that is already running has to be stopped before it will
		// take a new image.
		if (read_file(rproc[pru_num] + "/state") == "running")
			write_file(rproc[pru_num] + "/state", "stop");

		if (!write_file(rproc[pru_num] + "/firmware", fwname))
			return false;
		if (!write_file(rproc[pru_num] + "/state", "start"))
			return false;

		std::string state = read_file(rproc[pru_num] + "/state");
		if (state != "running") {
			ERROR("PRU%u did not start, state is \"%s\"", pru_num, state.c_str());
			return false;
		}
		return true;
	}

	void halt(unsigned pru_num) override {
		if (pru_num > 1 || rproc[pru_num].empty())
			return;
		if (read_file(rproc[pru_num] + "/state") == "running")
			write_file(rproc[pru_num] + "/state", "stop");
	}

	// PRU events arrive through a uio_pdrv_genirq device: a blocking read
	// returns the interrupt count, which is the same shape as the call this
	// replaces.
	int wait_event(unsigned timeout_us) override {
		if (event_fd < 0) {
			event_fd = open_event_device();
			if (event_fd < 0)
				return -1;
		}
		struct pollfd pfd;
		pfd.fd = event_fd;
		pfd.events = POLLIN;
		int res = poll(&pfd, 1, timeout_us / 1000);
		if (res <= 0)
			return res; // 0 on timeout, negative on error
		uint32_t count = 0;
		if (::read(event_fd, &count, sizeof(count)) != sizeof(count))
			return -1;
		return (int) count;
	}

	// Mapped once and kept: the worker reads it on every event.
	volatile uint32_t *cycle_counter(void) override {
		if (cycle_reg == NULL) {
			void *p = mmap(NULL, 0x1000, PROT_READ, MAP_SHARED, mem_fd,
					PRUSS_IEP_PA);
			if (p == MAP_FAILED) {
				ERROR("cannot map the PRU IEP counter: %s", strerror(errno));
				return NULL;
			}
			cycle_reg = (volatile uint32_t *) ((char *) p + PRU_IEP_TMR_CNT_OFFSET);
		}
		return cycle_reg;
	}

	void clear_event(void) override {
		// uio_pdrv_genirq masks the interrupt when it fires; writing re-arms
		// it. Nothing to clear when no event device was ever opened.
		if (event_fd < 0)
			return;
		uint32_t enable = 1;
		if (::write(event_fd, &enable, sizeof(enable)) != sizeof(enable))
			ERROR("cannot re-arm PRU events: %s", strerror(errno));
	}

private:
	// The uio device the cape overlay binds to a PRU host interrupt.
	int open_event_device(void) {
		for (unsigned i = 0; i < 8; i++) {
			char path[64];
			snprintf(path, sizeof(path), "/sys/class/uio/uio%u/name", i);
			std::string uio_name = read_file(path);
			if (uio_name.empty())
				continue;
			if (uio_name.find("pru") == std::string::npos)
				continue;
			snprintf(path, sizeof(path), "/dev/uio%u", i);
			int fd = ::open(path, O_RDWR);
			if (fd < 0) {
				ERROR("cannot open %s: %s", path, strerror(errno));
				return -1;
			}
			// uio_pdrv_genirq requests the interrupt with IRQ_NOAUTOEN,
			// so it stays disabled until userspace asks for it. Arm it
			// here: waiting first and arming afterwards loses the first
			// event, and with it the first bus transaction.
			uint32_t enable = 1;
			if (::write(fd, &enable, sizeof(enable)) != sizeof(enable)) {
				ERROR("cannot arm PRU events on %s: %s", path, strerror(errno));
				::close(fd);
				return -1;
			}
			INFO("PRU events on %s (%s)", path, uio_name.c_str());
			return fd;
		}
		ERROR("no PRU event device: the cape overlay must bind a PRU host "
				"interrupt to uio_pdrv_genirq");
		return -1;
	}
};

pru_backend_c *pru_backend_remoteproc(void) {
	// Built on first call: see the note in the prussdrv backend.
	static pru_backend_remoteproc_c backend;
	return &backend;
}
