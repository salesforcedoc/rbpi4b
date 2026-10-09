# scripts/device/vncserve/Makefile
#
# vncserve IS BUILT ON THE UNIT, BY THE UNIT'S OWN gcc.
#
# This is not a preference, it is the only route. The shims are cross-compiled
# soft-float armel in the rblive4-build image; the unit's userland is hard-float
# armhf. The image has no arm-linux-gnueabihf-gcc and no hard-float libc.a, so it
# cannot produce this binary at all -- and `make all` would still exit 0, which is
# why install.sh compiles it here and aborts loudly if that fails.
#
# tools/rbrowser/Makefile is the precedent (it builds jpeg565.c the same way, for
# the same reason, and its comment says so). The pure modules are portable C, so
# `make test` also runs on the Mac -- see README.md for that deliberate difference
# from scripts/shims/, whose tests are soft-float armel under qemu.
#
#   make         build vncserve for this machine (on the unit: the real thing)
#   make test    build and run the pure-module tests natively
#   make clean

CC       ?= cc
CFLAGS   ?= -O2 -Wall -Wextra -std=gnu11
# DUMB-BUFFER mmap OFFSETS START AT 4 GiB. A 32-bit off_t truncates them to 0 and
# the plane maps silently read the wrong memory, which looks like a capture bug and
# is not. work/drmplane.c passes this flag for the same reason.
CPPFLAGS += -D_FILE_OFFSET_BITS=64

# ZLIB, FOR TIGHT'S BASIC COMPRESSION. This is the one library the program links
# that is not libc, and it is not a new package on the unit: libz.so.1 has been there
# all along and is what half the userland already loads. What the unit does NOT have
# is the development package -- there is no /usr/include/zlib.h and no libz.so for
# `-lz` to find -- so it links the soname directly. macOS has the SDK's libz and wants
# plain -lz. vnc_zlib.c checks at startup that its idea of z_stream matches whichever
# one it got, and falls back to Raw frames if it does not.
UNAME_S  := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
ZLIB     := -lz
else
ZLIB     := -l:libz.so.1
endif

SRCS  := $(wildcard *.c)
LIB   := $(filter-out vncserve.c test_%.c,$(SRCS))
TESTS := $(patsubst test_%.c,test_%,$(wildcard test_*.c))

.PHONY: all test clean
all: vncserve

vncserve: vncserve.c $(LIB)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $^ $(ZLIB)

test: $(TESTS)
	@set -e; for t in $(TESTS); do printf '== %s\n' "$$t"; ./$$t; done
	@printf 'all %d suite(s) passed\n' $(words $(TESTS))

# Each test links only the module under test, so a broken dependency shows up as a
# link error rather than being masked by the rest of the tree.
test_%: test_%.c %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $^ $(ZLIB)

clean:
	rm -f vncserve $(TESTS)
