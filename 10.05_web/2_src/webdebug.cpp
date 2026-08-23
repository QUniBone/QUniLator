/* webdebug.cpp: GET /api/debug/cpu — what the processor holds

   Copyright (c) 2026, Frits Jalvingh
   jal@etc.to
   MIT license, see webserver.hpp for the full text.

   See webdebug.hpp for the three sources this answers from. The one thing
   worth repeating here is why "bus" reports addresses rather than register
   names: a processor decodes its own register file, and the addresses the big
   machines give it (777700..777717) are one apart, which is not a spacing a
   bus master can select between - a DATI carries a word address and bit 0 is
   the byte select. So what a probe of that window can honestly report is which
   addresses answered a cycle and with what, and it is left to whoever reads it
   to say which register a machine of that model keeps there. The processor
   state that *is* laid out as ordinary bus words - the status word at 777776,
   the memory management registers at 777572..777576 - is probed by name.

   On this board's own emulated processors none of that arises: their registers
   are read where they lie.
*/

#include <string.h>

#include <mutex>
#include <string>
#include <vector>

#include "civetweb.h"
#include "picojson.h"

#include "logger.hpp"
#include "timeout.hpp"
#include "qunibus.h"
#include "device.hpp"
#include "device_configuration.hpp"
#include "pdp11disas.hpp"
// for qunibus_disasmemory_c, the windowed DMA the interactive listing uses too
#include "application.hpp"
#if defined(UNIBUS)
#include "cpu.hpp"
#endif

#include "weblog.hpp"
#include "webevents.hpp"
#include "webpower.hpp"
#include "webbus.hpp"
#include "webdebug.hpp"

static void send_json(struct mg_connection *conn, int status, const picojson::value &val) {
	std::string body = val.serialize();
	mg_printf(conn,
			"HTTP/1.1 %d %s\r\n"
			"Content-Type: application/json\r\n"
			"Cache-Control: no-store\r\n"
			"Content-Length: %u\r\n\r\n",
			status, status == 200 ? "OK" : "Error", (unsigned) body.size());
	mg_write(conn, body.c_str(), body.size());
}

static void send_error(struct mg_connection *conn, int status, const std::string &message) {
	picojson::object err;
	err["error"] = picojson::value(message);
	send_json(conn, status, picojson::value(err));
}

static picojson::value num(uint64_t v) {
	return picojson::value((double) v);
}

// An octal address as the console writes it. Whole string or nothing: a typo
// silently read as its leading digits would send the listing somewhere else.
static bool parse_octal_addr(const std::string &s, uint32_t *out) {
	if (s.empty())
		return false;
	char *end = nullptr;
	unsigned long v = strtoul(s.c_str(), &end, 8);
	if (*end != '\0')
		return false;
	*out = (uint32_t) v;
	return true;
}

// The status word, taken apart. The mode fields are only there on a model that
// has modes: on one that does not, PSW<15:12> are not bits that read as
// "kernel", they are bits that do not exist.
//
// Mode 2 has no name on any PDP-11, and mode 1 has one only where there is a
// supervisor mode - an 11/34 has kernel and user and nothing between them - so
// a machine showing either is showing something wrong, and the name says so.
static picojson::value psw_json(uint16_t psw, bool has_modes) {
	static const char *mode_names[4] = { "kernel", "supervisor", "illegal", "user" };
	picojson::object o;
	o["value"] = num(psw);
	o["priority"] = num((psw >> 5) & 7);
	o["t"] = picojson::value((psw & 0020) != 0);
	o["n"] = picojson::value((psw & 0010) != 0);
	o["z"] = picojson::value((psw & 0004) != 0);
	o["v"] = picojson::value((psw & 0002) != 0);
	o["c"] = picojson::value((psw & 0001) != 0);
	o["has_modes"] = picojson::value(has_modes);
	if (has_modes) {
		o["mode"] = picojson::value(std::string(mode_names[(psw >> 14) & 3]));
		o["previous_mode"] = picojson::value(std::string(mode_names[(psw >> 12) & 3]));
	}
	return picojson::value(o);
}

