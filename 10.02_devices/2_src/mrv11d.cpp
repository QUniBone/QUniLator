/* mrv11d.cpp: MRV11-D (M8578) universal PROM module

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see mrv11d.hpp for the description of the device.
*/

#include <cstdio>
#include <cstring>

#include "utils.hpp"
#include "logger.hpp"
#include "qunibus.h"
#include "qunibusadapter.hpp"
#include "ddrmem.h"
#include "memoryimage.hpp"
#include "mrv11d.hpp"
#include "mxv11b2_bootrom.h"

// The bootstrap decode, as 16 bit addresses: the two windows and the register
// that pages the array through them.
static const uint32_t window0_addr = 0773000;
static const uint32_t window1_addr = 0765000;
static const uint32_t bpcr_addr = 0777520;

// Page numbers live in bits 0..6 for window 0 and 8..14 for window 1; bit 7 is
// unused and bit 15 carries no meaning in bootstrap mode.
static const uint16_t bpcr_page_mask = 0077577;

// An unprogrammed cell of an erased PROM reads all ones.
static const uint16_t unprogrammed = 0177777;

mrv11d_c::mrv11d_c() :
		qunibusdevice_c()
{
	name.value = "MRV11D";
	type_name.value = "MRV11-D";
	log_label = "mrv11d";

	// a board of memory cells and a decoder: nothing to run, nothing to time
	set_workers_count(0);

	// The bootstrap page control register. Its address is the module's to fix,
	// but the parameter stays so an operator can see where it answers.
	set_default_bus_params(bpcr_addr, 0, 0, 0);
	register_count = 1;

	reg_bpcr = &(this->registers[0]);
	strcpy(reg_bpcr->name, "BPCR");
	reg_bpcr->active_on_dati = false;   // the value is whatever was written
	reg_bpcr->active_on_dato = true;    // a write moves both windows
	reg_bpcr->writable_bits = bpcr_page_mask;
	reg_bpcr->reset_value = 0;          // power-up and INIT show page 0

	chipsize.value = 8192;              // the MXV11-B2 devices
	chipsets.value = 1;
	bootstrap.value = true;
	directmode.value = false;
	startaddr.value = 0;

	contents.kind = parameter_c::PARAM_STATUS;
	arraysize.kind = parameter_c::PARAM_STATUS;
	windows.kind = parameter_c::PARAM_STATUS;
	range.kind = parameter_c::PARAM_STATUS;

	window_installed[0] = window_installed[1] = false;
	direct_claimed = false;

	load_array();
	show_windows(0, 0);
	show_range();
}

mrv11d_c::~mrv11d_c()
{
	release_direct();
}

// A 16 bit I/O page address as the bus carries it, so the same constants serve
// an 18 and a 22 bit machine.
uint32_t mrv11d_c::iopage_addr(uint32_t addr_16bit) const
{
	return qunibus->iopage_start_addr + (addr_16bit & 0x1fff);
}

uint32_t mrv11d_c::window_base(unsigned window) const
{
	return iopage_addr(window ? window1_addr : window0_addr);
}

unsigned mrv11d_c::array_pages(void) const
{
	return array.size() / page_words;
}

// Fill the sockets: with the MXV11-B2 set when nothing is named, else with the
// file named in the media tree. A ".lst" is a MACRO-11 listing, whose own
// addresses set the order - its lowest word becomes the first word of the
// array, so a bootstrap assembled at 173000 is page 0. Anything else is read as
// a dump of the array itself.
bool mrv11d_c::load_array(void)
{
	unsigned words = chipsize.value * chipsets.value;

	array.assign(words, unprogrammed);

	if (romfile.value.empty()) {
		unsigned builtin = sizeof(mxv11b2_bootrom) / sizeof(mxv11b2_bootrom[0]);
		if (builtin > words) {
			ERROR("the MXV11-B2 set is %u words, the array holds %u", builtin, words);
			return false;
		}
		memcpy(array.data(), mxv11b2_bootrom, builtin * sizeof(uint16_t));
		describe_contents();
		return true;
	}

	const std::string &path = romfile.value;
	bool listing = path.size() > 4 && path.compare(path.size() - 4, 4, ".lst") == 0;

	if (listing) {
		membuffer->init();
		codelabel_map_c codelabels;
		if (!membuffer->load_macro11_listing(path.c_str(), &codelabels)) {
			ERROR("cannot program the array from \"%s\"", path.c_str());
			return false;
		}
		unsigned first, last;
		membuffer->get_addr_range(&first, &last);
		unsigned listing_words = (last - first) / 2 + 1;
		if (listing_words > words) {
			ERROR("\"%s\" holds %u words, the array holds %u", path.c_str(), listing_words, words);
			return false;
		}
		for (unsigned i = 0; i < listing_words; i++)
			if (membuffer->is_valid(first + 2 * i))
				array[i] = membuffer->get_word(first + 2 * i);
	} else {
		FILE *f = fopen(path.c_str(), "rb");
		if (f == nullptr) {
			ERROR("cannot read \"%s\"", path.c_str());
			return false;
		}
		size_t got = fread(array.data(), 1, words * sizeof(uint16_t), f);
		bool over = fgetc(f) != EOF;
		fclose(f);
		if (got % 2) {
			ERROR("\"%s\" holds an odd number of bytes; an array is words", path.c_str());
			return false;
		}
		if (over) {
			ERROR("\"%s\" is larger than the %u word array", path.c_str(), words);
			return false;
		}
		// a short image leaves the rest of the array unprogrammed
		for (unsigned i = got / 2; i < words; i++)
			array[i] = unprogrammed;
	}

	describe_contents();
	return true;
}

