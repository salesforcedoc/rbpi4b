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

# rbp_key.o is the second object both libraries link: the key path (are we rbp,
# where its KeyManager is, how to send it a keycode -- see rbp_key.h). It is
# deliberately NOT in either object list below, because those two lists are
# compiled by two rules with two different flag sets and this object has to be
# built the same way for both: hidden.
#
# Hidden even though every other fbshim object is not, and that is not an
# oversight -- fb_shim.o must export its interposers (open/read/close/poll) or
# LD_PRELOAD interposes nothing, and the one pattern rule covers the whole
# fbshim list. This object has no such excuse, and fbshim is FIRST in
# LD_PRELOAD, so an exported send_rx_key here would be the copy knobshim's maps
# bound to. It is the same compiled code in both libraries either way, so the
# two copies could not disagree -- the way to keep that from mattering is not to
# offer the name at all. `nm -D knobshim.so fbshim.so | grep send_rx_key` is
# silent; if it ever is not, this rule stopped applying.
rbp_key.o: rbp_key.c rbp_key.h rbp_abi.h syscalls.h
	$(CC) $(CFLAGS) -fvisibility=hidden -c -o $@ $<

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
#                (MixerEngine, beat loop, the meter hook). The KeyManager and
#                the keycode send path are rbp_key.o, above -- both shims link
#                that one, because the pointer path is where a QUANTIZE tap is
#                seen and the keycode is what it becomes.
#   rbp_led.o    panel LEDs, from rbp's own LedStat table and engine state
#   rbp_vu.o     panel meters, from rbp's own meter bitmasks
#   midi_io.o    the sequencer (in) and the surface's LED/meter port (out)
#   shimutil.o   the log, the clock, and the environment contract
#
# The split exists so that the part of this shim that is rbp (rbp_bridge) can be
# read and re-derived on its own, and so the part that is a particular controller
# (the maps, map_*.c) can be swapped without touching either.
#
# Every object in the list above except shmstate.o is compiled
# -fvisibility=hidden. An LD_PRELOAD library's global symbols enter the
# process's dynamic scope and can interpose anything; the modules call each
# other directly, so nothing needs to be visible except the three things rbp
# itself resolves: its IPowerManager stubs and the meter hook's getled_hook
# (annotated in the sources), and shmstate.o's globals, which the audio shim
# reads out of the global scope. (rbp_key.o is the third object outside this
# list; it is hidden too, and says why in its own rule above.)
CTRL_OBJS  := ctrlshim.o ctrl_map.o map_flx4.o map_jp21.o map_kbd.o evdev_io.o \
              mididump.o rbp_bridge.o rbp_led.o rbp_vu.o midi_io.o shimutil.o
CTRL_HDRS  := rbp_abi.h rbp_bridge.h rbp_key.h rbp_led.h rbp_vu.h midi_io.h \
              shimutil.h ctrl_map.h mididump.h evdev_io.h syscalls.h shmstate.h

knobshim.so: $(CTRL_OBJS) rbp_key.o shmstate.o $(CTRL_HDRS) \
             $(COMPAT)/libc_nonshared.a $(COMPAT)/libpthread_nonshared.a
	$(CC) $(CFLAGS) -shared -o $@ $(CTRL_OBJS) rbp_key.o shmstate.o -lpthread -lm -lc $(LDFLAGS)

$(CTRL_OBJS): %.o: %.c $(CTRL_HDRS)
	$(CC) $(CFLAGS) -fvisibility=hidden -c -o $@ $<

# A note on what used to be here: map_jp21.o carried
# -Wno-missing-field-initializers, because the map's initializer predated the
# non-MIDI members (ctrl_map.h's devices()/input()) and stopped short of the
# struct's end. The LED table gave that initializer reason to spell out every
# member, so the suppression is gone and -Wmissing-field-initializers now guards
# this file like the rest.

