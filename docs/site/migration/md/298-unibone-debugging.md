<!-- migrated from https://retrocmp.com/projects/unibone/298-unibone-debugging -->

From painful experience you will know: A hardware/software system is only as good as its debugging tools.

And investing time into debugging tools is always a good idea: you will spend so much time in debugging, you deserve some comfort.

Therefore UniBone contains several mechanisms to diagnose whats its doing.

In contrast to regular "desktop" software, embedded code needs to run in strict timing, which may not be changed by the debugging process. And timing relations (in sub-microsecond resolution) must be made visible. So direct debug-printouts ("printf()") or Breakpoints are of minor use.

Non-invasive debugging on embedded devices like UniBone uses GPIO pins, whose levels are changed by software under certain conditions. Signals are then captured with an logic analyzer and evaluated independet of the running device.

\*\*\*

UniBone software is distributed over 2 processors: the **ARM** is executing Linux and does all the high level emulation, while **PRU** is low level real-time UNIBUS interface.

\*\*\*

## PRU debugging: GPIO only

The PRU runs independently from main Linux ARM (thats the idea!). It communicates with the normal Linux program over a mailbox and is not directly reachable for Debugging. Regular debugging tools like "printf()" diagnostic or Eclipse based symbolic debugging are not available. The PRU has its own set of debugging tools, but I found it easier to rely totally on "GPIO debugging".

Debugging via GPIO means: connect a logic analyzer to some of the General Purpose IO pins, and trigger these pins under software control. Its like a high speed binary "printf()" on hardware level. The big advantage here is that we see the timing relations between hardware activity and program flow, in sub-microsecond resolution.

### Watching the PRU register bus

The PRU has not enough pins to read and write 56 UNIBUS signals, so an GPIO multiplier is used with 8 read registers, 8 write registersand a REG_WRITE signal. So UniBone has a local data bus with 8 DATAIN, 8 DATAOUT, 3 ADDRESS and 1 WRITE signal. For debugging purposes it is exposed on pin headers:

![debugging pru pins](images/debugging-pru-pins.jpg)

Under C the PRU register latches are accessed by function calls like

`buslatches_setbits(...) ;`\
`buslatches_setbyte(...) ;`\
`buslatches_getbyte(...) ;`

By monitoring the accesses to these gpio-registers it easy to follow PRUs program flow.

The relation between UNIBUS signals and buslatches and latch pins is given in the circuit schematic.

### PRU debug GPIO pin(s)

Additionally one unused PRU GPIO "1_12" can generated debug signals. You do that by inserting the expression "PRU_DEBUG_PIN0(1)" in your code.

For performance reasons "1_12" is automatically when PRU accesses its register bus.

A second GPIO "1_13" needs a modified BeagleBone to be usable.

## PRU Example debugging session

Here is a real-world PRU debugging session. Problem was: the RL11 device emulator had a buggy DMA protocol: it set NPR, received NPG, but then did not raised SACK.

Apparently this logic expression in was wrong, but which term of it?\
`     grant_mask &= (sm_arb.device_request_mask & ~sm_arb.device_forwarded_grant_mask);`

