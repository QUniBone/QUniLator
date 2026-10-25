/* rk611.cpp: RK611 disk controller for RK06/RK07 drives

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see any device source header for the full text.

   Register and command behaviour follow chapter 7 of EK-RK067-UG-001, the
   controller/drive interface its chapter 2. Where the manual leaves the
   interrupt logic open, the model keeps the three flops SimH's pdp11_hk.c
   derived from the hardware: a controller flop clocked from IE as Ready
   rises, a drive flop set by an Attention arriving while Ready (or present
   as Ready rises), and an error flop set with Combined Error. Each is set
   only while IE is: UG 7.7 has IE set before an operation begins for it to
   interrupt.
*/

#include <assert.h>
#include <string.h>

#include "logger.hpp"
#include "timeout.hpp"
#include "utils.hpp"
#include "qunibus.h"
#include "qunibusadapter.hpp"
#include "rk611.hpp"
#include "rk0607.hpp"

// RKCS1
#define CS1_GO      0000001
#define CS1_IE      0000100
#define CS1_RDY     0000200
#define CS1_CDT     0002000
#define CS1_CFMT    0010000
#define CS1_CCLR    0100000
// RKCS2 error bits
#define CS2_SCLR    0000040
#define CS2_UFE     0000400
#define CS2_MDS     0001000
#define CS2_PGE     0002000
#define CS2_NEM     0004000
#define CS2_NED     0010000
#define CS2_UPE     0020000
#define CS2_WCE     0040000
#define CS2_DLT     0100000
// RKER
#define ER_ILF      0000001
#define ER_SKI      0000002
#define ER_NXF      0000004
#define ER_DRPAR    0000010
#define ER_FMTE     0000020
#define ER_DTYE     0000040
#define ER_ECH      0000100
#define ER_BSE      0000200
#define ER_HVRC     0000400
#define ER_COE      0001000
#define ER_IDAE     0002000
#define ER_WLE      0004000
#define ER_DTE      0010000
#define ER_OPI      0020000
#define ER_UNS      0040000
#define ER_DCK      0100000
// RKMR1
#define MR1_MS      0000017
#define MR1_PAT     0000020
#define MR1_DMD     0000040
#define MR1_MCLK    0000400
#define MR1_WRITABLE 0001777
#define MR1_MEWD    0002000
#define MR1_ECCW    0020000

// functions, RKCS1 <4:1>
#define FNC_SELECT      000
#define FNC_PACK_ACK    001
#define FNC_DRIVE_CLEAR 002
#define FNC_UNLOAD      003
#define FNC_START       004
#define FNC_RECAL       005
#define FNC_OFFSET      006
#define FNC_SEEK        007
#define FNC_READ        010
#define FNC_WRITE       011
#define FNC_READ_HEADER 012
#define FNC_WRITE_HEADER 013
#define FNC_WRITE_CHECK 014

// UG 7.2.1: GO set for about 800 ms is a controller time-out
static const uint64_t controller_timeout_ns = 800000000ULL;

rk611_c::rk611_c(void) :
		storagecontroller_c()
{
	name.value = "hk";
	type_name.value = "RK611";
	log_label = "hk";

	// base addr, slot, vector, level: UG table 2-1, BR5
	set_default_bus_params(0777440, 23, 0210, 5);

	drivecount = drive_count;
	for (unsigned i = 0; i < drivecount; i++) {
		rk0607_c *d = new rk0607_c(this);
		d->unitno.value = i;
		d->activity_led.value = i % 4;
		d->name.value = name.value + std::to_string(i);
		d->log_label = d->name.value;
		d->parent = this;
		storagedrives.push_back(d);
	}

	register_count = 16;
	static const char *names[16] = { "CS1", "WC", "BA", "DA", "CS2", "DS", "ER", "ASOF", "DC",
			"SPARE", "DB", "MR1", "ECPS", "ECPT", "MR2", "MR3" };
	for (unsigned i = 0; i < register_count; i++) {
		qunibusdevice_register_t *r = &registers[i];
		strcpy_s(r->name, sizeof(r->name), names[i]);
		r->active_on_dati = false;
		// Every write may matter: a register written while GO is set is a
		// Programming Error, and byte writes are not recognized.
		r->active_on_dato = true;
		r->reset_value = 0;
		r->writable_bits = 0xffff;
	}
	busreg_CS1 = &registers[0];
	busreg_WC = &registers[1];
	busreg_BA = &registers[2];
	busreg_DA = &registers[3];
	busreg_CS2 = &registers[4];
	busreg_DS = &registers[5];
	busreg_ER = &registers[6];
	busreg_ASOF = &registers[7];
	busreg_DC = &registers[8];
	busreg_spare = &registers[9];
	busreg_DB = &registers[10];
	busreg_MR1 = &registers[11];
	busreg_ECPS = &registers[12];
	busreg_ECPT = &registers[13];
	busreg_MR2 = &registers[14];
	busreg_MR3 = &registers[15];

	busreg_DB->active_on_dati = true; // reading empties the silo
	busreg_CS1->reset_value = CS1_RDY;
	busreg_CS2->reset_value = 0000100; // IR
	busreg_MR1->reset_value = MR1_MEWD | MR1_ECCW;
	busreg_ECPS->reset_value = 004066;
}

rk611_c::~rk611_c(void)
{
	for (unsigned i = 0; i < drivecount; i++)
		delete storagedrives[i];
}

bool rk611_c::on_param_changed(parameter_c *param)
{
	if (param == &priority_slot) {
		dma_request.set_priority_slot(priority_slot.new_value);
		intr_request.set_priority_slot(priority_slot.new_value);
	} else if (param == &intr_level) {
		intr_request.set_level(intr_level.new_value);
	} else if (param == &intr_vector) {
		intr_request.set_vector(intr_vector.new_value);
	}
	return storagecontroller_c::on_param_changed(param);
}