# audioshim must bind dlopen/dlsym to libdl GLIBC_2.4 (the .symver directives
# are in the source); link the RX3 libdl explicitly. It deliberately does NOT
# link shmstate.o -- it reads those globals out of the global scope.
#
# s24pack.o is separate because it is pure: no ALSA, no allocation, no globals.
# That is what lets test_audio.c exercise the real packing code on a host with no
# card and no chroot. The deps list is spelled out per object rather than left to
# the pattern rule, because a header edit that no target depends on is a rebuild
# that silently does not happen.
#
# -lm is for mirror_policy.o's mirror_boost_gain(), and it is the RX3's libm rather
# than the build host's for the same reason -lc is the RX3's libc: the -L paths
# below put $(RX3)/lib first, and that rootfs ships libm-2.13.so with the dev
# symlink in usr/lib. knobshim.so and fbshim.so have linked -lm this way since
# rbp_vu.c's log10() and rbp_led.c. A host libm here would stamp GLIBC_2.29+
# references and the shim would refuse to load in the glibc-2.13 chroot.
audioshim.so: audioshim.o s24pack.o mirror_policy.o master_policy.o \
              pace_policy.o
	$(CC) $(CFLAGS) -shared -o $@ audioshim.o s24pack.o mirror_policy.o \
	    master_policy.o pace_policy.o $(RX3LIB)/libdl.so.2 -lm -lc $(LDFLAGS)

audioshim.o: audioshim.c shmstate.h envutil.h s24pack.h mirror_policy.h \
             master_policy.h pace_policy.h
s24pack.o: s24pack.c s24pack.h

# mirror_policy.o is the HDMI mirror's decisions (which candidates a device list
# yields, what a writei() return means, when a down device may be retried),
# split out of audioshim.c for the same reason s24pack.c was: the shim itself
# cannot be linked into a test, so anything left inside it is only checkable by a
# drill on the Pi. It is pure -- no ALSA, no allocation, no globals, no clock --
# which is what lets test_audio.c pin the verdicts on the host. The header is on
# audioshim.o's deps above as well, because a policy change that no object
# depends on is a rebuild that silently does not happen.
mirror_policy.o: mirror_policy.c mirror_policy.h

# master_policy.o is the same split applied to the master: the device chain it
# walks and what one writei() return on it means. It is the module the -EINVAL
# wedge came out of -- with no FLX4 the chain ended at the bare name `default`,
# which OPENS and then refuses every write, and the shim believed it was up
# because it had a handle. Both halves of that are decisions, so both are pinned
# in test_audio.c on the host rather than read off a log after a drill on the
# unit. Pure in the same sense as mirror_policy.c: no ALSA, no allocation, no
# globals, no clock.
master_policy.o: master_policy.c master_policy.h

# fbshim.so: the framebuffer lies and the fake touch controller rbp reads.
#
# Several objects into one library, and the separation is deliberate: exactly one
# of them (fb_shim.o) may define the interposed libc symbols. Splitting the other
# way -- several .so files each defining open() -- would make the LD_PRELOAD
# order decide which open() ran, which is the fragility this split removes.
#
#   fb_shim.o    open/open64/read/close/poll/ioctl, the fb ioctl lies, /dev/mem,
#                the gpiodrv stubs, the FBIOPAN_DISPLAY pacer
#   tscfake.o    the fake /dev/tsc2007_2-0048: rbp ABI, target-independent
#   pointsrc.o   evdev discovery + the absolute and relative reader loops
#   point_xform.o  raw -> logical coordinate math (pure, unit-tested)
#   touch_zone.o   the two deck QUANTIZE boxes, and the press that stands for
#                  them (pure, unit-tested). The one place a pointer position
#                  means something to the shim rather than to rbp -- see the
#                  header -- and it reaches rbp through rbp_key.o.
#   fb_cursor.o    the visible pointer: maps the real /dev/fb0 and composites
#                  an arrow at the live pointer position
#   cursor_paint.o the glyph and the save-under rule (pure, unit-tested)
#   menu_zone.o    the swipe-down top menu: the strip, the panel, the six buttons
#                  and the gesture, and the one answer the pointer loop needs --
#                  does rbp get to see this touch? (pure, unit-tested)
#   menu_paint.o   the panel's image: six labelled buttons at exact pixel values
#                  (pure, unit-tested). The second thing this shim draws.
#   menu_draw.o    the panel's driver: maps the real /dev/fb0 and keeps the panel
#                  on it from a thread of its own (fb_cursor.c's shape)
#
# This is the SECOND shim to reach rbp: pointsrc.c turns a recognized tap into
# the keycode rbp has for that button (send_rx_key, rbp_key.o below). It is a
# real change to a property fbshim had -- it referenced no rbp address at all --
# which is why the one object it needs is a leaf with its own rule rather than
# rbp_bridge.o, whose meter hook must not exist in two copies.
FBSHIM_OBJS := fb_shim.o tscfake.o pointsrc.o point_xform.o touch_zone.o \
               fb_cursor.o cursor_paint.o menu_zone.o menu_paint.o menu_draw.o
