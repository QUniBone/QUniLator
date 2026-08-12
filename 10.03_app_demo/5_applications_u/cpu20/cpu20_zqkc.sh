#!/root/10.03_app_demo/4_deploy/demo --verbose
# Main PDP-11/20 must be HALTed
# Inputfile for demo to execute "Hello world"
# Uses emulated CPU and (physical or emulated) DL11
#
# Listing corresponding to ZQKC rev E:
# bitsavers.informatik.uni-stuttgart.de/pdf/dec/pdp11/xxdp/diag_listings/MAINDEC-11-DZQKC-E-D_11_Family_Instruction_Exerciser_Mar75.pdf

d			    # "device with cpu" menu

# The processor first: enabling it makes the board the bus owner, which is
# what the memory sizing below needs on a backplane with no CPU of its own.
en cpu20		# switch on emulated 11/20 CPU

m i   			# emulate missing memory

sd dl11
p p ttyS2		# use "UART2" connector, see FAQ
en dl11			# switch on emulated DL11

sd cpu20		# select

m lp ZQKC_E_05_20.abs   # load test program

init
.wait 500

.print Emulated PDP-11/20 CPU will run ZQKC starting at pc set below
.print Regular RS232 port is UART2 and also console.
.print Make sure physical CPU is disabled.

p pc 0200
p swr 0114200
p swab 1        # ZQKC fails unless 11/20 SWAB insn sets psw v-bit (not std 11/20 behavior)
p pmi 1         # faster memory access
p
p s 1
