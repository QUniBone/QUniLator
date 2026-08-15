#!/root/10.03_app_demo/4_deploy/demo
# inputfile for demo to select a RD54 device in the "device" menu.
d			# device test menu

# first, make a serial port. Default is:
# sd dl11
# p p ttyS2		# use "UART2 connector
# en dl11
# en kw11

pwr
.wait 3000		# wait for PDP-11 to reset
# m i			# install max QBUS memory
#m i 1777776		# install max 512k QBUS memory

en uda			# enable UDA50 controller

# mount VMS7.3 in MSCP drive #0
en uda0			# enable drive #0
sd uda0			# select
p type RD54
# mount image
p image /var/lib/qunilator/images/du/vms073.rd54.dsk  # full install by Mark, many packets
p useimagesize	1

# mount test disk in MSCP drive #1
en uda1			# enable drive #1
sd uda1			# select
p type RA70
p image /var/lib/qunilator/images/du/test_du1.ra70.dsk      # mount image
p useimagesize	1


.print Disk drive now on track after 3 secs
.wait	3000		# wait until drive spins up

p                       # show all params

.print MSCP drives ready.
.print Set terminal to "8N1"
.print >>> BOOT DUA0
.print Username: SYSTEM, no password
