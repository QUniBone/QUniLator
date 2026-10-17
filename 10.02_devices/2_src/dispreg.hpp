/* dispreg.hpp: console switch and display register at 777570

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see any device source header for the full text.

   A PDP-11 console carries a switch register and a display register at one
   address, 777570: a read returns the switches the operator set, a write puts
   a word on the display lights. Programs and XXDP diagnostics write progress
   and error codes there and take their options from the switches. A Q-bus
   processor has neither, so this device supplies both on the bus, and the
   dashboard draws them.

   The register is active on DATO, which gives it the separate read and write
   latches the two halves need: DATI answers the switches out of the read
   latch, and each write lands in the write latch, from which the display is
   copied.

   A program can move the display far faster than the dashboard's status poll
   samples it - RSX's idle pattern shifts on every return from WAIT - so each
   change is also logged with the time it happened, and the web layer drains
   that log on every poll and sends it along, for the dashboard to replay at
   the program's own pace.
*/
#ifndef _DISPREG_HPP_
#define _DISPREG_HPP_

#include <atomic>
#include <cstdint>
#include <vector>
#include "qunibusdevice.hpp"
#include "parameter.hpp"

#define DISPREG_ADDR 0777570

// One change of the display: the word written and when, in microseconds of
// the monotonic clock.
struct dispreg_change_t {
	uint64_t us;
	uint16_t value;
};

class dispreg_c: public qunibusdevice_c {
public:
	const char *category(void) const override { return "console"; }

	dispreg_c();
	~dispreg_c();

	/// What a program reads at 777570: the console switches, set by the
	/// operator from the dashboard or the API. A change reaches a running
	/// program at its next read.
	parameter_unsigned_c switches = parameter_unsigned_c(this, "switches", "sw",
			/*readonly*/false, "", "%06o", "Console switch register, read at 777570.", 16, 8);

	/// The word a program last wrote to 777570, as the display lights show it.
	/// Bus INIT leaves it standing, so a diagnostic's last code stays readable
	/// after the program has stopped; powering the machine down clears it.
	parameter_unsigned_c display = parameter_unsigned_c(this, "display", "dsp",
			/*readonly*/true, "", "%06o", "Console display register, written at 777570.", 16, 8);

	bool on_before_install(void) override;
	void on_after_register_access(qunibusdevice_register_t *device_reg,
			uint8_t unibus_control, DATO_ACCESS access) override;
	bool on_param_changed(parameter_c *param) override;
	void on_power_changed(signal_edge_enum aclo_edge, signal_edge_enum dclo_edge) override;
	void on_init_changed(void) override;

	// Move the changes logged since the last call into `out`, oldest first.
	// Returns how many were dropped because the log was full.
	unsigned drain_changes(std::vector<dispreg_change_t> &out);

private:
	qunibusdevice_register_t *reg;

	// The change log, a single-producer single-consumer ring: the bus thread
	// writes it on DATO and the web layer's poll reads it, with no lock on the
	// bus thread's path. A change is logged only when at least
	// CHANGE_SPACING_US has passed since the last one logged, which bounds the
	// log to what a screen can draw however fast the program writes; the
	// display parameter always carries the latest word.
	static const unsigned CHANGES = 256;
	static const uint64_t CHANGE_SPACING_US = 1000;
	dispreg_change_t changes[CHANGES];
	std::atomic<unsigned> change_head { 0 }; // next slot written, by the bus thread
	std::atomic<unsigned> change_tail { 0 }; // next slot read, by the poll
	std::atomic<unsigned> changes_dropped { 0 };
	uint64_t last_logged_us = 0;
	uint16_t last_logged_value = 0;
	bool logged_any = false;
};

#endif // _DISPREG_HPP_
