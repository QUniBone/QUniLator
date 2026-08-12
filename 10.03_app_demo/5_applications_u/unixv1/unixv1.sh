#!/root/10.03_app_demo/4_deploy/demo --verbose
# Inputfile for UniBone "demo" to run UNIX V1 on physical PDP-11/20.
# RF11/RS11 DECdisk, RK05 disc, memory and KE11 EAE are emulated.
# (C) Josh Dersch <derschjo@gmail.com>

d			# device test menu

#pwr
 .wait 3000		# wait for PDP-11 to reset
m i			# install max UNIBUS memory

en ke                   # enable KE11-A EAE
en rf                   # enable RF11 fixed-head disk controller
en rs0                  # enable RS11 disk
sd rs0
p image ../diskimages/unixv1_rs0.rs11.dsk         # mount image

en rk			# enable RK11 controller
en rk0			# enable drive #0
sd rk0			# select
p image ../diskimages/unixv1_rk0.rk05.dsk 	# mount image

# poke bootstrap into memory
D 73700 012700
D 73702 177472
D 73704 012740
D 73706 000003
D 73710 012740
D 73712 140000
D 73714 012740
D 73716 054000
D 73720 012740
D 73722 176000
D 73724 012740
D 73726 000005
D 73730 105710
D 73732 002376
D 73734 000137
D 73736 054000
D 73740 012700
D 73742 177350
D 73744 005040
D 73746 010040
D 73750 012740
D 73752 000003
D 73754 105710
D 73756 002376
D 73760 005737
D 73762 177350
D 73764 001377
D 73766 112710
D 73770 000005
D 73772 105710
D 73774 002376
D 73776 005007

.print RF11 bootstrap is in memory at 73700.
.print The RF11 bootloader loads and starts the "Bootstrap Operating System" ("bos"),
.print whichs has severals functions depending on console switches.
.print SR=173700 will read "Warm UNIX" from RF core location 0 and transfer to 400.
.print See Unix V1 manual BOOT PROCEDURES (VII) 11/3/71
.print Unix V1 restoration project at https://github.com/jserv/unix-v1
.print Manuals at https://www.bell-labs.com/usr/dmr/www/1stEdman.html
.print Disk images here have been patched to run without DC11 serial.
.print
.print Load address 73700 into PDP-11 via front panel,
.print Then set the Switch Register to 173700 and start the processor.
.print Set terminal to "<baudrate> 7O1"
.print Login as "root"
