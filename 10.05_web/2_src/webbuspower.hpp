/* webbuspower.hpp: the backplane's power signals, as far as the board can tell

   Copyright (c) 2026, Frits Jalvingh
   jal@etc.to
   MIT license, see webserver.hpp for the full text.
*/
#ifndef _WEBBUSPOWER_HPP_
#define _WEBBUSPOWER_HPP_

/* BDCOK and BPOK are backplane signals. A supply drives them and every card on
 * the bus reads them; the board is one of those readers, and what it reports
 * here is what its latches saw, nothing else.
 *
 * In particular they are not the emulated machine's power switch. `dc_off`
 * takes the emulated cards out of the machine and clears the logical power
 * flag without touching a bus line, and an emulated processor never drives one
 * at all - so a machine that is switched off in the web interface leaves these
 * exactly where the backplane holds them. The two readings answer different
 * questions and are shown apart.
 *
 * What the board reads is not always somebody else's supply, either: a power
 * cycle drives the DCOK/POK sequence itself, so after one of those the lines
 * carry what the board last put on them. That is still the state of the bus,
 * which is the only thing this reports.
 *
 * A signal has three states here, not two. The board reads these lines through
 * the PRU, which samples them once per pass of its main loop and leaves the
 * result in the mailbox; a PRU that is not looping leaves the last sample
 * there, and a stale sample is a reading of an unknown moment rather than a
 * reading of now. Reporting it as "power good" would be the one wrong answer
 * that looks like the right one, so it is reported as unknown instead.
 */
enum bus_signal_e {
	BUS_SIGNAL_UNKNOWN = -1, // nothing is reading the bus; the state is not known
	BUS_SIGNAL_NEGATED = 0,  // read, and the signal is not asserted
	BUS_SIGNAL_ASSERTED = 1  // read, and the signal is asserted
};

// DCOK and POK as reported: asserted means the supply says that rail is good.
struct bus_power_reading_c {
	bus_signal_e dcok;
	bus_signal_e pok;
};

/* The reading, from the facts the caller gathered.
 *
 * `sampling` is whether the bus is being read at all. `dclo` and `aclo` are the
 * two UNIBUS-named power lines as last sampled, asserted meaning the rail is
 * *failing* - which is why they come out inverted into DCOK and POK. The two
 * buses encode them differently on the wire and the caller has already decoded
 * that; here they are one pair of levels either way.
 */
inline bus_power_reading_c bus_power_read(bool sampling, bool dclo, bool aclo) {
	bus_power_reading_c r;
	if (!sampling) {
		r.dcok = r.pok = BUS_SIGNAL_UNKNOWN;
		return r;
	}
	r.dcok = dclo ? BUS_SIGNAL_NEGATED : BUS_SIGNAL_ASSERTED;
	r.pok = aclo ? BUS_SIGNAL_NEGATED : BUS_SIGNAL_ASSERTED;
	return r;
}

#endif // _WEBBUSPOWER_HPP_
