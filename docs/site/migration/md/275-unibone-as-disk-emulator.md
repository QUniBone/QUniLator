<!-- migrated from https://retrocmp.com/projects/unibone/275-unibone-as-disk-emulator -->

Main use of UniBone is: emulate devices for a PDP-11s.

More precise, UniBone is *not* intended as emulator for a single specific device. The key concept here is

It was planned as *environment* to implement arbitrary devices.

This goal leads to features like

- Cheap and easy-to-build hardware.
- Standard programming environment: C/C++ under Linux. No microcontrollers, no FPGAs, no VHDL, no extra tools.
- Layered software: complicated system logic is hidden in the PRUs and in C++ helper classes.
- documented SDK (oh yes, that later!)
- patch field areas for own hardware interfaces.

Thats a challenging project ... and implementing all possible devices may keep many crafted programmers busy. Anyway, the existing RL02 emulation is a good starting point.

## Personal motivation

I had several reasons to start this UniBone-adventure:

- It all began with the desire to show my small PDP-11/05 on [VCF-shows](http://vcfed.org/wp/festivals/), but without hauling a whole rack with extra peripheral boxes and disk drives.
- I also felt the need to have a rock solid solution to boot the XXDP diagnostic system on various machines.
- I wanted a fast way to move SimH disk images to real machines (after having build the [PDP11GUI disk access](/pdp-11/pdp11gui/disk-images-readwrite) and the [file sharing TU58 emulation](/tools/tu58fs)).
- My RL02 drives are getting unreliable. Maybe because in typical repair session I started und stopped them 100s of times. And I'm too bored to repair them again.
- I wanted to see what these BeagleBone-PRU units are good for.
- Nobody did it so far !

 ![pdp1105 panel](images/pdp1105-panel.jpg)

## What is emulated?

Emulation is always at UNIBUS level, UniBone implements always an UNIBUS controller card AND the attached devices.

At the moment, we have a RL11 controller with 4 RL01/02 disk drives in UniBone.

![emulation](images/emulation.jpg)

 

## The RL11 / RL02 subsystem

The RL02 drives used changeable cartridges with 10MB capacity, the RL01 had 5MB. It was a DEC standard disk drive in the late 1970s. Small single-user machines run from one or two RL02s, bigger installations used them for data exchange.

![rl02 drive](images/rl02-drive.jpg)

We had one on the VCFB 2018 in Berlin:

![rl02 drive vcfb](images/rl02-drive-vcfb.jpg)

 

## Using the RL02

I made the emulated RL drives 100% compatible, including timing. Partly because this is necessary to pass the DEC "ZRL\*" diagnostics, partly because of retro-fun.

So bringing the RL02 online involves some manual interaction, just as with the original.

Lets see how an RL drive is brought on-line: Here is a walk-through with the preliminary "Demo" menu software.

### 1. Start "Demo" and enter the drive menu

![rl02 startup 1](images/rl02-startup-1.jpg)

###  2. Select a device

There are several "devices" in UniBone. The RL11 controller is one, as well as the 4 RL drives. And there are internal devices too.

Here the RL02 drive with unit number \#1 is "selected". After that more operations on the current device are possible.

 ![rl02 startup 2](images/rl02-startup-2.jpg)

### 3. Device parameters

Every device has lots of "parameters". These are name-value pairs, visible to the user. Most are compiled-in, some are information about the device state, some are user settable.

Switches and lamps on the RL02 drive are also "parameters", these can also be set over the panel.

 

 ![rl02 startup 3](images/rl02-startup-3.jpg)

We first enable diagnostic output (verbosity=4) and speed up simulated operation a bit (emulation speed = 10). Else the drive would need 25 seconds to spin up. That's fun the first time, but gets annoying later.

Then we need to operate the RL02 drive:

1.  Power it on.
2.  set the name of the image file ("put a cartridge into the drive"). If the file does not exist, it is created. So beware of typos!

### 4. LOAD the cartridge

By pressing the LOAD button (also called "run-stop-button") the drive is spun up, then it goes READY:

##  ![rl02 startup 4](images/rl02-startup-4.jpg)

Notice several parameters changed: The rotational speed is now 2400 rpm. The drive state is now 5 (Drive READY). The Run-Stop-Button remains in the "down" position. the "readylamp" is ON, as the drive heads are out and "on-track".

### 5. Setting write protection

There's also this WRITE PROT button. Lets protect the image file in drive \#1:

## ![rl02 startup 5](images/rl02-startup-5.jpg)

##  

This "power up" sequence is as cumbersome as on a real RL02 drive. Including the strange minute after pressing LOAD, when all lights are off and you do not know what the drive is doing !

So I made the spin-up optionally faster, and added crude scripting support for the "Demo" application. Now 4 RL drives are usable without manual intervention ... is this "cheating"?

Anyhow: bring up an array of RL02 drives is now just done by calling a single shell script.

![rl02 boot rt11 advent](images/rl02-boot-rt11-advent.jpg)

 And you can boot RT-11 from it!

## The Panel

You've read bravely through all the screen shots above, so here is some reward: the control panel for all RL02 drives.

![rl02 panel tilted](images/rl02-panel-tilted.jpg)

In UniBone-speech a panel is just a physical way to show and change parameters, as described above. Its use is optional, the RL02s will work without. But why should they?

Technically a panel is controlled over the I²C bus. Two MC23017 GPIO extender chips provide the 24 ON/OFF signals  for 16 lamps and 8 buttons.

Up to 4 of these panels can be daisy-chained, so other drive emulations may have there own ones too.

 

## Conformance testing with XXDP diagnostics

DEC made lots of diagnostics for their device, which all are awailable now. Every device emulation should pass these tests. See here [how to run them](/how-tos/using-pdp-11-diagnostics).

These diagnostics are brutal. They test not just functional behaviour, as decribed in the user and programmer's manuals. Additional they exercise error cases, and check timing conditions for head movement and rotational speed of sectors on rotating platters. Also timing of data transmission and interrupt delay is checked.

Here UniBone has a harder job than pure software emulators like SimH. In SimH and the like, the emulator itself is generating its internal clock, while UniBone has to respond to the actual timing forced by the running PDP-11.

Emulating the RL11/RL02 subsystem took longer than building the rest of UniBone. I needed several months to get my implementation forged in the eternal fires of DECs ZRLG, ZRLH, ZRLI, ZRLJ, ZRLN and ZRLK diagnostic suite, and its still not passing 100%.