I peppered the code in pru1_statemachine_arbitration.c with PRU_DEBUG_PIN0() statements and watched "GPIO 1_12".\

    Code from pru1_statemachine_arbitration.c

           ...
    144: uint8_t sm_arb_worker_device(uint8_t grant_mask) {
           ...
    174:    // Always update UNIBUS BR/NPR lines, are ORed with requests from other devices.
    175:    buslatches_setbits(1, PRIORITY_ARBITRATION_BIT_MASK, sm_arb.device_request_mask) ;
    176:    // now relevant for GRANt forwarding
    177:    sm_arb.device_request_signalled_mask = sm_arb.device_request_mask;
    178:
    179:    // read GRANT IN lines from CPU (Arbitrator).
    180:    // Only one bit on cpu_grant_mask at a time may be active, else arbitrator malfunction.
    181:    // Arbitrator asserts SACK is inactive
    182:
    183:    if (sm_arb.grant_bbsy_ssyn_wait_grant_mask == 0) {
    184:        PRU_DEBUG_PIN0(0); PRU_DEBUG_PIN0(0); PRU_DEBUG_PIN0(0);
    185:        // State 1: Wait For GRANT:
    186:        // process the requested grant for an open requests.
    187:        // "A device may not accept a grant (assert SACK) after it passes the grant"
    188:        if (grant_mask) PRU_DEBUG_PIN0_PULSE(50) ; else PRU_DEBUG_PIN0_PULSE(10) ;
    189:        PRU_DEBUG_PIN0(0); PRU_DEBUG_PIN0(0); PRU_DEBUG_PIN0(0);
    190:        if (grant_mask & sm_arb.device_request_mask) PRU_DEBUG_PIN0_PULSE(50) else PRU_DEBUG_PIN0_PULSE(10) ;
    191:        PRU_DEBUG_PIN0(0); PRU_DEBUG_PIN0(0); PRU_DEBUG_PIN0(0);
    192:        if (grant_mask & sm_arb.device_request_mask & ~sm_arb.device_forwarded_grant_mask)
    193:            PRU_DEBUG_PIN0_PULSE(50) ; else    PRU_DEBUG_PIN0_PULSE(10) ;
    194:
    195:        grant_mask &= (sm_arb.device_request_mask & ~sm_arb.device_forwarded_grant_mask);
    196:        if (grant_mask) {
    197:            PRU_DEBUG_PIN1(1);
    198:            // one of our requests was granted:
        ...

### Commented  Logic analyzer output:

Time resolution is 200MHz,

Channels are:

- "PRU_DBG0" is GPIO 1_12 / PRU_DEBUG_PIN0();
- REG_SEL, REG_WRITE. REG_DATOUT and REG_DATIN are the local PRU latch bus.
- NPR, SACK, BR4,5,6,7 etc are UNIBUS arbitration signals
- BBSYS, SMYN, etc are UNIBUS data signals (no activity here).
- ARM_DBG0,1,2,3 are GPIO pins used for ARM debugging., see below (not used here).

This subset of UNIBUS signals is of interest here:

- latch 1, bit \#4 (mask 0x10): NPR
- latch 0: GRANT INs.  BG4IN = bit \#0, BR5 = bit \#1, ... NPGIN = bit \#4.
- latch 4, bit \#4: MSYN
- latch 7: INTR, INIT, ACLO DCLO.

Comments in the LA screen shot are mostly line numbers in the code listing above.

![debugging pru la](images/debugging-pru-la.jpg)

 

You can follow the PRU signal processing loop by watching the  REG\_\* signals. Here:

[TABLE]

 

## PRU GPIOs enhance LA trigger logic

PRU debugging features are not jsut useful to debug PRU code itself. They also can be used to enhance the trigger capabilites of the logic anylzer.

"Triggering" means to generate an print-out or an logic-analyzer trace just for the error situation.

Bringing reliability from 99% to 99.9% is as much work as going from 50% to 90% and from 90% to 99%, hence the "[90:90](https://en.wikipedia.org/wiki/Ninety-ninety_rule)" rule (which better should be called "90:90:90:..."). When debugging for while, remaining errors gets more erratic and complicated than at first "power-on". Then setting up trigger conditions for a logic analyzer can be cumbersome or even impossible, as pre-history of many other conditions may be involved.

As UniBone is written in C/C++, its easy to write complex trigger conditions in C and make the final "trigger/no-trigger" condition visible via  ARM/PRI_DEBUG_PIN GPIO pin.

"C code trigger conditions" can also be used to analyze behaviour of the PDP-11 itself: in PRU code access to UNIBUS addresses and DATA can be evaluated and converted to trigger conditions.

For example, triggering on UNIBUS cycles for a given address range (hre: DL11 console) may be as easy as

    sm_data_slave_start() {
    ...
    if (addr >= 0777560 && addr <= 0777566) 
        PRU_DEBUG_PIN0(1) ; // trigger to LA.

Luckily recompiling and restarting the PRU software is quite fast.

 

## ARM debugging: LEDs and GPIO signals

ARM code (the regular Linux application compiled with "gcc") can also toggle GPIO pins for realtime debugging.

Everything described for the PRU GPIOs also applies here, especially the possibility to generate logic analyzer trigger signals.

All of the BeagleBone GPIOs can be used by ARM code, four of them are preconfigured. The code macros are ARM_DEBUG_PIN0 ... ARM_DEBUG_PIN3, which also drive the LEDs.

![debugging arm pins](images/debugging-arm-pins.jpg)

 

The BeagleBone exposes tons of GPIOs, some already routed out to additional pinheaders. To make these accessible, just generate ARM_DEBUG_PIN\*, by copy&pasting existing code.

## ARM debugging: device message printing

ARM code can be debugged like any other Linux-program with a symbolic debugger. UniBone has no graphical desktop, but development and debugging can be done on a remote PC, I use a cross-compile toolchain under Eclipse.

The emulator software is constructed from many intern "devices", which run on parallel threads.

Debugging multi-threaded applications is a challenge, as the most subtle errors occur from timing relations between different threads. Keywords  here: [race condition](https://en.wikipedia.org/wiki/Race_condition) & [Heisenbug](https://en.wikipedia.org/wiki/Heisenbug). Not funny!

To allow debugging with least timing impact, two additional features were build into UniBone:

- Each device can print messages into a global **trace buffer**. The buffer can later be dumped to a file or console. Saving messages into a buffer has much less impact than direct printout.\
  To speed things up, the buffer entries contain just the raw data for delayed printf() formatting.\
  \

&nbsp;

- Debug output **verbosity** can be fine tuned on a **per-device**. Message **severity** and device verbosity. In C++ code devices printed debug output with FATAL,WARNING,ERROR,INFO and DEBUG macros. Each device has a "verbosity level" which specifies which Macros to ignore. The "verbosity level" can be changed any time during runtime.\
  \

These severity levels exist:

[TABLE]

 Every C++ object derived from class "logsource_c" can use this message system.

## ARM debugging: message printing example session

Lets say we debug the RL02 drive emulator (just the drive, not the RL11 controller).\
Operation is shown for "demo" control program. Steps are:

1.  Instrumenting the code: add DEBUG() statements
2.  Test preparation: We set "verbosity" of the drive to "DEBUG" (level 5) and clear the debug log.
3.  Run a test: for debugging we watch drive activity while listing a small text file.
4.  Save results: printout the log buffer.

### Instrumenting the code

Debug print statements are inserted on several code lines in source "rl0102.cpp":

        DEBUG("Drive start seek from cyl.head %d.%d to %d.%d", cylinder, head, 
            destination_cylinder, destination_head);
        ...
        DEBUG("Change drive %s state from %d to %d. Status word %06o -> %06o.",
            name.value.c_str(), old_state, state.value, old_status_word, status_word);
        ...
        DEBUG("Seek: head switch to %d", head);
        ...
        DEBUG("drive seeking outward, cyl = %d", cylinder);
        ...
        DEBUG("File Read 0x%x words from c/h/s=%d/%d/%d, file pos=0x%llx, words = %06o, %06o, ...",
            sector_size_bytes / 2, cylinder, head, sectorno, offset, (unsigned )(buffer[0]),
            (unsigned )(buffer[1]));

The DEBUG() statements remain in the code. They are only enabled if device "verbosity" is "DEBUG".

### Test preparation

Setup the "rl0" device in "demo":

    >>>sd rl0
    Current device is "rl0"
    Controller base address = 774400
    *** Test of device parameter interface and states.
        "UniBone devices are clients to PDP-11 CPU doing NPR/INTR Arbitrator
        (CPU active, console processor inactive).
        CPU is physical or emulated.
        Memory access as Bus Master with NPR/NPG/SACK handshake.
        Current device is "rl0"
        UNIBUS unibuscontroller base address = 774400
        UNIBUS memory emulated from 000000 to 757776.
    m i                  Install (emulate) max UNIBUS memory
    m f [word]           Fill UNIBUS memory (with 0 or other octal value)
    m d                  Dump UNIBUS memory to disk
    m ll <filename>      Load memory content from MACRO-11 listing file (boot loader)
    m ll             Reload last memory content from file "dl.lst"
    m lp <filename>      Load memory content from absolute papertape image
    m lp                 Reload last memory content from file "dl.lst"
    ld                   List all defined devices
    en <dev>             Enable a device
    dis <dev>            Disable device
    sd <dev>             Select "current device"
    p <param> <val>      Set parameter value of current device
    p <param>            Get parameter value of current device
    p panel              Force parameter update from panel
    p                    Show all parameter of current device
    d <regname> <val>    Deposit octal value into named device register
    e <regname>          Examine single device register (regno decimal)
    e                    Examine all device registers
    e <addr>             Examine octal UNIBUS address.
    d <addr> <val>       Deposit octal val into UNIBUS address.
    dbg c|s|f            Debug log: Clear, Show on console, dump to File.
                           (file = unibone.log.csv)
    init                 Pulse UNIBUS INIT
    pwr                  Simulate UNIBUS power cycle (ACLO/DCLO)
    q                    Quit
    >>>
    >>>p v 5

    Name       Short  Value  Unit  Access    Info                                                  
    ---------  -----  -----  ----  --------  ------------------------------------------------------
    verbosity  v      5            writable  1 = fatal, 2 = error, 3 = warning, 4 = info, 5 = debug
     
    >>> p

    Parameters of device rl0:
    Name                Short  Value                   Info                                                    
    ------------------  -----  ----------------------  --------------------------------------------------------
    name                name   rl0                     Unique identifier of device                             
    type                type   RL02                    Type                                                    
    enabled             en     1                       device installed and ready to use?                      
    emulation_speed     es     10                      1 = original speed, > 1: mechanics is this factor faster
    verbosity           v      5                       1 = fatal, 2 = error, 3 = warning, 4 = info, 5 = debug  
    unit                unit   0                       Unit # of drive                                         
    capacity            cap    10485760                Storage capacity                                        
    image               img    rt11v5.5_games_34.rl02  Path to image file                                      
    rotation            rot    2400                    Current speed of disk                                   
    state               st     5                       Internal state                                          
    powerswitch         pwr    1                       State of POWER switch                                   
    runstopbutton       rb     1                       State of RUN/STOP button                                
    loadlamp            ll     0                       State of LOAD lamp                                      
    readylamp           rl     1                       State of READY lamp                                     
    faultlamp           fl     0                       State of FAULT lamp                                     
    writeprotectlamp    wpl    0                       State of WRITE PROTECT lamp                             
    writeprotectbutton  wpb    0                       Writeprotect button pressed                             
    coveropen           co     0                       1, if RL cover is open
    >>>dbg c
    Debug log cleared.
    >>>

### Run test

We boot the PDP-11 to RT11 from that drive. To exercise the drive under RT11, we list a small textfile then:

    .type starts.com
    ind datime
    !.MODULE STARTS,03,<BL/SJ Startup command file)
    !
    ! Select the editor of your choice. Take the default (KED) unless you are
    ! not using a VT100 compatible terminal.  If you are using an incompatible
    ! terminal (or a hard copy terminal) select the following command.
    !
    !SET EDIT EDIT
    !
    ! Get the revision levels of your MU's, if you have them
    !
    !R MSCPCK
    set tt scope
    set edit ked

 

The RL02 drive now has a lot to do: head positioning and transfering sector data via DMA.

### Inspect results

Now we display the buffered message onto UniBone's "demo" console:

    >>>dbg s

    [07:26:27.568355 Dbg    rl0 This email address is being protected from spambots. You need JavaScript enabled to view it.:0153] Drive start seek from cyl.head 15.1 to 16.1
    [07:26:27.568380 Dbg    rl0 This email address is being protected from spambots. You need JavaScript enabled to view it.:0178] Change drive rl0 state from 5 to 4. Status word 000235 -> 000234.
    [07:26:27.574092 Dbg    rl0 This email address is being protected from spambots. You need JavaScript enabled to view it.:0332] Seek: head switch to 1
    [07:26:27.574105 Dbg    rl0 This email address is being protected from spambots. You need JavaScript enabled to view it.:0340] drive seeking outward, cyl = 15
    [07:26:27.574110 Dbg    rl0 This email address is being protected from spambots. You need JavaScript enabled to view it.:0348] drive seek outwards complete, cyl = 16
    [07:26:27.574141 Dbg    rl0 This email address is being protected from spambots. You need JavaScript enabled to view it.:0178] Change drive rl0 state from 4 to 5. Status word 000334 -> 000335.
    [07:26:27.574543 Dbg    rl0 This email address is being protected from spambots. You need JavaScript enabled to view it.:0583] File Read 0x80 words from c/h/s=16/1/2, file pos=0x7700000000, words = 001124, 000020, ...
    [07:26:27.575054 Dbg    rl0 This email address is being protected from spambots. You need JavaScript enabled to view it.:0583] File Read 0x80 words from c/h/s=16/1/3, file pos=0x50000000000, words = 000104, 000020, ...
    [07:26:27.575617 Dbg    rl0 This email address is being protected from spambots. You need JavaScript enabled to view it.:0583] File Read 0x80 words from c/h/s=16/1/4, file pos=0x21200000000, words = 050513, 000020, ...
    [07:26:27.576152 Dbg    rl0 This email address is being protected from spambots. You need JavaScript enabled to view it.:0583] File Read 0x80 words from c/h/s=16/1/5, file pos=0xffe000000000, words = 122700, 000020, ...
    [07:26:28.108880 Dbg    rl0 This email address is being protected from spambots. You need JavaScript enabled to view it.:0583] File Read 0x80 words from c/h/s=16/1/34, file pos=0x35df00000000, words = 020000, 000020, ...
    [07:26:28.109377 Dbg    rl0 This email address is being protected from spambots. You need JavaScript enabled to view it.:0583] File Read 0x80 words from c/h/s=16/1/35, file pos=0x11c000000000, words = 062700, 000020, ...
    [07:26:28.109831 Dbg    rl0 This email address is being protected from spambots. You need JavaScript enabled to view it.:0583] File Read 0x80 words from c/h/s=16/1/36, file pos=0x2000000000, words = 012605, 000020, ...
    [07:26:28.110295 Dbg    rl0 This email address is being protected from spambots. You need JavaScript enabled to view it.:0583] File Read 0x80 words from c/h/s=16/1/37, file pos=0xe2c000000000, words = 103375, 000020, ...
    [07:26:28.128894 Dbg    rl0 This email address is being protected from spambots. You need JavaScript enabled to view it.:0153] Drive start seek from cyl.head 16.1 to 0.0
    [07:26:28.128908 Dbg    rl0 This email address is being protected from spambots. You need JavaScript enabled to view it.:0178] Change drive rl0 state from 5 to 4. Status word 000235 -> 000234.
    [07:26:28.134829 Dbg    rl0 This email address is being protected from spambots. You need JavaScript enabled to view it.:0332] Seek: head switch to 0
    [07:26:28.134839 Dbg    rl0 This email address is being protected from spambots. You need JavaScript enabled to view it.:0361] drive seek inwards complete, cyl = 0

    ... and so on ...

    >>>
