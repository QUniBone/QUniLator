#!/usr/bin/qbone-cli --verbose
# inputfile for the menu program to select a rx11 device in the "device test" menu.
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
m ll ../bootloaders/dx.lst

en rx			# enable RX11 controller
en rxbox

sd rxbox
p pwr 1			# power-on drive box

sd rx0
p img /var/lib/qunilator/images/rx/root.rx01.dsk  	# insert floppy into drive #0

sd rx1
p img /var/lib/qunilator/images/rx/usr.rx01.dsk  		# insert floppy into drive #0
# p emulation_speed 10	# 10x speed. Load disk in 5 seconds

.print Disk drive now on track after 5 secs
.wait	5000		# wait until drive spins up
p                       # show all params of RX1

.print RX01 drives ready.
.print RX11 boot loader installed.
.print Start 10000 to boot from drive 0 (10010 for drive 1)
.print Reload with "m ll"
.print On LSX "boot" prompt, enter "lsx".
.print After login "STTY -LCASE" is a good idea.
.print Your terminal emulator must filter three <DEL>s before each line end!

