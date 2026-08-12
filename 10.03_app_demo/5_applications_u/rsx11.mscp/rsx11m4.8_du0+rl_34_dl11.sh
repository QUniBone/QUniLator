#!/root/10.03_app_demo/4_deploy/demo
# RSX11M 4.8 from MSCP drive 0, the same setup as
# rsx11m4.8_du0+rl_34.sh with a second, emulated DL11 at 760010/300.
d			# device test menu

# Use 2nd emulated DL11
sd dl11
p p ttyS2		# use "UART2 connector
p addr 760010
p iv 300
en dl11

pwr
.wait 3000		# wait for PDP-11 to reset
m i			# install max UNIBUS memory

# Deposit bootloader into memory
m ll ../bootloaders/du.lst

en uda			# enable UDA50 controller

# mount RSX in MSCP drive #0
en uda0			# enable drive #0
sd uda0			# select
p type RA70
p image ../diskimages/rsx11m_4_8_bl70.ra70.dsk  # mount image
p useimagesize	1

# mount test disk in MSCP drive #1
en uda1			# enable drive #1
sd uda1			# select
p type RA70
p image ../diskimages/rsxm70.ra70.dsk      # mount image
p useimagesize	1


en rl			# enable RL11 controller

# mount RSX v4.16 in RL02 #0 and start
en rl0			# enable drive #0
sd rl0			# select
p emulation_speed 10	# 10x speed. Load disk in 5 seconds
p type rl02
p runstopbutton 0	# released: "LOAD"
p powerswitch 1		# power on, now in "load" state
p image ../diskimages/rsxm70.rl02.dsk  # mount image
p runstopbutton 1	# press RUN/STOP, will start

# mount DL1 in RL02 #1 and start
en rl1			# enable drive #1
sd rl1			# select
p emulation_speed 10	# 10x speed. Load disk in 5 seconds
p type rl02
p runstopbutton 0	# released: "LOAD"
p powerswitch 1		# power on, now in "load" state
p image ../diskimages/rsxdl1.rl02.dsk         # mount image
p runstopbutton 1	# press RUN/STOP, will start

.print Disk drive now on track after 5 secs
.wait	5000		# wait until drive spins up

p                       # show all params

.print MSCP drives ready.
.print UDA50 boot loader installed.
.print Start 10000 to boot from drive 0, 10010 for drive 1, ...
.print Reload with "m ll"


.end

# boot RSX11 from DU
dl11 rcv L\x2010000\r
dl11 rcv 500 S\r
dl11 wait 20000 Please\x20enter\x20time\x20and\x20date
dl11 rcv 500 20:00 27-aug-2019\r
dl11 wait 10000 ENTER\x20LINE\x20WIDTH
dl11 rcv 1000 80\r
dl11 wait 10000 >@
# startup complete
# logout, login as SYSTEM/SYSTEM
dl11 rcv 1000 LOG\r
dl11 rcv 3000 HELLO\x20SYSTEM\r
dl11 rcv 3000 SYSTEM\r

