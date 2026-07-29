# makefile: build the vendored simh VAX-11/780 on the host
#
# This is a HOST build, like 10.06_cputest/2_src: run "make" in this directory
# and it produces a workstation binary. No BeagleBone and no cross toolchain are
# involved; the board build (./crossbuild.sh, ./compile.sh) does not reach here.
# Never use $(BBB_CC) in this file.
#
# Targets:
#	all		both binaries below
#	vax780		the stock simh VAX-11/780, built from the vendored
#			sources of 91_3rd_party/simh_vax
#	vax780-shim	the same processor with shim/ in place of simh's command
#			interpreter, console and timer, which is the form the
#			core takes when it moves into the QUniLator application
#	clean		remove objects and binaries
#
# Use "make -j".
#
# For the board, run the same makefile under the armhf cross toolchain that
# crossbuild.sh carries, which is the Debian the appliance image runs:
#
#	docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/qunibone" \
#	    -w /qunibone/10.07_vax/2_src qunibone-crossbuild:<tag> \
#	    make QUNIBONE_DIR=/qunibone HOST_CC=arm-linux-gnueabihf-gcc \
#	         OBJDIR=/qunibone/10.07_vax/4_deploy_arm SIMH_USE_NETWORK=no -j
#
# Copy 4_deploy_arm/vax780 and vax780-shim to the board and run 3_tests there.

# Fixed immediately: the generated dependency files are included at the end of
# this makefile and join MAKEFILE_LIST, so its last word stops naming this file
# once they are there.
THIS_MAKEFILE := $(lastword $(MAKEFILE_LIST))

QUNIBONE_ROOT ?= $(abspath $(dir $(THIS_MAKEFILE))/../..)
ifneq ($(QUNIBONE_DIR),)
QUNIBONE_ROOT := $(QUNIBONE_DIR)
endif

SIMH_DIR = $(QUNIBONE_ROOT)/91_3rd_party/simh_vax
OBJDIR = $(abspath ../4_deploy)

VAX780 = $(OBJDIR)/vax780

HOST_CC ?= cc

# GCC_ROOT points at the ARM cross toolchain when a cross build has exported it,
# and gcc treats a non-empty GCC_ROOT as its own installation prefix. Keep it out
# of every recipe below; the ARM toolchain has no business in a host build.
unexport GCC_ROOT

# ---------------------------------------------------------------------------
# Sources
#
# The three lists are the VAX780 and SIM lists of the upstream makefile, and
# 91_3rd_party/simh_vax/README-qunibone.md says what each file is. Keep them in
# step with a re-harvest.
# ---------------------------------------------------------------------------

VAX_SRC = \
	vax_cpu.c vax_cpu1.c vax_fpa.c vax_cis.c vax_octa.c vax_cmode.c \
	vax_mmu.c vax_sys.c vax_syscm.c \
	vax780_stddev.c vax780_sbi.c vax780_mem.c vax780_uba.c vax7x0_mba.c \
	vax780_fload.c vax_uw.c vax780_syslist.c

PDP11_SRC = \
	pdp11_rl.c pdp11_rq.c pdp11_ts.c pdp11_dz.c pdp11_lp.c pdp11_tq.c \
	pdp11_xu.c pdp11_ry.c pdp11_cr.c pdp11_rp.c pdp11_tu.c pdp11_hk.c \
	pdp11_vh.c pdp11_dmc.c pdp11_dup.c pdp11_td.c pdp11_tc.c pdp11_rk.c \
	pdp11_io_lib.c pdp11_ch.c

SIM_SRC = \
	scp.c sim_console.c sim_fio.c sim_timer.c sim_sock.c sim_tmxr.c \
	sim_ether.c sim_tape.c sim_disk.c sim_serial.c sim_video.c sim_imd.c \
	sim_card.c

SIMH_OBJECTS = \
	$(patsubst %.c,$(OBJDIR)/simh/VAX/%.o,$(VAX_SRC)) \
	$(patsubst %.c,$(OBJDIR)/simh/PDP11/%.o,$(PDP11_SRC)) \
	$(patsubst %.c,$(OBJDIR)/simh/sim/%.o,$(SIM_SRC))

# ---------------------------------------------------------------------------
# Configuration of the vendored sources
# ---------------------------------------------------------------------------

# VM_VAX and VAX_780 pick the model; vax_defs.h keys its whole register and
# address layout off VAX_780. USE_INT64 and USE_ADDR64 are what the VAX build
# needs from the simh value types.
SIMH_MODEL = -DVM_VAX -DVAX_780 -DUSE_INT64 -DUSE_ADDR64

