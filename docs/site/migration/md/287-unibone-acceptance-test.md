<!-- migrated from https://retrocmp.com/projects/unibone/287-unibone-acceptance-test -->

If you received a UniBone hardware or build your own one, you should check wether it functions correctly.

## Prerequisites

For non-UNIBUS tests UniBone can be operated standalone. Power needs are +5V, to connect use pins of tantal capacitors or "+5V UB" pin header. Attention: If you use the BeagleBone +5V jack, parts of the circuit are unpowered, as the relay will not close.

Do perform the non-trivial UNIBUS/PRU tests, UniBone must be placed into a SPC slot in an empty, terminated UNIBUS backplane (yes, this is a challenge). An expansion backplane DD11-DK or -CK is best.

One terminator is enough, but it must be passive: No Boot ROM or M9302. M9302 drives SACK active LOW, if any of the BG\*IN or NPGIN is unconnected or HIGH, resulting in a loopback test error on latch 1.

The good news: Only +5V are required, no G727 continuity cards are needed, and the NPG chain may be open or closed.

 

![unibone in dd11](images/unibone-in-dd11.jpg)

##  

## Power On and Login

Wire BBB to Ethernet, then apply power to the DD11. The power relay must switch after 1 second delay. BBB produces 3.3V supply then and the green LED goes ON.

The blue LEDs on the BBB begin to flicker, as the Debian Linux is booting. After 30-60 seconds you can try to login via network.

    joerg@vmubuprog:~$ ssh root@unibone
    root@unibone's password: root

    Have fun with UniBone!

    Lost login: Fri Dec 14 15:28:24 2018

    root@unibone:~#

If you can not login, try repowering UniBone. There's a known bug in the BBB's Ethernet interface, which prevents network communication sometimes ... never fixed and very annoying.

## EEPROM, board pin config and I²C

There's a small EEPROM, accessed over I²C bus. As the UniBone PCB is a "cape" for the BBB, it needs the EEPROM to read out the board name. Then the Linux kernel should activate the correct configuration file to setup port pins of the Sitara CPU correctly. A kernel component named "cape manager" should do this ... it may or may not work in the Debian kernel in use. Doesn't matter.  Sigh.

Read the EEPROM:

\# hexdump -C /sys/bus/i2c/devices/i2c-2/2-0054/eeprom

Program the EEPROM:

\
\# cat cape.eeprom \>/sys/bus/i2c/devices/i2c-2/2-0054/eeprom

Check the EEPROM content:

\# hexdump -C /sys/bus/i2c/devices/i2c-2/2-0054/eeprom

Reboot, login again, test wether the correct board configuration is loaded:

    ...
    root@unibone2:~# cat /sys/devices/platform/bone_capemgr/slots
    0: P----- -1 UniBone,00B0,Hoppe,UniBone
    1: PF---- -1
    2: PF---- -1
    3: PF---- -1
    4: P-O-L- 0 Override Board Name,00B0,Override Manuf,UniBone

With that test also the I²C interface for add-on panels is tested.

 

## RS232 ports

UniBone exposes two serial RS232 ports. They are configured for regular Linux login session. To connect to the BBB via RS232, you need to crimp a simple cable:

![serial cable](images/serial-cable.jpg)

The pinout is NOT the most used PC standard. See [here](http://www.frontx.com/pro/cpx102_2.html) for a product and docs, only GND, TXD and RXD are connected.

Connect the male DSUB-9 connector to your PC with a null-modem cable and start a terminal emulator with 9600 baud, no hardware hand-shaking.

You should see another Debian shell prompt both on UART1 connector.\
UART2 is currently reserved for emulation of the DL11 serial card and can only be stimulated with "cat something \>/dev/TTYS2".

Logging in via RS232 may be useful if the Ethernet-bug occurs too often on your BBB.

##  

## Testing standard GPIOs

![leds switches](images/leds-switches.jpg)

The important part of UniBone is the PRU controlled interface to the UNIBUS. But the user LEDs and switches are traditional GPIOs.

Test with the "demo" application, main menu point "tg" for "GPIO", then "lb" for "manual loopback".

In loopback mode, the 4 switches control the four LEDs: toggle them and the LEDs should respond. Pressing the button will enable the DS8641 driver array, resulting in active 2nd green LED.

##  

## PRUs and UNIBUS interface

The real important test is access to the UNIBUS. Signals are output through the LS377 register array and 8641 drivers onto 56 UNIBUS data lines, then read back through drivers, LVTH541 input latches and PRU pins. This tests also terminators and parts of the UNIBUS backplane.

Preparation:

- Put UniBone into an empty terminated UNIBUS backplane.
- set loopback jumpers onto the 5 BGIN/OUT and NPGIN/OUT lines (yellow in the image below)

 

![grant jumper loopback](images/grant-jumper-loopback.jpg)

 

Use again the "demo" application, main menu "tl" for "test bus latches", then "\>\>\> \* r" to test all UNIBUS signals endlessly with random patterns.

Run this test as long as possible, and terminate with ^C. The most likely reason for errors are solder failures on the SMD input/output latches. You may have open solder joints, or shortcuts between pins. To help in debugging, the test software prints the signal path for failed bits.

The first thing to check on an loopback-error is the physical voltage level on the UNIBUS.

![unibus connector detail](images/unibus-connector-detail.jpg)

If a voltage level is as expected, the output branch is OK and the input branch over 74lvth541 registers is defective. Else it's the output over these 74ls377's. Remember: on UNIBUS, a logical "1" is represented as 0 volts, a logic "0" is about 3.4 volts (except the BG and NPG signals).

 

To test the test, you can remove one of the yellow loopback jumpers while the test is running. It should stop with an error.

Its a good idea to bend and knock the board a bit while under test: this may expose bad solder joints.

Pro tip: As the UNIBUS lines need pullup by terminators, this test can be used to check a big part of your backplane: plug in only a single terminator at either end, plug UniBone to different SPC slots of the backplane and run this test. It will fail, if any of the 56 signals has no connection to the remote terminator. You even can test UNIBUS cables between separate backplanes this way.

And don't forget to remove the loopback jumpers on BG\* and NPG after test!

 

## Standalone memory test

If the loopback test succeeds, you can go more realistic: add a UNIBUS memory card and let UniBone exercise it. This tests not just static connections, but also implementation and timing of UNIBUS DMA cycles. On the other hand, only addresses, data and MSYN/SSYN signals are tested, and functions of the memory card! Still no G727 grants are needed.

Attention: a memory card needs more amps, and probably more voltages than just +5V.

![unibone ms11 in dd11](images/unibone-ms11-in-dd11.jpg)

 

Then execute [memory operations as described](/projects/unibone/281-unibone-as-memory-emulator-user-view). As there's no PDP-11 CPU active, work in "Arbitration inactive" mode.

 

## Finally: run it in a PDP-11

If all went well, plug UniBone into a PDP-11.or boot operationg systems from [emulated RL02](/projects/unibone/275-unibone-as-disk-emulator).

 
