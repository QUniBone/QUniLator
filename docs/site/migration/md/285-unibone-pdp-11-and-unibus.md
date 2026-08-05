<!-- migrated from https://retrocmp.com/projects/unibone/285-unibone-pdp-11-and-unibus -->

 As UniBone is so specialized to older PDP-11s with UNIBUS, some general information about these machines may be helpful.

## PDP-11

DECs PDP-11s are 16 bit mini computers and were build from 1969 to 1995. Over 20 different models were introduced, all software compatible. Their architecture is beautiful and influenced lots of other designs. CPU speed ranged from a few 100 kHz to 20 MHz, memory from 8 kBytes to 4 MBytes. All kinds of peripherals were connected to PDP-11s, including dozends of different tape and disk drives.

Wikipedia lists 35 different operating systems!

Today some PDP-11s are still doing serious work, mostly controlling some special equipement. Far more PDP-11s are kept alive by retro-enthusiasts (like the one writing these pages).  The PDP-11 is a great hobbyist machine, as they are robust and mostly made of standard parts. And all documentation and software is available online now, including board schematics, user manuals, test programs and operating systems.

## Physical Packaging

In he 1970s a DEC PDP-11 computer were packaged in modular and hierarchical manner.

On top level, a larger machine consists of a row of 19" **racks**.

![unibus racks](images/unibus-racks.jpg)

A rack is filled with a stack of **boxes**, which could be pulled out on drawers.

A single box, contained a power supply and one or more **backplanes** - arrays of contact slots wire-wrapped together.

![unibus cardcage](images/unibus-cardcage.jpg)

The backplane were populated with **cards**. Slots and connectors all used DECs "FlipChip" standard.

![unibus backplane cards](images/unibus-backplane-cards.jpg)

 

A single UNIBUS connected all controllers in the different boxes in a daisy-chain manner. White flat cables were used. To connect all boxes in a rack, an UNIBUS could span several meters.

![unibus cable](images/unibus-cable.jpg)

These many layers of packing elements were quite expensive. PDP-11 competitor Data General could offer cheaper computers just by using larger boards, reducing the need for boxes and power supplies.

## UNIBUS

One invention which made up the success of DECs PDP-11 line of computers was the UNIBUS.

This was an UNIversal system BUS, connection all components in a PDP_11 in an uniform way. Main memory, mass storage device controllers, communication adapters, the system clock, memory management unit and CPU registers were all mapped into the same address space, so the same CPU instructions could access memory and IO devices. There were no special IO instructions in a PDP-11.

Around 1975 DEC intrduced a variant of UNIBUS: QBUS. It used fewer wires and was better suited for very small PDP-11s.

## Signals

Electricall\< UNIBUS consisted of 56 signal wires, most used for 18 address bits and 16 data bits. The bus protocol was asynchronous with a hand shake between bus master and bus slave, so fast and slow devices could use the same system bus without loosing speed. Propagation delay on very long bus cables were also no problem.

A typical bus transaction needed one microsecond to complete and looked like this on an oscilloscope:

![unibus write cycle](images/unibus-write-cycle.jpg)

Here a DATO (master writes into slave) is shown. The protocol wires are MSYN and SYN:

1.  with MSYN (cursor A) the bus master told the slaves an address anda data for a new access cycle were valid.
2.  all slave latches the address, the selected also latches the data and processes them. A memeory card would save the data word in its RAM chips now.  When complete, the slaves activates SSYN.
3.  the master processes slave data, releases the bus and deactivates MSYN (cursor B).
4.  the slave sees the master accepted the cycle, is also releasing the bus and deactivates SSYN.