FBSHIM_HDRS := tscfake.h pointsrc.h point_xform.h touch_zone.h syscalls.h \
               envutil.h fb_cursor.h cursor_paint.h fbdev.h menu_zone.h \
               menu_paint.h menu_font.h menu_draw.h

fbshim.so: $(FBSHIM_OBJS) rbp_key.o $(FBSHIM_HDRS) \
           $(COMPAT)/libc_nonshared.a $(COMPAT)/libpthread_nonshared.a
	$(CC) $(CFLAGS) -shared -o $@ $(FBSHIM_OBJS) rbp_key.o -lpthread -lm -lc $(LDFLAGS)

fb_shim.o: fb_shim.c $(FBSHIM_HDRS)
tscfake.o: tscfake.c tscfake.h pointsrc.h
# An interposer has to look at the arguments it is handed — that is the job — so
# glibc's `nonnull` on open()'s pathname and `access(write_only)` on poll()'s
# array are describing callers that conform, which is exactly what a shim cannot
# assume. Both warnings are false positives here; the original combined
# fbshim-tsc.c produced the same set. Scoped to this one file rather than turned
# off project-wide, where -Wmaybe-uninitialized does catch real bugs.
fb_shim.o: CFLAGS += -Wno-nonnull-compare -Wno-maybe-uninitialized

pointsrc.o: pointsrc.c pointsrc.h point_xform.h tscfake.h touch_zone.h \
            rbp_key.h rbp_abi.h
point_xform.o: point_xform.c point_xform.h
touch_zone.o: touch_zone.c touch_zone.h
fb_cursor.o: fb_cursor.c fb_cursor.h cursor_paint.h fbdev.h pointsrc.h point_xform.h
cursor_paint.o: cursor_paint.c cursor_paint.h
# menu_zone.h on menu_paint.o's line as well as its own .c's: the painter derives
# every rectangle from that header's logical geometry, so a change to a literal
# there has to rebuild it. A dependency that is not written down is a rebuild that
# silently does not happen.
menu_zone.o: menu_zone.c menu_zone.h
menu_paint.o: menu_paint.c menu_paint.h menu_zone.h menu_font.h
# menu_draw.o is the only object that measures the framebuffer; tscfake.o's own
# rule above already covers menu_draw.h through $(FBSHIM_HDRS), which is where its
# header is listed.
menu_draw.o: menu_draw.c menu_draw.h menu_paint.h menu_zone.h fbdev.h \
             point_xform.h pointsrc.h

# The one rule for the objects above. -Wall -Wextra is on, so a genuinely
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

# pcmprobe is a diagnostic, not a shim: NOT built by `all` and never shipped. It
# links the chroot's libasound — the same copy the audio shim dlopens — because
# the question it answers is "which integer is S24_3LE, and what does this card
# accept", and the answer has to come from the library that will be asked. Run the
# enum half under qemu-arm; run the device half inside the chroot on the Pi, where
# /dev/snd is bound in:
#
#   make -C scripts/shims pcmprobe
#   qemu-arm -L $(RX3) scripts/shims/pcmprobe
#   chroot /opt/rblive4/rbx3-run /tmp/pcmprobe hw:CARD=DDJFLX4,DEV=0
#
# It exists because one of the shim's hand-rolled SND_PCM_FORMAT_* values was
# wrong and surfaced only as a nameless -EINVAL from the card. See tools/pcmprobe.c.
pcmprobe: ../../tools/pcmprobe.c
	$(CC) $(CFLAGS) -o $@ $< -lasound $(LDFLAGS)

