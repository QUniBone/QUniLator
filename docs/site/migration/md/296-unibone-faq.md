<!-- migrated from https://retrocmp.com/projects/unibone/296-unibone-faq -->

### Which BeagleBone will work?

At the moment, only the classic "BeagleBone Black" is supported "out-of-the-box".

"Black Wireless" is under community test.

"Green" ("BBG") is not 100% compatible to BBB and not working out of the box. Apparently the GPIO impedance is different, which is a problem with these nano-second timed PRU GPIO singals. However BBG can be made to run by using different terminator resistors on the PCB, but will not reach the speed and safety margin of BBB.

"Enhanced" (Blue): could not make a network connection with standard BBB SDcard image. Probably needs an adapted Debian installation.

Forget about the old BeagleBone "White", not even tested.

The exciting new "BBONE-AI" has to be checked out.

 

### The best book about the BeagleBone?

http://derekmolloy.ie/tag/beaglebone-black/

\$27 at amazon. Covers EVERYTHING. Thanks, Derek!

 

### UNIBUS backplanes

UniBone needs a "SPC" Unibus slot. I can imagine 4 types backplanes, all looking exactly alike:

1\. Standard expansion backplanes, like DD11-DK. These have SPC slots in every socket row on slots C-F.

2\. CPU backplanes. These typically have special slots for some kind of processor boards. Then perhaps rows wired for core memory (11/05), or local memory (11/44, 11/84). The rows left after all that are SPC slots, for at least a serial console or one boot device.

3\. Specially wired controller backplanes, for older multi-board controllers (RK11, DL11). These have no SPC slots.

4\. QBUS backplanes. Don't even think of using these.

### UNIBUS terminators

When doing the "bus latch" stress test, every UNIBUS line is set to random state, then all states are checked. If an M9302 or similar termintor with active "SACK" locgic is used, this test will fail on SACK line.

 

### Powering UniBone outside a PDP-11

UniBone can be run outside UNIBUS. This is useful when doing program development or experimenting with Linux scripts and settings, you don't need to waste PDP-11 life time for these activties.

An UniBone runs on +5V only. Current draw well below 1.5 amp.  There are these power options:

- Use the +5V jack on the BBB with a BBB power supply. Driver chips and the I2C panel power will not have +5V then, as they are fed by the UNIBUS +5V.
- Feed power to the "UNIBUS 5V", best on leads of the silver capacitors or to "5VUB" pins. Then all components are powered, you should hear the delay-relay clicking.
- Use a strong (\> 2A) USB power supply and feed power to BBBs USB port.
- DO NOT power UniBone over BBB's USB connector from a PC USB ports. USB 2.0 data ports can drive only about 500mA, which is only sufficient for a bare BBB, not for BBB + UniBone.

In any case, the green "Power" LED will indicate 3.3V generated on the BBB is OK.

Power consumption will greatly increase when driving UNIBUS terminator arrays.

 

### Using all space on SDcards \> 16GB

The distributed file system image is made for a 16GB card. If it is dumped to a bigger card, you want to expand the file system to use all the extra space. That's an one-liner, with some info around:

    df /
    sudo /opt/scripts/tools/grow_partition.sh
    reboot
    df /

 

### Will UniBone get damaged if powered off without shutdown?

In theory, Linux system must properly shutdown. And you see warnings for the BBB here and there.

On the other side, I never cared for that and power-cycled my BBBs hundreds of times without any damage.

The SDcard in use seems to have also some protection against power-loss, the journaled Linux-filesystem is also fool-proof.

 

### Why does my UniBone not boot?

Well, most likely it does! But sometimes booting needs several minutes. I never inverstigated that, despite the Debian boot-time seems much too long. Specially without Ethernet-network communication Linux needs endless to start the user session.

When booting the BBB, watch for the blue LEDs. If the UBOOT bootloader starts properly from SDcard, all 4 LEDs are ON for a short moment. You must see this, and it never happens again. The power LED is steady all the time. While the kernel is booting two other LEDs flicker and pulse heavily. The green and yellow LED on the Ethernet port go off for a short period while Linux re-initializes the network. If the system is booted and idle, only on LEDs pulses in a semiregular pattern.

You debug BBB boot problems by attaching a RS232 adapter to the BBB-onboard console port and watch UBOOT/kernel messages.

### My UniBone does boot, but I don't see any software as described?

On the BBB there is a SDcard-like memory chip, named "eMMC". It also contains a factory-installed Debian installation. So if something is wrong with your SDcard, or the boot-selection mechanism, you will boot THAT and not the UniBone software installation.

When your BBB is unplugged from the UniBone base board, it looses some resistors encoding the boot device. Then it will ALWAYS boot from the internal eMMC chip.

 

### I seem to have network problems?

Two reasons:

A\) The BBB has a bug in its Ethernet PHY, I wrote about it at\
<https://groups.google.com/forum/#!topic/unibone/fq_IpLIuWT8>

\
Sometimes after powering on it doesn't contact the net, indicated by the yellow LED on the connector not shining.

\
I had this sometimes, in varying intensity. Never recognized a pattern. Problem is typically gone after the next power cycle (a simple "reboot" does not help).

At the moment there's no problem here, maybe it depends on the actual BBB in use.

B\) If you have several BBBs in use, and jingle SDcard with Linuxes on it between them:

This may confuse your DHCP server or other network members. Reason is that the EhterNet MACID is located on the BBB hardware, while 'hostname" is located on the SDcard installation. The wrong assignment may persit in some caches for minutes, even hours.

You can log into your DHCP server, perhaps you see multiple "UniBOne" s in your DHCP table. Try the indicated numeric IP address instead.

As backup to all network problems you can use the two serial console ports on the UniBone, they are configured for auto-login sessions.\
You can crimp a cable and connect with 9600 8N1.

 

 

### Writing and backup SDcards

The net is full of tools how to image SDcards.

I create and restore SDcards under a Ubuntu VM running on Win10, using "dd" to /dev/sdb.\
Once in a while I create damaged cards this way. To check, I replugin them after writing, and see wether the filesystem is popping up on the Ubuntu desktop.

Then do a "sudo fsck /dev/sdb" :\
\$ sudo umount /dev/sdb1\
\$ sudo fsck /dev/sdb1\
fsck von util-linux 2.20.1\
e2fsck 1.42.9 (4-Feb-2014)\
...

I also change /etc/hostname directly on the SDcard this way, before it is running on the BBB.

The path on my VM to the Sdcard is then: "/media/joerg/rootfs/etc/hostname"

 

### Booting disk images

UniBone emulates several kind of disk devices. However, you need an installed PDP-11 bootloader to boot from these.

These bootloaders are now dup,med into memory wehen installing a certain dik system, checkout the command files. So you don't need to have the correct BOOT ROM installed.

 

### A PDP-11 disk image is booting, then crashing?

When starting one of the xxdp.sh, rt11.sh, rsx.sh etc. scripts, the PDP-11 may hung or crash at some point.

This may have hundreds of reasons. Some popular ones:

1\. Verify the GRANT chain is closed on the backplane. Make all G727 grant cards good contact? Is the NPG chain on CAB1-CB1 closed on all un-occupied socket rows? Is UniBone plugged into a socket with OPEN CA1-CB1?

The UniProbe board is of great help here.

2\. Some operating system (namely: RSX) need to be SYSGEN to the target hardware. RSX for 11/34 will not run in an 11/84. So model your physical PDP-11 in SimH first and try to boot the disk images there. Then switch to UniBone.

 

### GRANT chain: Why is my machine not starting when UniBone is plugged in?

UniBone does close the GRANT chain in software, by monitoring pins and forwarding signals.\
So if UniBone is not running, the GRANT chain is open.

Background:\
UNIBUS systems acknowledge Interrupt and DMA request by routing the CPU acknowledge ("GRANT") not to all requesting cards in parallel, but only to the first card on the bus. This card has to forward the signals to the next, if it did not issue the request, and so on.\
This mechanism is called "GRANT chain". On un-occupied backplane slots the GRANT signals must be forwarded by dummy controller cards.\
The GRANT chain for Interrupts ("BG4,5,6,7" signals) is closed with G727 cards in row D, the DMA ("NPG") is closed on the backplane by connecting wire wrap pin CA1 with CB1.

When a PDP-11 is not using DMA and interrupts, it can run with open GRANT chain. However the common active terminator M9302 raises the SACK line on open GRANT chain, which allocated the bus and stops the machine. Thats a quick indicator for GRANT chain problems.

The LEDs on the [UniProbe](/tools/uniprobe) terminator show open BG4567 & NPG signals and raised SACK.

You can close the GRANT chain on UniBone by jumpers permanently, but then it can't do any Interrupts or DMA, no device emulation is possible anymore.

You also can configure UniBone Linux to autostart the device emulator scripts on power up. These scripts emulate a PDP-11 power cycle by stimulating ACLO and DCLO signals, this should reboot the PDP-11 again, this time with GRANT chain closed by running UniBone software.

###  

### Why can I boot XXDP from emulated disk drive, but no other OS?

XXDP disk drivers (at least RL11) don't use Interrupts, only DMA. So errors on the BG4-7 grant chain have no effect. In practice this means: XXDP is tolerant against missing G727 Grant Continuity cards, but no tolerance against broken NPG chain (CA1-CB1 slot jumpers).

 

### Can't install software with "apt-get install" ?

QUnibone is using Debian "Jessie",  8.10. The paket repository was put offline mid 2024.\
To fix, an mirror was copied to\
<http://files.retrocmp.com/qunibone-misc/debian-8.10.0-armhf-DVD-1/>\
and /etc/apt/sources.list was updated.\
To get these fixes, either

- copy content of <http://files.retrocmp.com/qunibone/02_bbb_config/03_debian-8.10.0-armhf/>\
  to your QUniBone file system and execute "update-apt.sh"
- or run ./update-files.sh

 

### Using the serial ports

The UniBone PCB exposes 2 (of 4) serial ports.

Pin header "UART1" is Linux device "/dev/ttyS1", "UART2" is "/dev/ttyS2".

Both serial ports can be configured for Linux-auto-login, or for use by a simulated PDP-11 SLU (DL11).

To configure a port (here ttyS2) for auto login (default):

    ln -sf /lib/systemd/system/getty@.service /etc/systemd/system/getty.target.wants/This email address is being protected from spambots. You need JavaScript enabled to view it.
    reboot

To configure for use by DL11 emulator:

    rm /etc/systemd/system/getty.target.wants/This email address is being protected from spambots. You need JavaScript enabled to view it.

Then reboot.
