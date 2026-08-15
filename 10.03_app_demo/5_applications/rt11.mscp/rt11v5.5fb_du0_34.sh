#!/root/10.03_app_demo/4_deploy/demo --verbose
# inputfile for demo to select a rl1 device in the "device test" menu.
d			# device menu

# en dl11		# use emulated serial console
# en kw11

pwr			# reboot PDP-11
.wait 3000		# wait for PDP-11 to reset
m i			# install max UNIBUS memory

# Deposit bootloader into memory
m ll ../bootloaders/du.lst

en uda			# enable UDA50 controller

# mount RT11 v5.5 in drive #0 and start
en uda0			# enable drive #0
sd uda0			# select
# set type to "RA80"
p type RA80
p image /var/lib/qunilator/images/du/rt11v5.5_34.ra80.dsk # mount image file with test pattern

# empty scratch disk in uda1:
en uda1			# enable drive #1
sd uda1			# select
p type RA80
p image /var/lib/qunilator/images/du/scratch1.ra80.dsk



.print MSCP drives ready.
.print UDA50 boot loader installed.
.print Start 10000 to boot from drive 0, 10010 for drive 1, ...
.print Reload with "m ll"