rk0607_c *rk611_c::drive(unsigned unitno)
{
	if (unitno >= storagedrives.size())
		return nullptr;
	return static_cast<rk0607_c *>(storagedrives[unitno]);
}

// A drive answers the controller when it is switched on.
bool rk611_c::drive_present(rk0607_c *d)
{
	return d != nullptr && d->enabled.value;
}

/*** register values ***/

uint16_t rk611_c::cs1_value(void)
{
	uint16_t v = 0;
	if (go)
		v |= CS1_GO;
	v |= (function & 017) << 1;
	if (cs1_spare)
		v |= 0000040;
	if (interrupt_enable)
		v |= CS1_IE;
	if (ready)
		v |= CS1_RDY;
	v |= (bus_address_ext & 3) << 8;
	if (drive_type_rk07)
		v |= CS1_CDT;
	if (timeout_error)
		v |= 0004000;
	if (format_20)
		v |= CS1_CFMT;
	if (dtc_parity_error)
		v |= 0020000;
	// DI follows the Attention lines: it is reset by INIT, a subsystem clear
	// or Drive Clear to every drive asserting Attention (UG 7.2.1)
	if (asof_value() & 0177400)
		v |= 0040000;
	if (combined_error)
		v |= 0100000;
	return v;
}

uint16_t rk611_c::cs2_value(void)
{
	uint16_t v = unit & 7;
	if (release)
		v |= 0000010;
	if (bus_address_inhibit)
		v |= 0000020;
	if (silo.size() < silo_depth)
		v |= 0000100; // IR
	if (!silo.empty())
		v |= 0000200; // OR
	return v | cs2_errors;
}

uint16_t rk611_c::ds_value(void)
{
	uint16_t v = drive_status;
	if (status_valid)
		v |= 0100000;
	// CDA is the selected drive's Drive Status-Change flip-flop
	rk0607_c *d = selected_drive();
	if (drive_present(d) && d->status_change)
		v |= 0040000;
	return v;
}

uint16_t rk611_c::er_value(void)
{
	return drive_errors | controller_errors;
}

uint16_t rk611_c::asof_value(void)
{
	uint16_t v = offset;
	for (unsigned i = 0; i < drivecount; i++) {
		rk0607_c *d = drive(i);
		if (drive_present(d) && d->status_change)
			v |= 0400 << i;
	}
	return v;
}

uint16_t rk611_c::mr1_value(void)
{
	// MEWD and ECCW read set outside diagnostic mode's write simulation
	return (maintenance1 & MR1_WRITABLE) | MR1_MEWD | MR1_ECCW;
}

void rk611_c::set_full_bus_address(uint32_t addr)
{
	addr &= 0777776;
	bus_address = addr & 0177776;
	bus_address_ext = (addr >> 16) & 3;
}

// make the registers show the state, as the PRU answers DATI from them
void rk611_c::publish(void)
{
	set_register_dati_value(busreg_CS1, cs1_value(), __func__);
	set_register_dati_value(busreg_WC, word_count, __func__);
	set_register_dati_value(busreg_BA, bus_address, __func__);
	set_register_dati_value(busreg_DA, disk_address, __func__);
	set_register_dati_value(busreg_CS2, cs2_value(), __func__);
	set_register_dati_value(busreg_DS, ds_value(), __func__);
	set_register_dati_value(busreg_ER, er_value(), __func__);
	set_register_dati_value(busreg_ASOF, asof_value(), __func__);
	set_register_dati_value(busreg_DC, desired_cylinder, __func__);
	set_register_dati_value(busreg_spare, 0, __func__);
	set_register_dati_value(busreg_DB, silo.empty() ? 0 : silo.front(), __func__);
	set_register_dati_value(busreg_MR1, mr1_value(), __func__);
	// UG 7.2.12: the position register's constant outside a correction, or
	// the one an uncorrectable error leaves
	set_register_dati_value(busreg_ECPS,
			ecc_uncorrectable ? 010040 : (format_20 ? 005066 : 004066), __func__);
	set_register_dati_value(busreg_ECPT, 0, __func__);
	set_register_dati_value(busreg_MR2, message_a, __func__);
	set_register_dati_value(busreg_MR3, message_b, __func__);
}

/*** interrupts ***/

// One request for the three flops; raising it delivers them.
void rk611_c::update_interrupt(void)
{
	if (interrupt_enable && (ci || di || ei)) {
		ci = di = ei = false;
		qunibusadapter->INTR(intr_request, busreg_CS1, cs1_value());
	}
}

void rk611_c::clear_interrupt_flops(void)
{
	ci = di = ei = false;
	qunibusadapter->cancel_INTR(intr_request);
}

void rk611_c::set_error(uint16_t cs2_bits, uint16_t er_bits)
{
	cs2_errors |= cs2_bits;
	controller_errors |= er_bits;
	combined_error = true;
	if (interrupt_enable)
		ei = true;
}

void rk611_c::command_done(void)
{
	DEBUG_FAST("done fnc=%o cs1=%06o cs2=%06o ds=%06o er=%06o da=%06o dc=%06o wc=%06o ba=%06o",
			function, cs1_value(), cs2_value(), ds_value(), er_value(), disk_address,
			desired_cylinder, word_count, bus_address);
	bool rising = !ready;
	go = false;
	ready = true;
	if (rising && interrupt_enable) {
		ci = true;
		if (asof_value() & 0177400)
			di = true;
	}
	publish();
	update_interrupt();
}

// A drive's Drive Status-Change flip-flop set: its Attention line.
void rk611_c::drive_attention(rk0607_c *d)
{
	// the selected drive's latched status is stale now (UG 7.2.6, SVAL)
	if (d == selected_drive())
		status_valid = false;
	if (ready && interrupt_enable)
		di = true;
	publish();
	update_interrupt();
}

void rk611_c::on_drive_status_changed(storagedrive_c *drive)
{
	UNUSED(drive);
}

