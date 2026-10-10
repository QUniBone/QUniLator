#!/usr/bin/qbone-cli
# inputfile for the menu program to select a rl1 device in the "device test" menu.
d			# device test menu

# first, make a serial port. Default is:
# sd dl11
# p p ttyS2		# use "UART2 connector
# en dl11
# en kw11

pwr
.wait 3000		# wait for PDP-11 to reset
m i			# install max QBUS memory
#m i 1777776		# install max 512k QBUS memory

# Deposit bootloader into memory
m ll ../bootloaders/du.lst

en uda			# enable UDA50 controller

# mount RSX in MSCP drive #0
en uda0			# enable drive #0
sd uda0			# select
p type RA70
# mount image
#p image /var/lib/qunilator/images/du/JH_DU0.ra70.dsk  # full install by Mark, many packets
p image /var/lib/qunilator/images/du/rsx11mpbl87.ra70.dsk  # full install by Mark, many packets
p useimagesize	1

# mount test disk in MSCP drive #1
en uda1			# enable drive #1
sd uda1			# select
p type RA70
p image /var/lib/qunilator/images/du/JH_DU1.ra70.dsk      # mount image
p useimagesize	1


en rl			# enable RL11 controller

# mount RSX v4.16 in RL02 #0 and start
en rl0			# enable drive #0
sd rl0			# select
p emulation_speed 10	# 10x speed. Load disk in 5 seconds
p type rl02
p runstopbutton 0	# released: "LOAD"
p powerswitch 1		# power on, now in "load" state
p image /var/lib/qunilator/images/dl/rsxm70.rl02.dsk  # mount image
p runstopbutton 1	# press RUN/STOP, will start

# mount DL1 in RL02 #1 and start
en rl1			# enable drive #1
sd rl1			# select
p emulation_speed 10	# 10x speed. Load disk in 5 seconds
p type rl02
p runstopbutton 0	# released: "LOAD"
p powerswitch 1		# power on, now in "load" state
p image /var/lib/qunilator/images/dl/rsxdl1.rl02.dsk         # mount image
p runstopbutton 1	# press RUN/STOP, will start



.print Disk drive now on track after 5 secs
.wait	5000		# wait until drive spins up

p                       # show all params

.print MSCP drives ready.
.print UDA50 boot loader installed.
.print Start 10000 to boot from drive 0, 10010 for drive 1, ...
.print Reload with "m ll"
