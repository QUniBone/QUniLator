<!-- migrated from https://retrocmp.com/projects/unibone/278-unibone-as-device-test-console -->

## Don't waste machine lifetime for repair!

UniBone can be used to stimulate isolated boards for repair. This ability is quite important, as older PDP-11s tend to break away. More and more, every passing year. Sigh.

When you try to repair defective hardware, its natural to use a PDP-11 as test bed. Some of us have worked on cards raised out of the card cage on extenders:

![pdp1134 extended boards](images/pdp1134-extended-boards.jpg)

 *Don't try that at home!*

However, repair may take a long time, and eats up valuable life time of the PDP-11. The result: while repairing one board, the debugging PDP-11 develops new errors. Yes, this happens ... its almost normal nowadays.\
Effect on morale is disastrous everytime!

As PDP-11 continue to age, you can predict that in a few years every repaired error will generate more than one failures in the test system ... this is of course the end of all attemps to make your '11s running again.

So having an independent test bed for defective cards seems to be a good idea. In theory, you can test all devices controllers, their devices, and ROM and RAM cards without stressing a physical PDP-11 CPU. 

## Standalone and embedded operation

UniBone can be used in two different ways:

- as **standalone**-exerciser, where no PDP-11 CPU is active and UniBone is always bus master.
- **embedded** into a more or less complete PDP-11 with running CPU.

"Embedded" operation is currently named as "Arbitration ACTIVE" in the "Demo" software.

![pdp1134 ms11](images/pdp1134-ms11.jpg)

*UniBone embedded into a PDP-11*

For "Standalone" operation you need an empty UNIBUS backplane segment in a DEC system case. However, you have best access to the device under test on an extracted UNIBUS backplane on your desk. I use a DD11-CK. +5V and +12V power are delivered by a PC power supply. And you need bus terminators.

![unibone testbed](images/unibone-testbed.jpg)

*UniBone testing a serial DL11-W card stand-alone.*

## UniBone as stand-alone PDP-11 emulator.

 UniBone is able (since October 2019) to emulate memory, RL drives, a serial DL11 card as well as a 11/20 CPU.

This is the minimal configuration to run XXDP diagnostics without any physical hardware. So it can test devices like an original PDP-11.

## Console-like functions

Because UniBone has so much control over the PDP-11 memory, it is almost a full console processor for a PDP-11.

To get appetite, see the menu in the current "Demo" software:\
 

![screenshot demo memory menu clean](images/screenshot-demo-memory-menu-clean.jpg)

In detail it can

\>\>\> exam or deposit memory locations or device registers, also in blocks or with address auto-increment

\>\>\> save memory content into a disk file

\>\>\> test memory

\>\>\> load memory content, with the data coming from different sources:

- binary data
- MACRO-11 listing file, to load freshly assmebled programs
- DECs absolute standard paper tape format, to load diagnostics and other paper tape software
- file with a list of simh-like exam/deposit commands

\>\>\> pulse the INIT signal line on UNIBUS

\>\>\> Reboot the PDP-11 CPU by simulatinga power cycle with the ACLO and DCLO lines.

In fact it has the same functions as the standalone "[PDP11/34 programmers console](/tools/pdp-1134-programmers-console)" .

But there's a limit: CPU functions like Start, Halt and Single Step are not possible. There are no signals to control the CPU on the standard SPC slot. However, there are only 56 UNIBUS wires of the 64 bit DS8641 array in use, so additional control lines could be attached to UniBone. This would be a CPU-specific hack.

 

\* \* \*

 

To exploit the full potential of UniBone as debugging tool more functions could be programmed. Remember, programming UniBone applications is done in plain Linux C/C++, so there's a straight way to fulfill these fantasies:

## Fantasy 1: SimH like interface for PDP11GUI

To have a more comfortable interface, UniBone's user interface should mimic SimHs console.

Then operation would be familiar, and it could be controlled by [PDP11GUI](/tools/pdp11gui) over telnet, resulting in rich graphical user interface.

## Fantasy 2: a disk or tape drive exerciser/dumper

DEC made stand-alone exerciser for their disk and tape drives. See here a tester for RK06/07 disk drives (picture from [pdp-11.nl](http://www.pdp-11.nl/))

![rk06 07tester 53 pdp11.nl](images/rk06-07tester-53-pdp11.nl.jpg)

UniBone can emulate drive exercisers too, in the "standalone" mode. However it would need the original DEC controller to attach to the drives. And it needs special software, which knows about the controller and drives to be tested.

The operation would be as follows (here shown for a disk subsystem, maybe RK11/RK05):

1.  the exerciser application issues commands to the controller registers, to setup DMA, cylinder and track select. Finally a "read track" comamnd would be given.
2.  the controller talks to the device, retriving track data into a buffer.
3.  the controller performs the priority arbitration protcol to become bus master ...
4.  ... then he transfers buffered track DATA with DMA into UniBone's emulated memory.
5.  the exerciser application reads memory data ...
6.  ... and assembles them in a Linux file to generate a SimH-compatible disk image.

![device exerciser](images/device-exerciser.jpg)

This would be the perfect device to read images from old disk or tape drives in high speed!

 

 

 
