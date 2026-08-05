<!-- migrated from https://retrocmp.com/projects/unibone/356-unibone-blinkenbone-programmers-reference -->

The BlinkenBone device has several registers and functions ... so we need one of these dry "Reference" pages here. 

### `Parameters of device BLINKENBONE:`

\
`Name             Short  Value          Access     Info`\
`---------------  -----  -------------  ---------  ------------------------------------------------------------------------------------------`\
`name             name   BLINKENBONE    read only  Unique identifier of device`\
`type             type   blinkenbone_c  read only  Type`\
`enabled          en     1              read only  device installed and ready to use?`\
`emulation_speed  es     1              writable   1 = original speed, > 1: faster`\
`verbosity        v      4              writable   1 = fatal, 2 = error, 3 = warning, 4 = info, 5 = debug`\
`base_addr        addr   760200         read only  controller base address in IO page`\
`slot             sl     30             read only  backplane slot #, interrupt priority within one level, 0 = next to CPU`\
`intr_vector      iv     310            read only  interrupt vector address`\
`intr_level       il     6              read only  interrupt bus request level: 4,5,6,7`\
`panel_host       ph     bigfoot        read only  hostname of Blinkenlight server, the computer running the panel .. physical, Java or PiDP11`\
`panel_addr       pa     0              read only  Address of panel in the Blinkenlight server`\
`panel_config     pc     123456         writable   Custom CONFIG register`\
`poll_period      pp     50             writable   Panel switches are polled every so many milliseconds. 0=disable.`\
`update_period    up     10             writable   Panel lamps are updated every so many milliseconds. 0=disable.`\

 

### PDP-11 register map

The BlinkenBone device appears in the PDP-11 I/O page as list of 16 bit registers. Position is given by parameter **`base_addr`**`.       `

1.  First comes a fix block with registers for the device itself. "Fix" means: they are independed of the connected BlinkenBone panel.
2.  Then there's the list of "mapped panel control registers". These are the memory location which give you access to the panels input switches and output lamps. These are highly dependend of the connected panel.

A panel needs much space in the I/O page. Actual register count depends on the panel ... so choose a base address with enough "air" behind. \
For example, the PDP-11/70 panel needs 29 words of I/O page. And the big "[PDP-10 KI10](/projects/blinkenbone/blinkenbone-physical-panels/245-blinkenbone-physical-pdp-10-ki10)" counts for 116 registers.

Default for base address is 760200, in the midst of the "floating" register space, which is defined to end at 763776.

On "enable", the BlinkenBone device dumps out its actual register layout, shown below for the PDP-11/70 panel. 

### `Fixed BlinkenBone device registers in PDP-11 address space:`

` Addr/Bits Reg name       Info`\
` --------- --------       ----`\
` 760200    PANEL_ICS      Command and Status Register for panel Inputs`\
`     <15>  ERR            Panel not connected, server error`\
`      <7>  IEVNT          Change Event on some input switches, may trigger INT`\
`      <6>  IIE            Interrupt Enable for Input`\
` 760202    PANEL_OCS      Command and Status Register for panel Outputs`\
`     <15>  ERR            Panel not connected, server error`\
`      <7>  OEVNT          Periodic panel Output (lamps) update occured, may trigger INT`\
`      <6>  OIE            Interrupt Enable for Output`\
`    <1:0>  OTSTMODE       panel test mode: 0=normal,1=lamp test,2=full test,3=powerless`\
` 760204    PANEL_IPERIOD  Interval for periodic panel input polling`\
`    <9:0>  IPERIOD        1..1000 millisecs, 0=off, inits to parameter "poll_period_ms"`\
` 760206    PANEL_OPERIOD  Interval for periodic panel output update`\
`    <9:0>  OPERIOD        1..1000 millisecs, ``0=off, ``inits to parameter "update_period_ms"`\
`  760210   PANEL_ICHGREG  Addr of last changed mapped input switch register`\
`  760212   PANEL_CONFIG   User defined bitpattern to tell PDP-11 panel config`\

 

### Detect Panel Switch changes with the "Input" logic

Register **PANEL_ICS** (Input Command and Status) gives error and status for the input side of BlinkenBone device, which reads out the panels switches periodically. Flag layout is DEC-style.

Flag **ERR** indicates that we have no connection to the panel, so there are no switches to handle and no mapped input registers for the panel.

The switches are polled from the Blinkenlight server periodically for changes. Polling speed can be set any time with register **PANEL_IPERIOD** in units of milliseconds. Default value after reset is given by parameter poll_period. There's no need to change polling period from default value 50ms, unless you have very fast fingers.

Bit **IEVNT** is set if a change in one of the panel switch states is detected. **IEVNT** is reset on read out (on DATI bus cycle).\
The address of the mapped register holding the switch state is captured into register **`PANEL_ICHGREG`**` then.`

If the interrupt enable bit **IIE** is set, an interrupt is triggered in **IEVNT**. Vector and bus request level are given by parameters **intr_vector** and **intr_level**.

### Setting Lamp patterns with the "Output" logic

Register **PANEL_OCS** (Output Command and Status) gives error and status for the output side.

Flag **ERR** indicates that we have no connection to the panel, so there are no lamps to handle.

Bitgroup **OTSTMODE **sets a self test mode:  `0=normal,1=lamp test,2=full test,3=powerless`

The panel lamps are updated periodically from the mapped registers. Update period is given by **PANEL_OPERIOD**, with reset value given by parameter update_period. 

After each update cycle flag **OEVNT** is set, it's cleared on readout of **PANEL_OCS**.

