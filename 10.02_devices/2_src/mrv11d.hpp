/* mrv11d.hpp: MRV11-D (M8578) universal PROM module

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see any device source header for the full text.

   The Q-bus board that holds a machine's bootstrap and nothing else: sixteen
   sockets of byte-wide ROM, an address decoder and a page control register.
   It carries no clock, no terminator and no serial line, so a machine whose
   processor has no boot ROM gets one by plugging this in.

   Modelled after EK-MRV1D-UG-001. The sockets form eight chip sets, each a
   low-byte and a high-byte device, so a set of N-byte devices holds N words.
   The array reaches the bus two ways, and both may be on at once:

   Bootstrap. Two windows of 512 bytes stand in the I/O page, window 0 at
   773000 and window 1 at 765000, and the bootstrap page control register at
   777520 says which 512-byte page of the array each one shows - its low byte
   for window 0, its high byte for window 1. Seven bits of page number reach
   64 KB of bootstrap code. The windows are always open, and power-up and INIT
   clear the register, so a machine comes up looking at page 0 through both.
   A page beyond the installed array answers nothing, which is what an
   out-of-range access on the real module does. This is how the board boots a
   machine: the operator starts it at 173000.

   Direct addressing. The whole array appears in memory space at a starting
   address on any 4 KB boundary, which is where a machine goes whose processor
   already answers the bootstrap window - a KDJ11 has its own ROM at 773000 and
   its own register at 777520. The array is served out of the board's DDR
   through the PRU's device range, the one the VCB01 bitmap also wants, so a
   machine carries one or the other. The real module's DATO-inhibit jumper is
   not modelled: the range answers writes, as it does with the static RAM the
   sockets also accept.

   Page mode addressing - two 2 KB windows in memory space through a second
   page control register - is a virtual-addressing scheme for reaching a large
   array, and is not modelled.

   The sockets come filled with the MXV11-B2 bootstrap set DEC supplied for
   this module, which boots MSCP disk, RL, RX and TU58. A MACRO-11 listing
   named in "romfile" replaces it.
*/
#ifndef _MRV11D_HPP_
#define _MRV11D_HPP_

#include <cstdint>
#include <vector>

#include "qunibusdevice.hpp"

class mrv11d_c: public qunibusdevice_c {
public:
	// Sampled by the status poll: lit while the machine is reading the module.
	// The PRU answers both of the module's forms on its own -- the windows in
	// the I/O page shadow and the direct-mode array in a DDR range -- so the
	// counts it keeps are the only trace a read leaves on the ARM side.
	void refresh_activity(void) override;

private:
	uint32_t last_rom_count = 0;      // PRU I/O-page ROM count at the last sample
	uint32_t last_array_count = 0;    // and its DDR range's
	uint64_t access_lamp_until_ms = 0; // the lamp stays lit until then
public:

	const char *category(void) const override { return "memory"; }

	mrv11d_c();
	~mrv11d_c();

	// Sixteen sockets in eight chip sets. All devices are the same size with
	// the array decoder DEC supplies, and a chip set holds one word per byte
	// of device.
	parameter_unsigned_c chipsize = parameter_unsigned_c(this, "chipsize", "cs",
			/*readonly*/false, "bytes", "%u", "size of each device: 2048 to 32768", 32, 10);
	parameter_unsigned_c chipsets = parameter_unsigned_c(this, "chipsets", "cn",
			/*readonly*/false, "", "%u", "populated chip sets, 1 to 8", 8, 10);

	// What the sockets are programmed with: a file of the board's media tree,
	// named by its subpath as a drive's image is ("roms/rt11.bin"). A ".lst" is
	// read as a MACRO-11 listing and anything else as a dump of the array, low
	// and high byte interleaved as the two devices of a chip set supply them.
	// Empty leaves the MXV11-B2 set in the sockets.
	parameter_string_c romfile = parameter_string_c(this, "romfile", "rf", /*readonly*/false,
			"ROM image or MACRO-11 listing from roms/; empty = MXV11-B2 set");

	// The bootstrap windows and the page control register at 777520.
	// Lit while the machine is reading the module: the ROM running, live.
	parameter_bool_c access_lamp = parameter_bool_c(this, "accesslamp", "acl",
			/*readonly*/true, "machine is reading the module");

	parameter_bool_c bootstrap = parameter_bool_c(this, "bootstrap", "bs", /*readonly*/false,
			"answer the bootstrap windows at 773000 and 765000");

	// The array in memory space, at a 4 KB boundary.
	parameter_bool_c directmode = parameter_bool_c(this, "directmode", "dm", /*readonly*/false,
			"answer the whole array in memory space at startaddr");
	parameter_unsigned_c startaddr = parameter_unsigned_c(this, "startaddr", "sa",
			/*readonly*/false, "", "%08o", "first address of the array in memory space", 22, 8);

	// what the sockets hold, by the name it came from
	parameter_string_c contents = parameter_string_c(this, "contents", "co", /*readonly*/true,
			"what the sockets hold");
	// how much of it, as the array and as bootstrap pages
	parameter_string_c arraysize = parameter_string_c(this, "arraysize", "as", /*readonly*/true,
			"size of the array");
	// the pages the two bootstrap windows show, as the register selects them
	parameter_string_c windows = parameter_string_c(this, "windows", "wi", /*readonly*/true,
			"pages the bootstrap windows show");
	// the addresses the array answers in memory space, when it is strapped to
	parameter_string_c range = parameter_string_c(this, "range", "ra", /*readonly*/true,
			"addresses the array answers in memory space");

	// the bootstrap page control register at 777520
	qunibusdevice_register_t *reg_bpcr;

	bool on_param_changed(parameter_c *param) override;
	bool on_before_install(void) override;
	void on_after_uninstall(void) override;
	void on_after_register_access(qunibusdevice_register_t *device_reg, uint8_t unibus_control,
			DATO_ACCESS access) override;
	void on_power_changed(signal_edge_enum aclo_edge, signal_edge_enum dclo_edge) override;
	void on_init_changed(void) override;

private:
	// A bootstrap page and a bootstrap window are both 512 bytes.
	static const unsigned page_words = 256;

	// the memory array, low byte and high byte of each word interleaved as the
	// two devices of a chip set supply them
	std::vector<uint16_t> array;

	// whether each bootstrap window currently answers, so a window is neither
	// registered twice nor removed twice
	bool window_installed[2];
	// the range claimed in memory space for direct addressing
	bool direct_claimed;

	uint32_t iopage_addr(uint32_t addr_16bit) const;
	uint32_t window_base(unsigned window) const;
	unsigned array_pages(void) const;

	bool load_array(void);
	void describe_contents(void);
	void show_windows(unsigned page0, unsigned page1);
	void show_range(void);

	// show a page of the array through a window, or let the window answer
	// nothing when the page is beyond what is installed
	void map_window(unsigned window, unsigned page);
	void unmap_window(unsigned window);
	// apply both page numbers the register holds
	void map_windows(uint16_t bpcr);

	bool claim_direct(void);
	void release_direct(void);
};

#endif // _MRV11D_HPP_
