#!/usr/bin/unibone-cli --verbose
# Main PDP-1120 must be HALTed
# Inputfile for the menu program to execute "Hello world"
# Uses emulated CPU and (physical or emulated) DL11

d			# "device with cpu" menu

# The processor first: enabling it makes the board the bus owner, which is
# what the memory sizing below needs on a backplane with no CPU of its own.
en cpu20		# switch on emulated 11/20 CPU

m i   			# emulate missing memory
sd dl11

p p ttyS2		# use "UART2 connector, see FAQ

# p b 300		# reduced baudrate

en dl11			# switch on emulated DL11

sd cpu20		# select

m ll serial.lst		# load test program

p

init

.print Emulated PDP-11/20 CPU will now output "Hello world"
.print and enter a serial echo loop on simulated DL11 at 177650.
.print Regular RS232 port is UART2.
.print Make sure physical CPU is disabled.

.input

p c 1

# .print CPU20 started... wait for auto-typed input.
# dl11 rcv 5000 <This\x20text\x20is\x20typed\x20and\x20echoed\x0d\0x0a>