/*** clears ***/

// CCLR: the controller's registers; the drives keep their state
void rk611_c::controller_clear(void)
{
	abort_epoch++;
	command_for_worker = false;
	go = false;
	function = 0;
	cs1_spare = false;
	interrupt_enable = false;
	ready = true;
	bus_address_ext = 0;
	drive_type_rk07 = false;
	timeout_error = false;
	format_20 = false;
	dtc_parity_error = false;
	combined_error = false;
	word_count = bus_address = disk_address = desired_cylinder = 0;
	offset = 0;
	unit = 0;
	release = bus_address_inhibit = false;
	cs2_errors = 0;
	status_valid = false;
	drive_status = drive_errors = controller_errors = 0;
	maintenance1 = 0;
	message_a = message_b = 0;
	ecc_uncorrectable = false;
	silo.clear();
	clear_interrupt_flops();
}

// SCLR and INIT: the controller, and the Initialize line to the drives
void rk611_c::subsystem_clear(void)
{
	controller_clear();
	for (unsigned i = 0; i < drivecount; i++) {
		rk0607_c *d = drive(i);
		if (drive_present(d))
			d->init_line();
	}
}

void rk611_c::on_power_changed(signal_edge_enum aclo_edge, signal_edge_enum dclo_edge)
{
	storagecontroller_c::on_power_changed(aclo_edge, dclo_edge);
	if (dclo_edge == SIGNAL_EDGE_RAISING) {
		std::lock_guard<std::mutex> lock(state_mutex);
		subsystem_clear();
		publish();
	}
}

void rk611_c::on_init_changed(void)
{
	storagecontroller_c::on_init_changed();
	std::lock_guard<std::mutex> lock(state_mutex);
	if (init_asserted)
		subsystem_clear();
	publish();
}

/*** messages ***/

// Send the command message pair to the drive. With Parity Test set the
// controller sends even parity, which the drive reports as a parity error
// while it carries out the command, and checks the drive's odd-parity reply
// against even parity, which fails (UG 7.2.11). False: the command ends here.
bool rk611_c::exchange_messages(rk0607_c *d, bool check_drive_type)
{
	d->sector_format_20 = format_20;
	if (maintenance1 & MR1_PAT) {
		d->parity_error(function == FNC_RECAL || function == FNC_OFFSET
				|| function == FNC_SEEK || function >= FNC_READ);
		dtc_parity_error = true;
		set_error(0, 0);
	}
	if (check_drive_type && d->is_rk07() != drive_type_rk07) {
		// UG 7.2.6: the drive type must compare before a command executes
		receive_status(d, 0);
		return false;
	}
	return true;
}

// The drive's status reply: RKMR2/RKMR3 hold the message pair asked for, and
// RKDS and RKER latch from it when it is A0/B0.
void rk611_c::receive_status(rk0607_c *d, unsigned message)
{
	latch_status(d->message_a(message), d->message_b(message), message);
}

// RKDS and RKER are the A0/B0 pair; another pair leaves them unchanged and
// Status Valid reset.
void rk611_c::latch_status(uint16_t a0, uint16_t b0, unsigned message)
{
	message_a = a0;
	message_b = b0;
	if (message != 0)
		return;
	uint16_t ds = 0;
	if (a0 & 0000040)
		ds |= 0000001; // DRA
	if (a0 & 0002000)
		ds |= 0000004; // OFST
	if (b0 & 0000100)
		ds |= 0000010; // ACLO
	if (b0 & 0010000)
		ds |= 0000020; // SPLS
	if (b0 & 0020000)
		ds |= 0000040; // DROT
	if (a0 & 0000100)
		ds |= 0000100; // VV
	if (a0 & 0000200)
		ds |= 0000200; // DRDY
	if (a0 & 0000400)
		ds |= 0000400; // DDT
	if (a0 & 0004000)
		ds |= 0004000; // WRL
	if (a0 & 0020000)
		ds |= 0020000; // PIP
	drive_status = ds;

	uint16_t er = 0;
	if (b0 & 0002000)
		er |= ER_SKI;
	if (b0 & 0000400)
		er |= ER_NXF;
	if (b0 & 0001000)
		er |= ER_DRPAR;
	if (b0 & 0000040)
		er |= ER_IDAE;
	if (b0 & 0004000)
		er |= ER_WLE;
	if (b0 & 0040000)
		er |= ER_UNS;
	drive_errors = er;
	status_valid = true;

	if (b0 & 0000200) // FAULT
		set_error(0, 0);
	if (!!(a0 & 0000400) != drive_type_rk07)
		set_error(0, ER_DTYE);
	if (!!(a0 & 0001000) != format_20)
		set_error(0, ER_FMTE);
}

// The controller-to-drive message pair of the command in RKCS1 (UG figures
// 7-15, 7-17 and 7-19 to 7-28): Message A the drive select code, Release,
// the command bit and the head select code; Message B the status request or
// the cylinder.
uint16_t rk611_c::command_message_a(void)
{
	uint16_t v = unit & 7;
	if (release)
		v |= 0000010;
	switch (function) {
	case FNC_PACK_ACK:
		v |= 0004000; // volume valid
		break;
	case FNC_DRIVE_CLEAR:
		v |= 0000400;
		break;
	case FNC_UNLOAD:
		v |= 0002000; // set medium off-line
		break;
	case FNC_START:
		v |= 0000100;
		break;
	case FNC_RECAL:
		v |= 0000040;
		break;
	case FNC_SEEK:
	case FNC_READ:
	case FNC_WRITE:
	case FNC_READ_HEADER:
	case FNC_WRITE_HEADER:
	case FNC_WRITE_CHECK:
		v |= 0000020 | (((disk_address >> 8) & 7) << 12);
		break;
	}
	if (format_20)
		v |= 0001000;
	return with_odd_parity(v);
}

