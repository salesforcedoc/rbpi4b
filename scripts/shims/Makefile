# rblive4 shim build
#
# All shims must be soft-float EABI5 and reference only GLIBC_2.4/2.7, because
# they are LD_PRELOADed into the XDJ-RX3 soft-float glibc-2.13 chroot.
#
# Usage:
#   make RX3=/path/to/extracted/XDJRX3-rootfs
#   make clean
#
# The RX3 rootfs supplies libc/libdl/librt/pthread so that symbol versioning
# matches the target (modern host glibc would pull in GLIBC_2.17+).  What
# actually achieves that is the -L$(RX3LIB) -L$(RX3ULIB) in LDFLAGS below, NOT
# the --sysroot: GCC's built-in directories come after every -L, so a link with
# only --sysroot still resolves libc against the build machine's glibc and
# stamps references GLIBC_2.34.  See tools/build-directfb/README.md.

CROSS   ?= arm-linux-gnueabi-
CC      := $(CROSS)gcc
STRIP   ?= $(CROSS)strip

RX3     ?= ../../extracted/XDJRX3-rootfs
RX3LIB  := $(RX3)/lib
RX3ULIB := $(RX3)/usr/lib

# The RX3 runtime rootfs has no libc_nonshared.a / libpthread_nonshared.a
# (dev-only archives); give the linker empty stubs so -lpthread links.
COMPAT  ?= compat

CFLAGS  := -O2 -march=armv5t -mfloat-abi=soft -fno-stack-protector -fPIC \
           -Wall -Wextra -Wno-unused-parameter
LDFLAGS := -L$(COMPAT) -L$(RX3LIB) -L$(RX3ULIB) -Wl,-rpath-link,$(RX3LIB):$(RX3ULIB)

$(COMPAT)/libc_nonshared.a $(COMPAT)/libpthread_nonshared.a:
	@mkdir -p $(COMPAT)
	@ar rcs $(COMPAT)/libc_nonshared.a
	@ar rcs $(COMPAT)/libpthread_nonshared.a

# Shared LD_PRELOAD libraries (soft-float, libc-only NEEDED)
SHARED  := knobshim.so audioshim.so fbshim.so crashcatch.so

# Static ARM helpers (have no soft-float runtime dependency)
STATIC  := seqinject2 udplog

# Retained as reference only, NOT built by `all`: superseded by the shims above
# and, on the Pi, by the pointer path in fbshim.so.
REFERENCE := gpioshim.so tscshim.so

.PHONY: all reference clean check test
all: $(SHARED) $(STATIC)

# Build the reference-only shims explicitly if you need them.
reference: $(REFERENCE)

# Without this the default goal is the first rule in the file -- the
# compat/libc_nonshared.a rule below -- so a bare `make` silently builds nothing.
.DEFAULT_GOAL := all

# shmstate.o holds the globals shared with the audio shim. It is linked into the
# CONTROLS shim only: that shim is first in LD_PRELOAD, so its definitions win
# in the global scope. Linking it into a consumer as well would give that
# consumer a private copy it would silently bind to -- see shmstate.h.
shmstate.o: shmstate.c shmstate.h
	$(CC) $(CFLAGS) -c -o $@ $<

# knobshim.so: the control surface, split into modules.
#
#   ctrlshim.o   the front end: environment, map selection, the threads, and the
#                MIDI_DUMP recorder / MIDI_REPLAY player
#   ctrl_map.o   the binding tables every map fills, and their invalidation
#   map_flx4.o   the DDJ-FLX4 surface: the target's own tables (unverified --
#                from Pioneer's published MIDI list, see the file's header)
#   map_jp21.o   the SC Live 4 / Prime GO surface: its notes, CCs and timing
#   map_kbd.o    the keyboard fallback: evdev keycodes, no controller, no MIDI
#   evdev_io.o   the non-MIDI event source (/dev/input/event*), which knows no
#                keycode -- a map asks for it through ctrl_map.h's devices()
#   mididump.o   the dump format: record a surface, replay it with no hardware
#   rbp_bridge.o everything that resolves or calls into one of rbp's objects
#                (KeyManager, MixerEngine, beat loop, the meter hook)
#   rbp_led.o    panel LEDs, from rbp's own LedStat table and engine state
#   rbp_vu.o     panel meters, from rbp's own meter bitmasks
#   midi_io.o    the sequencer (in) and the surface's LED/meter port (out)
#   shimutil.o   the log, the clock, and the environment contract
#
# The split exists so that the part of this shim that is rbp (rbp_bridge) can be
# read and re-derived on its own, and so the part that is a particular controller
# (the maps, map_*.c) can be swapped without touching either.
#
# Every object except shmstate.o is compiled -fvisibility=hidden. An LD_PRELOAD
# library's global symbols enter the process's dynamic scope and can interpose
# anything; the modules call each other directly, so nothing needs to be visible
# except the three things rbp itself resolves: its IPowerManager stubs and the
# meter hook's getled_hook (annotated in the sources), and shmstate.o's globals,
# which the audio shim reads out of the global scope.
CTRL_OBJS  := ctrlshim.o ctrl_map.o map_flx4.o map_jp21.o map_kbd.o evdev_io.o \
              mididump.o rbp_bridge.o rbp_led.o rbp_vu.o midi_io.o shimutil.o