// The name of what is in the sockets: the set they came as, or the file that
// programmed them, without the media tree's path in front of it.
void mrv11d_c::describe_contents(void)
{
	char buf[80];
	if (romfile.value.empty())
		contents.set("MXV11-B2");
	else {
		size_t slash = romfile.value.find_last_of('/');
		contents.set(slash == std::string::npos ? romfile.value : romfile.value.substr(slash + 1));
	}
	snprintf(buf, sizeof buf, "%u KB, %u pages", (unsigned) (array.size() * 2 / 1024),
			array_pages());
	arraysize.set(buf);
}

void mrv11d_c::show_windows(unsigned page0, unsigned page1)
{
	char buf[80];
	if (!bootstrap.value) {
		windows.set("off");
		return;
	}
	snprintf(buf, sizeof buf, "%u / %u", page0, page1);
	windows.set(buf);
}

// The addresses the array answers in memory space, as the strapping leaves them.
void mrv11d_c::show_range(void)
{
	char buf[80];
	if (!directmode.value) {
		range.set("off");
		return;
	}
	uint32_t start = startaddr.value;
	uint32_t end = start + array.size() * 2 - 2;
	snprintf(buf, sizeof buf, "%s..%s", qunibus->addr2text(start), qunibus->addr2text(end));
	range.set(buf);
}

// Show one 512 byte page of the array through a window. The words go into the
// DDR the PRU reads the I/O page from, and the addresses are marked as ROM so
// it reads them from there rather than looking for a register.
void mrv11d_c::map_window(unsigned window, unsigned page)
{
	if (page >= array_pages()) {
		unmap_window(window);
		return;
	}

	uint32_t base = window_base(window);
	const uint16_t *src = &array[page * page_words];
	for (unsigned i = 0; i < page_words; i++)
		if (!ddrmem->iopage_deposit(base + 2 * i, src[i]))
			FATAL("cannot place bootstrap window %u at %s", window, qunibus->addr2text(base));

	if (window_installed[window])
		return;
	for (unsigned i = 0; i < page_words; i++)
		if (!qunibusadapter->register_rom(base + 2 * i)) {
			ERROR("%s is taken by a device register; the bootstrap window cannot answer there",
					qunibus->addr2text(base + 2 * i));
			break;
		}
	window_installed[window] = true;
}

// Let the window answer nothing, as an access beyond the installed array does.
void mrv11d_c::unmap_window(unsigned window)
{
	if (!window_installed[window])
		return;
	uint32_t base = window_base(window);
	for (unsigned i = 0; i < page_words; i++)
		qunibusadapter->unregister_rom(base + 2 * i);
	window_installed[window] = false;
}

void mrv11d_c::map_windows(uint16_t bpcr)
{
	unsigned page0 = bpcr & 0177;
	unsigned page1 = (bpcr >> 8) & 0177;
	map_window(0, page0);
	map_window(1, page1);
	show_windows(page0, page1);
}

// Have the PRU answer the whole array in memory space out of DDR. The device
// range is the one a VCB01 bitmap also takes, and a range claimed over memory
// the machine already carries would put two slaves on one cycle, so both are
// checked before the board answers anything.
bool mrv11d_c::claim_direct(void)
{
	uint32_t start = startaddr.value;
	uint32_t bytes = array.size() * 2;
	uint32_t end = start + bytes - 2;

	if (start % 4096) {
		ERROR("start address %s is not on a 4 KB boundary", qunibus->addr2text(start));
		return false;
	}
	if (end >= qunibus->iopage_start_addr) {
		ERROR("the array at %s..%s reaches the I/O page at %s", qunibus->addr2text(start),
				qunibus->addr2text(end), qunibus->addr2text(qunibus->iopage_start_addr));
		return false;
	}

	uint32_t answered = qunibus->probe_range(start, end);
	if (answered != QUNIBUS_PROBE_NONE) {
		ERROR("%s is answered by the machine already; an array claimed over it "
				"would drive the bus against what answers there", qunibus->addr2text(answered));
		return false;
	}

	if (!ddrmem->set_range(DDRMEM_RANGE_DEVICE, start, end)) {
		ERROR("cannot serve the array at %s..%s", qunibus->addr2text(start),
				qunibus->addr2text(end));
		return false;
	}
	memcpy((void *) (ddrmem->base_virtual->memory.bytes + start), array.data(), bytes);
	direct_claimed = true;
	INFO("array at %s..%s", qunibus->addr2text(start), qunibus->addr2text(end));
	return true;
}

