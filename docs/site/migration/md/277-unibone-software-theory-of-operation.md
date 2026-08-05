<!-- migrated from https://retrocmp.com/projects/unibone/277-unibone-software-theory-of-operation -->

## Mission Impossible?

A UniBone application (emulated device, test logger, device tester, SimH) always consists of voluminuous C logic programs, plus a high speed signal processing core.

Target is to make a clean API to the signal processing PRUs, so end user only have to deal with their application in the future.

This is the UniBone mantra:

***The whole device emulation logic shall be done in Linux user processes.***

I first planned for a custom kernel module (aka device driver) to interface to the PRUs, but that's not necessary. However, you can always write a "/dev/unibus" kernel module, if you're a fan of additional software layers.

Even if the PRUs handle the [UNIBUS](http://www.pdp-11.nl/unibus-sys/unibus-intro.html) transactions in real-time, device emulation applications will be much slower. Although ARM code is executed at 1GHz, routing data through several abstraction layers can be slow.

### Interrupt latency and context switches

Ah yes: software people have their "technobabble" too!

Most critical is the latency between a PRU interrupt and the start of an associated Linux thread. This is called "context switch": Before the Linux process can handle the interrupt, it has to be activated by the kernel scheduler. And even if the process has highest priority on a realtime-kernel, this may take several hundreds of microseconds.

On the other hand, if a PDP-11 program writes into a controller command register, it expects a changed logic state (and changed status register values) on the next UNIBUS cycle.

See this possible (pseudo) code in a PDP-11 device driver:

        mov #1,cmd    ; set some bits in a controller command register
        tstb status   ; read out bits of the associated status registers
        bne error     ; not as expected: alert!
        ...           ; proceed

In this situation, the emulated device logic must process the "write cmd" event within one PDP-11 cycle, in about one microsecond. This includes interrupt wake-up time and also running the code to change the controller state ... which may be an uncalculable amount of work.

At first look, it seems impossible to implement UNIBUS cycle handling with Linux processes.

By the way: The problem of "interrupt-latency" is independent of the real-time signal processing layer. Even if the PRUs were replaced by FPGA logic, Linux processes could not be signaled fast enough.

### Parallelism with threads

Now to something (not so) completely different:

In an electronic device or in FPGA logic, many things run in parallel. Software on the other hand is sequential.\
To get a degree of parallelism, UniBone software makes heavy use of "threads". Threads are parallel running parts inside a program.\
All devices (emulated DEC devices or internal helpers) have a standard thread called "worker()", in which most of the function is coded. So all these devices run parallel.

Devices are implemented as state machines: The state of an emulated device is defined as a set of variables and UNIBUS register content.

A "worker()" thread is sleeping most of the time. It only woken by "events", then actual thread run time is short.\
Events changing a device state may be

- access of an UNIBUS register of a device
- change of parameters by user
- time controlled.

##  

## UNIBUS cycles and C++ devices: signal flow

Following is a more or less helpless try to visualize the interaction between various software components inside an UniBone emulation. Enjoy the mess!

When a PDP-11 CPU is writing data into an emulated controller register, a lot of things happen (and yes, this IS a simplified view):

![software signal path](images/software-signal-path.jpg)

The signal flow explained in detail:

**1.** PRU detects an UNIBUS DATO access. It compares the bus address with the addresses of known device registers.

**2**. If the PRU recognizes a register access to one of the emulated devices, he

- saves register address, the type of access (DATI/DATO) and (in case of DATO) the data in the PRU-ARM mailbox (2a).
- triggers an interrupt to the ARM (2b)

**3.** The interrupt is processed by the PRU helper driver "pruss_drv" and triggers an event.\
The helper object "UNIBUS adapter" is waiting for PRU events in his "worker()" thread.\
On event receive, the worker() gets active (3a) and reads info about the register access from the ARM-PRU mailbox (3b). From register info data its clear to which emulated device the accessed register belongs.

"UNIBUS adapter" is central, as it allows multiples devices to access the single PRU bus interface. It distributes DATI and DATO cycles to the correct device, and serializes parallel Interrupts and DMA requests from different controllers. It has 100% realtime (RT) priority, for shortest run time.

**4.** On DATI or DATO, "UNIBUS adapter" calls a special callback procedure of the emulated device.\
This callback is named "on_after_register_access()" and works a bit like an "Interrupt Service Routine" (ISR) for register accesses.\
The call may interrupt regular device operation. While "on_after_register_access()" is running, UNIBUS operation is blocked.

**5.** In "on_after_register_access()" the state variables of the device must be updated. Not all work is done here, but the UNIBUS device registers must get final new values, before  "on_after_register_access()" ends. This is done here by pseudo function "change_state_fast()".\
No long running operations are allowed in "on_after_register_access()": no locks, no waits, no disk accesses, no unbound while() loops.\
A typical operation would be: "set a BUSY bit in a status registers immediately after receiving a controller command".

**6.** If the register access triggers long running device activity (like the "GO" command for disk controllers, which causes head movement or\
reading data from the disk image file), then "on_after_register_access()" send a signal to the "worker()" thread of the device. This thread is still running at RT priority (to have priority against the rest of the linux system), but is not synchronized with UNIBUS DATI/DATO cycles (unlike on_after_register_access()).\
So timing is uncritical here.

**7,8.** When "on_after_register_access()" is terminated, the UNIBUS adapter tells the PRU to complete the UNIBUS cycle, unblocking the bus. Communication to the PRU is by a shared variable in the PRU-ARM mailbox.

**9.** The device performs lot of functions, which are not triggered by UNIBUS register access (spinning up a disk, moving tape, accessing data files, polling timeout conditions).\
Most of these are done in the device "worker()", and change also the device state and the device UNIBUS registers.\
For example, in a status register a "timeout" error bit may become active independet of any UNIBUS activity.

Also access to all other BeagleBone resources, especially the Linux file system (disk/tape image files), is done in worker().\
The emulated device can use abritray resources here: an emulated serial card would access UARTs, an emulated network card would use the ethernet interface,\
or hardware soldered onto the patch areas can be used. Thats the "Arduino-like" corner of UniBone.

**10.** the emulated device can run as hidden service, but most likely it will have an user interface (like the menus in "Demo" or a SimH-like command language).\
These programs also change the device state, mostly over "parameters" (see before). Some parameters are changed by operating an attached I²C panel.

**11,12.** After DATO, the PDP-11 CPU may execute a read (DATI) cycle on the controller immediately.\
Then changed controller register values are read.

 

## UNIBUS cycles and C++ devices: timing

UNIBUS is asynchronous: it does not operate with fixed timing, but implements a handshake between bus master and bus slave. This way slow and fast devices can be mixed on the same bus without sacrifying performance.

An UNIBUS data access cycle look like this:

![software msyn ssyn timing normal](images/software-msyn-ssyn-timing-normal.jpg)\
**a)** the BUS master sets up ADDRESS lines, CONTROL lines (wether the access is a read or a write), and - in case of write - DATA.

