#!/root/10.03_app_demo/4_deploy/demo --verbose
# inputfile for demo to run Joshs UNIX V1 on emualted
# CPU, serial+clock, memory, RF11/RS11 and EA
#

d			# device menu

# first, make a serial port. Default ist
sd dl11
en dl11			# use emulated serial console
p p ttyS2		# use "UART2 connector, see FAQ
en kw11			# enable KW11 on DL11-W

# The processor first: enabling it makes the board the bus owner, which is
# what the memory sizing below needs on a backplane with no CPU of its own.
en cpu20

m i			# install max UNIBUS memory

# Deposit bootloader into memory
m ll ../bootloaders/rf11.lst


en ke                   # enable PDP-11/20 EAE

en rf                   # enable RF11 controller
en rs0                  # enable RS11 drive #0
sd rs0
p image /var/lib/qunilator/images/rf/unixv1_rs0.rs11.dsk  # mount image file

en rk			# enable RK11 controller
en rk0			# enable drive #0
sd rk0			# select
p image /var/lib/qunilator/images/dk/unixv1_rk0.rk05.dsk 	# mount image file

# enable CPU
sd cpu20
p pmi 1
p swr 173700
#p pc 10000

.print Unix V1 restoration project at https://github.com/jserv/unix-v1
.print Manuals at https://www.bell-labs.com/usr/dmr/www/1stEdman.html
.print The RF11 bootloader loads and starts the "Bootstrap Operating System" ("bos"),
.print which has severals functions depending on console switches.
.print SR=173700 will read "Warm UNIX" from RF core location 0 and transfer to 400.
.print See Unix V1 manual BOOT PROCEDURES (VII) 11/3/71
.print Disk images here have been patched to run without DC11 serial.
.print
.print Set terminal to 9600 7O1
.print Start CPU20 by toggeling CONT switch with "p c 1"
.print Login as "root"