# --- unit tests ---------------------------------------------------------------
#
# All of them run as a real ARM binary under qemu-arm, with no device, no Pi and
# no rootfs to load. Built STATIC, like the other ARM helpers.
#
#   make test            (needs qemu-arm in PATH)
#
# test_point.c provides its own pointsrc_start(), so the pointer source does not
# need simulating: the test is about what rbp receives, not where it came from.
# It links touch_zone.c for the same reason it links point_xform.c -- the zone
# is a pure decision about a coordinate, so it is pinned here rather than by a
# finger on the unit. (The other half of the gesture, the keycode send, is
# pointsrc.c's one line of rbp work and is not reachable from a test at all:
# rbp_key.c holds absolute addresses.)
# test_audio.c pins what the audio path emits byte by byte — the formats, the
# extremes, the refusals — which is the half of the audio port that is checkable
# without a DDJ-FLX4 on the other end. It also pins the HDMI mirror's policy
# (mirror_policy.c) and the master's (master_policy.c), which is the same trick
# applied to both: their verdicts are arithmetic, so they are checked here rather
# than read off a log line after a drill. The one part of the mirror NOT covered
# here is the fill in flush_master(), which lives in the shim; see docs/09-audio.md.
# The same is true of the master's write site, which uses master_policy.c's
# verdicts but is itself unreachable from a test.
# test_midi.c replays a fixture through the real JP21 map, which is the controls
# port's equivalent: see the comment at the top of it.
# test_flx4.c is the same discipline against the FLX4 map -- the target's own
# surface -- with one difference worth knowing: its assertions are pins on a
# table written from a vendor document, so they prove the map matches ITSELF,
# not that it matches the unit. See that file's header.
# test_kbd.c drives synthetic evdev triples through the real keyboard map and
# pins the rbp keycodes that come out. evdev_io.o is deliberately NOT linked
# into it: test_kbd is about the MAP, so it hands the map the triples itself and
# keeps the reader out of the picture.
# test_evdev.c is what covers the reader, and it links the PRODUCTION evdev_io.o
# the same way test_kbd links the production map. What is faked there is not the
# module but the kernel: -Wl,--wrap=syscall (plus --wrap=usleep and
# --wrap=clock_gettime, for a virtual clock) turns /dev/input, poll() and the
# sleep into a script. That is possible only because evdev_io.c reaches the
# kernel through syscalls.h's `static inline` real_* helpers, which all go
# through the one public syscall() symbol -- and because glibc's own internal
# syscalls do NOT, so the fake sees this module's calls and no one else's. See
# the header of test_evdev.c; a --wrap against a static link is the sort of
# thing that works by luck, and the spike that confirmed it is recorded there.
# test_cursor.c links the PRODUCTION glyph and compositing rule, because the rule
# it pins -- that a restore only puts back a pixel that is still ours -- is
# exactly the kind of thing that would be rewritten in a copy. Its surface is a
# local array: no framebuffer, no thread, no device.
# test_cursor_dev.c is the same seam as test_evdev applied to the pointer's
# *output* side, and it is fb_cursor.c's first coverage of any kind: test_cursor
# pins the glyph and stops exactly where the device begins. The production
# fb_cursor.o is linked and the kernel is scripted, so what is pinned is the retry
# and the rectangle -- see that file's header.
TESTS := test_point test_audio test_midi test_flx4 test_kbd test_cursor \
         test_evdev test_cursor_dev test_menu test_menu_dev

test_point: test_point.c tscfake.c point_xform.c touch_zone.c $(FBSHIM_HDRS)
	$(CC) -O2 -march=armv5t -mfloat-abi=soft -fno-stack-protector \
	    -Wall -Wextra -static -o $@ test_point.c tscfake.c point_xform.c \
	    touch_zone.c

# test_menu links the PRODUCTION menu_zone.c and menu_paint.c -- not copies, and
# not the compiled objects either, so the test needs no cross-built .o to run. Its
# framebuffer is a local array: there is no device, no thread and no rbp anywhere
# in it, which is the whole reason both modules are pure. What it stops is a
# gesture rule or a column boundary that is only ever checked by a finger.
test_menu: test_menu.c menu_zone.c menu_paint.c menu_zone.h menu_paint.h \
           menu_font.h
	$(CC) -O2 -march=armv5t -mfloat-abi=soft -fno-stack-protector \
	    -Wall -Wextra -static -o $@ test_menu.c menu_zone.c menu_paint.c