uint16_t rk611_c::command_message_b(void)
{
	uint16_t v = 0;
	if (function == FNC_SELECT)
		v = maintenance1 & 3;
	else if (function == FNC_OFFSET)
		v = (offset & 0277) << 4;
	else if (function == FNC_SEEK || function >= FNC_READ)
		v = (desired_cylinder & 01777) << 4;
	return with_odd_parity(v);
}

uint16_t rk611_c::with_odd_parity(uint16_t v)
{
	v &= 077777;
	return (__builtin_popcount(v) & 1) ? v : (v | 0100000);
}

// UG 7.4: in diagnostic mode each shift register's output feeds the other,
// so the controller receives its own Message B as status A0 and its own
// Message A as B0. The maintenance clock steps the exchange. A command it
// takes through the whole exchange completes inside diagnostic mode, where the
// comparisons are inhibited. One the program takes out of diagnostic mode
// before then is checked as it leaves: a drive select code that does not
// compare, or a Release (Deselect) bit sent, raise the Unit Field Error, and
// the drive type and format bits are compared. A read has had no encoded read
// data when it leaves: Drive Timing Error. A write holds Write Gate, which now
// reaches the drive.
void rk611_c::complete_diagnostic_command(bool in_diagnostic_mode)
{
	if (function >= FNC_READ) {
		bool write = function == FNC_WRITE || function == FNC_WRITE_HEADER;
		rk0607_c *d = selected_drive();
		if (!write)
			set_error(0, ER_DTE);
		else if (drive_present(d))
			d->cmd_write_gate();
		command_done();
		return;
	}
	uint16_t a = command_message_a();
	uint16_t b = command_message_b();
	if (in_diagnostic_mode) {
		message_a = b;
		message_b = a;
		command_done();
		return;
	}
	if ((b & 7) != unit || release)
		set_error(CS2_UFE, 0);
	// with no drive to decode the status request, the reply is checked as A0
	// and B0; a released drive sends none
	if (!release)
		latch_status(b, a, 0);
	command_done();
}

// UG 7.2.7, IDAE: the controller's own check of RKDA and RKDC as a command
// starts - a track address of 3 to 7, or for an RK06 a cylinder of 411 to 814
bool rk611_c::address_invalid(void)
{
	if (((disk_address >> 8) & 7) > 2)
		return true;
	return !drive_type_rk07 && desired_cylinder >= 411 && desired_cylinder <= 814;
}

// RKDA steps to the next sector, track and cylinder (RKDC). True when the
// cylinder changed.
bool rk611_c::advance_disk_address(unsigned sectors)
{
	unsigned s = (disk_address & 037) + 1;
	unsigned t = (disk_address >> 8) & 7;
	bool next_cylinder = false;
	if (s >= sectors) {
		s = 0;
		if (++t >= rk0607_c::tracks_per_cylinder) {
			t = 0;
			desired_cylinder = (desired_cylinder + 1) & 01777;
			next_cylinder = true;
		}
	}
	disk_address = (t << 8) | s;
	return next_cylinder;
}

/*** register access ***/

void rk611_c::start_command(void)
{
	// UG 7.2.1: GO cannot be set while Combined Error is
	if (combined_error) {
		publish();
		update_interrupt();
		return;
	}
	DEBUG_FAST("go fnc=%o unit=%u da=%06o dc=%06o wc=%06o ba=%06o mr1=%06o of=%03o", function,
			unit, disk_address, desired_cylinder, word_count, bus_address, maintenance1, offset);
	clear_interrupt_flops();
	go = true;
	ready = false;
	status_valid = false;
	ecc_uncorrectable = false;
	diagnostic_clocks = 0;
	silo.clear(); // GO sets IR and clears OR
	if (function != FNC_SELECT)
		maintenance1 &= ~MR1_MS;

	if (maintenance1 & MR1_DMD) {
		// The drives are disconnected and the maintenance clock stands in for
		// the control clock; the command completes on its own swapped
		// messages once the program leaves diagnostic mode.
		publish();
		return;
	}
	dispatch_command();
}

void rk611_c::dispatch_command(void)
{
	if (function > FNC_WRITE_CHECK) {
		set_error(0, ER_ILF);
		command_done();
		return;
	}
	rk0607_c *d = selected_drive();
	if (!drive_present(d)) {
		// no Select Acknowledge
		set_error(CS2_NED, 0);
		command_done();
		return;
	}
	if (function >= FNC_READ) {
		command_for_worker = true;
		publish();
		worker_cond.notify_all();
		return;
	}

	if (function == FNC_SEEK && address_invalid())
		set_error(0, ER_IDAE);
	if (!exchange_messages(d, function != FNC_SELECT)) {
		command_done();
		return;
	}
	unsigned message = 0;
	switch (function) {
	case FNC_SELECT:
		message = maintenance1 & 3;
		break;
	case FNC_PACK_ACK:
		d->cmd_pack_acknowledge();
		break;
	case FNC_DRIVE_CLEAR:
		d->cmd_drive_clear();
		break;
	case FNC_UNLOAD:
		d->cmd_unload();
		break;
	case FNC_START:
		d->cmd_start_spindle();
		break;
	case FNC_RECAL:
		d->cmd_recalibrate();
		break;
	case FNC_OFFSET:
		d->cmd_offset(offset);
		break;
	case FNC_SEEK:
		d->selected_head = (disk_address >> 8) & 7;
		d->cmd_seek(desired_cylinder, false);
		break;
	}
	// a drive released by the command (RKCS2 RLS) returns no status
	if (!release)
		receive_status(d, message);
	command_done();
}