/* ---- the emulated processors ------------------------------------------- */

#if defined(UNIBUS)

// One named register. Only the emulated source reports registers by name: what
// the bus probe finds it reports by address, for the reason in the file header.
static picojson::value reg_json(const char *name, uint16_t value) {
	picojson::object o;
	o["name"] = picojson::value(std::string(name));
	o["value"] = num(value);
	return picojson::value(o);
}

// The eight page address and page descriptor registers of one mode, as two
// arrays of eight, page 0 first. Kept as a pair rather than interleaved: a PAR
// and its PDR are read together, but they are separate registers at separate
// addresses, and pairing them in the JSON would invent a structure the machine
// does not have.
static picojson::value mode_pages_json(const uint16_t *par, const uint16_t *pdr) {
	picojson::array pars, pdrs;
	for (unsigned i = 0; i < 8; i++) {
		pars.push_back(num(par[i]));
		pdrs.push_back(num(pdr[i]));
	}
	picojson::object o;
	o["par"] = picojson::value(pars);
	o["pdr"] = picojson::value(pdrs);
	return picojson::value(o);
}

// The processor the machine carries, dark or not.
//
// device_configuration's emulated_cpu() reads the live "enabled" flag, which
// says what is on the bus this instant: a switched-off machine has its cards
// out and would report no processor at all, though the processor it carries is
// exactly the one this panel is about. So the device set is read the way
// everything else describing the machine reads it, through webpower.
//
// Caller holds device_c::mydevices_mutex.
static unibuscpu_c *carried_cpu_locked(void) {
	for (device_c *d : device_c::mydevices) {
		unibuscpu_c *cpu = dynamic_cast<unibuscpu_c *>(d);
		if (cpu != nullptr && webpower_is_in_machine(d))
			return cpu;
	}
	return nullptr;
}

static const char *run_state_name(enum cpu_base_c::cpu_state_e state) {
	switch (state) {
	case cpu_base_c::cpu_state_running: return "running";
	case cpu_base_c::cpu_state_waiting: return "waiting";
	default:                            return "halted";
	}
}

// A core of the board's own, read where its registers lie.
//
// Registers are reported only while the CPU is halted. Of a running processor
// they would be a set of numbers that were never all true at once - each read
// separately, out of values changing every instruction - and asking for them
// repeatedly is not free either: the emulation runs one instruction per pass of
// a worker holding whatever processor time is left over, so answering costs the
// machine speed. A halted CPU's registers are what a debugger is after anyway.
static picojson::value emulated_json(cpu_base_c *cpu) {
	picojson::object o;
	o["source"] = picojson::value(std::string("emulated"));
	o["device"] = picojson::value(cpu->name.value);
	o["model"] = picojson::value(cpu->type_name.value);

	enum cpu_base_c::cpu_state_e state = cpu->core_get_state();
	o["run_state"] = picojson::value(std::string(run_state_name(state)));
	// what the machine has got through, which is a rate rather than a value
	// and stays readable while it runs
	o["cycle_count"] = num(cpu->cycle_count.value);

	if (state != cpu_base_c::cpu_state_halted) {
		o["available"] = picojson::value(false);
		o["reason"] = picojson::value(std::string(
				state == cpu_base_c::cpu_state_waiting
				? "the processor is in a WAIT: it resumes on the next interrupt, so its "
				  "registers are not being held still. Halt it to read them."
				: "the processor is running: its registers change with every instruction, "
				  "and reading them one at a time would only produce numbers that were "
				  "never all true at once. Halt it to read them."));
		return picojson::value(o);
	}

	cpu_base_c::state_snapshot_c s;
	memset(&s, 0, sizeof s);
	cpu->core_get_snapshot(&s);
	o["available"] = picojson::value(true);

	static const char *reg_names[8] = { "R0", "R1", "R2", "R3", "R4", "R5", "SP", "PC" };
	picojson::array regs;
	for (unsigned i = 0; i < 8; i++)
		regs.push_back(reg_json(reg_names[i], s.r[i]));
	o["registers"] = picojson::value(regs);

	// The stack pointer of each mode, the one in R6 included: a debugger
	// wanting the other stack should not have to work out from the mode which
	// of the two R6 is showing.
	if (s.has_stackpointers) {
		picojson::array sps;
		sps.push_back(reg_json("KSP", s.sp_kernel));
		sps.push_back(reg_json("USP", s.sp_user));
		o["stackpointers"] = picojson::value(sps);
	}

	o["psw"] = psw_json(s.psw, s.has_modes);
	o["ir"] = num(s.ir);
	o["bus_addr"] = num(s.bus_addr);
	o["bus_data"] = num(s.bus_data);
	if (s.has_mmu) {
		picojson::object mmu;
		mmu["enabled"] = picojson::value(s.mmu_enabled);
		mmu["mmr0"] = num(s.mmr0);
		mmu["mmr1"] = num(s.mmr1);
		mmu["mmr2"] = num(s.mmr2);
		// The page registers of each mode, page 0 first. Both sets go out
		// whatever mode the CPU is in: which of them is in force follows from
		// the status word, and a debugger reading a machine that has just
		// trapped out of user mode wants the other set as much as this one.
		mmu["kernel"] = mode_pages_json(s.kernel_par, s.kernel_pdr);
		mmu["user"] = mode_pages_json(s.user_par, s.user_pdr);
		o["mmu"] = picojson::value(mmu);
	}
	return picojson::value(o);
}

