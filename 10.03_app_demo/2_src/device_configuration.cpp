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

	// serial multiplexers over TCP: a 4-line DZV11/DZQ11 and an 8-line
	// DHV11/DHQ11. Both ship disabled; per-line tcp_role/tcp_host/tcp_port
	// parameters map each line to a TCP endpoint before enabling.
	DZV11 = new dzv11_c();
	DHV11 = new dhv11_c();

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
	DHV11->enabled.set(false);
	delete DHV11;
	DZV11->enabled.set(false);
	delete DZV11;
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
