#!/root/10.03_app_demo/4_deploy/demo --verbose
# inputfile for demo to select a rl1 device in the "device test" menu.
d			# device menu

# first, make a serial port. Default ist
sd dl11
en dl11			# use emulated serial console
p p ttyS2		# use "UART2 connector, see FAQ
en kw11			# enable KW11 on DL11-W

pwr			# reboot PDP-11
.wait 3000		# wait for PDP-11 to reset
# The processor first: enabling it makes the board the bus owner, which is
# what the memory sizing below needs on a backplane with no CPU of its own.
en cpu20

m i			# install max UNIBUS memory

# Deposit bootloader into memory
m ll ../bootloaders/dl.lst

en rl			# enable RL11 controller

# mount RT11 v5.3 in RL02 #0 and start
en rl0			# enable drive #0
sd rl0			# select
p emulation_speed 10	# 10x speed. Load disk in 5 seconds
# set type to "rl02"
p runstopbutton 0	# released: "LOAD"
p powerswitch 1		# power on, now in "load" state
p image /var/lib/qunilator/images/dl/xxdp25.rl02.dsk	# mount image
p shared_filesystem XXDP # now switch to shared
p shared_dir xxdp_shared_rl0
p runstopbutton 1	# press RUN/STOP, will start
p v 4 # debug output

en rl1			# enable drive #1
sd rl1			# select
p emulation_speed 10	# 10x speed. Load disk in 5 seconds
# set type to "rl02"
p runstopbutton 0	# released: "LOAD"
p powerswitch 1		# power on, now in "load" state
p image /var/lib/qunilator/images/dl/xxdp_test1.rl02.dsk
p shared_filesystem XXDP # now switch to shared
p shared_dir xxdp_shared_rl1
p runstopbutton 1	# press RUN/STOP, will open shared dir and start
p v 4 # debug output


.print Disk drive now on track after 5 secs
.wait	6000		# wait until drive spins up
p                       # show all params of RL1



sd cpu20
p pmi 1

.print RL drives ready.
.print RL11 boot loader installed.
.print Emulated PDP-11/20 CPU will now boot RT11.
.print Serial I/O on simulated DL11 at 177650, RS232 port is UART2.
.print Start 10000 to boot from drive 0, 10010 for drive 1, ...
.print Reload with "m ll"
.print Start CPU20 by toggeling CONT switch with "p c 1"



