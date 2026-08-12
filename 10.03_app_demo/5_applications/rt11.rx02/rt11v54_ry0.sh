#!/root/10.03_app_demo/4_deploy/demo --verbose
# inputfile for demo to select a rx211 device in the "device test" menu.
d			# device test menu

# first, make a serial port. Default ist
# sd dl11
# p p ttyS2		# use "UART2 connector
# en dl11
# en kw11


pwr
.wait 3000		# wait for PDP-11 to reset
m i			# install max UNIBUS memory

# Deposit bootloader into memory
m ll ../bootloaders/dy.lst

en ry			# enable RX11 controller
en rybox

sd rybox
p pwr 1			# power-on drive box

sd ry0
p it0 1 # track 0 in image
p img ../diskimages/RT11.rx02.dsk 	# insert floppy into drive #0

#sd ry1
#p it0 1 # track 0 in image
#p img ../diskimages/NEW_GAMES.rx02.dsk  	# insert floppy into drive #1
## p emulation_speed 10	# 10x speed. Load disk in 5 seconds

.print Disk drive now on track after 5 secs
.wait	5000		# wait until drive spins up
p                       # show all params of RX1

.print RX02 drives ready.
.print RX211 boot loader installed.
.print Start 10000 to boot from drive 0 (10010 for drive 1)
.print Reload with "m ll"

