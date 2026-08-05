<!-- migrated from https://retrocmp.com/projects/qbone/313-qbone-introduction -->

"**QBone**" is a bridge between DEC **QBUS** and modern Linux environment. Primary applications is device emulation. It can interface to many PDP-11's, as well as to QBUS-based Micro-VAXes.

It is a "Quad"- Flip-Chip card and requires QBUS on the A/B fingers.

Onboard Linux device is a [BeagleBone Black](https://beagleboard.org/black) ("BBB").

*Proudly presenting:*

[![qbone upward](images/qbone-upward.jpg)](/images/stories/joerg/qbone/qbone-upward-big.jpg)

[![qbone facing](images/qbone-facing.jpg)](/images/stories/joerg/qbone/qbone-facing-big.jpg)

* Click images to enlarge*

"QBone" is very similar to "**UniBone**", the Linux-to-**UNIBUS**-bridge.

*Much of the [UniBone documentation](/projects/unibone) applies to QBone as well and is not duplicated here!*

## How to get one

Business stuff first! QBone is available for "do-it-yourself-builders", or as kit or ready build & tested.

A kit with all SMD parts mounted is €180, the ready build and tested is €300. The BeagleBoneBlack is extra, day to day prices are about €70.

And regarding U.S. politics: depending on current tarriff chaos (or my bad mood) shipping to the U.S. may be temporarly impossible.

Contact me via This email address is being protected from spambots. You need JavaScript enabled to view it. or This email address is being protected from spambots. You need JavaScript enabled to view it..

### Downloads

QBone software and hardware project is totally "open source", it comes under the "BSD license".\
You are allowed to forward, modify and even sell everything, as long as my Copyright remains visible. Needless to say, I exclude any warranty.

PCB design files are in a big zip at [http://files.retrocmp.com/qunibone-misc/01.01_pcb/\
](http://files.retrocmp.com/qunibone-misc/01.01_pcb/)This includes 3D prints and the two CPLD projects.

The SDCard image is a \*.gz at <http://files.retrocmp.com/qunibone-misc/sdcard/>

The software project is at <https://github.com/j-hoppe/QUniBone>

## QBone in a PDP-11

Thanks to a special mounting solution, QBone fits into one standard DEC Flip-Chip slot (utilizing all tolerances). So it can be plugged into every QBUS card cage, from old LSI11/03 to latest micro VAX. See it here in an open 11/23+ :

![qbone cage full](images/qbone-cage-full.jpg)

![qbone cage detail](images/qbone-cage-detail.jpg)

As usual, these cards sit quite sloppy in their slots. The [wikipedia article on PDP-11](https://en.wikipedia.org/wiki/PDP-11#Designed_for_mass_production) turns it positive: "The PDP-11 was designed for ease of manufacture by semiskilled labor. The dimensions of its pieces were relatively non-critical."

## What can it do: Yet another device emulator?

As older PDP-11s tend to break often, QBone is primarily meant to keep machines running by emulating one or more failed subsystems and aid in repair.

So QBone \*is\* a device emulator. There are already some projects on the web, so QBone has to be different:

- QBone is configurable, allowing to emulate arbitrary devices in parallel up to a full PDP-11 system. See the article on "[Frankenstein-PDPs](/articles/frankenstein-pdps)".
- instead of emulating just a disk or floppy drive connected to a DEC controller card, it emulates device & controllers together at the QBUS level.
- media images for simulated storage devices (disk or tape) are SimH compatible Linux files and can be exchange via FTP ... no SDcard plugging.
- simulated devices can have their own physical "lamps & switches" panels.
- simulated memory can be monitored while  the PDP-11 is running. Memory can be pre-filled or saved, so its like core.
- having a full Linux behind allows even to run SimH or complex diagnostic software on real hardware, beside all the other benefits.
- Hardware should be simple enough for a "do it yourself" kit: low-cost and low-tech.

QBUS controller cards that interface to any modern standard are rare and expensive, albeit not as rare as UNIBUs cards.  A QBUS SCSI card or ethernet card will often cost several \$100s. With QBone, in time a variety of devices could be emulated.

QBone is designed to substitute any part of a PDP-11, this includes backplanes or power supplies.

- One application is to plug QBOne in an empty backplane together with single DEC cards to test....a QBUS test bench.  You can provide own power to feed the backplane.
- You can operate QBone even if the enclosing PDP-11 is switched off: a relay separates it from main PDP-11 +5V power.
- a delayed "power-on" protects QBone, until the old DEC power supply delivers steady voltage.
- the QBUS interface has all signals and power pins routed out. You can design own bus adapters for non-DEC backplanes (examples are Eastern-Europe PDP clones).

QBone is also a platform for hardware & software development. You can can build hardware extensions and control  htem over QBUS.

- the BeagleBone is full of interfaces (GPIOs, I²C, SPI, A/D,...), its pinheaders P8 and P9 have solder points and are labeled.
- a powered I²C bus is routed out, for frontpanel replicas or own projects.
- bread board areas to mount own electronics, provded with GND, 3.3V and 5V power points.
- All programming is done in C/C++ .
- QBone can compile its own software, it is its own development tool.
- testpoints on the board can be accesses under program control, to synchronize C++ program flow with logic analyzer pictures.

When using QBone as diagnostic tool, it mates well with the signal adapter "[QProbe](http://retrocmp.com/tools/qprobe)".

Other use cases include

- monitor or trace QBUS traffic (record the bus traffic). This replaces a 42+ channel logic analyzers when tracing the PDP-11 instruction stream.
- stimulate standalone QBUS devices, for test or to dump disk/tape media.

Dynamic discussion at the [Google group](https://groups.google.com/forum/#!forum/unibone).

\*\*\*

QBone can be seen in action here, it's used to emulate memory and provides 50Hz LTC on EVENT line:

 

Some more exotic ideas include

- rebuild own DEC devices you never could acquire. For example the VC11 video controller ... for Moon Lander.
- access physical QBUS devices from within SimH.
- couple PDP-11s by shared memory emulation over internet.

##  

## Whats really there now?

All vaporware and marketing speech aside: At the moment we have

- stable hardware
- a memory emulator (4MB on QBUS22)
- RL11 controller with 4 RL01/02 disk drives and console panel
- the RXV11 controller with dual RX01 floppy drive
- the RXV21 controller with dual RX02 floppy drive
- RK11/RK05 subsystem (thanks to Josh Dersch) \*
- MSCP disk subsystem (thanks to Josh Dersch) \*
- serial DL11-W interface with KW11 clock
- a front-end processor for the PDP-11
- a QBUS hardware test adapter.
- a "demo" device for QBUS access to the board's LEDs and switches.
- a selftest tool.
- several ready-to-run configurations for XXDP,RT11,RSX11M operating systems.

(\* means: does not pass all DEC diagnostics, but verified by user experience)

All these functions are controlled with a big, clumsy, multi-menu application, named "Demo".

The RL02s pass (almost) all ZRL\* diagnostics, RT-11 does boot on a LSI11/03, PDP-11/23 and PDP-11/73.

![emulation](images/emulation.jpg)

## Programmers play ground!

Since the volume of possible device emulations and test applications is too much to develop for one person (at least: for me!), QBone is made "community friendly" and tries to be "as least surprising as possible".

- The hardware is cheap and robust, parts should be available for years.
- there's a big community behind the BeagleBone and Debian Linux.
- Soldering can be done by hand, no fine-pitch parts.
- Schematics are made with [KiCad](http://kicad-pcb.org/).
- No FPGA is used, programming is done in plain C/C++.
- Software is public at <https://github.com/j-hoppe/QUniBone>
- The gcc-compiler for the ARM processor and the "clpru" compiler for the PRUs run on the BBB itself, so *QBone is its own development platform!*
- Software is split into layers. Programmers of applications and device emulators use a set of core classes, so something like a SDK (system developers kit) exists. Your are completely hidden from the PRUs, all the highspeed stuff and details of QBUS control. There's even a small "hello world" QBUS test device implemented as example.

##  

## Why a BBB?

First question you hear today is: "Why not using a Raspberry Pi"? Comparing RPi with BBB is a bit pointless, as both have different strengths. BBB is weak at CPU power and multimedia, but is a great industrial controller. There's an incredible amount of peripherals built into it. The System Reference Manual is the largest PDF I've ever seen: 5000+ pages.

Sure the latest RPi 3B has 4 cores, is 64 bit, is faster than BBB, has more RAM and better graphics. And every generation adds more GHz, cores, and RAM. But all this is of no big value for QBone, what we need are fast real-time GPIOs.

For real-time applications the Sitara AM335X CPU on the BBB has two separate I/O processors called [PRU](https://hackaday.com/2014/06/22/an-introduction-to-the-beaglebone-pru/) ("Programmable Realtime Unit"). They execute code at 200MOps, have own GPIOs attached and are especially built for doing bit-banged protocols in software. They can replace FPGAs in medium speed-applications.

A simple benchmark for GPIO perfomance is a loop in C like this:

`while(1) {`\
`   my_gpio_pin = 0 ;`\
`   my_gpio_pin = 1 ;`\
`}`

This produces a square wave on an external pin. On UniBone, the loop time is 15ns, the BBB-PRUs reach 66MHz output here. Beat that, RPi!

What's almost important as speed is stability: user programs under Linux will get stopped shortly by the task scheduler on multitasking. Signals produced in software would reflect that: outputs jitter, sampled inputs can loose signal edges. The PRUs run independent of Linux timing and are fully deterministic. They are constructed without pipelines or cache, so one opcode executes always in 5 nanoseconds.

![bbb](images/bbb.jpg)

Regarding the design targets above, BeagleBone has advantages over an "ARM + FPGA" solution:

- It's a cheap and complete Linux platform with community support.
- In contrast to FPGA developer boards, BBBs should be available for years.
- small enough to fit into DECs Flip-Chip slots.
- the PRU processors are already there for free;
- all the highspeed signal stuff is already solved on the BeagleBone;
- PRU integration into ARM Linux is excellent (shared memory with 200MB/sec bandwidth);
- the PRUs are programmable in (almost) plain C.
- the C++ based development cycle is fast: Full recompile and download onto the board is about 30 seconds on my x64 host. Compare that with FPGA synthesis.
- the whole project can be compiled on the BeagleBone itself. Then it needs 4 minutes.

## Linux and real time

QBone has to react on QBUS signals very fast (well in sub-micro-second range). Thats not the domain of Linuxes at first, but as you see it's possible.

The Linux system is an off-the-shelf [BeagleBone Black](https://beagleboard.org/black) ("BBB"), running Debian Linux with RT patch. The BBB includes two high speed microcontrollers ("PRU"), these do all the low-level QBUS realtime-protocol logic. No FPGA is needed.

"Linux with RT patch" means: [This Linux kernel](https://rt.wiki.kernel.org/index.php/Frequently_Asked_Questions) has support for fast real-time applications. Linux started as desktop operation system, where the only interface to an "outside-world" is an user slowly clicking the mouse. If the Linux is embedded into circuitry, it has to react on events much faster and predictable. Making Linux real-time capable was a decade-long struggle.

For some reason, "PRUs" are not called "microcontrollers", but "**p**rogrammable **r**ealtime **u**nits" (wait, wasn't a series of famous minicomputers called "**P**rogrammable **D**ata **P**rocessors"?) These are two independent 200MHz 32 bit RISC processors, sharing memory and all resources with the main ARM CPU. The PRU provide an application interface (API) to Linux programs, so these can access the UNIBUS with some high-level functions. This closes the gap between the multi-megahertz realtime signal handling on the bus wires and those indeterministic Linux processes.

"Project QBone" consists not just of a hardware board, the most complex part is the software stack.

 

 