void rk611_c::on_after_register_access(qunibusdevice_register_t *device_reg,
		uint8_t unibus_control, DATO_ACCESS access)
{
	std::lock_guard<std::mutex> lock(state_mutex);

	if (unibus_control == QUNIBUS_CYCLE_DATI) {
		if (device_reg == busreg_DB) {
			// reading the output buffer empties the silo by one word; with
			// Output Ready reset it is a Data Late
			if (silo.empty())
				set_error(CS2_DLT, 0);
			else
				silo.pop_front();
			publish();
			update_interrupt();
		}
		return;
	}

	// UG 7.2: the RK611 does not recognize DATOB
	if (access != DATO_WORD) {
		publish();
		return;
	}
	uint16_t v = device_reg->active_dato_flipflops;
	bool diagnostic_mode = maintenance1 & MR1_DMD;

	if (device_reg == busreg_CS1 && (v & CS1_CCLR)) {
		controller_clear();
		publish();
		return;
	}
	if (device_reg == busreg_CS2 && (v & CS2_SCLR)) {
		subsystem_clear();
		publish();
		return;
	}
	if (go && !diagnostic_mode) {
		// UG 7.2.5: a register written with GO set is a Programming Error;
		// the command stops
		abort_epoch++;
		command_for_worker = false;
		set_error(CS2_PGE, 0);
		command_done();
		return;
	}

	switch (device_reg->index) {
	case 0: { // RKCS1
		bool old_ie = interrupt_enable;
		function = (v >> 1) & 017;
		cs1_spare = v & 0000040;
		interrupt_enable = v & CS1_IE;
		bus_address_ext = (v >> 8) & 3;
		drive_type_rk07 = v & CS1_CDT;
		format_20 = v & CS1_CFMT;
		if (old_ie && !interrupt_enable)
			clear_interrupt_flops();
		// UG 7.2.1: a write that sets IE and RDY together forces an
		// interrupt. IE alone enables the next one: VMS's handler for an
		// interrupt no unit owns clears the controller and writes IE, and
		// expects no interrupt from that write.
		if (interrupt_enable && ready && !(v & CS1_GO) && (v & CS1_RDY))
			ci = true;
		if (v & CS1_GO)
			start_command();
		else {
			publish();
			update_interrupt();
		}
		return;
	}
	case 1: // RKWC
		word_count = v;
		break;
	case 2: // RKBA, bit 0 forced 0
		bus_address = v & 0177776;
		break;
	case 3: // RKDA: sector <4:0>, track <10:8>
		disk_address = v & 0003437;
		break;
	case 4: // RKCS2
		unit = v & 7;
		release = v & 0000010;
		bus_address_inhibit = v & 0000020;
		status_valid = false; // a new drive selected
		break;
	case 7: // RKAS/OF: the offset byte, OF6 not stored
		offset = v & 0277;
		break;
	case 8: // RKDC
		desired_cylinder = v & 01777;
		break;
	case 10: // RKDB fills the silo; with Input Ready reset it is a Data Late
		if (silo.size() >= silo_depth)
			set_error(CS2_DLT, 0);
		else
			silo.push_back(v);
		break;
	case 11: { // RKMR1
		bool was_diagnostic = maintenance1 & MR1_DMD;
		bool mclk_rising = (v & MR1_MCLK) && !(maintenance1 & MR1_MCLK);
		maintenance1 = v & MR1_WRITABLE;
		bool pending = go && !command_for_worker;
		if (was_diagnostic && pending && !(maintenance1 & MR1_DMD)) {
			complete_diagnostic_command(false);
			return;
		}
		if (was_diagnostic && pending && mclk_rising
				&& ++diagnostic_clocks == diagnostic_exchange_clocks && function < FNC_READ) {
			complete_diagnostic_command(true);
			return;
		}
		break;
	}
	default: // read-only and unused registers
		break;
	}
	publish();
	update_interrupt();
}

/*** data and header commands ***/

void rk611_c::worker_wake(void)
{
	worker_cond.notify_all();
}

void rk611_c::worker(unsigned instance)
{
	UNUSED(instance);
	worker_init_realtime_priority(rt_device);
	std::unique_lock<std::mutex> lock(state_mutex);
	while (!workers_terminate) {
		worker_cond.wait_for(lock, std::chrono::milliseconds(100),
				[this] { return command_for_worker || workers_terminate; });
		if (workers_terminate)
			break;
		if (!command_for_worker)
			continue;
		command_for_worker = false;
		lock.unlock();
		execute_transfer();
		lock.lock();
	}
}

// Under state_mutex: is the command still the one this thread started?
bool rk611_c::transfer_step_ok(uint32_t epoch)
{
	return epoch == abort_epoch && !workers_terminate;
}

// Wait, without the lock, until the time has come or the command is gone.
bool rk611_c::wait_until(uint64_t when_ns, uint32_t epoch)
{
	for (;;) {
		{
			std::lock_guard<std::mutex> lock(state_mutex);
			if (!transfer_step_ok(epoch))
				return false;
		}
		uint64_t now = timeout_c::abstime_ns();
		if (now >= when_ns)
			return true;
		uint64_t left = when_ns - now;
		timeout_c::wait_ns(left > 1000000 ? 1000000 : left);
	}
}

// Wait for the drive to finish positioning and come ready. A drive that does
// not within the controller time-out ends the command with CTO. False: the
// command is over.
bool rk611_c::wait_drive_ready(rk0607_c *d, uint32_t epoch)
{
	uint64_t deadline = timeout_c::abstime_ns() + controller_timeout_ns;
	for (;;) {
		{
			std::lock_guard<std::mutex> lock(state_mutex);
			if (!transfer_step_ok(epoch))
				return false;
			if (!drive_present(d)) {
				set_error(CS2_NED, 0);
				command_done();
				return false;
			}
			if (d->drive_ready())
				return true;
			if (timeout_c::abstime_ns() >= deadline) {
				timeout_error = true;
				set_error(0, 0);
				receive_status(d, 0);
				command_done();
				return false;
			}
		}
		timeout_c::wait_us(200);
	}
}

