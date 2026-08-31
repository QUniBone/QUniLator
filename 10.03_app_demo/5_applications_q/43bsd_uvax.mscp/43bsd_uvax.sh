#!/usr/bin/qbone-cli
# inputfile for the menu program to select a RD54 device in the "device" menu.
d			# device test menu

pwr
.wait 3000		# wait for PDP-11 to reset
# m i			# install max QBUS memory
#m i 1777776		# install max 512k QBUS memory

sd uda
p iv 154
#qp il 5
en uda			# enable UDA50 controller

# mount ULTRIX2 in MSCP drive #0
en uda0			# enable drive #0
sd uda0			# select
p type RA82
p image /var/lib/qunilator/images/du/quas.ra82.dsk
p useimagesize	1


p                       # show all params

.print 4.3 BSD Quasijarus
.print See https://gunkies.org/wiki/Installing_4.3_BSD_Quasijarus_on_SIMH
.print MSCP drives ready.
.print Set terminal to 7E1
.print Root account without password.
