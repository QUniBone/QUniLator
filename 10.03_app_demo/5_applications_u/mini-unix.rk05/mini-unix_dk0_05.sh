#!/root/10.03_app_demo/4_deploy/demo --verbose
# inputfile for demo to select a rk05 device in the "device test" menu.
# mounts 4 "Mini.Unix" RK05 images
d			# device test menu
pwr
.wait 3000		# wait for PDP-11 to reset

m i			# install max UNIBUS memory

# Deposit bootloader into memory
m ll ../bootloaders/dk.lst

en rk			# enable RK11 controller


en rk0			# enable drive #0
sd rk0			# select
p image ../diskimages/mini-unix-tape1_05.rk05.dsk   # The BIN disk (RK05)

en rk1			# enable drive #1
sd rk1			# select
p image ../diskimages/mini-unix-tape2.rk05.dsk # The SRC disk (RK05)

en rk2			# enable drive #2
sd rk2			# select
p image ../diskimages/mini-unix-tape3.rk05.dsk       # The MAN disk (RK05)

.print Disk drive now on track after 5 secs
.wait	6000		# wait until drive spins up
p                       # show all params of RL1


.print RK drives ready.
.print RK11 boot loader installed.
.print Start 10000 to boot from drive 0
.print Reload with "m ll"
.print
.print Set terminal to 9600 7O1
.print At @ prompt, select kernel to boot with "rkmx"
.print Login with "root"

