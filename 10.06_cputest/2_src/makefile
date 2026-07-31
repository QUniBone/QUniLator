# makefile: build and run the CPU emulation core tests
#
# These tests are built by the HOST compiler and run on the build machine, with
# no BeagleBone and no cross toolchain in the picture: run "make" in this
# directory, which is also what CI does. The emulator itself is built elsewhere
# by the ARM cross compiler (./crossbuild.sh) or on the board (./compile.sh);
# those builds do not reach here. Never use $(BBB_CC) or $(CC) in this file.
#
# There is no _u/_q split: a CPU emulation core reaches the world only through
# the unibone_*() functions of cpu_bus_adapter.h and knows nothing about UNIBUS
# or QBUS, so one set of tests serves both platforms.
#
# Targets:
#	all / test	build cputest, then run every (core, tape) pair whose
#			stamp is missing or out of date
#	retest		drop all stamps and run everything again
#	clean		remove objects, binary and stamps
#	list		show which tapes were found
#
# Use "make -j" - the runs are independent, and a full run is ~80s serial.

# QUNIBONE_DIR names the checkout when a caller sets it. Fall back to this
# file's own location, so "make" in this directory works on its own.
QUNIBONE_ROOT ?= $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
ifneq ($(QUNIBONE_DIR),)
QUNIBONE_ROOT := $(QUNIBONE_DIR)
endif

DEVICE_SRC_DIR = $(QUNIBONE_ROOT)/10.02_devices/2_src
TAPE_DIR = $(abspath ../3_tapes)
OBJDIR = $(abspath ../4_deploy)
STAMPDIR = $(OBJDIR)/stamps

PROG = $(OBJDIR)/cputest

HOST_CXX ?= g++

# GCC_ROOT points at the ARM cross toolchain when a cross build has exported it.
# gcc treats a non-empty GCC_ROOT as its own installation prefix, so leaving it
# set makes the HOST g++ look for its internal headers under the ARM toolchain
# and fail with
#	/usr/include/stdio.h: fatal error: stddef.h: No such file or directory
# Keep it out of the environment of every recipe below. It is of no use here: the
# ARM toolchain has no business in a host build.
unexport GCC_ROOT

# -O2 always, whatever MAKE_CONFIGURATION says for the ARM build: a -O0 core
# makes a full run several times slower for no benefit.
# No -DARM: that is what gives the cores the no-op ARM_DEBUG_PIN* of
# cpu_debug_pins.h instead of the real GPIO ones.
CXXFLAGS = \
	-std=c++11 \
	-O2 -Wall -Wextra \
	-I. \
	-I$(DEVICE_SRC_DIR) \
	-MMD -MP \
	-c

LDFLAGS = -lpthread

# The emulation cores. Written as C, compiled as C++, exactly as
# 10.03_app_demo/2_src/makefile_u does it.
CORE_CXXFLAGS = $(CXXFLAGS) -Wno-parentheses -Wno-unused-parameter

HARNESS_OBJECTS = \
	$(OBJDIR)/cputest.o \
	$(OBJDIR)/papertape.o \
	$(OBJDIR)/testbus.o \
	$(OBJDIR)/testcore.o \
	$(OBJDIR)/testcore_cpu20.o \
	$(OBJDIR)/testcore_cpu34.o

CORE_OBJECTS = \
	$(OBJDIR)/ka11.o \
	$(OBJDIR)/kd11ea.o \
	$(OBJDIR)/kt11d.o

OBJECTS = $(HARNESS_OBJECTS) $(CORE_OBJECTS)

