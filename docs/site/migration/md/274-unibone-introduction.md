<!-- migrated from https://retrocmp.com/projects/unibone/274-unibone-introduction -->

"**UniBone**" is a bridge between DEC [UNIBUS](https://en.wikipedia.org/wiki/Unibus) and modern Linux environment. Primary applications is device emulation. It can interface to many PDP-11's, as well as to PDP-10 and VAX expansions.

It needs a standard quad "SPC" ("small peripheral connector") slot.

![boardnice feets](images/boardnice-feets.jpg)

UniBone is very similar to its QBUS successor "[QBone](/projects/qbone)".

## Yet another device emulator?

There are already several device-emulator projects in the web, so UniBone has to be different:

- instead of emulating just a disk or floppy drive connected to a DEC controller card, it emulates device & controllers together at the UNIBUS level.
- UniBone is configurable, allowing to emulate arbitrary devices in parallel up to a full PDP-11 system. See the article on "[Frankenstein-PDPs](/articles/frankenstein-pdps)".
- having a full Linux behind allows even to run SimH or complex diagnostic software on real hardware, beside all the other benefits.
- All programming is done in C/C++ .
- Hardware should be simple enough for a "do it yourself" kit: low-cost and low-tech.

 

Unibus controller cards that interface to any modern standard are very rare and expensive.  A Unibus SCSI card or ethernet card will often cost \$1000 if you can find one. With UniBone, in time a variety of devices could be emulated.

UniBone can be plugged into every PDP-11 with quad SPC slot, see here a PDP-11/05:

![pdp1105 top Bearbeitet](images/pdp1105-top-Bearbeitet.jpg)

 

Even DECs tight limits on vertical board height are met by the BeagleBone ... with some squeezing and a special mounting solution.

![board fit](images/board-fit.jpg)

 

## What can it do?

As older PDP-11s tend to break often, UniBone is primarily meant to keep machines running by emulating one or more failed subsystems and aid in repair.

UniBone is designed to fulfill many roles:

- simulate a memory card for a running PDP-11. Memory can be pre-filled or saved, so its like core.
- simulate storage devices (disk or tape). Media images are SimH compatible Linux files then.
- simulate devices that control own physical hardware, as the BBB is full of interfaces (GPIOs, I²C, A/D,...) and patch fields can hold own circuitry. 
- simulated devices can have their own physical "lamps & switches" panels.
- monitor or trace UNIBUS traffic (record the bus traffic). This replaces a 56+ channel logic analyzers when tracing the PDP-11 instruction stream.
- stimulate UNIBUS devices, for test or to dump disk/tape media.

When using UniBone as diagnostic tool, it mates well with the signal adapter "[UniProbe](/tools/uniprobe)".

 

Some more exotic ideas include

- rebuild own DEC devices you never could acquire. For example the VT11 video controller ... for Moon Lander.
- access physical UNIBUS devices from within SimH.
- couple PDP-11s by shared memory emulation over internet.

##  

## Whats really there now?

All vaporware and marketing speech aside: At the moment we have

- stable hardware
- a memory emulator
- RL11 controller with 4 RL01/02 disk drives and console panel
- the RX11 controller with dual RX01 floppy drive
- the RX211 controller with dual RX02 floppy drive
- RK11/RK05 subsystem (thanks to Josh Dersch)\*
- MSCP disk subsystem (thanks to Josh Dersch)\*
- RS11/RF11 DECdisk emulation (thanks to Josh Dersch)
- KE11 EAE Arithmetik (thanks to Josh Dersch)
- serial DL11-W interface with KW11 clock
- a PDP11/20 CPU emulation (thanks to Angelo Papenhof) \*
- emulation of M9312 ROM bootstrap
- real time encode/decode of DEC file systems to/from Linux files ("host file sharing")
- a front-end processor for the PDP-11
- a UNIBUS hardware test adapter.
- a "demo" device for UNIBUS access to the board's LEDs and switches.
- a selftest tool.
- several ready-to-run configurations for XXDP,RT11,RSX11M operating systems.

(\* means: does not pass all DEC diagnostics, but verified by user experience)

 

 

All these functions are controlled with a big, clumsy, multi-menu application, named "Demo".

The RL02s pass (almost) all ZRL\* diagnostics, RT-11 does boot on a PDP-11/34 and PDP-11/05.

![emulation](images/emulation.jpg)

 

## Programmers play ground!

Since the volume of possible device emulations and test applications is possibly too much for one person (at least: for me!), UniBone is made "community friendly" and tries to be "as least surprising as possible".

- The hardware is cheap and robust, parts should be available for years.
- there's a big community behind the BeagleBone and Debian Linux.
- Soldering can be done by hand, no fine-pitch parts.
- Schematics are made with [KiCad](http://kicad-pcb.org/).
- No FPGA is used, programming is done in plain C/C++.
- Software is public at <https://github.com/j-hoppe/QUniBone>
- The gcc-compiler for the ARM processor and the "clpru" compiler for the PRUs run on the BBB itself, so UniBone runs its own development tools.
- Software is split into layers. Programmers of applications and device emulators use a set of core classes, so something like a SDK (system developers kit) exists. Your are completely hidden from the PRUs, all the highspeed stuff and details of UNIBUS control. There's even a small "hello world" UNIBUS test device implemented as example.

 

## Why a BBB?

First question you hear today is: "Why not using a Raspberry Pi"? Comparing RPi with BBB is a bit pointless, as both have different strengths. BBB is weak at CPU power and multimedia, but is a great industrial controller. There's an incredible amount of peripherals built into it. The System Reference Manual is the largest PDF I've ever seen: 5000+ pages.

Sure the latest RPi 3B has 4 cores, is 64 bit, is faster than BBB, has more RAM and better graphics. But all this is of no big value for UniBone, what we need are fast real-time GPIOs.

For real-time applications the Sitara AM335X CPU on the BBB has two separate I/O processors called [PRU](https://hackaday.com/2014/06/22/an-introduction-to-the-beaglebone-pru/) ("Programmable Realtime Unit"). They execute code at 200MOps, have own GPIOs attached and are especially built for doing bit-banged protocols in software. They can replace FPGAs in medium speed-applications.

A simple benchmark for GPIO perfomance is a loop in C like this:

`while(1) {`\
`   my_gpio_pin = 0 ;`\
`   my_gpio_pin = 1 ;`\
`}`

This produces a square wave on an external pin. On UniBone, the loop time is 15ns, the BBB-PRUs reach 66MHz output here. Beat that, RPi!

What's almost important as speed is stability: user programs under Linux will get stopped shortly by the task scheduler on multitasking. Signals produced in software would reflect that: outputs jitter, sampled inputs can loose signal edges. The PRUs run independent of Linux timing and are fully deterministic. They are constructed without pipelines or cache, so one opcode executes always in 5 nanoseconds.

![bbb](images/bbb.jpg)

Regarding the design targets above, BeagleBone has many advantages over an "ARM + FPGA" solution:

- It's a cheap and complete Linux platform with community support.
- small enough to fit into DECs Flip-Chip slots.
- the PRU processors are already there for free;
- all the highspeed signal stuff is already solved on the BeagleBone;
- no 100+ pins CPLD/FPGAs to solder, no fine-pitch ICs! The PCB itself can be distributed as "build-yourself" kit.
- PRU integration into ARM Linux is excellent (shared memory with 200MB/sec bandwidth);
- the PRUs are programmable in (almost) plain C.
- the C++ based development cycle is fast: Full recompile and download onto the board is about 30 seconds on my x64 host. Compare that with FPGA synthesis.
- the whole project can be compiled on the BeagleBone itself. Then it needs 4 minutes.

##  

## Linux and real time

QBone has to react on UNIBUS signals very fast (well in sub-micro-second range). Thats not the domain of Linuxes at first, but as you see it's possible.

The Linux system is an off-the-shelf [BeagleBone Black](https://beagleboard.org/black) ("BBB"), running Debian Linux with RT patch. The BBB includes two high speed microcontrollers ("PRU"), these do all the low-level UNIBUS realtime-protocol logic. No FPGA is needed.

"Linux with RT patch" means: [This Linux kernel](https://rt.wiki.kernel.org/index.php/Frequently_Asked_Questions) has support for fast real-time applications. Linux started as desktop operation system, where the only interface to an "outside-world" is an user slowly clicking the mouse. If the Linux is embedded into circuitry, it has to react on events much faster and predictable. Making Linux real-time capable was a decade-long struggle.

For some reason, "PRUs" are not called "microcontrollers", but "**p**rogrammable **r**ealtime **u**nits" (wait, wasn't a series of famous minicomputers called "**P**rogrammable **D**ata **P**rocessors"?)These are two independent 200MHz 32 bit RISC processors, sharing memory and all resources with the main ARM CPU. The PRU provide an application interface (API) to Linux programs, so these can access the UNIBUS with some high-level functions. This closes the gap between the multi-megahertz realtime signal handling on the bus wires and those indeterministic Linux processes.

"UniBone" could easily be folllowed by a similar "QBone" for QBUS systems: QBUS is just "UNIBUS with fewer wires".

"Project UniBone" consists not just of a hardware board, the most complex part is the software stack.

 
