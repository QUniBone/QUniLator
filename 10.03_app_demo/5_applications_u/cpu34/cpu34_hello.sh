#!/root/10.03_app_demo/4_deploy/demo --verbose
# Main PDP-11 must be HALTed
# Inputfile for demo to execute "Hello world"
# Uses emulated CPU and physical DL11

d			# "device with cpu" menu

# The processor first: enabling it makes the board the bus owner, which is
# what the memory sizing below needs on a backplane with no CPU of its own.
en cpu34		# switch on emulated 11/34 CPU

m i   			# emulate missing memory

sd cpu34		# select

m ll serial.lst		# load test program

p

init

.print Emulated PDP-11/34 CPU will now output "Hello world"
.print and enter a serial echo loop on physical DL11 at 177650.
.print Make sure physical CPU is disabled.

.input

p c 1

.print CPU34 started


