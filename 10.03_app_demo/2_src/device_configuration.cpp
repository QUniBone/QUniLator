/* device_configuration.cpp: the set of emulated devices

   Copyright (c) 2018-2020, Joerg Hoppe; 2026, Hans Huebner
   j_hoppe@t-online.de, www.retrocmp.com
   MIT license, see device_configuration.hpp for the full text.

   Construction and destruction order follow the devices menu, which this
   code was extracted from.
*/

#include "panel.hpp"
#include "device_configuration.hpp"

device_configuration_c *device_configuration = nullptr;
std::mutex device_configuration_c::operations_mutex;

/* build_mux_pool: construct a fixed pool of one serial-mux device type, all
   disabled, each instance at a distinct, non-overlapping bus footprint.

   Instance 0 keeps the type's single-device name/address (dzv11, dhv11);
   instances 1.. get the letter-suffix name (dzv11b, dzv11c, ...) matching the
   DL11/DL11b convention. Each instance k is offset from the type's DEC defaults:

     base_addr    addr0    + k * (2 * register_count)   // spaced by the register span
     intr_vector  vec0     + k * 010                    // each device uses RCV(+0) and XMT(+4)
     priority_slot slot    + k * 2                       // each device uses slot and slot+1
     tcp_port[i]  (type base + i) + k * 16               // a 16-port block per instance

   The address/vector/slot steps derive from the device itself (register count,
   the RCV/XMT vector+slot pair), so pooling another mux type later is one more
   call with that type's DEC address and vector base. */
template<typename T>
static std::vector<T *> build_mux_pool(unsigned count, const char *base_name,
		uint32_t addr0, uint16_t vec0)
{
	std::vector<T *> pool;
	pool.reserve(count);
	for (unsigned k = 0; k < count; k++) {
		T *d = new T();
		// instance 0: bare type name; 1->b, 2->c, 3->d
		std::string suffix = k ? std::string(1, char('a' + k)) : std::string();
		d->name.value = std::string(base_name) + suffix;
		d->log_label = std::string(base_name) + suffix;

		uint32_t addr = addr0 + k * (2 * d->register_count);
		uint16_t vec = (uint16_t)(vec0 + k * 010);
		unsigned slot = d->default_priority_slot + k * 2;
		d->set_default_bus_params(addr, slot, vec, d->default_intr_level);

		// shift this instance's per-line default listen ports into its own
		// 16-port block, so listen ports don't collide across instances
		unsigned line_count = sizeof(d->tcp_port) / sizeof(d->tcp_port[0]);
		for (unsigned i = 0; i < line_count; i++)
			d->tcp_port[i].value += k * 16;

		pool.push_back(d);
	}
	return pool;
}

