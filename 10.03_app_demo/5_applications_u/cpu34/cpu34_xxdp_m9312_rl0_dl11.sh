#!/root/10.03_app_demo/4_deploy/demo --verbose
# inputfile for demo to select a rl1 device in the "device test" menu.
d			# "device + cpu" test menu

# first, make a serial port.
sd dl11
p p ttyS2		# use "UART2 connector, see FAQ
en dl11			# use emulated serial console
en kw11			# enable KW11 on DL11-W

sd m9312		# further commands form M9312
p bl DIAG		# set start label to console emulator entry 765020
p cer ../bootloaders/23-248F1.lst	# plug console emulator ROM into socket
p br1 23-756A9.lst	# plug RK BOOT ROM into socket1
p br2 23-751A9.lst	# plug RL BOOT ROM into socket2
p br3 23-767A9.lst	# plug MSCP DU BOOT ROM into socket3
p
en m9312		# online

# The processor first: enabling it makes the board the bus owner, which is
# what the memory sizing below needs on a backplane with no CPU of its own.
en cpu34

m i			# install max UNIBUS memory

# Deposit "serial echo" program into memory
m ll serial.lst

en rl			# enable RL11 controller

# mount XXDP disk in RL02 #0 and start
en rl0			# enable drive #0
sd rl0			# select
p emulation_speed 10	# 10x speed. Load disk in 5 seconds
# set type to "rl02"
p runstopbutton 0	# released: "LOAD"
p powerswitch 1		# power on, now in "load" state
p image /var/lib/qunilator/images/dl/xxdp25.rl02.dsk  	# mount image file with test pattern
p runstopbutton 1	# press RUN/STOP, will start

.print Disk drive now on track after 5 secs
.wait	5000		# wait until drive spins up
p                       # show all params of RL1

sd cpu34
p h 0			# release HALT switch

pwr
# Do not issue DMA-like accesses for 300ms, M9312 manipulates ADDR lines after ACLO


.print M9312 boot rom installed.
.print "Serial echo" program loaded at memory 1000.
.print Serial I/O on simulated DL11 at 177650, RS232 port is UART2.
.print RL drives ready.
.print Emulated PDP-11/34 CPU will booted into M9312 console emulator.
.print Make sure physical CPU is disabled.
.print Start "Serial echo" with "@L 1000", "@S"
.print Boot from RL0 into XXDP with "@DL0"