**b)** the bus master asserts the MSYN signal line to show that an access cycle starts.

**c)** all UNIBUS controller card listen on the bus and compare the address with their own position.

If one board wants to respond to a DATI (read), it fetches data from a register (or in case of memory, from a memory cell) and drives the DATA bus lines.\
In case of DATO, it reads the DATA lines and latches them into the correct internal register.

**d)** When the slave is done, it asserts SSYN.

**e)** If the cycle was DATI (read), the master fetches the DATA lines and processes the result.

**f)** if the bus master is ready, he removes his address and other data from the bus lines and sets MSYN inactive.

**g)** the slave sees that the cycle is terminated. It removes its data from the DATA lines and deactivates SSYN. The bus is idle again.

 

The typical timing for a whole bus cycle is 1 microsecond, the single phases are some hundreds of nanoseconds apart.

There is only one timing contraint: Bus timeout. After the master has signaled "b", he is waiting some time for one slave responding with "d". After that time he aborts the cycle, and signals a "bus timeout". (A PDP-11 CPU calls a trap, device controllers set a NXM status bit for Not eXisting Memory). This is a non-fatal exception, as not all addresses in the address range implement memory or devices.

The "bus timeout" period is typical 10 microseconds or longer. That means, the UniBone PRU has to respond to an address within that interval , "b - d"  can not be stretched.

You see where this is going?

If the bus master deactivates MSYN, normally the slave will lower SSYN almost immediately, within 100 nanoseconds or so. But the slave has endless time to complete the cycle.\
UniBone elongates the SSYN phase and uses the "f - g" time to do all the interrupting, thread wake-up and processing.

![software msyn ssyn timing delayed](images/software-msyn-ssyn-timing-delayed.jpg)

The line of colored blocks shows which software components are active in which phase of the bus cycle. The UniBone software runs on 4 different priority levels:

- PRU (real time resolution about 10-100 ns)
- 100% RT thread for register access (real time resolution about 500 to 2000 µsecs)
- cooperative RT workers (no realtime, but priority over Linux processes)
- non-RT application code (normal)

Eeeh, should've told you earlier: "RT" means "real-time" process in the Linux kernel.

##  

## Don't care for all that

You think all this is brain damaging complex? Well, it is. Even for me.

But the price for simple hardware is always complex software.

The good news: to implement a new device, only a single C++ class must be coded. It contains functions to

- setup UNIBUS register properties in PRU tables
- implement the "worker()" thread with most of logic
- implement "fast" logic in "on_after_register_access()"
- optionally implement a parameter interface.

The rest of the components shown here do their work in the dark.
