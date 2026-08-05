<!-- migrated from https://retrocmp.com/projects/unibone/273-unibone-hardware -->

## The Board

Now this is about the actual UniBone hardware. Its not as complicated as the software, but still interesting. Lets have a closer look at the PCB, latest version as of december 2019.

[![pcb front](images/pcb-front.jpg)](/images/stories/joerg/unibone/pcb-front-big.jpg)

*(click for large version)*

First: Thanks to Malcolm from [avitech.com.au](http://avitech.com.au/) for providing the KiCad symbol of a flip chip quad board.

The board looks definitely "retro", this is no pure coincidence !

I intentionally mimiced the look of an 1980's quad board. So we have "through the hole" components (instead of SMD) as much as possible, silver tantals at the slot fingers (sometimes even taken from old boards) and that green color. Only standard TTLs, no FPGAs. No white or blue LEDs are allowed, connectors are BERG style, transistors and resistors are big.

On later versions there are more SMD parts and a different BeagleBone mount, mostly to reduce

 

## Board functions

 The UniBone PCB consists of several components, and is only usable as part of a surrounding PDP-11 system on the other hand.

![hardware](images/hardware.jpg)

 

UniBone is plugged into a UNIBUS **backplane** in parallel to other PDP-11 boards.

**UNIBUS** has 56 standard signal lines, all of these are connected bi-directionally. The good old DEC bus driver chip **DS8641** is used here. The DS8641 is vintage, I got them for 50 cent a piece (don't pay \$3, you need 16 of these!).

UniBone can act as

- UNIBUS slave (accessed as memory location or device register by PDP-11 CPU),
- it can be UNIBUS master (arbitrating the bus, doing interrupts and DMA).

Even the power supply signals ACLO and DCLO can be driven by UniBone. This way the PDP-11 CPU can be commanded to go through a power-cycle reset.

With a global "Driver enable" the whole board can be "plugged off" from the UNIBUS.

The two microcontrollers **PRU**0 and PRU1 have each 32 dedicated high-speed GPIOs. Only some of these are routed out of the BBB pin headers, we only use 8 output pins on PRU0 and 4 outputs and 8 inputs on PRU1. These 8-bit 100+MHz ports are expanded to 64bit 10+MHz ports with an array of register latches. PRU-GPIOs are best driven uni-directional, as changing from input to output takes "much" time: range of a micro second, while regular operation is at max 5ns. So there two separate unidirectional datapathes for input and output to the bidirectional UNIBUS drivers are used.

The UNIBUS protocol logic is done completely on **PRU1**. PRU0 is just forwarding data to its GPIO outputs.

A challenge for interfacing modern electronics like the BBB to old hardware is the conversion of 5V logic levels to 3.3V. Old 5V logic can be driven by 3.3V levels, only losing some noise margins. But special "5V" tolerant 3.3V logic must protect BBB inputs from 5V signals. You could use voltage dividers, here 74LVTH family latches are used.

UniBone gets its +5V **power** from the SPC slot, the BBB produces 3.3V for free. Old DEC power supplies can behave arbitrarily odd on Power-ON, and the DCLO "power-good" signal is under application control. So to get a clean power-ramp for the BBB, a relay delays +5V by one second or so.

Communication between the PRUs and the main ARM CPU is over **shared memory windows**. Small local PRU-RAM is mapped into the ARM address space and is used as a command/data mailbox. The big ARM DDR RAM can be accessed by PRUs at a slower rate, as several cache and bus layers have to be crossed. It is used for emulating UNIBUS memory.

As PRU1 listens to UNIBUS traffic and decodes register accesses to emulated devices, he notifies the ARM software with an **interrupt** signal.

Some **LEDs and switches** are connected to BBB-GPIOs as eye candy. Two **patch fields** provide space for own peripherals (or to fix my design flaws). You can connect special hardware to BBB pins (GPIOs, I²C, analog) and let the PDP-11 drive them after writing own UNIBUS controllers.

All high-speed PRU signals and all UNIBUS signals have labled **pin headers**. An empty UniBone PCB is still an adapter to connect logic analyzers to the UNIBUS. And we have GRANT jumpers too: UniBone is also a G727.

Preferred access to the BBB is over **Ethernet** and TCP/IP services. WLAN would not work, as UniBone will be probably be mounted inside an all-metal DEC case. The media images for simulated storage devices can even sit on remote computers.

UNIBUS SPC slots carry +5V, -15V and +15V. These voltages are routed on to a **power connector**. You can feed stand-alone backplanes with other SPC cards over UniBone.

To operate emulated PDP-11 devices "like in reality", custom made lamp&button **panels** can be attached to an I²C bus. 2 UARTs are routed to DSUB 9 connectors, they allow Linux session on real VT100s. Or can be used if serial PDP-11 controllers shall be emulated.

 

![rl02 panel white](images/rl02-panel-white.jpg)

 

## Mechanics

A BBB is only a little heigher than a DEC board slot. And neighbor boards often have space atop their ICs, because DEC logic ICs are usually not socketed. This gives extra space for the bulky Ethernet port.

So the BBB is mounted not on top of the UniBone PCB, but is hovering upside-down in a cutout. Length-reduced pin headers and little piggy-back PCBs give the correct vertical alignment. Some special parts, and time consuming to build!

But it is worth the work:

![board fit](images/board-fit.jpg)

The Ethernet port needs an 90° angle plug, then UniBone fits even in a closed DEC BA11 case. As I learned from earlier designs, SDcard slot and USB port remain accessible.

## Use without electronics

I hope it never will be the main use case, but: The PCB is even useful if not populated with any parts.

![pcb naked](images/pcb-naked.jpg)

 

 You can always use the PCB as a long G727 GRANT continuity card, with added NPG-closing function.

![grant jumper disabled detail](images/grant-jumper-disabled-detail.jpg)

 

Or you use the labeled slot fingers, the UNIBUS pinheaders and the patch fields as UNIBUS prototyping board. The DS8641 drivers are surely helpful then.

![unibus connector detail](images/unibus-connector-detail.jpg)

 

Pro tip: wrap yourself a bus adapters for your logic analyzer! Else your lab will look like mine: the typical "mad scientist" setup from a B-movie.

![workplace](images/workplace.jpg)

 

For  schematics et.c see attachements on the [Download page](/projects/unibone/283-unibone-getting-one).