# Tapes to run. Drop further tape images into ../3_tapes/<dir>/ and they are
# picked up here without editing anything.
#	both/	run against every core
#	cpu20/	11/20 only
#	cpu34/	11/34 only, e.g. the KT11-D tests
# Both extensions in use for the same absolute loader format are matched: .BIN
# as the paper tape images are named, .BIC as the XXDP distributions name them.
TAPE_EXTENSIONS = BIN BIC
tapes_in = $(foreach e,$(TAPE_EXTENSIONS),$(wildcard $(1)/*.$(e)))

BOTH_TAPES = $(sort $(call tapes_in,$(TAPE_DIR)/both))
CPU20_TAPES = $(sort $(call tapes_in,$(TAPE_DIR)/cpu20))
CPU34_TAPES = $(sort $(call tapes_in,$(TAPE_DIR)/cpu34))

# stamp path for one (core, tape) pair
stamp = $(STAMPDIR)/$(1)-$(notdir $(2)).ok

STAMPS = \
	$(foreach t,$(BOTH_TAPES),$(call stamp,cpu20,$(t)) $(call stamp,cpu34,$(t))) \
	$(foreach t,$(CPU20_TAPES),$(call stamp,cpu20,$(t))) \
	$(foreach t,$(CPU34_TAPES),$(call stamp,cpu34,$(t)))

.PHONY: all test retest clean list

all: test

test: $(STAMPS)
	@echo "CPU core tests: $(words $(STAMPS)) diagnostic runs up to date."

retest:
	rm -rf $(STAMPDIR)
	$(MAKE) test

clean:
	rm -rf $(OBJDIR)

list: $(PROG)
	@echo "cores:       $$($(PROG) --help 2>&1 | sed -n 's/.*core <name>.*: //p')"
	@echo "both:        $(notdir $(BOTH_TAPES))"
	@echo "cpu20 only:  $(notdir $(CPU20_TAPES))"
	@echo "cpu34 only:  $(notdir $(CPU34_TAPES))"

$(OBJDIR) $(STAMPDIR):
	mkdir -p $@

$(PROG): $(OBJECTS)
	$(HOST_CXX) -o $@ $(OBJECTS) $(LDFLAGS)

# A stamp depends on its tape, its .opt sidecar if there is one, and the binary
# (which depends on the core sources). So an edit to ka11.c re-runs the cpu20
# diagnostics, and an unrelated build re-runs nothing.
# $(1) = core, $(2) = tape
define TESTRULE
$(call stamp,$(1),$(2)): $(2) $(wildcard $(2).opt) $(PROG) | $(STAMPDIR)
	$(PROG) --core $(1) --tape $(2)
	@touch $$@
endef

$(foreach t,$(BOTH_TAPES),$(eval $(call TESTRULE,cpu20,$(t))))
$(foreach t,$(BOTH_TAPES),$(eval $(call TESTRULE,cpu34,$(t))))
$(foreach t,$(CPU20_TAPES),$(eval $(call TESTRULE,cpu20,$(t))))
$(foreach t,$(CPU34_TAPES),$(eval $(call TESTRULE,cpu34,$(t))))

### harness

$(OBJDIR)/cputest.o: cputest.cpp testbus.hpp testcore.hpp papertape.hpp | $(OBJDIR)
	$(HOST_CXX) $(CXXFLAGS) $< -o $@

$(OBJDIR)/papertape.o: papertape.cpp papertape.hpp testbus.hpp | $(OBJDIR)
	$(HOST_CXX) $(CXXFLAGS) $< -o $@

$(OBJDIR)/testbus.o: testbus.cpp testbus.hpp $(DEVICE_SRC_DIR)/cpu_bus_adapter.h | $(OBJDIR)
	$(HOST_CXX) $(CXXFLAGS) $< -o $@

$(OBJDIR)/testcore.o: testcore.cpp testcore.hpp | $(OBJDIR)
	$(HOST_CXX) $(CXXFLAGS) $< -o $@

$(OBJDIR)/testcore_cpu20.o: testcore_cpu20.cpp testcore.hpp \
		$(DEVICE_SRC_DIR)/cpu_core.h $(DEVICE_SRC_DIR)/cpu20/ka11.h | $(OBJDIR)
	$(HOST_CXX) $(CXXFLAGS) $< -o $@

# kt11d.h too: KD11EA holds a KT11D by value, so a change to the MMU struct
# changes the size of the core this object allocates. Missing that dependency
# does not fail to link, it corrupts the heap at run time.
$(OBJDIR)/testcore_cpu34.o: testcore_cpu34.cpp testcore.hpp \
		$(DEVICE_SRC_DIR)/cpu_core.h $(DEVICE_SRC_DIR)/cpu34/kd11ea.h \
		$(DEVICE_SRC_DIR)/cpu34/kt11d.h | $(OBJDIR)
	$(HOST_CXX) $(CXXFLAGS) $< -o $@

### the emulation cores under test, compiled from the shipped sources

$(OBJDIR)/ka11.o: $(DEVICE_SRC_DIR)/cpu20/ka11.c $(DEVICE_SRC_DIR)/cpu20/ka11.h \
		$(DEVICE_SRC_DIR)/cpu_core.h $(DEVICE_SRC_DIR)/cpu_bus_adapter.h \
		$(DEVICE_SRC_DIR)/cpu_debug_pins.h | $(OBJDIR)
	$(HOST_CXX) $(CORE_CXXFLAGS) -I$(DEVICE_SRC_DIR)/cpu20 -x c++ $< -o $@

$(OBJDIR)/kd11ea.o: $(DEVICE_SRC_DIR)/cpu34/kd11ea.c $(DEVICE_SRC_DIR)/cpu34/kd11ea.h \
		$(DEVICE_SRC_DIR)/cpu_core.h $(DEVICE_SRC_DIR)/cpu34/kt11d.h \
		$(DEVICE_SRC_DIR)/cpu_bus_adapter.h $(DEVICE_SRC_DIR)/cpu_debug_pins.h | $(OBJDIR)
	$(HOST_CXX) $(CORE_CXXFLAGS) -I$(DEVICE_SRC_DIR)/cpu34 -x c++ $< -o $@

$(OBJDIR)/kt11d.o: $(DEVICE_SRC_DIR)/cpu34/kt11d.c $(DEVICE_SRC_DIR)/cpu34/kt11d.h \
		$(DEVICE_SRC_DIR)/cpu_core.h $(DEVICE_SRC_DIR)/cpu_bus_adapter.h | $(OBJDIR)
	$(HOST_CXX) $(CORE_CXXFLAGS) -I$(DEVICE_SRC_DIR)/cpu34 -x c++ $< -o $@

# The header dependencies of every object, as the compiler saw them. The rules
# above name each object's headers by hand, which is how the kt11d.h dependency
# noted there came to be written down; -MMD keeps the list complete without
# anyone having to remember, and -MP keeps a deleted header from breaking the
# build.
-include $(OBJECTS:.o=.d)