CTRL_HDRS  := rbp_abi.h rbp_bridge.h rbp_led.h rbp_vu.h midi_io.h shimutil.h \
              ctrl_map.h mididump.h evdev_io.h syscalls.h shmstate.h

knobshim.so: $(CTRL_OBJS) shmstate.o $(CTRL_HDRS) \
             $(COMPAT)/libc_nonshared.a $(COMPAT)/libpthread_nonshared.a
	$(CC) $(CFLAGS) -shared -o $@ $(CTRL_OBJS) shmstate.o -lpthread -lm -lc $(LDFLAGS)

$(CTRL_OBJS): %.o: %.c $(CTRL_HDRS)
	$(CC) $(CFLAGS) -fvisibility=hidden -c -o $@ $<

# map_jp21.o's initializer predates the map's non-MIDI members (ctrl_map.h's
# devices()/input(), added after tick for the keyboard fallback), so it stops
# short of the struct's end and -Wextra's -Wmissing-field-initializers fires on
# a zero-initialised suffix that is exactly right. The map is extracted JP21
# behaviour and is not the file to re-indent, so the warning is off here rather
# than project-wide, where it does catch a real omitted field. Scoped to one
# file, like fb_shim.o's below.
map_jp21.o: CFLAGS += -Wno-missing-field-initializers

# audioshim must bind dlopen/dlsym to libdl GLIBC_2.4 (the .symver directives
# are in the source); link the RX3 libdl explicitly. It deliberately does NOT
# link shmstate.o -- it reads those globals out of the global scope.
#
# s24pack.o is separate because it is pure: no ALSA, no allocation, no globals.
# That is what lets test_audio.c exercise the real packing code on a host with no
# card and no chroot. The deps list is spelled out per object rather than left to
# the pattern rule, because a header edit that no target depends on is a rebuild
# that silently does not happen.
audioshim.so: audioshim.o s24pack.o
	$(CC) $(CFLAGS) -shared -o $@ audioshim.o s24pack.o \
	    $(RX3LIB)/libdl.so.2 -lc $(LDFLAGS)

audioshim.o: audioshim.c shmstate.h envutil.h s24pack.h
s24pack.o: s24pack.c s24pack.h

# fbshim.so: the framebuffer lies and the fake touch controller rbp reads.
#
# Four objects into one library, and the separation is deliberate: exactly one of
# them (fb_shim.o) may define the interposed libc symbols. Splitting the other
# way -- several .so files each defining open() -- would make the LD_PRELOAD
# order decide which open() ran, which is the fragility this split removes.
#
#   fb_shim.o    open/open64/read/close/poll/ioctl, the fb ioctl lies, /dev/mem,
#                the gpiodrv stubs, the FBIOPAN_DISPLAY pacer
#   tscfake.o    the fake /dev/tsc2007_2-0048: rbp ABI, target-independent
#   pointsrc.o   evdev discovery + the absolute and relative reader loops
#   point_xform.o  raw -> logical coordinate math (pure, unit-tested)
FBSHIM_OBJS := fb_shim.o tscfake.o pointsrc.o point_xform.o
FBSHIM_HDRS := tscfake.h pointsrc.h point_xform.h syscalls.h envutil.h

fbshim.so: $(FBSHIM_OBJS) $(FBSHIM_HDRS) \
           $(COMPAT)/libc_nonshared.a $(COMPAT)/libpthread_nonshared.a
	$(CC) $(CFLAGS) -shared -o $@ $(FBSHIM_OBJS) -lpthread -lm -lc $(LDFLAGS)

