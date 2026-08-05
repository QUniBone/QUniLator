<!-- migrated from https://retrocmp.com/projects/unibone/283-unibone-getting-one -->

## Free as in "free beer"

UniBone software and hardware project is totally "open source", it comes under the "[BSD license"](https://en.wikipedia.org/wiki/BSD_licenses).\
You are allowed to forward, modify and even sell everything, as long as my Copyright remains visible. Needless to say, I exclude any warranty.

### SDcard image

You find a zipped image of the SDcard at

<http://files.retrocmp.com/qunibone-misc/sdcard/>

 Unzip and "dd" to a 16GB (or larger) SDcard.

Use a Linux commandline like:

gunzip \<\$zipped_sdcard_image_file  \| sudo time dd of=\$sdcard_device bs=1M

"\$sdcard_device" is "/dev/sdb1" on my Ubunu VM.

 The network name for the BeagleBone is "unibone". Network login is "root/root". Both UARTs login as "root" automatically.

### Hardware design files

The KiCad project for the main PCB is a \*.zip at 

<http://files.retrocmp.com/qunibone-misc/01.01_pcb/>

 The little spacers for the BeagleBone mount are at "bbbadapter".

### Github

The SDcard image is not updated very often. The latest software source can be found at [github](https://github.com/j-hoppe/QUniBone).

The "10.\*" directories contains all sources for PRU and ARM and the "Demo" application.

IF you want to recompile with "make", first set up environment variables in the current sh session with

. ~/compile-bbb.env

## Buying one

I don't see myself as a hardware-maker, but I can offer ready-to-use UniBones. They'd be build and tested, and would come with the SDcard.

Sadly since 2021 electronic components clearly increased in price. As an UniBone is laborious to complete, I have to take €300 for one, without a BeagleBone (see picture). With BBB add €70 (changes day-to-day).

There were requests for blank boards. I do not offer these anymore, but will give you the Gerber files so you can order them yourself.

And couldn't escape to offer a kit: it will come "with SMDs populated" + remaining parts = 180€.

E.U. shipping is about 16€, international around 60€.

And regarding U.S. politics: depending on current tarriff chaos (or my bad mood) shipping to the U.S. may be temporarly impossible.

This email address is being protected from spambots. You need JavaScript enabled to view it. or This email address is being protected from spambots. You need JavaScript enabled to view it..

![board without bbb](images/board-without-bbb.jpg)

 

### The RL02 panel box

This control box is also available. It is attached over I²C bus, the RL02 device emulator inputs/outputs several "parameters" also over lamps and buttons. Several boxes can be daisy chained, to allow for future device panels too.

![rl02 panel white](images/rl02-panel-white.jpg)

The PCB is not specific to RL02, its just a generic GPIO extender, able to drive 16 lamps and read 16 inputs. Many driver boards can be daisy-chained over I²C. The bread board area allows to implement different power solutions, depending on the lamps you want to drive. 2A 5V anbd 1A 3.3V are available directly over UniBone cable.

![rl02panel pcb](images/rl02panel-pcb.jpg)

 

In reality it looks like this:

[![rl02 panel inside2 small](images/rl02-panel-inside2-small.jpg)](/images/stories/joerg/unibone/rl02-panel-inside2-big.jpg)

*(Click to enlarge)*

You can build "the box" from the files attached (Gerbers on request). A complete kit is 160€, includes 3D printed case, PCB, electronic parts, chinese push buttons&lamps, cables and lamp labels on adhesive foil. Assembled & tested its €220.

## UniProbe

"[UniProbe](http://retrocmp.com/tools/uniprobe)" is a debugging terminator for UNIBUS systems. It has signal LEDs and connectors for logic analyzers, and makes a nice companion for UniBone.

 

## First steps after building or unpacking

First, you should check out latest news in the [UniBone Google Group](https://groups.google.com/forum/#!forum/unibone).

Then:

**Power On**

1.  plug UniBone into a PDP-11 SPC slot with OPEN NPG grant chain at CA1-CB1. As we will emulate an RL11 controller, its best to remove the existing RL11 and put UniBone into that hole.
2.  attach Ethernet cable
3.  Connect console terminal to PDP-11 and power on. The PDP-11will show errors until UniBone software „closes“ the GRANT chains.
4.  after 1 second, UniBone gets power (1st green LED)
5.  Debian Linux will boot, needs a minute or so.
6.  login to UniBone: per ssh to host „unibone“ or "unibone2" (you can use the PUTTY tool)
7.  user and password is "root/root"
8.  you can also login over serial UART1 or UART2 at 9600. For connector, crimp a DSUB9 male and a 2x5 Berg to a piece of flat cable.

**To emulate RL11 and 4 RL02 drives with operating systems:**

1.  Make sure there's no other RL11 controller is installed at 774400, else there's an address conflict.
2.  to boot XXDP2.5, execute „./xxdp.sh“ on UniBone
3.  2nd green LED goes on, as UniBone now controls the UNIBUS.
4.  wait until PDP-11 „resets“ (UniBone simulates a power cycle).
5.  The PDP-11 is now functional. Use as usual, or boot from device „DL1“.

To boot an RT-11, execute „rt11.sh“. On DL1: is an ADVENTURE.

 

## Updating and compiling

UniBone software will (hope so!) updated,as future developments and bug fixes appear. Software is distributed as a gzipped tar archive.

 

**Unpacking:**

Software comes as two files "unibone-deploy.gz" and "unibone-develop.gz".

1.  Copy this file and "deploy_unpack.sh" to the BBB into /root.
2.  Make the unpack script executable: chmod +x \*.sh
3.  Then run ./deploy_unpack.sh

***Attention: this will override existing source files in the 10\* directories, so BACKUP if you made changes!***

**Re-compile**

To recompile new software (example: "demo" application):

In the current sh session:

    root@unibone:~# . ~/compile-bbb.env
    root@unibone:~# cd ~/10.03_app_demo/2_src/
    root@unibone:~# make clean
    root@unibone:~# make

**Updating the PRU compiler and libraries**

Development software comes in archive "unibone-develop.gz". It is unpacked together with the soruce codes.

To install the latest clpru compiler, execute the sh-sript in 91_3rd_party/pru-c-compiler.

Check the current version by just calling "clpru" without arguments.

 

 \* \* \*

Of course there will be errors ... we are so early in "beta". Lets stay in touch.

[rl02panel.stl -- RL02 panel box 3D print](/attachments/article/283/rl02panel.stl)

[rl02panel-schematic.pdf -- RL02 panel PCB schematic](/attachments/article/283/rl02panel-schematic.pdf)

[rl02panel-pcb.jpg -- RL02 panel PCB picture](/attachments/article/283/rl02panel-pcb.jpg)

[schematic.pdf -- UniBone schematic v2019-12](/attachments/article/283/schematic.pdf)