device_configuration_c::device_configuration_c(bool with_emulated_CPU) :
		dl11_rcv_stream(std::ios::app | std::ios::in | std::ios::out),
		dl11b_rcv_stream(std::ios::app | std::ios::in | std::ios::out) {
	// memory mapped blinkenbone panels
	blinkenbone = new blinkenbone_c();

	// demo controller
	demo_io = new demo_io_c();

	// RF11 + RS11 drive
	RF11 = new rf11_c();

	// RL11 + 4 RL01/02 drives
#if defined(UNIBUS)
	RL11 = new RL11_c();
#elif defined(QBUS)
	RL11 = new RLV12_c();
#endif
	paneldriver->reset(); // reset I2C, restart worker()

	// RK11 + drives
#if defined(UNIBUS)
	RK11 = new rk11_c();
#elif defined(QBUS)
	RK11 = new rkv11_c();
#endif

	UDA50 = new uda_c();

	// 2 SLUs; 2nd UART different parameters, default for TU58 interface
	// !!! disable Linux usage, agetty !!!
	DL11 = new slu_c();
	DL11b = new slu_c();
	DL11b->name.value = "DL11b";
	DL11b->log_label = "slub";
	DL11b->priority_slot.value = DL11->priority_slot.value + 1; // next to 1st uart
	DL11b->base_addr.value = 0176500; //  AK6DN boot loader listing
	DL11b->intr_vector.value = 0300; // PDP-11/44 doc
	DL11b->intr_level.value = 4; // PDP-11/44 doc
	DL11b->serialport.value = "ttyS1"; // well, cross-over 2nd UART == UART1 on board
	DL11b->baudrate.value = 38400;
	DL11b->mode.value = "8N1";
	DL11b->error_bits_enable.value = false; // M7856 SW4-7 ?
	DL11b->break_enable.value = true; // TU58 needs BREAK

	// to inject characters into the DL11 receivers (menu scripting on the
	// console, web terminals on both)
	DL11->rs232adapter.stream_rcv = &dl11_rcv_stream;
	DL11->rs232adapter.stream_xmt = NULL; // do not echo output to stdout
	DL11->rs232adapter.baudrate = DL11->baudrate.value; // limit speed of injected chars
	DL11b->rs232adapter.stream_rcv = &dl11b_rcv_stream;
	DL11b->rs232adapter.stream_xmt = NULL;
	DL11b->rs232adapter.baudrate = DL11b->baudrate.value;

	// Serial multiplexers over TCP, held as fixed pools so several of a type can
	// run at once. Both types ship disabled; per-line tcp_role/tcp_host/tcp_port
	// map each line to a TCP endpoint before enabling. Instance sequences:
	//   DZV11 (4-reg span, 010 vector pair): dzv11  760100 v300, dzv11b 760110 v310,
	//                                        dzv11c 760120 v320, dzv11d 760130 v330
	//   DHV11 (8-reg span, 010 vector pair): dhv11  760440 v300, dhv11b 760460 v310
	DZV11 = build_mux_pool<dzv11_c>(4, "dzv11", DZV11_ADDR, DZV11_VECTOR);
	DHV11 = build_mux_pool<dhv11_c>(2, "dhv11", DHV11_ADDR, DHV11_VECTOR);

	LTC = new ltc_c();

#if defined(UNIBUS)
	RX11 = new RX11_c();
	RX211 = new RX211_c();
	m9312 = new m9312_c();
	KE11A = new ke11_c();
	DEUNA = new deuna_c();
	cpu = NULL;
	if (with_emulated_CPU) {
		cpu = new cpu_c();
		cpu->enabled.set(true);
	}
#elif defined(QBUS)
	RX11 = new RXV11_c();
	RX211 = new RXV21_c();
	DELQA = new delqa_c();
	VCB01 = new vcb01_c();
	(void) with_emulated_CPU; // no emulated CPU on QBUS
#endif
}

device_configuration_c::~device_configuration_c() {
#if defined(UNIBUS)
	if (cpu != NULL) {
		cpu->enabled.set(false);
		delete cpu;
	}
	m9312->enabled.set(false);
	delete m9312;
	KE11A->enabled.set(false);
	delete KE11A;
	DEUNA->enabled.set(false);
	delete DEUNA;
#endif

	RX11->enabled.set(false);
	delete RX11;
	RX211->enabled.set(false);
	delete RX211;

#if defined(QBUS)
	DELQA->enabled.set(false);
	delete DELQA;
	VCB01->enabled.set(false);
	delete VCB01;
#endif

	LTC->enabled.set(false);
	delete LTC;
	for (dhv11_c *d : DHV11) {
		d->enabled.set(false);
		delete d;
	}
	for (dzv11_c *d : DZV11) {
		d->enabled.set(false);
		delete d;
	}
	DL11b->enabled.set(false);
	delete DL11b;
	DL11->enabled.set(false);
	delete DL11;

	RF11->enabled.set(false);
	delete RF11;
	RL11->enabled.set(false);
	delete RL11;
	RK11->enabled.set(false);
	delete RK11;
	UDA50->enabled.set(false);
	delete UDA50;

	demo_io->enabled.set(false);
	delete demo_io;

	blinkenbone->enabled.set(false);
	delete blinkenbone;
}