// One DMA to or from memory. end_addr: the last address the transfer reached.
//
// With RKCS2 BAI every word goes to or comes from the one address in RKBA.
// Memory there ends up holding the last word read from the disk, and every
// word written comes from it, so one bus cycle per block stands for them.
bool rk611_c::dma(uint8_t cycle, uint32_t addr, uint16_t *buffer, unsigned wordcount,
		uint32_t &end_addr)
{
	end_addr = addr;
	if (wordcount == 0)
		return true;
	if (!bus_address_inhibit) {
		qunibusadapter->DMA(dma_request, true, cycle, addr, buffer, wordcount);
		end_addr = dma_request.qunibus_end_addr;
		return dma_request.success;
	}
	if (cycle == QUNIBUS_CYCLE_DATO) {
		qunibusadapter->DMA(dma_request, true, cycle, addr, buffer + wordcount - 1, 1);
		return dma_request.success;
	}
	qunibusadapter->DMA(dma_request, true, cycle, addr, buffer, 1);
	for (unsigned i = 1; i < wordcount; i++)
		buffer[i] = buffer[0];
	return dma_request.success;
}

// Search the passing headers for the one RKDA and RKDC name. The controller
// reads up to 32 headers (UG 7.2.7, OPI), the first passing after the
// reference time, or now. On success, position is the physical sector to
// transfer; otherwise the command has ended.
bool rk611_c::find_header(rk0607_c *d, unsigned &position, uint32_t epoch, uint64_t reference)
{
	uint64_t now = reference ? reference : timeout_c::abstime_ns();
	uint64_t sector_ns;
	bool vrc_seen = false;
	{
		std::lock_guard<std::mutex> lock(state_mutex);
		if (!transfer_step_ok(epoch))
			return false;
		unsigned cyl = d->cylinder;
		unsigned track = (disk_address >> 8) & 7;
		unsigned sector = disk_address & 037;
		unsigned sectors = d->track_sectors(cyl, track);
		sector_ns = d->scaled_ns(rk0607_c::revolution_ns) / sectors;
		unsigned first = (d->sector_under_heads(now, sectors) + 1) % sectors;
		// word 2 as UG figure 2-3 has it, Good Sector flags aside
		uint16_t want_w2 = (sector & 037) | ((track & 3) << 5) | (format_20 ? 01000 : 0);
		for (unsigned i = 0; i < 32; i++) {
			unsigned p = (first + i) % sectors;
			uint16_t h[3];
			d->header_at(cyl, track, p, h);
			bool vrc_ok = h[2] == (uint16_t) (h[0] ^ h[1]);
			bool match = h[0] == desired_cylinder && (h[1] & 037777) == want_w2;
			if (!match) {
				if (!vrc_ok)
					vrc_seen = true;
				continue;
			}
			if (!vrc_ok) {
				set_error(0, ER_HVRC);
				receive_status(d, 0);
				command_done();
				return false;
			}
			if ((h[1] & 0140000) != 0140000) {
				set_error(0, ER_BSE);
				receive_status(d, 0);
				command_done();
				return false;
			}
			position = p;
			return true;
		}
	}
	// 32 headers pass without the one wanted
	if (!wait_until(now + 33 * sector_ns, epoch))
		return false;
	std::lock_guard<std::mutex> lock(state_mutex);
	if (!transfer_step_ok(epoch))
		return false;
	set_error(0, ER_OPI | (vrc_seen ? ER_HVRC : 0));
	receive_status(d, 0);
	command_done();
	return false;
}