# Host facilities simh probes for. This build takes the ones present on any
# POSIX workstation and leaves out the optional libraries upstream links when it
# finds them (pcre2, editline, libpng, zlib, vde, slirp): none of them is needed
# to run a VAX, and leaving them out keeps the build free of packages a checkout
# may not have.
SIMH_HOST = -D_GNU_SOURCE -DHAVE_SYS_IOCTL -DHAVE_UTIME -DHAVE_GLOB -DHAVE_SHM_OPEN

# Ethernet reaches libpcap through dlopen() at run time, so pcap is a run-time
# dependency and not a link-time one. SIM_HAVE_DLOPEN names the shared-library
# suffix to look for.
UNAME_S = $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
SIMH_DLOPEN = -DSIM_HAVE_DLOPEN=dylib
else
SIMH_DLOPEN = -DSIM_HAVE_DLOPEN=so
endif

# simh's own ethernet, which the stock simulator's XU device needs and which
# wants pcap's header at compile time. Set SIMH_USE_NETWORK=no for a build whose
# ethernet is an emulated device on the bus rather than simh's, which is what a
# build for the board is; sim_ether.c then compiles to its own stub.
SIMH_USE_NETWORK ?= yes
ifeq ($(SIMH_USE_NETWORK),yes)
SIMH_NETWORK = -DHAVE_PCAP_NETWORK -DUSE_SHARED -DBPF_CONST_STRING
else
SIMH_NETWORK =
endif

# The reader thread and asynchronous I/O, as the upstream makefile builds them.
SIMH_ASYNCH = -DUSE_READER_THREAD -DSIM_ASYNCH_IO

# Reported by SHOW VERSION, so a binary says which harvest it came from. The
# value is the revision recorded in 91_3rd_party/simh_vax/README-qunibone.md.
SIMH_COMMIT_ID = a1f57fa3738ed31148d31126ba1a7278ff845c6d
SIMH_COMMIT_TIME = 2026-07-03T13:48:11+0200
SIMH_PROVENANCE = \
	-DSIM_GIT_COMMIT_ID=$(SIMH_COMMIT_ID) \
	-DSIM_GIT_COMMIT_TIME=$(SIMH_COMMIT_TIME) \
	-DSIM_BUILD_TOOL=qunilator-makefile

# Warnings for the vendored sources. They are upstream code, which the project
# keeps unedited, so -Wall runs and each diagnostic it raises on the harvest is
# turned off here by name. shim/ is held to a stricter level further down.
#
#   unused-function		pdp11_ddcmp.h and pdp11_dup.c define the whole
#				DDCMP helper set in a header, and each includer
#				uses a few of them; scp.c, sim_console.c and
#				sim_sock.c define helpers for build variants
#				this configuration leaves out
#   unused-variable		sim_ether.c and sim_disk.c keep locals that only
#				the optional-library variants read
#   unused-but-set-variable	sim_tmxr.c collects return codes it deliberately
#				ignores
#   missing-braces		vax_uw.c writes the UNIT table with the simh
#				UDATA() macro and no inner braces
#   uninitialized		sim_exp_set() in scp.c copies re_nsub on a path
#				that only a regex-enabled build reaches
#   misleading-indentation	simh's house style puts the closing brace of a
#				block at the indent of its body
#   array-bounds		mba_wrreg() in vax7x0_mba.c indexes mba_sr[] with
#				the adapter number before testing it for -1
SIMH_WARNINGS = \
	-Wall \
	-Wno-unused-function \
	-Wno-unused-variable \
	-Wno-unused-but-set-variable \
	-Wno-missing-braces \
	-Wno-uninitialized \
	-Wno-misleading-indentation \
	-Wno-array-bounds

# gcc spellings, which clang rejects and says so:
#
#   maybe-uninitialized	sim_tmxr.c has three locals gcc cannot see are set on
#			every reached path
#   format-overflow	sim_tape.c formats an int into a three-character buffer,
#			which a 32-bit target makes gcc count as up to ten
HOST_CC_IS_CLANG = $(shell echo | $(HOST_CC) -dM -E - 2>/dev/null | grep -c __clang__)
ifeq ($(HOST_CC_IS_CLANG),0)
SIMH_WARNINGS += -Wno-maybe-uninitialized -Wno-format-overflow
endif

# -MMD -MP writes a dependency file beside each object, so a changed header
# rebuilds what includes it.
SIMH_CFLAGS = \
	-std=c99 -U__STRICT_ANSI__ -MMD -MP \
	-O2 -DNDEBUG=1 -fno-strict-overflow -finline-functions \
	$(SIMH_MODEL) $(SIMH_HOST) $(SIMH_DLOPEN) $(SIMH_NETWORK) \
	$(SIMH_ASYNCH) $(SIMH_PROVENANCE) $(SIMH_WARNINGS) \
	-I$(SIMH_DIR)/sim -I$(SIMH_DIR)/VAX -I$(SIMH_DIR)/PDP11

