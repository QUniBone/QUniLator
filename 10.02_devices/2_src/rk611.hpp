/* rk611.hpp: RK611 disk controller for RK06/RK07 drives

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see any device source header for the full text.

   The UNIBUS RK611 as chapter 7 of DEC's RK06/RK07 Disk Drive User's Guide
   (EK-RK067-UG-001) programs it: fifteen registers at 777440, an 18-bit NPR
   data path through a 66-word silo, BR5 at vector 210, and the thirteen disk
   commands. Up to eight drives hang off it.

   The register frontend runs on the adapter's event thread and finishes the
   nondata commands there, the way the controller exchanges a pair of serial
   messages with a drive in a few microseconds. The data and header commands
   - implied seek, header search, transfer - run on the worker thread.

   The controller reaches a drive the way the hardware does: it sends the
   command message, the drive acts on it, and RKDS and RKER are latched from
   the status messages A0 and B0 the drive returns.

   Diagnostic mode (RKMR1 DMD) disconnects the drives, as the manual has it,
   but does not simulate the bit-serial data path its maintenance clock steps:
   a command started in diagnostic mode completes on the status its own
   swapped messages make, inside diagnostic mode or as the program leaves it,
   depending on how far the maintenance clock took it.
*/
#ifndef _RK611_HPP_
#define _RK611_HPP_

#include <stdint.h>
#include <condition_variable>
#include <deque>
#include <mutex>

#include "qunibusadapter.hpp"
#include "storagecontroller.hpp"

class rk0607_c;

class rk611_c: public storagecontroller_c {
	friend class rk0607_c;
public:
	static const unsigned drive_count = 8;

	rk611_c(void);
	~rk611_c(void);

	bool on_param_changed(parameter_c *param) override;
	void on_after_register_access(qunibusdevice_register_t *device_reg, uint8_t unibus_control,
			DATO_ACCESS access) override;
	void on_power_changed(signal_edge_enum aclo_edge, signal_edge_enum dclo_edge) override;
	void on_init_changed(void) override;
	void on_drive_status_changed(storagedrive_c *drive) override;
	void worker(unsigned instance) override;
	void worker_wake(void) override;

private:
	// guards every register, the silo, the interrupt flops and the drives'
	// mutable state
	std::mutex state_mutex;
	std::condition_variable worker_cond;

	qunibusdevice_register_t *busreg_CS1, *busreg_WC, *busreg_BA, *busreg_DA, *busreg_CS2,
			*busreg_DS, *busreg_ER, *busreg_ASOF, *busreg_DC, *busreg_spare, *busreg_DB,
			*busreg_MR1, *busreg_ECPS, *busreg_ECPT, *busreg_MR2, *busreg_MR3;

	dma_request_c dma_request { this };
	intr_request_c intr_request { this };

	// RKCS1
	bool go = false;
	unsigned function = 0;     // F4-F1
	bool cs1_spare = false;    // bit 5
	bool interrupt_enable = false;
	bool ready = true;
	unsigned bus_address_ext = 0; // BA17, BA16
	bool drive_type_rk07 = false; // CDT
	bool timeout_error = false; // CTO
	bool format_20 = false;    // CFMT
	bool dtc_parity_error = false;
	bool combined_error = false; // CERR, latched

	uint16_t word_count = 0;
	uint16_t bus_address = 0;
	uint16_t disk_address = 0;
	uint16_t desired_cylinder = 0;
	uint8_t offset = 0;

	// RKCS2
	unsigned unit = 0;
	bool release = false;
	bool bus_address_inhibit = false;
	uint16_t cs2_errors = 0;   // bits <15:8>

	// RKDS and RKER as latched from the selected drive's status messages
	bool status_valid = false;
	uint16_t drive_status = 0; // without SVAL, CDA
	uint16_t drive_errors = 0; // the drive's own RKER bits
	uint16_t controller_errors = 0; // the RKER bits the controller detects

	uint16_t maintenance1 = 0; // RKMR1 bits <9:0>
	// Maintenance clock pulses given a command started in diagnostic mode,
	// and the count that takes a message pair out and the reply back in. The
	// UG gives a control clock as four maintenance clock pulses and a message
	// as 16 control clocks, but not the controller's turnaround. The count
	// lies between the 90 pulses DEC's CZR6K diagnostic gives a select it
	// expects to be checked as it leaves diagnostic mode and the 94 it gives
	// one it expects to have completed inside it.
	unsigned diagnostic_clocks = 0;
	static const unsigned diagnostic_exchange_clocks = 92;
	bool ecc_uncorrectable = false; // RKECPS shows 010040
	uint16_t message_a = 0;    // RKMR2
	uint16_t message_b = 0;    // RKMR3

	std::deque<uint16_t> silo;
	static const unsigned silo_depth = 66;

	// interrupt flops: controller, drive, error
	bool ci = false, di = false, ei = false;

	// a data or header command for worker()
	bool command_for_worker = false;
	uint32_t abort_epoch = 0;  // bumped by a controller clear, a subsystem clear, INIT

	// all below run under state_mutex
	rk0607_c *drive(unsigned unitno);
	rk0607_c *selected_drive(void) { return drive(unit); }
	bool drive_present(rk0607_c *d);
	uint16_t cs1_value(void);
	uint16_t cs2_value(void);
	uint16_t ds_value(void);
	uint16_t er_value(void);
	uint16_t asof_value(void);
	uint16_t mr1_value(void);
	uint32_t full_bus_address(void) {
		return ((uint32_t) bus_address_ext << 16) | bus_address;
	}
	void set_full_bus_address(uint32_t addr);
	void publish(void);
	void update_interrupt(void);
	void clear_interrupt_flops(void);

	void controller_clear(void);
	void subsystem_clear(void);
	void start_command(void);
	void dispatch_command(void);
	bool exchange_messages(rk0607_c *d, bool check_drive_type);
	void receive_status(rk0607_c *d, unsigned message);
	void latch_status(uint16_t a0, uint16_t b0, unsigned message);
	uint16_t command_message_a(void);
	uint16_t command_message_b(void);
	static uint16_t with_odd_parity(uint16_t v);
	void complete_diagnostic_command(bool in_diagnostic_mode);
	bool address_invalid(void);
	bool advance_disk_address(unsigned sectors);
	void set_error(uint16_t cs2_bits, uint16_t er_bits);
	void command_done(void);
	void drive_attention(rk0607_c *d);

	// the data and header commands, on the worker thread
	void execute_transfer(void);
	bool transfer_step_ok(uint32_t epoch);
	bool wait_drive_ready(rk0607_c *d, uint32_t epoch);
	bool wait_until(uint64_t when_ns, uint32_t epoch);
	bool find_header(rk0607_c *d, unsigned &position, uint32_t epoch, uint64_t reference);
	bool dma(uint8_t cycle, uint32_t addr, uint16_t *buffer, unsigned wordcount, uint32_t &end_addr);
};

#endif
