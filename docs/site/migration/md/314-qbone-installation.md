<!-- migrated from https://retrocmp.com/projects/qbone/314-qbone-installation -->

Here some info how to get QBone running after unpacking.

## Which slot?

Well, QBUS is less complicated than UNIBUS: QBone can be put into any quad QBUS slot.

Just be sure to close empty QBUS slots between CPU and the last used slot with G9047 GRANT continuity cards. Not all slots are QBUS slots, and slot order is not trivial, see "Backplane geometry" below.

### Attention: possible short cuts

As the BeagleBone is a bit too fat, some of its parts may touch the solder side of a module sitting over it.

Either push thin card board between the modules, or mount QBone below an empty slot, filled with a G9047 GRANT then.

### Terminators

Single-backplane QBUS systems (I've never seen other ones!) do not need a terminator pack at bus-end, as DEC CPUs always terminate the bus.

For multi-backplane systems there's some alchemy where to use additional terminators and whether to use 120 or 240 terminators on the other bus end.

Some backplane even have resistor packs soldered on.

QBone has no onboard-terminators: If QBone replaces the CPU in a QBUS system (such as for self test), you need additional terminator packs.

## On-board jumpers

QBUS comes in different variants, and QBone can be plugged into different backplanes. There are some jumpers to verify:

[![qbone jumpers](images/qbone-jumpers.jpg)](/images/stories/joerg/qbone/qbone-jumpers-big.jpg)

* Click to enlarge*

### Setting up QBUS address width

QBUS systems may use 16, 18 or 22 bit width addresses. The address width of a QBUS system is primarily determined by the CPU in use, QBone has no means to guess it from bus traffic.\
So two settings are necessary.

- If using an LSI11/03 system, remove the "**DAL\<21:18\>**" jumpers, as these lines carry special CPU-internal signals.
- Call the "demo" application and the prepared \*.sh script with command line option "**-aw 16**", "**-aw 18**" or "**-aw 22**"

These settings determine how much memory QBone emulates and for which addresses it activates the "BS7" IO page signal.

### Closing C/D GRANT continuity

QBUS IRQ and DMA GRANT signals are daisy-chained from slot to slot. Insert the "**CD GRANT**" jumpers, if the C/D fingers of QBone are plugged into a socket which carries a standard QBUS.

Some values:

|           |                |                  |
|:---------:|:--------------:|:----------------:|
| Backplane | QBone position | CD GRANT jumpers |
|   H9270   |    any slot    |      closed      |
|   H9275   |    any slot    |      closed      |
|   H9276   |    any slot    |       open       |
|   H9278   |   slots 1-3    |       open       |
|   H9278   |   slots 4-8    |      closed      |

Check backplane geometries below.

If the CD GRANT jumpers are not closed correctly, controllers plugged behind QBone will not work when doing Interrupts or DMA.

### Generating 50Hz clock on EVNT

DEC power supplies generate a 50/60 Hz square wave signal on the EVNT bus line. If you use another power option, and still need the LTC signal, then close jumper "**EVNT 50Hz**".

### Test/factory jumpers

The "**IRQ/DMA GRANT**" jumper block can disconnect the IAKI/IAKO/DMGI/DMGO lines from QBone, or short-cut them for self test.

A selectable "**EEPROM ADDR**" is required for BeagleBone "Capes" ... their name for plug-on boards. A guy from beaglebord.org once called UniBone "the biggest cape ever".

Leave these jumpers in their factory-position.

\
BeagleBone operation
--------------------

You access the BBB software via login to a Linux terminal session as root/root. Best you plug BBB via Ethernet to your home network, your DHCP server will assign the name "qbone" or alike ... that depends on the text in /etc/hostname. Or you login via serial port on UART1, use a null-modem cable to your PC.

After applying +5V to QBUS power, BeagleBoneBlack receives +5V power approx. 1 second delayed by the onboard relay. This delay should be enough to give BBBs reset circuitry a clean "power-on" signal, despite the vintage PDP-11 power supply may come up quite unexpected. +3.3V power is generated on BBB and indicated by the green power-LED.\
Then BBB is booting Debian, which takes much too long... in worst case over 1 minute! Some Linux guru should improve this. 

You also can power BBB over its front jack, even when plugged into a unpowered PDP-11. The relay will not close then and isolate BBB's +5V from the huge PDP-11. Don't use local BBB power when the PDP-11 is ON, two power sources working against each other may overload the relay contacts.

Login via ssh or UART then, "putty" is a good Win10/Linux host terminal emulator. Additional you may ftp to the BBB, best with a graphical client like WinSCP. You will use file access often when dumping own disk images to the disk emulators.

On power-down you may use a clean "shutdown +0" before. But that's not necessary, Debian has a crash-save journaled file system.\
If you want to restart the BBB, don't powercycle the whole PDP-11, use the reset button instead.

When using alternate SDcards, get the best you can. Unlike in smartphones or cameras, the SDcard is in use by Debian all the time, speed and reliability are crucial here.\
\

## Ready-to-run shell scripts

In the SDcards main "/root" directory you'll find a couple of executable shell scripts. These usually start the "demo" software with control files to emulate several QBUS devices.

Be sure to always add the "address width" option "**-aw**" ... see above and below.

## Autostart

QBone can be configured to start one of the emulation-shell-scripts automatically on power-on. Up to 15 script can be prepared, selection is via DIP switch settings and feedback after emulation start via LED codes.

See file "/root/autostart.sh" for details.\

*Be sure to leave all switches in the upper "OFF" position at first.*

You can connect to an already auto-started session after login via ssh.

 

## Background info: QBone on QBUS

First: I owe so much to the [QBUS information page](http://web.frainresearch.org:8080/projects/pdp-11/)! Thank you!

 ![qbone cage full](images/qbone-cage-full.jpg)

 

 

### QBUS 16, 18, 22 ?

DEC UNIBUS has a fixed address width of 18 bit, allowing addresses 0..777776 or 256KB. Bigger UNIBUS-CPUs (11/44, 11/70) with 22 bit address width (4MB range) must use separate enhanced memory busses.

In contrast, QBUS was designed to work with 16 bit (64KB), 18 bit (256KB) and 22bit (4MB) address width. Address lines are multiplexed with 16-bit data on the QBUS "DAL" lines. On later MicroVAXes with memory sizes \> 4MB, again local memory busses are used.

This flexibility is mostly hidden in CPU logic, IO device get the IOpage (upper 8KB) addressed with a special "IOPage" address line, named "BS7" ("bank 7 select").

QBone acts as bus master (CPU or DMA device) and slave (memory or device register) and has to deal with additional complexity.

The number of address lines in use requires attention on several places. Pluggin together a non-standard system requires major research!

**CPU**: the CPU has to generate a proper BS7 signal for the upper 8KB page of the address range in use.\
The LSI11/03 M7264 and 11/2 are 16bit, 11/23 with F-11 is 18 bit, 11/23+ and J11 based 11/53, 73, 93 are 22 bit.\
The LSI11's use backplane address lines DAL\<21:18\> for other signals. DAL\<17,16\> are always terminated properly and just not used on 16bit systems.

[TABLE]

 

**Backplane**: the backplane must carry the required address lines between CPU and memory or IO device.\
As far as I know all backplanes are at least 18 bit capable, meaning DAL\<17:0\> is always wired correctly.\
The LSI11 backplane H9270 can be wired to carry DAL\<21:18\> too, allowing 4MB machines (23+, J11's) to run in this handsome 4\*4 slot cage. Several web pages know how to do it.\
Furthermore, backplanes differ in their slot order ("zig-zag"!) and number of special C/D" slots.

**Passive IO device** (memory or device): must only respond to address lines in use, must ignore unused upper address lines.

**Active DMA device**: must generate proper BS7 IOpage signals for addresses in IOPage.

Some devices have QBUS18/22 jumper (for example RLV12 RL disk controller). Others work only on 16/18 bit busses, for example DRV11-B M7950 has only 16 address lines.

 

### Backplane geometries: AB, CD, zig-zag

There are several types of QBUS backplanes. The good news: All use the rows A&B for QBUS.\
Use of C&D rows differs: sometimes they also contains QBUS, sometimes all C&D slots are connect to a separate bus, sometimes C&D are a separate bus with some kind of "daisy-chain" logic.

[TABLE]

There is only one rule: If QBone's C&D slots also go into a QBUS slot (not a special C&D), then set the GRANT continuity jumpers JP7 and JP8. 

### EVNT and the 50/60Hz Line Time Clock signal

QBUS has a signal EVNT, which causes an unconditional CPU trap (like powerfail). Its mostly used to implement a software clock (LTC= Line Time Clock), DEC power supplies generate a 50 or 60Hz square wave here.\
If you have a non-DEC power supply (providing +5/+12V is not SUCH a challenge), you need to generate LTC on yourself. QBone has a 50Hz source, just set jumper JP9 to route it to QBUS EVNT, and that's it.

That's it? Just kidding!

LTC/EVNT can be **produced** by more than source:

- the power supply,
- peripheral cards like BDV11 M8012, which has a software register to switch LTC EVNT generation by programs.
- QBone via jumper

You may have only ONE LTC source in the system!

LTC **use** by the CPU is also complex:

- LTC EVNT signal use can be inhibit via a front panel switch "AUX ON/OFF" on some cases (BA-23S, 11/23), which tie EVNT to ground. On other cases, "AUX ON/OFF" is main power.

- LTC EVNT trap can be disabled for some CPUs by setting or opening a wire wrap jumper (LSI11, 11/23)

- LTC EVNT trap can be disabled for some CPUs (11/23+, J11) via a software register. And even the presence of this register can be configured (on KDJ11-E 11/93). AND this can be combined with a hardware jumper.

  ![pdp1123plus panel](images/pdp1123plus-panel.jpg)

Do you need 50Hz EVNT? It depends.

- UNIXes need a LTC signal, they will not boot if it's missing.
- Some DEC OSses need lTC too. At least RSX11M and M+ needs it for task scheduling.
- Other OS are less sensible. For example RT11 needs it for the system time-of-day.
- If LTC is enabled (present on EVNT and enabled in CPU), the system needs a trap handler at vector 100, else it will crash.

Bottom line: Good luck!

 

![cage wide](images/cage-wide.jpg)