SIMH_LDFLAGS = -lpthread -lm

# ---------------------------------------------------------------------------
# Rules
# ---------------------------------------------------------------------------

.PHONY: all clean vax780 vax780-shim

all: vax780 vax780-shim

vax780: $(VAX780)

$(VAX780): $(SIMH_OBJECTS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(SIMH_OBJECTS) $(SIMH_LDFLAGS) -o $@

$(OBJDIR)/simh/VAX/%.o: $(SIMH_DIR)/VAX/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(SIMH_CFLAGS) -c $< -o $@

$(OBJDIR)/simh/PDP11/%.o: $(SIMH_DIR)/PDP11/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(SIMH_CFLAGS) -c $< -o $@

$(OBJDIR)/simh/sim/%.o: $(SIMH_DIR)/sim/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(SIMH_CFLAGS) -c $< -o $@

# ---------------------------------------------------------------------------
# The shimmed core
#
# The same processor and the same 780 system files, without simh's command
# interpreter, console, timer and terminal multiplexer, and without the UNIBUS
# peripherals of the PDP-11 simulator: on the board those live on the real bus.
# shim/ supplies what scp supplied, and shim_main.c hosts it on the workstation.
#
# The vendored sources are compiled a second time here rather than shared with
# the target above, because the configuration differs: no asynchronous I/O, no
# networking, and the objects must not be mixed.
# ---------------------------------------------------------------------------

VAX780_SHIM = $(OBJDIR)/vax780-shim

SHIM_VAX_SRC = \
	vax_cpu.c vax_cpu1.c vax_fpa.c vax_cis.c vax_octa.c vax_cmode.c \
	vax_mmu.c vax_sys.c vax_syscm.c \
	vax780_stddev.c vax780_sbi.c vax780_mem.c vax780_uba.c vax7x0_mba.c \
	vax780_fload.c vax_uw.c

# The UNIBUS address and vector map of the PDP-11 simulator. The DW780 model
# builds its I/O page through it, so it comes along even though no PDP-11
# peripheral does.
SHIM_PDP11_SRC = pdp11_io_lib.c

# simh's file layer, which depends on nothing above it.
SHIM_SIM_SRC = sim_fio.c

SHIM_SRC = \
	simh_shim.c simh_shim_console.c simh_shim_timer.c simh_shim_tmxr.c \
	vax780_shim_syslist.c shim_main.c

SHIM_OBJECTS = \
	$(patsubst %.c,$(OBJDIR)/shim/VAX/%.o,$(SHIM_VAX_SRC)) \
	$(patsubst %.c,$(OBJDIR)/shim/PDP11/%.o,$(SHIM_PDP11_SRC)) \
	$(patsubst %.c,$(OBJDIR)/shim/sim/%.o,$(SHIM_SIM_SRC)) \
	$(patsubst %.c,$(OBJDIR)/shim/shim/%.o,$(SHIM_SRC))

# No SIMH_ASYNCH and no SIMH_NETWORK: an embedded core has neither a reader
# thread nor a host network stack of its own.
SHIM_CFLAGS = \
	-std=c99 -U__STRICT_ANSI__ -MMD -MP \
	-O2 -DNDEBUG=1 -fno-strict-overflow -finline-functions \
	$(SIMH_MODEL) $(SIMH_HOST) $(SIMH_PROVENANCE) $(SIMH_WARNINGS) \
	-I$(SIMH_DIR)/sim -I$(SIMH_DIR)/VAX -I$(SIMH_DIR)/PDP11 -Ishim

vax780-shim: $(VAX780_SHIM)

$(VAX780_SHIM): $(SHIM_OBJECTS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(SHIM_OBJECTS) $(SIMH_LDFLAGS) -o $@

$(OBJDIR)/shim/VAX/%.o: $(SIMH_DIR)/VAX/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(SHIM_CFLAGS) -c $< -o $@

$(OBJDIR)/shim/PDP11/%.o: $(SIMH_DIR)/PDP11/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(SHIM_CFLAGS) -c $< -o $@

$(OBJDIR)/shim/sim/%.o: $(SIMH_DIR)/sim/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(SHIM_CFLAGS) -c $< -o $@

# The shim is this project's own code, so it is held to the project's warning
# level and not to the vendored tree's suppressions.
SHIM_OWN_CFLAGS = $(filter-out $(SIMH_WARNINGS),$(SHIM_CFLAGS)) -Wall -Wextra

$(OBJDIR)/shim/shim/%.o: shim/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(SHIM_OWN_CFLAGS) -c $< -o $@

clean:
	rm -rf $(OBJDIR)

-include $(SIMH_OBJECTS:.o=.d) $(SHIM_OBJECTS:.o=.d)
