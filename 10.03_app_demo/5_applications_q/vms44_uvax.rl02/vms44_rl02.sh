#!/root/10.03_app_demo/4_deploy/demo
# inputfile for demo to select a RD54 device in the "device" menu.
d			# device test menu

pwr
.wait 3000		# wait for PDP-11 to reset
# m i			# install max QBUS memory
#m i 1777776		# install max 512k QBUS memory


# mount NanoVMS044-1M in RL02 #0 and start
en rl           # enable controller
en rl0			# enable drive #0
sd rl0			# select
p emulation_speed 10	# 10x speed. Load disk in 5 seconds
# set type to "rl02"
p runstopbutton 0	# released: "LOAD"
p powerswitch 1		# power on, now in "load" state
p image /var/lib/qunilator/images/dl/NanoVMS044-1M.rl02.dsk # mount image file
p runstopbutton 1	# press RUN/STOP, will start

# scratch disk into UDA0
en uda			# enable UDA50 controller
en uda0			# enable drive #0
sd uda0			# select
p type RD53
# mount image
p image /var/lib/qunilator/images/du/scratch.rd53.dsk
p useimagesize	0  # new&empty iamge

.print Disk drive now on track after 5 secs
.wait	5000		# wait until drive spins up

.print NanoVMS044-1M is a very small version of VMS V4.4 made for testing purpose.
.print It runs with only 1MB memory (included on the KA630 MicroVAX II CPU board).
.print It is below 10MB in size and thus fits onto an RD51 or RL02.
.print Log in with Username: SYSTEM, no password required.
.print Because of the small disk and memory size the functionality is limited,
.print although a lot of things still work.

