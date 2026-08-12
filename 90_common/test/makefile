# makefile: build and run the host tests for the shared code of 90_common
#
# Like the CPU core tests of 10.06_cputest these are built with the HOST
# compiler and run on the build machine: 90_common is target independent, so one
# set of tests serves both buses and neither the board nor the cross toolchain
# takes part. Never use the cross compiler here.
#
# Targets:
#	all / test	build what is out of date, then run the tests
#	retest		drop the stamps and run everything again
#	clean		remove objects, binaries and stamps
#
# A test binary may also be run by hand for its verbose output, e.g.
#	../4_deploy/pdp11disastest -v

OBJDIR = $(abspath ../4_deploy)
STAMPDIR = $(OBJDIR)/stamps
SRCDIR = $(abspath ../src)

HOST_CXX ?= g++

# GCC_ROOT points at the ARM cross toolchain where one is set up, and gcc treats
# a non-empty GCC_ROOT as its own installation prefix - which makes the host g++
# look for its internal headers under the ARM toolchain and fail with
#	/usr/include/stdio.h: fatal error: stddef.h: No such file or directory
unexport GCC_ROOT

CXXFLAGS = \
	-std=c++11 \
	-O2 -Wall -Wextra -Wshadow \
	-I$(SRCDIR) \
	-c

# One test binary per unit under test: <name>test from <name>_test.cpp plus the
# unit's own source out of ../src. Add a unit by adding its name here.
TESTS = pdp11disas scriptpath getopt2

PROGS = $(foreach t,$(TESTS),$(OBJDIR)/$(t)test)
STAMPS = $(foreach t,$(TESTS),$(STAMPDIR)/$(t).ok)

.PHONY: all test retest clean

# else make deletes the objects and even the test binaries again as
# intermediates of the pattern rule chain below, and relinks them every time.
.SECONDARY:

all: test

test: $(STAMPS)
	@echo "90_common tests up to date: $(TESTS)"

retest:
	rm -rf $(STAMPDIR)
	$(MAKE) test

clean:
	rm -rf $(OBJDIR)

$(OBJDIR) $(STAMPDIR):
	mkdir -p $@

# A stamp depends on its test binary only, and that on the unit under test. So
# an edit to pdp11disas.cpp re-runs the disassembler tests, and an unrelated
# build runs nothing.
$(STAMPDIR)/%.ok: $(OBJDIR)/%test | $(STAMPDIR)
	$<
	@touch $@

$(OBJDIR)/%test: $(OBJDIR)/%_test.o $(OBJDIR)/%.o
	$(HOST_CXX) -o $@ $^

$(OBJDIR)/%_test.o: %_test.cpp $(SRCDIR)/%.hpp | $(OBJDIR)
	$(HOST_CXX) $(CXXFLAGS) $< -o $@

$(OBJDIR)/%.o: $(SRCDIR)/%.cpp $(SRCDIR)/%.hpp | $(OBJDIR)
	$(HOST_CXX) $(CXXFLAGS) $< -o $@
