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
*/
#ifndef _DISPREG_HPP_
#define _DISPREG_HPP_

#include "qunibusdevice.hpp"
#include "parameter.hpp"

#define DISPREG_ADDR 0777570

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

private:
	qunibusdevice_register_t *reg;
};

#endif // _DISPREG_HPP_