#endif // UNIBUS

/* ---- a processor the board can only reach over the bus ------------------ */

// What is probed, as offsets into the I/O page, so the same table serves a
// 16-, 18- or 22-bit machine. The named ones are ordinary bus words; the
// window is walked at word spacing because that is the only spacing a bus
// master has, and its points are reported by address alone (see the file
// header).
struct probe_point_c {
	unsigned offset;    // from the start of the I/O page
	const char *name;   // null inside the register window
};

static const probe_point_c probe_points[] = {
	{ 017572, "MMR0" }, { 017574, "MMR1" }, { 017576, "MMR2" },
	{ 017700, nullptr }, { 017702, nullptr }, { 017704, nullptr },
	{ 017706, nullptr }, { 017710, nullptr }, { 017712, nullptr },
	{ 017714, nullptr }, { 017716, nullptr },
	{ 017776, "PSW" },
};
static const unsigned probe_point_count = sizeof(probe_points) / sizeof(probe_points[0]);

// The last probe, so a page polling this does not put a dozen cycles that are
// expected to time out on a running machine's bus every second. Only the
// negative is worth caching - a window that answered is re-read on every
// request, which is the point of having it - and ?probe=1 asks again anyway.
static std::mutex probe_mutex;
static bool probe_done = false;
static bool probe_answered = false;

// Where a probed word lands. A bounded DMA that times out stays scheduled - the
// PRU may still be working on it - so the buffer it writes into has to outlive
// the call that asked for it. A static one does; a local would be a piece of
// somebody's stack by the time the write came.
static uint16_t probe_words[probe_point_count];

// DATI every point, filling `values` with what answered. Returns how many did,
// and sets *no_grant when the board asked for the bus and did not get it.
// The caller holds web_bus_mutex().
static unsigned probe_bus_locked(std::vector<bool> *answered, std::vector<uint16_t> *values,
		bool *no_grant) {
	unsigned hits = 0;
	*no_grant = false;
	answered->assign(probe_point_count, false);
	values->assign(probe_point_count, 0);
	for (unsigned i = 0; i < probe_point_count; i++) {
		uint32_t addr = qunibus->iopage_start_addr + probe_points[i].offset;
		timeout_c cycle;
		cycle.start_ns(0);
		bool ok = qunibus->probe_word(addr, &probe_words[i], /*share_bus*/true,
				web_bus_probe_timeout_ms);
		if (ok) {
			(*answered)[i] = true;
			(*values)[i] = probe_words[i];
			hits++;
		} else if (cycle.elapsed_ms() >= web_bus_probe_timeout_ms) {
			// nothing granted the bus; the remaining points would each wait
			// out the same quarter second to learn the same thing
			*no_grant = true;
			break;
		}
	}
	return hits;
}