void mrv11d_c::release_direct(void)
{
	if (!direct_claimed)
		return;
	// start > end disables the range
	ddrmem->set_range(DDRMEM_RANGE_DEVICE, 0xffffffff, 0);
	direct_claimed = false;
}

bool mrv11d_c::on_param_changed(parameter_c *param)
{
	if (param == &enabled) {
		if (!enabled.new_value) {
			// off the bus first, then the memory space range
			bool ok = qunibusdevice_c::on_param_changed(param);
			release_direct();
			return ok;
		}
		return qunibusdevice_c::on_param_changed(param);
	}

	// The sockets and the strapping are the operator's while the board is out
	// of the backplane; a board on the bus keeps what it was enabled with.
	if (param == &chipsize || param == &chipsets || param == &romfile || param == &bootstrap
			|| param == &directmode || param == &startaddr) {
		if (enabled.value) {
			ERROR("disable the board before changing its configuration");
			return false;
		}
		if (param == &chipsize) {
			unsigned size = chipsize.new_value;
			if (size < 2048 || size > 32768 || (size & (size - 1))) {
				ERROR("device size %u is not one of 2048, 4096, 8192, 16384, 32768", size);
				return false;
			}
		}
		if (param == &chipsets && (chipsets.new_value < 1 || chipsets.new_value > 8)) {
			ERROR("%u chip sets; the module has 8", chipsets.new_value);
			return false;
		}
		// The array follows from the sockets, and load_array() reads them from
		// the committed values, which the framework writes only after this call
		// returns true. Commit the socket parameters, and put them back when the
		// array cannot be programmed that way.
		unsigned saved_chipsize = chipsize.value, saved_chipsets = chipsets.value;
		std::string saved_romfile = romfile.value;
		if (param == &chipsize)
			chipsize.value = chipsize.new_value;
		else if (param == &chipsets)
			chipsets.value = chipsets.new_value;
		else if (param == &romfile)
			romfile.value = romfile.new_value;

		if (!load_array()) {
			chipsize.value = saved_chipsize;
			chipsets.value = saved_chipsets;
			romfile.value = saved_romfile;
			load_array();
			return false;
		}
		// the strapping the status lines report
		if (param == &bootstrap)
			bootstrap.value = bootstrap.new_value;
		else if (param == &directmode)
			directmode.value = directmode.new_value;
		else if (param == &startaddr)
			startaddr.value = startaddr.new_value;
		show_windows(0, 0);
		show_range();
		return qunibusdevice_c::on_param_changed(param);
	}

	return qunibusdevice_c::on_param_changed(param);
}

// Enabling plugs the board into the backplane: the array is programmed, the
// memory space range claimed and the bootstrap windows opened. A board with
// the bootstrap strapped off implements no register at all, which is what
// keeps it clear of a processor that answers 777520 itself.
bool mrv11d_c::on_before_install(void)
{
	if (array.empty() && !load_array())
		return false;

	register_count = bootstrap.value ? 1 : 0;

	if (!bootstrap.value && !directmode.value) {
		ERROR("neither the bootstrap nor direct addressing is strapped; the board would answer "
				"nothing");
		return false;
	}

	if (directmode.value && !claim_direct())
		return false;

	// The windows follow from the register, which install() clears with the
	// power cycle it drives; opening them here would register the addresses a
	// second time.
	return true;
}

void mrv11d_c::on_after_uninstall(void)
{
	unmap_window(0);
	unmap_window(1);
	show_windows(0, 0);
}

// A write to the register moves both windows. The board switches the pages
// without a cycle in between, so the bus access is held until they are there.
void mrv11d_c::on_after_register_access(qunibusdevice_register_t *device_reg,
		uint8_t unibus_control, DATO_ACCESS access)
{
	UNUSED(unibus_control);
	UNUSED(access);
	if (device_reg != reg_bpcr)
		return;
	uint16_t bpcr = get_register_dato_value(reg_bpcr) & bpcr_page_mask;
	set_register_dati_value(reg_bpcr, bpcr, __func__);
	map_windows(bpcr);
}

// Power-up clears the register, so a machine comes up looking at page 0
// through both windows.
void mrv11d_c::on_power_changed(signal_edge_enum aclo_edge, signal_edge_enum dclo_edge)
{
	UNUSED(aclo_edge);
	if (dclo_edge == SIGNAL_EDGE_NONE || !bootstrap.value)
		return;
	set_register_dati_value(reg_bpcr, 0, __func__);
	map_windows(0);
}

// INIT clears the register as power-up does.
void mrv11d_c::on_init_changed(void)
{
	if (!init_asserted || !bootstrap.value)
		return;
	set_register_dati_value(reg_bpcr, 0, __func__);
	map_windows(0);
}