void rk611_c::execute_transfer(void)
{
	uint32_t epoch;
	rk0607_c *d;
	unsigned fnc;
	bool write_cmd;
	uint64_t started;
	rk0607_c::seek_result_e seek;
	{
		std::lock_guard<std::mutex> lock(state_mutex);
		epoch = abort_epoch;
		fnc = function;
		d = selected_drive();
		if (!go || !drive_present(d))
			return;
		started = timeout_c::abstime_ns();
		d->begin_access(started);
		d->selected_head = (disk_address >> 8) & 7;
		if (!exchange_messages(d, true)) {
			command_done();
			return;
		}
		write_cmd = fnc == FNC_WRITE || fnc == FNC_WRITE_HEADER;
		// The implied seek (UG 7.3). With an address the controller refuses,
		// the seek still goes out and the transfer does not follow.
		bool invalid = address_invalid();
		if (invalid)
			set_error(0, ER_IDAE);
		seek = d->cmd_seek(desired_cylinder, write_cmd);
		if (invalid || seek == rk0607_c::SEEK_REFUSED) {
			receive_status(d, 0);
			command_done();
			return;
		}
	}
	if (seek == rk0607_c::SEEK_IGNORED) {
		// The drive was not ready and ignored the seek (UG 7.7): the
		// Attention the controller waits for never comes, and GO stays set
		// until the controller time-out.
		if (!wait_until(started + controller_timeout_ns, epoch))
			return;
		std::lock_guard<std::mutex> lock(state_mutex);
		if (!transfer_step_ok(epoch))
			return;
		timeout_error = true;
		set_error(0, 0);
		receive_status(d, 0);
		command_done();
		return;
	}
	// The drive's Attention ends the seek; the controller takes its status
	// and clears the Attention. The disk position the transfer starts from is
	// the one when the command started, or when the heads arrived if they
	// moved: what the controller itself takes to get here on the board is no
	// time the hardware spends.
	bool moved;
	{
		std::lock_guard<std::mutex> lock(state_mutex);
		moved = d->positioning();
	}
	if (!wait_drive_ready(d, epoch))
		return;
	uint64_t reference = moved ? timeout_c::abstime_ns() : started;
	{
		std::lock_guard<std::mutex> lock(state_mutex);
		if (!transfer_step_ok(epoch))
			return;
		receive_status(d, 0);
		d->cmd_drive_clear();
		receive_status(d, 0);
		if (combined_error) {
			command_done();
			return;
		}
	}

	uint64_t rev_ns = d->scaled_ns(rk0607_c::revolution_ns);
	// the controller's sector count: CFMT selects 20 sectors of 18-bit words
	unsigned sectors = format_20 ? 20 : rk0607_c::sectors_per_track;
	uint16_t buffer[rk0607_c::words_per_sector];
	uint16_t compare[rk0607_c::words_per_sector];

	if (fnc == FNC_READ_HEADER) {
		// the three words of the first header to pass the heads
		unsigned p, track_sectors;
		uint64_t start;
		{
			std::lock_guard<std::mutex> lock(state_mutex);
			track_sectors = d->track_sectors(d->cylinder, (disk_address >> 8) & 3);
			p = (d->sector_under_heads(reference, track_sectors) + 1) % track_sectors;
			start = d->sector_start_ns(p, reference, track_sectors);
		}
		if (!wait_until(start, epoch))
			return;
		std::lock_guard<std::mutex> lock(state_mutex);
		if (!transfer_step_ok(epoch))
			return;
		uint16_t h[3];
		d->header_at(d->cylinder, (disk_address >> 8) & 3, p, h);
		silo.push_back(h[0]);
		silo.push_back(h[1]);
		silo.push_back(h[2]);
		// the header has passed; the next one begins a third of a sector on
		d->end_access(start + rev_ns / track_sectors / 3);
		receive_status(d, 0);
		command_done();
		return;
	}

	if (fnc == FNC_WRITE_HEADER) {
		// UG 7.3: from Index to Index, each sector's header from memory and
		// an all-zero data field. RKWC is -66 for a 22-sector track, -60 for
		// a 20-sector one.
		unsigned cyl, track, words;
		uint32_t addr;
		uint64_t index;
		{
			std::lock_guard<std::mutex> lock(state_mutex);
			if (!transfer_step_ok(epoch))
				return;
			d->cmd_write_gate();
			if (d->fault()) {
				receive_status(d, 0);
				command_done();
				return;
			}
			cyl = d->cylinder;
			track = (disk_address >> 8) & 3;
			unsigned wc = word_count ? 0x10000 - word_count : 0x10000;
			words = wc < 3 * sectors ? wc : 3 * sectors;
			addr = full_bus_address();
			index = d->sector_start_ns(0, timeout_c::abstime_ns());
		}
		if (!wait_until(index, epoch))
			return;
		uint16_t headers[3 * rk0607_c::sectors_per_track];
		memset(headers, 0, sizeof headers);
		uint32_t end_addr;
		bool ok = dma(QUNIBUS_CYCLE_DATI, addr, headers, words, end_addr);
		static const uint8_t zeros[rk0607_c::sectors_per_track * 2 * rk0607_c::words_per_sector] = { 0 };
		if (ok)
			d->image_write((uint8_t *) zeros, d->geometry.get_image_offset(cyl, track, 0),
					sizeof zeros);
		if (!wait_until(index + rev_ns, epoch))
			return;
		std::lock_guard<std::mutex> lock(state_mutex);
		if (!transfer_step_ok(epoch))
			return;
		// a transfer stopped by NXM counts the word that found no memory
		unsigned done_words = ok ? words : (end_addr - addr) / 2 + 1;
		word_count = (word_count + done_words) & 0xffff;
		if (!bus_address_inhibit)
			set_full_bus_address(addr + 2 * done_words);
		if (!ok)
			set_error(CS2_NEM, 0);
		else {
			DEBUG_FAST("write header cyl %u track %u: %06o %06o %06o, %06o %06o %06o", cyl, track,
					headers[0], headers[1], headers[2], headers[3], headers[4], headers[5]);
			d->set_track_headers(cyl, track, headers, sectors);
		}
		d->end_access(index + rev_ns);
		receive_status(d, 0);
		command_done();
		return;
	}

	// Read Data, Write Data, Write Check: sector by sector, spiralling across
	// tracks and cylinders until the word count reaches zero.
	//
	// Write Data and Write Check take their words from memory into the
	// 66-word silo ahead of the disk: the silo fills while the controller
	// searches for the header (UG 7.3), so RKWC and RKBA have moved by up to
	// a silo's worth beyond the sector being written when it starts - and when
	// a header search ends the command.
	bool from_memory = fnc == FNC_WRITE || fnc == FNC_WRITE_CHECK;
	unsigned prefetched = 0; // words of the coming sector already in the silo
	// words still to move between memory and the silo: RKWC 0 is 65536
	unsigned remaining;
	{
		std::lock_guard<std::mutex> lock(state_mutex);
		remaining = word_count ? 0x10000 - word_count : 0x10000;
	}
	uint16_t *silo_words = fnc == FNC_WRITE ? buffer : compare;
	bool first_sector = true;
	for (;;) {
		if (from_memory && prefetched == 0) {
			uint32_t addr, end_addr;
			unsigned want;
			{
				std::lock_guard<std::mutex> lock(state_mutex);
				if (!transfer_step_ok(epoch))
					return;
				want = remaining < silo_depth ? remaining : silo_depth;
				addr = full_bus_address();
			}
			bool ok = dma(QUNIBUS_CYCLE_DATI, addr, silo_words, want, end_addr);
			std::lock_guard<std::mutex> lock(state_mutex);
			if (!transfer_step_ok(epoch))
				return;
			unsigned taken = ok ? want : (bus_address_inhibit ? 1 : (end_addr - addr) / 2 + 1);
			remaining -= taken;
			word_count = (word_count + taken) & 0xffff;
			if (!bus_address_inhibit)
				set_full_bus_address(addr + 2 * taken);
			if (!ok) {
				set_error(CS2_NEM, 0);
				receive_status(d, 0);
				command_done();
				return;
			}
			prefetched = taken;
			publish();
		}
		unsigned position;
		if (!find_header(d, position, epoch, first_sector ? reference : 0))
			return;
		uint64_t start, sector_ns;
		unsigned cyl, track, words;
		uint32_t addr;
		{
			std::lock_guard<std::mutex> lock(state_mutex);
			if (!transfer_step_ok(epoch))
				return;
			uint64_t now = timeout_c::abstime_ns();
			cyl = d->cylinder;
			track = (disk_address >> 8) & 3;
			unsigned track_sectors = d->track_sectors(cyl, track);
			sector_ns = rev_ns / track_sectors;
			start = first_sector ? d->sector_start_ns(position, reference, track_sectors) : now;
			// the sector's words still to come from memory, past the silo's
			unsigned rest = rk0607_c::words_per_sector - (from_memory ? prefetched : 0);
			words = remaining < rest ? remaining : rest;
			addr = full_bus_address();
			if (write_cmd) {
				d->cmd_write_gate();
				if (d->fault()) {
					receive_status(d, 0);
					command_done();
					return;
				}
			}
		}
		if (!wait_until(start, epoch))
			return;
		first_sector = false;

		uint64_t offset_bytes = d->geometry.get_image_offset(cyl, track, position);
		uint32_t end_addr = addr;
		bool ok = true;
		int transferred = (int) words;
		bool mismatch = false;
		switch (fnc) {
		case FNC_READ:
			d->image_read((uint8_t *) buffer, offset_bytes, sizeof buffer);
			ok = dma(QUNIBUS_CYCLE_DATO, addr, buffer, words, end_addr);
			break;
		case FNC_WRITE:
			// a short last sector is zero-filled; it goes to the medium as it
			// passes the heads, below
			memset(buffer + prefetched, 0, sizeof buffer - 2 * prefetched);
			ok = dma(QUNIBUS_CYCLE_DATI, addr, buffer + prefetched, words, end_addr);
			break;
		case FNC_WRITE_CHECK: {
			d->image_read((uint8_t *) buffer, offset_bytes, sizeof buffer);
			ok = dma(QUNIBUS_CYCLE_DATI, addr, compare + prefetched, words, end_addr);
			unsigned total = prefetched + (ok ? words : 0);
			for (unsigned i = 0; i < total; i++)
				if (compare[i] != buffer[i]) {
					// RKBA ends at the mismatched word + 2 (UG 7.2.5): the
					// silo's words past it are given back
					mismatch = true;
					transferred = (int) (i + 1) - (int) prefetched;
					break;
				}
			break;
		}
		}
		prefetched = 0;
		if (!ok)
			// the word that found no memory counts: RKBA ends past it
			transferred = bus_address_inhibit ? 1 : (int) ((end_addr - addr) / 2 + 1);

		{
			// The silo takes the sector's words ahead of the disk, so RKWC and
			// RKBA have moved before the sector has passed the heads.
			std::lock_guard<std::mutex> lock(state_mutex);
			if (!transfer_step_ok(epoch))
				return;
			remaining -= transferred;
			word_count = (uint16_t) (word_count + transferred);
			if (!bus_address_inhibit)
				set_full_bus_address((uint32_t) ((int32_t) addr + 2 * transferred));
			if (!ok) {
				set_error(CS2_NEM, 0);
				receive_status(d, 0);
				command_done();
				return;
			}
			if (mismatch) {
				set_error(CS2_WCE, 0);
				receive_status(d, 0);
				command_done();
				return;
			}
			publish();
		}

		// The rest of the sector and its ECC pass the heads. The words in the
		// silo still have to go to the disk when the last one has come from
		// memory: up to 66 of them and the two ECC words at the disk's 3.7
		// microseconds a word (UG 1.x) - the time a program has to stop a
		// write before its ECC is recorded.
		uint64_t drained = from_memory
				? timeout_c::abstime_ns() + d->scaled_ns((silo_depth + 3) * 3700ULL) : 0;
		uint64_t sector_end = start + sector_ns > drained ? start + sector_ns : drained;
		bool passed = wait_until(sector_end, epoch);
		if (fnc == FNC_WRITE)
			d->image_write((uint8_t *) buffer, offset_bytes, sizeof buffer);
		if (!passed) {
			// a write stopped here leaves the sector without its ECC
			if (fnc == FNC_WRITE) {
				std::lock_guard<std::mutex> lock(state_mutex);
				d->bad_ecc.insert(d->sector_key(cyl, track, position));
			}
			return;
		}

		bool next_cylinder = false;
		{
			std::lock_guard<std::mutex> lock(state_mutex);
			if (!transfer_step_ok(epoch))
				return;
			d->end_access(sector_end);
			unsigned key = d->sector_key(cyl, track, position);
			bool data_check = false;
			if (fnc == FNC_WRITE)
				d->bad_ecc.erase(key);
			else if (d->bad_ecc.count(key)) {
				// UG 7.5: an error burst past what the ECC corrects
				data_check = true;
				ecc_uncorrectable = true;
				set_error(0, ER_DCK | ER_ECH);
			}
			next_cylinder = advance_disk_address(sectors);
			d->selected_head = (disk_address >> 8) & 7;
			if (remaining == 0 || data_check) {
				receive_status(d, 0);
				command_done();
				return;
			}
			if (next_cylinder) {
				if (desired_cylinder >= d->cylinder_count()) {
					// words left after the last sector of the pack
					set_error(0, ER_COE);
					receive_status(d, 0);
					command_done();
					return;
				}
				// a single-cylinder seek; the next sector is a new search
				if (d->cmd_seek(desired_cylinder, write_cmd) != rk0607_c::SEEK_STARTED) {
					receive_status(d, 0);
					command_done();
					return;
				}
				first_sector = true;
			}
			publish();
		}
		if (next_cylinder) {
			if (!wait_drive_ready(d, epoch))
				return;
			reference = timeout_c::abstime_ns();
			std::lock_guard<std::mutex> lock(state_mutex);
			if (!transfer_step_ok(epoch))
				return;
			d->cmd_drive_clear();
			publish();
		}
	}
}
