/* kw11p.hpp: KW11-P programmable real-time clock

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see any device source header for the full text.

   The KW11-P (DEC-11-HPWB-D, M7228) is a programmable interval clock: a 16-bit
   up/down counter, a 16-bit count-set buffer and a 9-bit control/status
   register at three word addresses, interrupting at vector 104 on BR6. It runs
   in single-interrupt, repeat-interrupt and external-event-counter modes at one
   of four rates (100 kHz, 10 kHz, line frequency, external).

   The register model is built from DEC-11-HPWB-D chapters 2-4; the CSR bit
   assignments and count/reload behaviour follow the bit-function table in
   section 3.4 and the interrupt sequence in section 3.6.2. Rather than ticking
   the counter at up to 100 kHz, the counter value is derived from wall-clock
   time elapsed since the clock started and the interrupt is scheduled at the
   computed overflow/underflow instant.
*/
#ifndef _KW11P_HPP_
#define _KW11P_HPP_

#include <cstdint>

#include "utils.hpp"
#include "qunibusdevice.hpp"
#include "parameter.hpp"

// DEC defaults (DEC-11-HPWB-D section 3.2, 3.6.1)
#define KW11P_ADDR   0772540   // CSR; BUF = +2, CTR = +4
#define KW11P_VECTOR 0104      // assigned vector, section 3.6.1
#define KW11P_LEVEL  06        // priority level 6, section 3.6.2
// Own arbitration slot at BR6. Clear of the console DL11 (1,2), the KW11-L line
// clock (3), the serial-mux pools (dzv11 4..11, dhv11 12..15), the RL11 (15), the
// RX floppy controllers (rxv11 16, rxv21 17) and the disk/network controllers
// (uda 20, delqa 21, vcb01 24): a slot shared with another device's request would
// drop one device's interrupts.
#define KW11P_SLOT   22

// register indices
enum kw11p_reg_index {
	kw11p_idx_csr = 0,   // 772540 Control/Status Register
	kw11p_idx_buf,       // 772542 Count Set Buffer
	kw11p_idx_ctr,       // 772544 Counter
	kw11p_idx_count
};

// CSR bit assignments (DEC-11-HPWB-D Figure 3-5, section 3.4)
#define KW11P_CSR_RUN       0000001  // 0  gate clock to counter (start). R/W
#define KW11P_CSR_RATE      0000006  // 1-2 rate select. R/W
#define KW11P_CSR_RATE_SHIFT 1
#define KW11P_CSR_MODE      0000010  // 3  0=single, 1=repeat interrupt. R/W
#define KW11P_CSR_UPCOUNT   0000020  // 4  1=count up, 0=count down. R/W
#define KW11P_CSR_FIX       0000040  // 5  single-step counter (maint). Write only
#define KW11P_CSR_INTR_ENB  0000100  // 6  interrupt enable. R/W
#define KW11P_CSR_DONE      0000200  // 7  underflow/overflow occurred. Read only
#define KW11P_CSR_ERROR     0100000  // 15 second event before service. Read only
// bits 0-6 accept a program write; DONE and ERROR are set by internal logic
#define KW11P_CSR_WRITABLE  0000177

// rate-select codes (Table 3-3)
enum kw11p_rate {
	KW11P_RATE_100KHZ = 0,
	KW11P_RATE_10KHZ  = 1,
	KW11P_RATE_LINE   = 2,
	KW11P_RATE_EXT    = 3
};

class kw11p_c: public qunibusdevice_c {
public:
	const char *category(void) const override { return "clock"; }

	kw11p_c();
	~kw11p_c();

	// line-frequency count rate (Hz) for KW11P_RATE_LINE
	parameter_unsigned_c line_frequency = parameter_unsigned_c(this, "Line frequency", "freq",
			/*readonly*/false, "", "%d", "line-frequency rate: 50/60 Hz", 32, 10);

	void reset(void);

	void worker(unsigned instance) override;
	void worker_wake(void) override;

	void on_after_register_access(qunibusdevice_register_t *device_reg,
			uint8_t unibus_control, DATO_ACCESS access) override;
	bool on_param_changed(parameter_c *param) override;
	void on_power_changed(signal_edge_enum aclo_edge, signal_edge_enum dclo_edge) override;
	void on_init_changed(void) override;

private:
	qunibusdevice_register_t *reg_csr;
	qunibusdevice_register_t *reg_buf;
	qunibusdevice_register_t *reg_ctr;

	intr_request_c intr_request = intr_request_c(this);

	// serializes state between the bus-access callback and the worker
	pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;

	// CSR-derived control state
	bool running;         // RUN: clock gated to counter
	unsigned rate;        // RATE SELECT (kw11p_rate)
	bool mode_repeat;     // MODE: 1 = repeat interrupt
	bool count_up;        // UP/DN: 1 = count up
	bool intr_enable;     // INTR ENB
	bool done;            // DONE (bit 7)
	bool error;           // ERROR (bit 15)

	// counter / buffer
	uint16_t buf;           // count set buffer
	uint16_t count_start;   // value loaded into the counter when the run began
	uint16_t counter_frozen;// counter value while stopped
	int64_t run_start_ns;   // wall time this counting interval began

	// an interrupt was raised and not yet read by the CPU: a second event
	// before this clears sets ERROR in repeat mode (section 3.6.2)
	bool intr_pending;

	int64_t tick_period_ns(void);   // clock period for the selected rate, 0 if external
	uint32_t ticks_to_event(void);  // counts from load to underflow/overflow
	int64_t event_period_ns(void);  // wall time from run start to the event
	uint16_t current_counter(void); // counter value derived from elapsed time
	uint16_t compute_csr_value(void);
	void update_csr_register(void); // store CSR read value, no INTR
	void fire_event(void);          // handle a counter underflow/overflow
	void single_step(void);         // FIX: advance the counter by one count
};

#endif // _KW11P_HPP_
