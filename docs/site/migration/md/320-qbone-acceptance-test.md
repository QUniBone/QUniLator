<!-- migrated from https://retrocmp.com/projects/qbone/320-qbone-acceptance-test -->

If you received a QBone hardware or build your own one, you should check wether it functions correctly.

## Prerequisites

Even if you just like to play with the BBB alone, it must be plugged into QBone to boot from SDcard. Else it will boot *another Debian installation* from the internal eMMC chip.

For non-QBUS tests QBone can be operated standalone. Power needs are +5V, to connect use pins of the four power capacitors or "+5V UB" pin header. Attention: If you use the BeagleBone +5V jack, parts of the circuit are unpowered, as the relay will not close.

Do perform the non-trivial QBUS/PRU tests, QBone must be placed into in an empty, terminated QBUS backplane (yes, this is a challenge). All types of QBUS "quad" backplanes work.

On some backplanes one QBUS terminator is needed, but it must be passive: No Boot ROM.

The good news: Only +5V are required, no G9047 grant continuity cards are needed.

![qbone acceptance](images/qbone-acceptance.jpg)

 

## Power-ON

Plug BBB into QBone PCB, connect +5V to the auxillary QBus power plugs on the QBone PCB.\
The power relay must switch after 1 second delay. BBB produces 3.3V supply then and the green LED goes ON.

## Program both CPLDs

QBone PCBs come with already programmed CPLDs, so no need to do anything here.

If you build QBone on your own, you need to download CPLD code with Lattice Diamond software and a programming adapter, which is not covered here.

## Test boot and network access

Wire BBB via Ethernet to your home network. BBB will talk to your DHCP server. WLAN operation is possible, but not covered here.\
Use an ssh client like "PuTTY" to login.

After Power-ON the blue LEDs on the BBB begin to flicker, as the Debian Linux is booting. After 30-90 seconds you can try to login via network.

The default hostname is "qbone", but may change.

    joerg@vmubuprog:~$ ssh root@qbone 

    Password: root

    Have fun with QBone!

    Last login: Fri Dec 14 15:28:24 2020

If BBB running, but you get no ssh access to qbone:

- BBB must be plugged into PCB to boot from external SDcard. Else it may boot another OS from its internal eMMC chip.
- "yellow" and "green" network LED active?
- hostname entry on SDcard? (`/etc/hostname`)
- use RS232 serial login over UART as backup

Also try repowering QBone. There's a known bug in the BBB's Ethernet interface, which seldom prevents network communication sometimes ... never fixed.

 

## RS232 ports

QBone exposes two serial RS232 ports. They are configured for regular Linux login sessions or PDP-11 serial port emulation.\
Typical UART1 is always for Linux, UART2 maybe configured for the emulation.

To connect to the BBB via RS232, you need to crimp a simple cable:

![serial cable](images/serial-cable.jpg)

***This pinout is NOT the one you find on PC-mainboard adapters!***

See [here](http://www.frontx.com/pro/cpx102_2.html) for a product and docs, only GND, TXD and RXD are connected.

Connect the male DSUB-9 connector to your PC with a null-modem cable and start a terminal emulator with 9600 baud, no hardware hand-shaking.

You should see another Debian shell prompt both on UART1 connector.\
UART2 is currently reserved for emulation of the DL11 serial card and can only be stimulated with "cat something \>/dev/TTYS2".

Logging in via RS232 may be useful if the Ethernet-bug occurs too often on your BBB.

##  

## BeagleBone Debian Linux operation

Here we put a heavy load onto BBB's network, CPU, main memory, and SDcard. This verifies BBB has a full featured Debian installation.

Login via ssh, then update program code and emulation images:

    ./update-codes.sh
    ./update-files.sh

Also play with the "mc" Midnight commander.

Test remote file access to BBB via sftp. Best with a graphical client (example: winscp under Win10, filezilla).

The manual system is working: "man man".

## EEPROM, board pin config and I²C

Optionally, there's a small EEPROM, accessed over I²C bus. As the QBone PCB is a "cape" for the BBB, it needs the EEPROM to read out the board name. Then the Linux kernel should activate the correct configuration file to setup port pins of the Sitara CPU correctly. A kernel component named "cape manager" should do this ... it may or may not work in the Debian kernel in use. Doesn't matter.  Sigh.

Read the EEPROM:

    # hexdump -C /sys/bus/i2c/devices/i2c-2/2-0054/eeprom

Program the EEPROM:

    # cat cape.eeprom >/sys/bus/i2c/devices/i2c-2/2-0054/eeprom

Check the EEPROM content:

    # hexdump -C /sys/bus/i2c/devices/i2c-2/2-0054/eeprom

Reboot, login again, test wether the correct board configuration is loaded:

    ...
    root@unibone2:~# cat /sys/devices/platform/bone_capemgr/slots
    0: P----- -1 UniBone,00B0,Hoppe,UniBone
    1: PF---- -1
    2: PF---- -1
    3: PF---- -1
    4: P-O-L- 0 Override Board Name,00B0,Override Manuf,UniBone

With that test also the I²C interface for add-on panels is tested.

 

## Testing standard GPIOs

![leds switches](images/leds-switches.jpg)

The important part of QBone is the PRU controlled interface to the QBUS. But the user LEDs and switches are traditional GPIOs.

Test with the "demo" application, main menu point "TG" for "GPIO", then "LB" for "manual loopback".

In loopback mode, the 4 switches control the four LEDs: toggle them and the LEDs should respond. Pressing the button will enable the DS8641 driver array, resulting in active 2nd green LED.

##  

## PRUs and QBUS interface

The real important test is access to the QBUS. Signals are output through the CPLDs and 8641 drivers onto QBUS data lines, then read back through drivers, CPLDs and PRU pins. This tests also terminators and parts of the QBUS backplane.

![qbone acceptance qbus](images/qbone-acceptance-qbus.jpg)

Preparation:

- Put QBone into an empty QBUS backplane.

- Some backplanes have terminators already onboard, else:\
  Add a terminator to QBUS lines ([QProbe](/tools/qprobe) or M9400 REV11 with resistors only, no logic!)

- make sure the 50Hz LTC source is disabled (blue jumper in image below)

- set two loopback jumpers onto the IAKI-IAKO and DMGI-DMGO lines (yellow in the image below). Remove these after test!

![qbone acceptance grant loopback](images/qbone-acceptance-grant-loopback.jpg)

Use again the "demo" application, main menu "TL" for "test bus latches", then "\>\>\> \* r" to test all QBUS signals endlessly with random patterns.

Run this test as long as possible, and terminate with ^C. My most likely reason for errors are QBUS setup errors, then mis-inserted 8641 drivers (not kidding). You also may have open solder joints, or shortcuts between pins. To help in debugging, the test software prints the signal path for failed bits.

To "test the test", you can remove one of the yellow loopback jumpers while the test is running. It should stop with an error.

Its a good idea to bend and knock the board a bit while under test: this may expose bad solder joints.

Pro tip: As the QBUS lines need pullup by terminators, this test can be used to check a big part of your backplane: plug in only a single terminator at either end, QBone to different slots of the backplane and run this test. It will fail, if any of the 44 signals has no connection to the remote terminator. You even can test QBUS cables between separate backplanes this way.

And don't forget to remove the loopback jumpers on IAK\* and DMG\* after test!

 

## Finally: run it in a PDP-11

If all went well, plug QBone into a PDP-11.or boot operationg systems from [emulated RL02](http://retrocmp.com/projects/unibone/275-unibone-as-disk-emulator).
