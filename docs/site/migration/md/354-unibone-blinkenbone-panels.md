<!-- migrated from https://retrocmp.com/projects/unibone/354-unibone-blinkenbone-panels -->

### About BlinkenBone

"[BlinkenBone](/?view=category&id=47)" is a project to revive historic front panels by giving them a network interface and drive them by software ... via the interface "Blinkenlight API".

Let's recapitulate some concepts:

**Server**: A BlinkenLight panel is remote from the driving application, and implemented as "Blinkenlight API server". It can be

- an instrumented [historical panel](/?view=category&id=68) (not  necessarily built by DEC),
- a [PiDP-8](https://obsolescence.wixsite.com/obsolescence/pidp-8),  [PiDP-11](https://obsolescence.dev/pdp11.html) or perhaps other replicas,
- any of the pool of [photorealistic Java emulations](/?view=category&id=66) (11/20,40,70, PDP-8/I, PDP-15, PDP-10 KI10)\
  Download these from [github](https://github.com/j-hoppe/BlinkenBone/releases), and use one of the "\*\_panel_only" starter scripts.

![building adapters 1](images/blinkenbone-pdp8-threesome.jpg)

 

**Controls**: the BlinkenLight API exposes a panel as "set of panel control"s. A "panel control" is just a functional group of switch and/or lamp fields on a panel. [Read more](/projects/blinkenbone/172-blinkenbone-high-level-interface-api-to-panels).

 ![blinkenbone controls pdp11 70](images/blinkenbone-controls_pdp11-70.jpg)

**Client**: A "Blinkenlight API client" connects to the panel, queries its switches and sets its lamp via network. A client can be

- a computer emulation like SimH with "REALCONS" extension.
- a separate test program like "blinkenlightapitest"
- or (and thats we you're here): ***a real PDP-11 equipped with QBone/UniBone running the "blinkenbone device".***

 

### Using a panel on QUniBone?

Bad news first: a DEC CPU panel is highly bound to a specific CPU hardware. That's clear: you can Start, Stop or Single step the CPU, see Micro code execution, see execution levels like "Super", "User", "Kernel", manipulate the Memory Mapping and so on.

As QUniBone is a pure UNIBUS peripheral device, it has no insight to the local CPU. The only thing a panel could do is simple EXAM and DEPOSIT, perhaps as well as displaying the flickering glow on some ADDR and DATA lines. 

That has almost no entertainment value, and seems not worth the work of integrating BlinkenBone to QUniBone.

So a different approach was choosen: "Programmers Playground".

- The panels lamps and switches are under full software control by a PDP-11 or µVAX equipped with an UniBone/QBone.
- The panel itself has no build-in function, its just a passive peripheral.
- All switches and lamps of a given panel are simply mapped into the PDP-11 I/O page as a register set.
- So you can max out the show effect on your own. 
- Reading / writing theses registers by PDP-11 programs directly influence the panel. Sync with the panel is done periodically in background. The PDP-11 itself is not involved with TCP/IP traffic, the BeagleBone handles that. Registers can be accessed at any time.
- *Every* Blinkenbone panel can be connected to your PDP-11: for example, a 11/05 can now play with the PDP10-KI10 monstrum.
- The layout of the switch/lamp mapping registers changes with panel type.

As a BlinkenBone panel is controlled via I/O page registers, you can play with a connect panel also via manual EXAM/DEPOSIT operation. You can issue these over the "demo" emulator command prompt, over ODT console on QBUS systems .... or over the system front panel on UNIBUS PDP-11s. And then things go weird: you operate *one* panel to checkout a *2nd one*? Nerds!

### Function overview

From PDP-11 side, die "BlinkenBone" device consists in fact of two logical separated devices, much like the serial DL11 has a receiver and a transmitter part.

The "Input" section reads the panels switches, the "Output" section controls the panels lamps.

First in the memory map comes a fixed register set, which is the same for all connected panels. \
This includes the typical Command and Status registers (CSR) for input and output.

After the fixed register block follow registers which map the panels switches and lamps. Number and function of these depend on the connected panel.

For the input side, the polling frequency can be set to a period of 1..1000 millisconds.\
If a change on one of the panel switches is detect, a flag is set in the input CSR, and optional an interrupt is triggered. You can read-out the address of the most recently changed panel input registers also.

On output, the panel lamps are updated periodically from the mapped registers. Update frequency can also be set, and after each update a status bit is set in CSR and an interrupt can be triggered. So you can drive dynamic light effects with the speed of the periodic output interupt.

The BlinkenBone device even can be operated without a connected panel ... the output side can be used as a programmable interrupting timer then.

### Using the BlinkenBone device

##### Short version first:

1.  Start the panel on a remote PC or RPi.
2.  configure the "blinkenbone" device, set the panels host name, and "enable".
3.  read and write the I/O page registers, which map the panels switches and LED banks.

##### Longer:

Under QUniBone, a BlinkenBone panel is a device just like DL11 serial port or RL11 disk controller, or any other.\
From UNIBUS/QBUS perspective the BlinkenBone device is a register block in the I/O page and an interrupt source.

All further examples are made with the PDP-11/70 front panel.\
And the network name of the host running the panel is important ... let assume its called "bigfoot" for now.

##### Step 1: Get the panel online

Startup the physical, replicated or emulated panel on a remote PC, which is connected to the BeagleBone via LAN.

![blinkenbone pdp1170 smallbox 1](images/blinkenbone_pdp1170_smallbox-1.jpg)

Make sure no other client is controlling the panel ... this can happen, if you run for example a simulator/panel bundle like PiDP-11. In that case, you have to kill the simulator, so the panel can be controlled by QUniBone alone.

##### Step 2: Verify panel access

Log on to BeagleBone and make a test access to the panel, so you know you can talk to it: Use "ping " and the Blinkenlight API test program "./91_3rd_party/blinkenlightapitst".\
You see which panels the server provides (usually only one with index "0"), and what "control" elements are exposed.

\
`login as: root`\
`root@unibone's password:`\
`Have fun with UniBone !`\
`Last login: Tue Jul 29 12:27:27 2025 from bigfoot.fritz.box`\
`root@unibone:~# ping bigfoot`\
`PING bigfoot.fritz.box (192.168.1.163) 56(84) bytes of data.`\
`64 bytes from bigfoot.fritz.box (192.168.1.163): icmp_seq=1 ttl=128 time=0.409 ms`\
`64 bytes from bigfoot.fritz.box (192.168.1.163): icmp_seq=2 ttl=128 time=0.623 ms`

`root@unibone:~# ./91_3rd_party/blinkenlightapitst bigfoot`

`*** blinkenlightapitst v1.09 - client for BeagleBone Blinkenlight API panel interface ***`\
`    Build Feb 3 2019 11:31:25`\
`    Copyright (C) 2012-2016 Joerg Hoppe.`\
`    Contact: This email address is being protected from spambots. You need JavaScript enabled to view it.`\
`    Web: `[`www.retrocmp.com/projects/blinkenbone`](/projects/blinkenbone)\
\
`Connecting to bigfoot ...`\
`*** Select a panel from server on host "bigfoot" ***`\
\
`0) Panel "11/70"`\
`       27 controls: 13 inputs + 14 outputs`\
`       Total 88 value bits: 37 inputs + 51 outputs`\
`       Value data stream: all inputs = 15 bytes, all outputs 17 bytes.`\
`       State of BlinkenBoards Enable=ACTIVE. Panel mode = 0 = "normal"`\
`e <n> 'enable': toggle tristate of BlinkenBoards assigned to panel <n>`\
`t <n> 'test': circulate lamp & switch test modes for panel <n>`\
`i)     print info about server`\
`q)     quit`\
`>>> 0`\
\
`*** Controls of panel "11/70", screen #0 ***`\
`* Inputs:`\
`POWER ( 1 bit SWITCH )=1o !   || PANEL_LOCK ( 1 bit SWITCH )=0o  || SR (22 bit SWITCH )=00000000o`\
`LOAD_ADRS ( 1 bit SWITCH )=0o || EXAM ( 1 bit SWITCH )=0o        || DEPOSIT ( 1 bit SWITCH )=0o`\
`CONT ( 1 bit SWITCH )=0o      || HALT ( 1 bit SWITCH )=0o        || S_BUS_CYCLE ( 1 bit SWITCH )=0o`\
`START ( 1 bit SWITCH )=0o     || ADDR_SELECT ( 3 bit KNOB )=7o ! || DATA_SELECT ( 2 bit KNOB )=3o !`\
`PANEL_LOCK ( 1 bit KNOB )=0o`\
`* Outputs:`\
`0) ADDRESS (22 bit LAMP )=00000000o || 1) DATA (16 bit LAMP )=000000o`\
`2) PARITY_HIGH ( 1 bit LAMP )=0o    || 3) PARITY_LOW ( 1 bit LAMP )=0o`\
`4) PAR_ERR ( 1 bit LAMP )=0o        || 5) ADRS_ERR ( 1 bit LAMP )=0o`\
`6) RUN ( 1 bit LAMP )=0o            || 7) PAUSE ( 1 bit LAMP )=0o`\
`8) MASTER ( 1 bit LAMP )=0o         || 9) MMR0_MODE ( 2 bit LAMP )=0o`\
`10) DATA_SPACE ( 1 bit LAMP )=0o    || 11) ADDRESSING_16 ( 1 bit LAMP )=0o`\
`12) ADDRESSING_18 ( 1 bit LAMP )=0o || 13) ADDRESSING_22 ( 1 bit LAMP )=0o`\

##### Step 3: set QUniBone "BlinkenBone" device parameters 

We're working under the "demo" application here, in the "device" menu.\
Just like other devices, you setup operating parameters first, then "enable" it.\
Default base address and interupt vector sit in the PDP-11 "floating" address space, check for collision with other devices.\
\
`DC>>> ``sd blinkenbone`\
`DC>>> p base_addr 760200`\
`DC>>> p intr_vector 310`\
`DC>>> p intr_level 6`\
`DC>>> p panel_host bigfoot`\
`DC>>> p panel_addr 0`\
`DC>>> p panel_id 1234`

For a full list,see next "reference" page.

##### Step 4: activate the panel.

When you "enable" the BlinkenBone device, connection to server is established and the register set is construced in the I/O page.

DC\>\>\> en\
`Connected to BlinkenBone server bigfoot:`\
`QUnibusAdapter: Registering device BLINKENBONE`\
`Server info................: Panelsim1170_app v1.01`\
`Server program name........: blinkenbone.panelsim.panelsim1170.Panelsim1170_app`\
`Server command line options: --width 1000`\

`[... Dump of fixed registers and mapped panel control registers ...]`\

Again, for a full list,see next "reference" page.\
 

##### Step 5: work with the panel

 You can now 

- manually EXAM and DEPOSIT in the BlinkenBone device registers.
- run standalone PDP-11 programs. The distribution contains a test program which show several techniques to access a panel.
- or modify some operating system like RT-11, RSX11 or UNIX to react on the panel. We're talking of idle patterns here.

Needless to say, all this works on UNIBUS and QBUS machines. Yes, QBUS machines have Blinkenlight panels now!