fb_shim.o: fb_shim.c $(FBSHIM_HDRS)
tscfake.o: tscfake.c tscfake.h pointsrc.h
# An interposer has to look at the arguments it is handed — that is the job — so
# glibc's `nonnull` on open()'s pathname and `access(write_only)` on poll()'s
# array are describing callers that conform, which is exactly what a shim cannot
# assume. Both warnings are false positives here; the original combined
# fbshim-tsc.c produced the same set. Scoped to this one file rather than turned
# off project-wide, where -Wmaybe-uninitialized does catch real bugs.
fb_shim.o: CFLAGS += -Wno-nonnull-compare -Wno-maybe-uninitialized

pointsrc.o: pointsrc.c pointsrc.h point_xform.h tscfake.h
point_xform.o: point_xform.c point_xform.h

# The one rule for the four objects above. -Wall -Wextra is on, so a genuinely
# unused helper fails the build rather than lingering.
%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

gpioshim.so: gpioshim.c
	$(CC) $(CFLAGS) -shared -o $@ $< -lc $(LDFLAGS)

tscshim.so: tscshim.c $(COMPAT)/libc_nonshared.a $(COMPAT)/libpthread_nonshared.a
	$(CC) $(CFLAGS) -shared -o $@ $< -lpthread -lm -lc $(LDFLAGS)

crashcatch.so: crashcatch.c
	$(CC) $(CFLAGS) -shared -o $@ $< -lc $(LDFLAGS)

seqinject2: seqinject2.c
	$(CC) -O2 -march=armv5t -mfloat-abi=soft -static -o $@ $<

udplog: udplog.c
	$(CC) -O2 -march=armv5t -mfloat-abi=soft -static -o $@ $<

# --- unit tests ---------------------------------------------------------------
#
# All three run on a real ARM binary under qemu-arm, with no device, no Pi and no
# rootfs to load. Built STATIC, like the other ARM helpers.
#
#   make test            (needs qemu-arm in PATH)
#
# test_point.c provides its own pointsrc_start(), so the pointer source does not
# need simulating: the test is about what rbp receives, not where it came from.
# test_audio.c pins what the audio path emits byte by byte — the formats, the
# extremes, the refusals — which is the half of the audio port that is checkable
# without a DDJ-FLX4 on the other end.
# test_midi.c replays a fixture through the real JP21 map, which is the controls
# port's equivalent: see the comment at the top of it.
# test_flx4.c is the same discipline against the FLX4 map -- the target's own
# surface -- with one difference worth knowing: its assertions are pins on a
# table written from a vendor document, so they prove the map matches ITSELF,
# not that it matches the unit. See that file's header.
# test_kbd.c drives synthetic evdev triples through the real keyboard map and
# pins the rbp keycodes that come out. evdev_io.o is deliberately NOT linked
# into it: opening /dev/input is the one thing in this port that cannot be
# exercised without a device.
TESTS := test_point test_audio test_midi test_flx4 test_kbd

test_point: test_point.c tscfake.c point_xform.c $(FBSHIM_HDRS)
	$(CC) -O2 -march=armv5t -mfloat-abi=soft -fno-stack-protector \
	    -Wall -Wextra -static -o $@ test_point.c tscfake.c point_xform.c

test_audio: test_audio.c s24pack.c s24pack.h
	$(CC) -O2 -march=armv5t -mfloat-abi=soft -fno-stack-protector \
	    -Wall -Wextra -static -o $@ test_audio.c s24pack.c

# test_midi links the PRODUCTION objects -- map_jp21.o, ctrl_map.o, mididump.o,
# shimutil.o, shmstate.o -- rather than copies of them, so it cannot pass against
# a version of the map that is not the one that ships. test_midi.c supplies the
# stubs that keep the map off the device: the rbp bridge, the LED and meter
# modules' globals, and `verbose`.
#
# MIDI_FIXTURE is absolute rather than relative to the working directory, so
# `make -C scripts/shims test` works from the repository root as well.
MIDI_TEST_OBJS := test_midi.o map_jp21.o ctrl_map.o mididump.o shimutil.o shmstate.o

test_midi: $(MIDI_TEST_OBJS) tests/midi_jp21.dump
	$(CC) -O2 -march=armv5t -mfloat-abi=soft -fno-stack-protector \
	    -Wall -Wextra -static -o $@ $(MIDI_TEST_OBJS) -lpthread -lm -lc

