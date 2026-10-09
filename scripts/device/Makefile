# scripts/device/Makefile
#
# Builds the on-device `timeout` helper (hard-float ARM, static). The rest of
# scripts/device/ is shell that runs as-is on the SC Live 4.
#
#   make CROSS=arm-linux-gnueabihf-
#
# vncserve/ is delegated to, not built here, and the delegation runs in BOTH
# directions: this is the directory a developer types `make` in, so `all` has to
# reach the viewer, and `test` has to reach its tests. See vncserve/Makefile for
# why that build is on the unit rather than cross-compiled.

CROSS ?= arm-linux-gnueabihf-
CC    := $(CROSS)gcc

.PHONY: all clean test vncserve

all: timeout vncserve

timeout: timeout.c
	$(CC) -O2 -static -Wall -Wextra -o $@ $<

# Compiled with the HOST compiler, deliberately, and it is the one target here that
# ignores CROSS: vncserve is built by the unit's own gcc at install time, so a
# cross-built copy would be a second, differently-flavoured binary that nothing
# deploys. Building it here at all is so that `make` catches a compile error on the
# workstation instead of at install time on the unit.
vncserve:
	$(MAKE) -C vncserve

# The pure-module tests, run natively (vncserve/Makefile explains why they are not
# soft-float under qemu like scripts/shims/'s), plus bootscreen.py's own suite --
# which, unusually for this tree, runs the daemon as a real subprocess against a
# plain file standing in for /dev/fb0 (every path it touches is env-overridable).
test:
	python3 test_bootscreen.py
	$(MAKE) -C vncserve test

clean:
	rm -f timeout
	$(MAKE) -C vncserve clean
