<!-- migrated from https://retrocmp.com/projects/unibone/282-unibone-as-memory-emulator-internals -->

## Technobabble ahead

Now lets get technical.

While memory emulation seems simple from user's side, UniBone must implement the complete UNIBUS interface, including priority arbitration and DMA logic. However it is still the "easy" use case, as no device logic must be implemented. The whole memory logic is handled by the PRU controlled bus interface.

Oh by the way: see [here](/how-tos/interpreting-unibus-signals) for the UNIBUS protocol!

## PRU activity

Normally, the PRU is in "UNIBUS slave" mode and listens passively to traffic on the UNIBUS wires. Especially, he fetches address lines at the falling edge of MSYN and decides whether to ignore this UNIBUS cycle or whether some of the emulated devices are addressed by the PDP-11 CPU.

But the PRU does more: ARM/Linux code can request an active DMA burst, then the PRU has to execute the UNIBUs arbitration protocol, becomes "UNIBUS master" and generates actively bus cycles. These are answered by other UNIBUS peripherals then ... or generate a "bus timeout".

As DEC designed the UNIBUS protocol for cheap slaves and complex masters, the "bus master" logic in UniBone was the hard part.

Memory emulation in the PRU is supported by a page table: for every chunk of 4KB a flag indicates wether UNIBUS addresses should be ignored, or wether the PRU should answer read/write cycles (DATI/DATO in UNIBUS talk) with access to emulated memory.

## Emulated memory is shared memory

Emulated memory is just a piece of Linux main memory: /dev/mem. It is locked against use by other processes and paging, the "PRU UIO" helper driver reserves shared ARM DDR memory this way. Since UNIBUS contains only 18 address bits, 256kB of memory are reserved ... not much compared to the 512MB of a BBB (which in fact is "not much" against modern multi-GB PCs). Big PDP-11s with 22 bit address space have local memory busses. UniBone is bound to the SPC slot and can not emulate the full 4MB for these machines.

## Multi-node data travel

The UniBone software handling the UNIBUS protocol deals with different situations:

[TABLE]

 

In **situtation 1 and 2**, the CPU accesses memory with DATI/DATO cycles, not caring wether it is physical or emulated. PRU must watch the bus and decode addresses in realtime.

In **situation 3 and 4**, a device controller (perhaps disk or tape) write data in to physical or emulated memory. The controller has to perform the NPR/NPG/SACK protocoll to become bus master for the transaction. For the PRU there's no difference to a CPU access.

In **situation 5** an UniBone applications transfers data between some buffers and physical memory. The application may be an emulated storage device controller, or perhaps a debugging tool. The PRU has to perform the NPG/NPR/SACK protocoll to become bus master. Then he produces DATI or DATO cycles as bus master.

**Situation 6** is like 5, but target for data is emulated memory. The PRU is is "master" and "slave" now at the same time.\
It may seem tempting to not produce UNIBUS cycles in this case, as a super-fast Linux-internal access like in **situation 7** could be used.

But its important to make every memory access visible on the UNIBUS because:

- if the PDP-11 has cache installed, the cache logic is "snooping" on the bus and must see all memory transaction to update cached data.
- when debugging UNIBUS traffic, every memory cycle must be visible on the wires.

\<link: ubu adapter LA\>

In **situation 8**, a direct Linux-internal memory access is used for memory monitoring. While the PDP-11 is running, the emulated memory can be read or written to over a 2nd port.\
So emulated memory can be accessed by Linux/ARM code while the PRU exposes it to the PDP-11. This allows some new aplications:

- auto save of memory ... to emulated true "core".
- super fast memory load, instead of slow dump of data over serial console protocolls (like PDP11GUI or pdp11monloader do)
- shared memory between several PDP-11s: memory content for some pages is transfered between UniBones in different machines
- EXAM/DEPOSIT like we are used to, but even for running PDP-11s.

An finally **setup 9**: if UniBone is plugged into an UNIBUS segment without PDP-11 CPU, it is always bus master and must not execute the NPG/NPR/SACK protocoll ... nobody would respond.

![unibone ms11 in dd11](images/unibone-ms11-in-dd11.jpg)

 

So even plain memory is not so boring at all, isn't it?

\
 

## A parallel torture test

For testing the PRU the bus cycle logic, the memory and the DMA logic, I used one cruel test:

**In shor**t: a PDP-11/34 is equipped with a mix of physical and emulated memory. The PDP-11 executes the DEC diagnostic ZKMA to tests one part of memory, in parallel a UniBone application tests another memory range in parallel with DMA cycles.

**More precise:**

A PDP11/34 is populated with a 32KB MS11 board and an UniBone. The MS11 implements addresses 000000-077777, the UniBone emulateds all other 216kB from 100000 to 757776.

Now the DEC memory diagnostic ZKMA is used. It can run standalone and comes as paper tape image.

1\. We have two terminal sessions open: one to the UniBone Linux with "demo" text-menu application, the other one to the PDP-11/34s serial console, showing the M9312 console emulator prompt.

2\. UniBone reads the paper tape image and writes the code words per DMA into lower physical memory (situation 5).

![screenshot demo memtest](images/screenshot-demo-memtest.jpg)

3\. We tell UniBone to exercise memory from 20000 to 757776. It does so by producing DMA cycles, interrupting the PDP-11/34 heavily (situtation 6). This test must not touch the lower 56KB of memory, as the PDP-11 executes ZKMA there. The whole memory is written with a random pattern ("W"), then read back and checked ("R") ... forever. No errors so far!

4\. While UniBone is working, the PDP-11/34 CPU executes the M9312 console emulator all the time. Now that ZKMA was written into memory, we can start it at address 200. The PDP-11 executes the memory test ZKMA in lower 56KB of RAM, the addresses are 0 - 157776 (a mix of physical and emulated, situation 1 and situtation 2 in parallel with situation 6).

![screenshot zkma passes](images/screenshot-zkma-passes.jpg)

* When I first saw this picture, it was better than sex!*

 

The interleaved UNIBUS accesses of PDP-11 and UniBone can be seen with an logic analyzer:

![logicanalyzer zkma dma](images/logicanalyzer-zkma-dma.jpg)

UniBone requests bus mastership by lowering the NPR signal. Then access is granted, indicated by SACK going Low.\
When SACK is High, the 11/34 CPU uses the bus, if SACK is Low we see the UniBone cycles.

The image above shows 600µs of bus traffic. At higher resolution of 10 µs we see individual DATI and DATO cycles, as well as the priority arbitration signals in detail:

![logicanalyzer pdp11 unibone dma](images/logicanalyzer-pdp11-unibone-dma.jpg)

Again UniBone sets NPR Low - bus access is granted with SACK Low. So left of "cursor line A" the 11/34 CPU is accessing memory, then UniBone takes over.

UniBone produces visually different signals: MSYN/SSYN pulses are shorter but the overall cycle is a bit slower. Who cares until the UNIBUS specs are met?

 
