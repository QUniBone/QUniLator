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
#include "dl11w.hpp"
#include "dzv11.hpp"
#include "dhv11.hpp"
#include "rx11211.hpp"
#if defined(QBUS)
#include "delqa.hpp"
#include "vcb01.hpp"
#endif
#if defined(UNIBUS)
#include "m9312.hpp"
#include "ke11.hpp"
#include "deuna.hpp"
#include "cpu.hpp"
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
	cpu_c *cpu; // only set with_emulated_CPU, else NULL
#elif defined(QBUS)
	RLV12_c *RL11;
	rkv11_c *RK11;
	RXV11_c *RX11;
	RXV21_c *RX211;
	delqa_c *DELQA;
	vcb01_c *VCB01;
#endif
	uda_c *UDA50;
	slu_c *DL11, *DL11b;
	dzv11_c *DZV11;
	dhv11_c *DHV11;
	ltc_c *LTC;

	// to inject characters into the SLU receivers (console scripting,
	// web terminal)
	std::stringstream dl11_rcv_stream;
	std::stringstream dl11b_rcv_stream;

	device_configuration_c(bool with_emulated_CPU);
	~device_configuration_c();
};

// non-NULL while a device set exists
extern device_configuration_c *device_configuration;

#endif // _DEVICE_CONFIGURATION_HPP_