test_midi.o: test_midi.c ctrl_map.h mididump.h shmstate.h rbp_abi.h rbp_bridge.h \
             rbp_led.h rbp_vu.h
	$(CC) -O2 -march=armv5t -mfloat-abi=soft -fno-stack-protector \
	    -Wall -Wextra -c -o $@ $< \
	    -DMIDI_FIXTURE='"$(CURDIR)/tests/midi_jp21.dump"'

# test_flx4 links the PRODUCTION FLX4 map and the production binding tables, for
# the same reason test_midi links the JP21 ones. It supplies the stubs that keep
# the map off the device (the rbp bridge, the meter module's fader globals,
# `verbose`), and shmstate.o like test_midi, because the headphone knobs write
# g_cue_mix/g_cue_gain -- state the audio shim reads. flx4_startup() is NOT part
# of what it exercises: it writes two absolute rbp addresses (see the file), and
# an address that is not mapped in this process is a segfault, not a failed
# check. That is the same reason the JP21 fixture avoids the events that reach an
# absolute address.
FLX4_TEST_OBJS := test_flx4.o map_flx4.o ctrl_map.o mididump.o shimutil.o shmstate.o

test_flx4: $(FLX4_TEST_OBJS) tests/midi_flx4.dump
	$(CC) -O2 -march=armv5t -mfloat-abi=soft -fno-stack-protector \
	    -Wall -Wextra -static -o $@ $(FLX4_TEST_OBJS) -lpthread -lm -lc

test_flx4.o: test_flx4.c ctrl_map.h mididump.h shmstate.h rbp_abi.h rbp_bridge.h \
             rbp_vu.h
	$(CC) -O2 -march=armv5t -mfloat-abi=soft -fno-stack-protector \
	    -Wall -Wextra -c -o $@ $< \
	    -DFLX4_FIXTURE='"$(CURDIR)/tests/midi_flx4.dump"'

# test_kbd links the PRODUCTION map (map_kbd.o) and the production binding
# tables (ctrl_map.o), for the same reason test_midi does: the test must be
# about the map that ships. It supplies the one stub map_kbd.c needs
# (send_rx_key) and the `verbose` flag; it needs no shmstate.o, because a
# keyboard writes no gain -- that is the point of the map.
KBD_TEST_OBJS := test_kbd.o map_kbd.o ctrl_map.o shimutil.o

test_kbd: $(KBD_TEST_OBJS)
	$(CC) -O2 -march=armv5t -mfloat-abi=soft -fno-stack-protector \
	    -Wall -Wextra -static -o $@ $(KBD_TEST_OBJS) -lpthread -lm -lc

test_kbd.o: test_kbd.c ctrl_map.h rbp_abi.h rbp_bridge.h
	$(CC) -O2 -march=armv5t -mfloat-abi=soft -fno-stack-protector \
	    -Wall -Wextra -c -o $@ $<

test: $(TESTS)
	@for t in $(TESTS); do echo "-- $$t"; qemu-arm ./$$t || exit 1; done

# Fail the build if any GLIBC version above 2.7 or a hard-float tag sneaks in.
#
# The exit status is the point.  This printed the version list and left the
# reading to a human, which is how a GLIBC_2.34 reference (pthread/dlfcn, which
# glibc folded into libc at 2.34) can sit in a shim that then refuses to load in
# the chroot.  objdump -T is used rather than nm because nm -D prints a
# versioned symbol as name@GLIBC_x.y, so a name-based grep matches only the
# unversioned references and never sees a version at all.
check: $(SHARED)
	@echo "== GLIBC symbol versions =="
	@rc=0; for f in $(SHARED); do \
	  echo "-- $$f"; \
	  v=$$($(CROSS)objdump -T $$f 2>/dev/null | grep -o 'GLIBC_[0-9.]*' | sort -uV); \
	  echo "$$v" | sed 's/^/   /'; \
	  if echo "$$v" | grep -qE 'GLIBC_(2\.([89]|[1-9][0-9])|[3-9])'; then \
	    echo "   !! FAIL: references a symbol version newer than the target's 2.13"; rc=1; \
	  fi; \
	  $(CROSS)readelf -A $$f | grep -i 'Tag_ABI_VFP_args' && { echo "  !! HARD-FLOAT"; rc=1; } || true; \
	done; exit $$rc

clean:
	rm -f $(SHARED) $(STATIC) $(REFERENCE) $(TESTS) *.o
