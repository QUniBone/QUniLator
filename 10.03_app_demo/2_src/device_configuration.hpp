/* device_configuration.hpp: the set of emulated devices

   Copyright (c) 2018-2020, Joerg Hoppe; 2026, Hans Huebner
   j_hoppe@t-online.de, www.retrocmp.com

   Permission is hereby granted, free of charge, to any person obtaining a
   copy of this software and associated documentation files (the "Software"),
   to deal in the Software without restriction, including without limitation
   the rights to use, copy, modify, merge, publish, distribute, sublicense,
   and/or sell copies of the Software, and to permit persons to whom the
   Software is furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
   THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
   IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
   CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

   The complete set of emulated devices, extracted from the devices menu so
   it can outlive menu navigation: the application owns it for the process
   lifetime in web mode, the devices menu owns it otherwise and borrows it
   when it already exists.
*/
#ifndef _DEVICE_CONFIGURATION_HPP_
#define _DEVICE_CONFIGURATION_HPP_

#include <sstream>
#include <mutex>
#include <vector>
#include <string>

#include "timeout.hpp"
#include "parameter.hpp"
#include "qunibus.h"
#include "qunibusadapter.hpp"
#include "qunibusdevice.hpp"
#include "storagedrive.hpp"

#include "blinkenbone.hpp"
#include "demo_io.hpp"
#include "rf11.hpp"
#include "rl11.hpp"
#include "rk11.hpp"
#include "uda.hpp"
#include "tqk50.hpp"
#include "ts11.hpp"
#include "dl11w.hpp"
#include "kw11p.hpp"
#include "memory.hpp"
#include "dzv11.hpp"
#include "dhv11.hpp"
#include "rx11211.hpp"
#if defined(QBUS)
#include "delqa.hpp"
#include "vcb01.hpp"
#include "mrv11d.hpp"
#endif
#if defined(UNIBUS)
#include "m9312.hpp"
#include "ke11.hpp"
#include "deuna.hpp"
#include "cpu20.hpp"
#include "cpu34.hpp"
#include "cpuvax.hpp"
#endif

class device_configuration_c {
public:
	// serializes device operations (parameter set, enable/disable, bus
	// actions) between the menu thread and the web API threads
	static std::mutex operations_mutex;

	blinkenbone_c *blinkenbone;
	demo_io_c *demo_io;
	rf11_c *RF11;
#if defined(UNIBUS)
	RL11_c *RL11;
	rk11_c *RK11;
	RX11_c *RX11;
	RX211_c *RX211;
	m9312_c *m9312;
	ke11_c *KE11A;
	deuna_c *DEUNA;
	// The emulated CPU models, both only set with_emulated_CPU, else NULL.
	// Both ship disabled: the operator picks a model by enabling it, and the
	// second one is refused while the first is enabled, since the emulation
	// cores reach the bus through one installed CPU. emulated_cpu() names
	// whichever is running.
	cpu20_c *CPU20;
	cpu34_c *CPU34;
	cpuvax_c *CPUVAX;
#elif defined(QBUS)
	RLV12_c *RL11;
	rkv11_c *RK11;
	RXV11_c *RX11;
	RXV21_c *RX211;
	delqa_c *DELQA;
	vcb01_c *VCB01;
	// the bootstrap PROM card, ships disabled: a processor that carries its own
	// boot ROM answers the bootstrap window itself
	mrv11d_c *MRV11D;
#endif
	uda_c *UDA50;
	tqk50_c *TQK50;
	// TS11/TSV05 tape controller + one TS05 transport (ships disabled)
	ts11_c *TS11;
	slu_c *DL11, *DL11b;
	// Fixed pools of the serial muxes, so an operator can enable several of one
	// type at once. All instances ship disabled; each defaults to a distinct
	// I/O-page address, interrupt vector, priority slot and per-line TCP port
	// block (see build_mux_pool in device_configuration.cpp). DZV11[0] and
	// DHV11[0] keep the single-device names/addresses used before pooling.
	std::vector<dzv11_c *> DZV11;
	std::vector<dhv11_c *> DHV11;
	ltc_c *LTC;
	kw11p_c *KW11P;
	// the memory card, ships disabled: a machine with memory of its own needs
	// none, and a range claimed over that memory would collide with it
	memory_c *MEM;

	// to inject characters into the SLU receivers (console scripting,
	// web terminal)
	std::stringstream dl11_rcv_stream;
	std::stringstream dl11b_rcv_stream;
	// the VAX console, which is part of the processor and not a line on the bus
	std::stringstream cpuvax_rcv_stream;

	device_configuration_c(bool with_emulated_CPU);
	~device_configuration_c();

#if defined(UNIBUS)
	// the enabled CPU model, or NULL when the machine runs on a physical CPU
	unibuscpu_c *emulated_cpu() const;
	// the enabled PDP-11 model, for the callers that drive its sixteen bit
	// program counter; NULL when the VAX (or a physical CPU) runs the machine
	cpu_base_c *emulated_pdp11() const;
#endif
};

// non-NULL while a device set exists
extern device_configuration_c *device_configuration;

#endif // _DEVICE_CONFIGURATION_HPP_