static picojson::value bus_json(bool force) {
	picojson::object o;

	// A switched-off machine is not asked. Nothing would answer, and the
	// cycles would be made against a bus with no arbitrator, which is where a
	// probe waits rather than fails.
	if (!webevents_is_powered()) {
		o["source"] = picojson::value(std::string("none"));
		o["available"] = picojson::value(false);
		o["reason"] = picojson::value(std::string(
				"the machine is switched off, so nothing answers on the bus"));
		return picojson::value(o);
	}

	{
		std::lock_guard<std::mutex> plock(probe_mutex);
		if (probe_done && !probe_answered && !force) {
			o["source"] = picojson::value(std::string("none"));
			o["available"] = picojson::value(false);
			o["reason"] = picojson::value(std::string(
					"no processor state answered when the bus was last probed. A "
					"processor keeps its registers to itself unless the model puts "
					"them in the I/O page, and this one does not: they are reachable "
					"only from its console ODT or its front panel. Probing again "
					"repeats the cycles."));
			return picojson::value(o);
		}
	}

	std::vector<bool> answered;
	std::vector<uint16_t> values;
	unsigned hits;
	bool no_grant = false;
	{
		std::lock_guard<std::mutex> block(web_bus_mutex());
		hits = probe_bus_locked(&answered, &values, &no_grant);
	}
	{
		std::lock_guard<std::mutex> plock(probe_mutex);
		probe_done = true;
		probe_answered = (hits > 0);
	}

	// A bus nobody arbitrates says nothing about the processor: the cycles were
	// never made. Reported apart from silence, because the two look alike from
	// here and mean entirely different things - one is a processor that keeps
	// its registers, the other a backplane that is not running.
	if (no_grant) {
		WEB_INFO("debug: the bus was not granted within %u ms, nothing arbitrates",
				web_bus_probe_timeout_ms);
		o["source"] = picojson::value(std::string("none"));
		o["available"] = picojson::value(false);
		o["reason"] = picojson::value(std::string(
				"QUniLator asked for the bus and was not granted it: nothing on this "
				"backplane is arbitrating. A machine that is running grants QUniLator "
				"its cycles; one whose processor is halted or absent does not."));
		return picojson::value(o);
	}

	picojson::array points;
	for (unsigned i = 0; i < probe_point_count; i++) {
		picojson::object p;
		uint32_t addr = qunibus->iopage_start_addr + probe_points[i].offset;
		p["address"] = num(addr);
		if (probe_points[i].name != nullptr)
			p["name"] = picojson::value(std::string(probe_points[i].name));
		// what the address means on a PDP-11, for the points the table does not
		// name itself - the disassembler's map of the I/O page knows the rest
		std::string info = pdp11disas_address_info(addr);
		if (!info.empty())
			p["info"] = picojson::value(info);
		p["value"] = answered[i] ? num(values[i]) : picojson::value();
		points.push_back(picojson::value(p));
	}
	o["probe"] = picojson::value(points);
	o["registers"] = picojson::value(picojson::array());

	if (hits == 0) {
		o["source"] = picojson::value(std::string("none"));
		o["available"] = picojson::value(false);
		o["reason"] = picojson::value(std::string(
				"no processor state answered on the bus. A processor keeps its "
				"registers to itself unless the model puts them in the I/O page, "
				"and this one does not: they are reachable only from its console "
				"ODT or its front panel."));
		return picojson::value(o);
	}

	o["source"] = picojson::value(std::string("bus"));
	o["available"] = picojson::value(true);
	// The status word is the one probed point whose meaning is the same on
	// every model that answers it, so it is reported taken apart as well.
	// Every model that lays its registers out in the I/O page has modes.
	for (unsigned i = 0; i < probe_point_count; i++)
		if (answered[i] && probe_points[i].name != nullptr
				&& strcmp(probe_points[i].name, "PSW") == 0)
			o["psw"] = psw_json(values[i], true);
	o["reason"] = picojson::value(std::string(
			"read over the bus. The general registers are not among these: a "
			"processor decodes its own register file, so what a bus master can "
			"see is only the processor state the model lays out as bus words."));
	return picojson::value(o);
}