Bit** OIE** controls enable of the periodic "output update" interrupt. Vector and bus request level are given by parameters **intr_vector+4 **and **intr_level**.

As the timer and interrupt work even without connected panel, you have a small programmable universal timer here.

Other as for the input side, it makes sense to control the lamp update period: you can program animated light patterns, and drive there display speed via interrupt and variable update frequency. 

Or your light effects are changing with PDP-11 execution speed, so the update frequency should be as high as possible.\
1kHz (period=1ms) is max, but may cause too much load on the network between BeagleBone and panel server.

As said, communication with a panel server involves network transactions.\
This means: switches and lamps can not be updated with PDP-11 execution speed at ~1MHz, they are updated periodically with a slower synchronisation rate.\
Keep in mind that routing to the server may involve unreliable WLAN segments, so be speed-conscious.

### Accessing panel controls with mapped registers

![blinkenbone pdp1170 smallbox 1](images/blinkenbone_pdp1170_smallbox-1.jpg)

\
**Mapped Input Registers**: Starting from fixed register PANEL_CONFIG+2, registers are mapped to input controls (=switches and knobs) of the selected panel.\
These registers are "read-only" and reflect the state of switches. Number and function of these  registers depend on the connected panel.\
Every switch control is mapped at least to a full 16 bit PDP-11 register, whose name is that of the panel control as provided by the panel server.\
If a control is more then 16 bit width, more PDP-11 registers are used. Then suffixes "\_A\_, "\_B", "\_C\_", "\_D" are used.\
Example: on PDP-11/70, the ADDR switch bank is 22 bit width.\
You read switches \<15:00\> from register "SR_A", and switches \<21:16\> from "SR_B", bits \<5:0\>.

Same for the **Mapped Output Registers**: they follow the panel input registers in address space.\
Nothing new here: Writing into one of these puts lamps on and off, with some delay given by the periodic update logic..\
Lamp controls wider then 16 bits also get multiple registers.

**`Controls of panel 0 "11/70" on server "bigfoot" mapped into PDP-11 address space:`** 

`  Addr    In/out  Reg name       Bits     Panel control idx`\
`  ----    ------  --------       ----     -----------------`\
`  760214  input   SR_A           <15:00>  2`\
`  760216  input   SR_B           <21:16>  2`\
`  760220  input   LOAD_ADRS      <0>      3`\
`  760222  input   EXAM           <0>      4`\
`  760224  input   DEPOSIT        <0>      5`\
`  760226  input   CONT           <0>      6`\
`  760230  input   HALT           <0>      7`\
`  760232  input   S_BUS_CYCLE    <0>      8`\
`  760234  input   START          <0>      9`\
`  760236  input   ADDR_SELECT    <2:0>    24`\
`  760240  input   DATA_SELECT    <1:0>    25`\
`  760242  input   PANEL_LOCK     <0>      1`\
`  760244  output  ADDRESS_A      <15:00>  10`\
`  760246  output  ADDRESS_B      <21:16>  10`\
`  760250  output  DATA           <15:00>  11`\
`  760252  output  PARITY_HIGH    <0>      12`\
`  760254  output  PARITY_LOW     <0>      13`\
`  760256  output  PAR_ERR        <0>      14`\
`  760260  output  ADRS_ERR       <0>      15`\
`  760262  output  RUN            <0>      16`\
`  760264  output  PAUSE          <0>      17`\
`  760266  output  MASTER         <0>      18`\
`  760270  output  MMR0_MODE      <1:0>    19`\
`  760272  output  DATA_SPACE     <0>      20`\
`  760274  output  ADDRESSING_16  <0>      21`\
`  760276  output  ADDRESSING_18  <0>      22`\
`  760300  output  ADDRESSING_22  <0>      23`

 

### Hello Blinkenlight World

And here is the short-most program for the PDP11/70 panel. It just copies the state of 22 SR switches to the 22 ADDR LEDs.

\
` 1                                .title BBHELLO - Short QUniBone BlinkenBone Device Demo`\
` 2                                ; This program exercises a connected PDP11/70 panel.`\
` 3                                ; It copies the state of 22 SR switches to the 22 ADDR LEDs.`\
` 4`\
` 5                             ``   ``.asect`\
` 6`\
` 7 `` ``160200                          bbaddr = 160200         ; BlinkenBone device start addr in I/O page`\
` 8`\
` 9 000000                          . = 0`\
`10 000000 000137 001000            jmp     @#start         ; early emulation started code execution from 0`\
`11`\
`12 000024                          . = 24                  ; If not HALTed: start on power-up`\
`13 000024 001000                   .word   start           ; PC`\
`14 000026 000340                   .word   340             ; PSW with priority level 7`\
`15`\
`16 001000                          . = 1000`\
`17                             start:`\
`18 001000 012705 160200            mov     #bbaddr,r5      ; r5 = panel base addr`\
`19                             loop:`\
`20                                 ; for mapped registers offsets see 11/70 memory dump`\
`21 001004 016500 000014            mov     14(r5),r0       ; get SR switches <15:00>`\
`22 001010 016501 000016            mov     16(r5),r1       ; get SR switches <21:16>`\
`23 001014 010065 000044            mov     r0,44(r5),      ; set ADDR LEDs <15:00>`\
`24 001020 010165 000046            mov     r1,46(r5)       ; set ADDR LEDs <21:16>`\
`25 001024 000767                   br      loop`\
`26                                 ; shorter using PDP-11 address modes:`\
`27 001026 016565 000014 000044     mov     14(r5),44(r5)`\
`28 001034 016565 000016 000046     mov     16(r5),46(r5)`\
`29`\
`30 .end`
