#!/usr/bin/unibone-cli
# inputfile for the menu program to select a rl1 device in the "device test" menu.
d			# device test menu
pwr
.wait 3000		# wait for PDP-11 to reset
m i			# install max UNIBUS memory

# Deposit bootloader into memory
m ll ../bootloaders/du.lst

en uda			# enable UDA50 controller

# mount RSX in MSCP drive #0
en uda0			# enable drive #0
sd uda0			# select
p type RA70
p image /var/lib/qunilator/images/du/rsx11m_4_8_bl70.ra70.dsk  # mount image
p useimagesize	1

# mount test disk in MSCP drive #1
en uda1			# enable drive #1
sd uda1			# select
p type RA70
p image /var/lib/qunilator/images/du/rsxm70.ra70.dsk      # mount image
p useimagesize	1


.print Disk drive now on track after 5 secs
.wait	5000		# wait until drive spins up

p                       # show all params

.print MSCP drives ready.
.print UDA50 boot loader installed.
.print Start 10000 to boot from drive 0, 10010 for drive 1, ...
.print Reload with "m ll"