/* ---- the code listing --------------------------------------------------- */

// Which machine the listing is decoded for. An 11/20 has fewer instructions
// than an 11/70, so a disassembler told the wrong model invents instructions
// the CPU would trap on - which is why the caller may name one, and why the
// default is the processor the machine actually carries rather than a guess.
// A model the module does not know leaves *options untouched and answers false.
static bool listing_options(const std::string &requested, pdp11disas_options_c *options) {
	if (!requested.empty())
		return options->set_cpu_model(requested);
#if defined(UNIBUS)
	std::lock_guard<std::mutex> lock(device_c::mydevices_mutex);
	cpu_base_c *pdp11 = dynamic_cast<cpu_base_c *>(carried_cpu_locked());
	// "PDP-11/34" as the device names it; the module strips the prefix itself
	if (pdp11 != nullptr)
		options->set_cpu_model(pdp11->type_name.value);
#endif
	return true;
}

static picojson::value instruction_json(const pdp11disas_instruction_c &i) {
	picojson::object o;
	o["address"] = num(i.addr);
	picojson::array words;
	for (unsigned w = 0; w < i.wordcount; w++)
		words.push_back(num(i.word[w]));
	o["words"] = picojson::value(words);
	o["mnemonic"] = picojson::value(i.mnemonic);
	o["operands"] = picojson::value(i.operands);
	o["known"] = picojson::value(i.known);
	o["available"] = picojson::value(i.available);
	o["truncated"] = picojson::value(i.truncated);
	std::string comment = i.comment();
	if (!comment.empty())
		o["comment"] = picojson::value(comment);
	// The addresses the operands name which mean something on a PDP-11: device
	// registers, the processor and memory management registers, the trap and
	// interrupt vectors. What makes a listing readable without a manual beside
	// it - "@#177564" is the console transmitter, not a number.
	if (!i.known_addresses.empty()) {
		picojson::array known;
		for (uint32_t addr : i.known_addresses) {
			picojson::object k;
			k["address"] = num(addr);
			k["info"] = picojson::value(pdp11disas_address_info(addr));
			known.push_back(picojson::value(k));
		}
		o["known_addresses"] = picojson::value(known);
	}
	return picojson::value(o);
}

// GET /api/debug/disassemble?address=<octal>&count=<n>&model=<name>
//
// Reads the machine's memory over the bus and turns it back into instructions.
// The listing stops where memory stops answering rather than failing: a region
// running into the end of what a card answers is a listing that ends there.
static void disassemble(struct mg_connection *conn, uint32_t address, unsigned count,
		const std::string &model) {
	pdp11disas_options_c options;
	if (!listing_options(model, &options)) {
		send_error(conn, 400, "unknown CPU model \"" + model + "\". Known are: "
				+ pdp11disas_options_c::cpu_model_list());
		return;
	}

	std::vector<pdp11disas_instruction_c> instructions;
	uint32_t next;
	{
		// The same lock every other bus-master transfer takes, held across the
		// whole listing: the adapter fetches a window at a time and would
		// otherwise interleave its DMAs with another request's.
		std::lock_guard<std::mutex> block(web_bus_mutex());
		qunibus_disasmemory_c memory(web_bus_timeout_ms);
		next = pdp11disas_region(options, memory, address, count, &instructions);
	}

	picojson::object res;
	res["address"] = num(address);
	res["next"] = num(next);
	res["model"] = picojson::value(options.cpu_model);
	res["options"] = picojson::value(
			pdp11disas_options_c::options_as_text(options.options));
	picojson::array arr;
	for (const pdp11disas_instruction_c &i : instructions)
		arr.push_back(instruction_json(i));
	res["instructions"] = picojson::value(arr);
	res["complete"] = picojson::value(instructions.size() >= count);
	if (instructions.size() < count) {
		char msg[160];
		snprintf(msg, sizeof msg,
				"nothing readable at %06o: the listing ends where the memory does",
				(unsigned) next);
		res["reason"] = picojson::value(std::string(msg));
	}
	send_json(conn, 200, picojson::value(res));
}

/* ---- the endpoint ------------------------------------------------------- */

static int api_debug_handler(struct mg_connection *conn, void * /*cbdata*/) {
	const struct mg_request_info *ri = mg_get_request_info(conn);
	std::string uri = ri->local_uri ? ri->local_uri : "";
	std::string rest = uri.substr(strlen("/api/debug"));

	if (rest != "/cpu" && rest != "/disassemble") {
		send_error(conn, 404, "unknown debug path");
		return 404;
	}
	if (strcmp(ri->request_method, "GET") != 0) {
		send_error(conn, 405, "GET required");
		return 405;
	}

	if (rest == "/disassemble") {
		char buf[64];
		const char *q = ri->query_string;
		size_t qlen = q ? strlen(q) : 0;
		uint32_t address = 0;
		if (q == nullptr || mg_get_var(q, qlen, "address", buf, sizeof buf) <= 0
				|| !parse_octal_addr(buf, &address)) {
			send_error(conn, 400, "address=<octal> required");
			return 400;
		}
		// The instruction count is decimal, unlike the octal addresses: it
		// counts instructions rather than naming a place in the machine, and
		// "count=10" meaning eight is a trap not worth setting.
		unsigned count = 10;
		if (mg_get_var(q, qlen, "count", buf, sizeof buf) > 0)
			count = (unsigned) strtoul(buf, nullptr, 10);
		if (count < 1 || count > 200) {
			send_error(conn, 400, "count is 1..200 instructions, decimal");
			return 400;
		}
		if (address >= qunibus->addr_space_byte_count || (address & 1)) {
			char msg[128];
			snprintf(msg, sizeof msg,
					"address out of range: an instruction is at an even address, and "
					"the machine's address space is %u bit, ending at %06o",
					qunibus->addr_width, qunibus->addr_space_byte_count - 2);
			send_error(conn, 400, msg);
			return 400;
		}
		std::string model;
		if (mg_get_var(q, qlen, "model", buf, sizeof buf) > 0)
			model = buf;
		disassemble(conn, address, count, model);
		return 200;
	}
	bool force = false;
	if (ri->query_string != nullptr) {
		char buf[16];
		if (mg_get_var(ri->query_string, strlen(ri->query_string), "probe", buf, sizeof buf) > 0)
			force = (buf[0] == '1' || buf[0] == 't' || buf[0] == 'y');
	}

	picojson::value doc;
#if defined(UNIBUS)
	// A processor of the board's own answers for itself, whether or not the
	// machine is switched on: a dark board still carries the cards its
	// configuration names, and the registers it would start from are real.
	//
	// Read under the device lock and released before anything reaches the bus:
	// filling a snapshot takes no other lock, and a probe takes the bus lock.
	{
		std::lock_guard<std::mutex> lock(device_c::mydevices_mutex);
		unibuscpu_c *cpu = carried_cpu_locked();
		cpu_base_c *pdp11 = dynamic_cast<cpu_base_c *>(cpu);
		if (pdp11 != nullptr) {
			doc = emulated_json(pdp11);
		} else if (cpu != nullptr) {
			// the VAX: an emulated processor, but not one this reads
			picojson::object o;
			o["source"] = picojson::value(std::string("none"));
			o["available"] = picojson::value(false);
			o["device"] = picojson::value(cpu->name.value);
			o["reason"] = picojson::value(std::string(
					"the emulated VAX does not publish its registers here"));
			doc = picojson::value(o);
		}
	}
	if (doc.is<picojson::null>())
		doc = bus_json(force);
#else
	doc = bus_json(force);
#endif

	picojson::object o = doc.get<picojson::object>();
	o["powered"] = picojson::value(webevents_is_powered());
	o["halted"] = picojson::value(webevents_is_halted());
	o["addr_width"] = num(qunibus->addr_width);
	send_json(conn, 200, picojson::value(o));
	return 200;
}

void webdebug_register(struct mg_context *ctx) {
	mg_set_request_handler(ctx, "/api/debug", api_debug_handler, nullptr);
}