test_audio: test_audio.c s24pack.c s24pack.h mirror_policy.c mirror_policy.h \
            master_policy.c master_policy.h pace_policy.c pace_policy.h
	$(CC) -O2 -march=armv5t -mfloat-abi=soft -fno-stack-protector \
	    -Wall -Wextra -static -o $@ test_audio.c s24pack.c mirror_policy.c \
	    master_policy.c pace_policy.c -lm

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

# FORCE, because MIDI_FIXTURE is `$(CURDIR)` made absolute (see above) and make
# has no way to see that value change: a .o left over from a build under a
# different mount root carries the old path, is never rebuilt, and fails at run
# time with `cannot open the fixture /w/scripts/shims/tests/midi_jp21.dump` --
# which reads as a missing fixture, not as a stale object. Both fixture-baking
# objects are therefore rebuilt on every `make`, which costs about a second.
test_midi.o: FORCE test_midi.c ctrl_map.h mididump.h shmstate.h rbp_abi.h \
             rbp_bridge.h rbp_led.h rbp_vu.h
	$(CC) -O2 -march=armv5t -mfloat-abi=soft -fno-stack-protector \
	    -Wall -Wextra -c -o $@ test_midi.c \
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
#
# map_jp21.o is here for one assertion: the FLX4's LED table must not carry any
# of the SC Live 4's (channel, note) pairs, and that comparison needs both
# tables. It costs one object and no new stubs -- the two maps' rbp calls are the
# same set, already stubbed below.
FLX4_TEST_OBJS := test_flx4.o map_flx4.o map_jp21.o ctrl_map.o mididump.o \
                  shimutil.o shmstate.o

test_flx4: $(FLX4_TEST_OBJS) tests/midi_flx4.dump
	$(CC) -O2 -march=armv5t -mfloat-abi=soft -fno-stack-protector \
	    -Wall -Wextra -static -o $@ $(FLX4_TEST_OBJS) -lpthread -lm -lc

# FORCE, for the same reason as test_midi.o above: FLX4_FIXTURE bakes $(CURDIR).
test_flx4.o: FORCE test_flx4.c ctrl_map.h mididump.h shmstate.h rbp_abi.h \
             rbp_bridge.h rbp_vu.h
	$(CC) -O2 -march=armv5t -mfloat-abi=soft -fno-stack-protector \
	    -Wall -Wextra -c -o $@ test_flx4.c \
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

# test_evdev links the PRODUCTION evdev_io.o (and shimutil.o, for klog/env_text),
# with the syscall boundary wrapped. The three wraps are the whole seam:
#
#   syscall           /dev/input/event*, ioctl, read, poll, close and the log's
#                     write all come through here (syscalls.h)
#   usleep            the reader's only sleeps; wrapped so they cost no real time
#   clock_gettime     a virtual clock, because the property under test is a
#                     LATENCY (shim_now_ms is clock_gettime)
#
# The five scenarios fork, so each gets a virgin reader; see the file's header.
# No device, no rootfs, no Pi: `qemu-arm ./test_evdev` is the whole invocation.
EVD_TEST_OBJS := test_evdev.o evdev_io.o shimutil.o

test_evdev: $(EVD_TEST_OBJS)
	$(CC) -O2 -march=armv5t -mfloat-abi=soft -fno-stack-protector \
	    -Wall -Wextra -static -o $@ $(EVD_TEST_OBJS) \
	    -Wl,--wrap=syscall -Wl,--wrap=usleep -Wl,--wrap=clock_gettime \
	    -lpthread -lm -lc

test_evdev.o: test_evdev.c evdev_io.h
	$(CC) -O2 -march=armv5t -mfloat-abi=soft -fno-stack-protector \
	    -Wall -Wextra -c -o $@ $<

test_cursor: test_cursor.c cursor_paint.c cursor_paint.h
	$(CC) -O2 -march=armv5t -mfloat-abi=soft -fno-stack-protector \
	    -Wall -Wextra -static -o $@ test_cursor.c cursor_paint.c

# fb_cursor.o, cursor_paint.o and point_xform.o are the PRODUCTION objects (the
# generic %.o rule above), so this cannot pass against a version of the module
# that does not ship -- which is the point, because what it pins is a behaviour
# (retry forever rather than exit; draw inside the picture, not the page) that a
# copy of the module would be free to get right on its own. test_cursor_dev.c
# supplies pointsrc_cursor() and pointsrc_log(), which is the whole of
# pointsrc.c's interface to fb_cursor.c, so no pointer device is opened.
#
# --wrap=usleep as well as --wrap=syscall, unlike test_evdev: the module's sleep
# is its only tick and the wrapped one is what stops the scripted thread. And
# NOT --wrap=clock_gettime, also unlike test_evdev: nothing fb_cursor.c,
# cursor_paint.c or point_xform.c touches reads a clock, so a virtual one would
# be machinery with nothing to hold.
CUR_TEST_OBJS := test_cursor_dev.o fb_cursor.o cursor_paint.o point_xform.o

test_cursor_dev: $(CUR_TEST_OBJS)
	$(CC) -O2 -march=armv5t -mfloat-abi=soft -fno-stack-protector \
	    -Wall -Wextra -static -o $@ $(CUR_TEST_OBJS) \
	    -Wl,--wrap=syscall -Wl,--wrap=usleep -lpthread -lm -lc

test_cursor_dev.o: test_cursor_dev.c fb_cursor.h fbdev.h cursor_paint.h \
                   point_xform.h pointsrc.h
	$(CC) -O2 -march=armv5t -mfloat-abi=soft -fno-stack-protector \
	    -Wall -Wextra -c -o $@ $<

# test_menu_dev.c is the menu's OTHER half. test_menu pins the gesture, the geometry
# and the image -- pure work against a local array -- and stops exactly where the
# device begins; this picks up there, with the same seam test_cursor_dev.c
# established: menu_draw.o, menu_paint.o, menu_zone.o and point_xform.o are the
# PRODUCTION objects (so what is pinned is the shipped driver, not a copy of it) and
# the kernel is scripted underneath them.
#
# What it pins is the part of menu_draw.c that was only ever argued: the image is
# built at map time so the panel's first appearance is a blit, the double buffer
# makes a press transition cost a tick of highlight lag and no blanking, the vsync
# hook never classifies, and the column-granular rebuild is exact at every step of a
# slide. --wrap=usleep is not a delay here but a HANDSHAKE: the thread's tick is the
# last thing one of its iterations does, so granting it one iteration at a time is
# what makes every assertion about what the module DID rather than about who won a
# race. See that file's header.
MENU_TEST_OBJS := test_menu_dev.o menu_draw.o menu_paint.o menu_zone.o \
                  point_xform.o

test_menu_dev: $(MENU_TEST_OBJS)
	$(CC) -O2 -march=armv5t -mfloat-abi=soft -fno-stack-protector \
	    -Wall -Wextra -static -o $@ $(MENU_TEST_OBJS) \
	    -Wl,--wrap=syscall -Wl,--wrap=malloc -Wl,--wrap=usleep \
	    -lpthread -lm -lc

test_menu_dev.o: test_menu_dev.c menu_draw.h menu_paint.h menu_zone.h \
                 point_xform.h pointsrc.h fbdev.h
	$(CC) -O2 -march=armv5t -mfloat-abi=soft -fno-stack-protector \
	    -Wall -Wextra -c -o $@ $<

test: $(TESTS)
	@for t in $(TESTS); do echo "-- $$t"; qemu-arm ./$$t || exit 1; done
	@echo "-- test_menu_dev (padded page stride)"
	@qemu-arm ./test_menu_dev 8 || exit 1

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
	rm -f $(SHARED) $(STATIC) $(REFERENCE) $(TESTS) pcmprobe *.o

# The always-out-of-date prerequisite that keeps the two fixture-baking objects
# above from being reused across working directories.
.PHONY: FORCE
FORCE:
