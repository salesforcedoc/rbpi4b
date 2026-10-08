# rbpi4b shim build
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

# Shared LD_PRELOAD libraries (soft-float, libc-only NEEDED).
#
# The ORDER here is not the load order -- that is RB_LD_PRELOAD's business -- but
# it is the order `make check` reports in, and the two that carry a constraint are
# worth naming: crashcatch.so must be FIRST in the preload list (its constructor
# arms the signal handlers before anything else can fault), and netshim.so must
# be SECOND, before fbshim.so, because both define ioctl and the first one loaded
# wins. See netshim.c's header and doctor.sh.
SHARED  := knobshim.so audioshim.so fbshim.so crashcatch.so netshim.so

# The load order itself, as rb.conf ships it. This is NOT cosmetic: a preloaded
# library resolves its undefined names against the libraries loaded BEFORE it, so
# "which shim defines this" is a question with an order-dependent answer, and
# `check` needs the order to answer it. Kept here as its own list rather than
# derived from SHARED (whose order is only the report order) precisely so that a
# new shim has to be placed in it deliberately -- check asserts the two agree.
PRELOAD := crashcatch.so netshim.so fbshim.so knobshim.so audioshim.so

# Static ARM helpers (have no soft-float runtime dependency)
STATIC  := seqinject2 udplog controllers_cli

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

# fader_state.o holds the channel-fader bookkeeping -- g_fader and g_fader_seen,
# the position and "has anyone reported one" -- and it is linked into BOTH shims,
# which is the whole point: the touch drawers (fbshim, pointsrc.c) WRITE them and
# the 30 s unity seed plus the meter scaling (knobshim, rbp_vu.c) READ them. A
# definition in either shim alone would be a private copy to the other, and the
# seed would silently override the operator's fader for the first 30 s of every
# run -- see fader_state.c's header comment.
#
# BOTH, and not one plus an extern, for two reasons. `make check` fails any shim
# with an undefined symbol the target libc cannot supply, so the reader's shim
# would have to define it anyway. And with both defining it the sharing does not
# depend on which is loaded first: LD_PRELOAD order is fbshim:knobshim:audioshim,
# and a default-visibility symbol defined in two preloaded libraries resolves for
# BOTH of them to the first one in the search order -- one array either way, and
# the arrangement still works if the order is ever changed.
#
# NO -fvisibility=hidden here, unlike every object in CTRL_OBJS: hidden would leave
# the name undefined in the global scope and put the two shims back on private
# copies. The one rule below is the explicit `%.o: %.c` for this file, so the flag
# set cannot be changed by moving the object into a list.
fader_state.o: fader_state.c rbp_vu.h
	$(CC) $(CFLAGS) -c -o $@ $<

# rbp_transport.o is the FOURTH object both libraries link, and for the same reason
# as fader_state.o above: the transport state the edge drawers draw is written by
# knobshim's LED thread and read by fbshim's painter, and those are two libraries with
# fbshim FIRST in RB_LD_PRELOAD. NO -fvisibility=hidden, and NOT in either object list
# (CTRL_OBJS would hide it), exactly like fader_state.o -- the explicit rule below is
# what fixes the flag set.
#
# This one is not a tidiness choice, it is the fix for a launch failure. The first
# build declared the accessors in rbp_led.h instead, and rbp died three seconds in with
# `/root/pdj/rbp: symbol lookup error: /usr/lib/fbshim.so: undefined symbol:
# rbp_led_transport_want` -- fbshim cannot call into knobshim at all, and CTRL_OBJS's
# -fvisibility=hidden meant knobshim exported nothing to call in any case.
# rbp_transport.h carries the account. `arm-linux-gnueabi-nm -D --defined-only
# knobshim.so fbshim.so | grep rbp_transport` must show both defining it.
rbp_transport.o: rbp_transport.c rbp_transport.h
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

# usb_label.o is the THIRD object both libraries link, and for a third reason:
# knobshim writes a device's name into rbp's own record with it, and fbshim draws the
# same name on the USB STOP chooser's buttons (pointsrc.c reads the file, prompt_zone.c
# composes the label from it). Each library gets its own copy and neither needs the
# other's -- but the copy has to be hidden, so it is a member of CTRL_OBJS and there is
# NO rule here for it: the static pattern rule over CTRL_OBJS below is the one that
# builds it, with -fvisibility=hidden, and make builds a target once. A rule of its own
# here would be a second recipe for the same target -- which make warns about and
# silently resolves to the pattern rule anyway. Same reasoning as rbp_key.o's rule
# above, except that rbp_key.o is outside CTRL_OBJS and so does need its own.

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
#   led_table.o  the SETTLED view of that table (two reads that agree), and the
#                blink phase read from rbp's own period. Pure, so the filter and
#                the duty cycle are testable; see test_led_table.
#   rbp_vu.o     panel meters, from rbp's own meter bitmasks
#   midi_io.o    the sequencer (in) and the surface's LED/meter port (out)
#   controllers.o one row per control surface: the map it selects, how it is
#                found (port-name hint, card id, USB id) and the SysEx it wants.
#                Pure, so the anti-drift claim is a host test; see test_controllers.
#   shimutil.o   the log, the clock, and the environment contract
#   usb_label.o  the text rules for a media device's name: the volume label the
#                host read off a stick, turned into the 64-byte UTF-16 field one
#                of rbp's device rows draws in place of its own "USB1"/"USB2".
#                Pure -- no file, no rbp address, no global -- so the rules (a
#                shorter label's tail, an empty answer, the drawable range) are
#                pinned on a host by test_usblabel.
#
# The split exists so that the part of this shim that is rbp (rbp_bridge) can be
# read and re-derived on its own, and so the part that is a particular controller
# (the maps, map_*.c) can be swapped without touching either.
#
# controllers.o is the one object here that BOTH shims link: audioshim.c takes its
# default ALSA card from the same row midi_io.c takes its port-name hint from, and
# the whole point of the table is that those two cannot disagree. It is compiled
# by the -fvisibility=hidden pattern rule below like the rest -- nothing needs to
# resolve it across a library boundary, because each library links its own copy
# the same way rbp_key.o already does.
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
              mididump.o rbp_bridge.o rbp_led.o led_table.o plinn_scan.o \
              rbp_vu.o midi_io.o controllers.o shimutil.o usb_label.o
CTRL_HDRS  := rbp_abi.h rbp_bridge.h rbp_key.h rbp_led.h led_table.h rbp_vu.h \
              midi_io.h shimutil.h ctrl_map.h mididump.h evdev_io.h syscalls.h \
              shmstate.h plinn_scan.h controllers.h usb_label.h
knobshim.so: $(CTRL_OBJS) rbp_key.o shmstate.o fader_state.o rbp_transport.o \
             $(CTRL_HDRS) rbp_transport.h \
             $(COMPAT)/libc_nonshared.a $(COMPAT)/libpthread_nonshared.a
	$(CC) $(CFLAGS) -shared -o $@ $(CTRL_OBJS) rbp_key.o shmstate.o \
	    fader_state.o rbp_transport.o -lpthread -lm -lc $(LDFLAGS)

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
              pace_policy.o controllers.o
	$(CC) $(CFLAGS) -shared -o $@ audioshim.o s24pack.o mirror_policy.o \
	    master_policy.o pace_policy.o controllers.o $(RX3LIB)/libdl.so.2 -lm -lc \
	    $(LDFLAGS)

audioshim.o: audioshim.c shmstate.h envutil.h s24pack.h mirror_policy.h \
             master_policy.h pace_policy.h controllers.h
s24pack.o: s24pack.c s24pack.h

# controllers.o is shared with knobshim.so, and audioshim.o now includes its
# header: the default ALSA card and the FLX4's port-name hint are two fields of
# ONE row, which is the drift this table removes. It is on audioshim.o's deps as
# well as in CTRL_HDRS (which the pattern rule already uses), because a table
# change that no object depends on is a rebuild that silently does not happen.
# There is deliberately no rule of its own here: a per-object rule would replace
# the pattern rule's -fvisibility=hidden and give the two shims two differently
# built copies of the same table.

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
#   menu_window.o  the browser window's geometry, state and hit test: the X, the
#                  minimize, the title bar, the url field, the page rect (pure,
#                  unit-tested). NOT a second plane -- menu_window.h states why
#   menu_window_paint.o  the window's chrome, drawn over the page (pure, and the
#                  same palette as the panel by construction: menu_pixel())
#   menu_keyboard.o  the popup keyboard's table, geometry and hit test (pure,
#                  unit-tested). Its caps are drawn from menu_font.h, which is why
#                  the atlas grew lowercase and punctuation before this existed
#   browser_link.o the shim's end of the pipe to the browser on the host -- the
#                  frame mmap, the status read and the command append, and the ONLY
#                  file I/O in the menu (browser_link.h says why /tmp is shared)
#   drmband.o      the overlay plane the band is presented on instead, when the
#                  machine has one (drmband.h). It carries its own copy of the
#                  thirteen DRM uapi structs it needs rather than taking a
#                  libdrm-dev dependency into an armel build -- the same reasoning
#                  tools/build-toolchain/Dockerfile gives for leaving
#                  libasound2-dev out -- so there is no include path and nothing
#                  to link. The WINDOW is presented on this same plane, handed
#                  over from the band: DRM master is one per device.
#   side_zone.o    the left/right edge drawers' geometry, hit test and gesture --
#                  CUE, PLAY and an absolute channel fader per deck (pure,
#                  unit-tested). menu_zone.o's shape, rotated onto the x axis
#   side_paint.o   the drawer's image: title, the two transport boxes, the fader
#                  and its readout (pure, unit-tested)
#   prompt_zone.o  the USB STOP chooser's box, hit test and gesture -- the three
#                  rows USB 1 / USB 2 / CANCEL the seventh menu column raises in
#                  place of stopping a device on one tap (pure, unit-tested).
#                  menu_zone.o's shape a third time: a box that owns every report
#                  while it is up and answers on the release
#   prompt_paint.o the chooser's image: frame, title, rule and three row buttons,
#                  the two dead-device rows in the palette's OFF classes (pure)
#   fx_zone.o     the BEAT FX panel's two controls: the CH SELECT cycle and the
#                  effect picker rbp's own effect-name cell raises -- one column,
#                  fourteen rows, in the order the FLX4's FX SELECT button steps
#                  (pure, unit-tested). prompt_zone.o's shape a fourth time, and it
#                  holds both interactions because the picker IS raised by the
#                  zone's own tap
#   fx_paint.o    the picker's image: a black bed, a centred title, one rule and
#                  fourteen row buttons (pure)
#   hc_zone.o     the HOT CUE pad row rbp draws across the bottom of each deck:
#                  sixteen cells that trigger their own cue, and NOTHING at all on
#                  a pad with no cue registered -- the gate that stops a touch
#                  writing a cue onto the operator's own track (pure, unit-tested).
#                  fx_zone.o's shape: the drawn cell is the visual promise, the
#                  release answers for the cell the press began on, and a gesture
#                  that began off the grid is rbp's for its whole life
#   fxpad_zone.o  the momentary X/Y pad in the BEAT FX panel's BPM detail cell:
#                  X is the effect's level, Y is the beat ladder, it is engaged
#                  while the finger is down and the release puts all three axes
#                  back (pure, unit-tested). The EIGHTH of the family, and the
#                  first whose values are read out of rbp rather than computed --
#                  it still never reads rbp itself: the caller hands them in
#   fxpad_paint.o the pad's HUD: a frame round rbp's own cell and a dot at the
#                  finger. The family's first COPIER -- a plane is opaque and
#                  rbp's live BPM/msec/BEAT readout has to stay visible
#                  underneath, so each call takes rbp's pixels in and annotates
#                  them (pure over the page it is handed)
#
# This is the SECOND shim to reach rbp: pointsrc.c turns a recognized tap into
# the keycode rbp has for that button (send_rx_key, rbp_key.o below). It is a
# real change to a property fbshim had -- it referenced no rbp address at all --
# which is why the one object it needs is a leaf with its own rule rather than
# rbp_bridge.o, whose meter hook must not exist in two copies.
#
# It reached rbp's *mixer* as well once the drawers arrived: a drag sends
# K_FADER/OP_VALUE and records the position in g_fader -- see fader_state.o,
# which is NOT in this list on purpose and is linked into BOTH shims explicitly,
# the way rbp_key.o is.
FBSHIM_OBJS := fb_shim.o tscfake.o pointsrc.o point_xform.o touch_zone.o \
               fb_cursor.o cursor_paint.o menu_zone.o menu_paint.o menu_draw.o \
               menu_window.o menu_window_paint.o menu_keyboard.o \
               browser_link.o drmband.o side_zone.o side_paint.o util_zone.o \
               wave_zone.o prompt_zone.o prompt_paint.o fx_zone.o fx_paint.o \
               hc_zone.o usb_label.o fxpad_zone.o fxpad_paint.o
FBSHIM_HDRS := tscfake.h pointsrc.h point_xform.h touch_zone.h syscalls.h \
               envutil.h fb_cursor.h cursor_paint.h fbdev.h menu_zone.h \
               menu_paint.h menu_font.h menu_draw.h drmband.h menu_window.h \
               menu_window_paint.h menu_keyboard.h browser_link.h side_zone.h \
               side_paint.h rbp_vu.h util_zone.h wave_zone.h prompt_zone.h \
               prompt_paint.h fx_zone.h fx_paint.h hc_zone.h usb_label.h \
               fxpad_zone.h fxpad_paint.h

fbshim.so: $(FBSHIM_OBJS) rbp_key.o fader_state.o rbp_transport.o \
           $(FBSHIM_HDRS) rbp_transport.h \
           $(COMPAT)/libc_nonshared.a $(COMPAT)/libpthread_nonshared.a
	$(CC) $(CFLAGS) -shared -o $@ $(FBSHIM_OBJS) rbp_key.o fader_state.o \
	    rbp_transport.o -lpthread -lm -lc $(LDFLAGS)

fb_shim.o: fb_shim.c pointsrc.h $(FBSHIM_HDRS)
tscfake.o: tscfake.c tscfake.h pointsrc.h
# An interposer has to look at the arguments it is handed — that is the job — so
# glibc's `nonnull` on open()'s pathname and `access(write_only)` on poll()'s
# array are describing callers that conform, which is exactly what a shim cannot
# assume. Both warnings are false positives here; the original combined
# fbshim-tsc.c produced the same set. Scoped to this one file rather than turned
# off project-wide, where -Wmaybe-uninitialized does catch real bugs.
fb_shim.o: CFLAGS += -Wno-nonnull-compare -Wno-maybe-uninitialized

pointsrc.o: pointsrc.c pointsrc.h point_xform.h tscfake.h touch_zone.h \
            rbp_key.h rbp_abi.h menu_zone.h menu_window.h side_zone.h rbp_vu.h \
            prompt_zone.h fx_zone.h usb_label.h fxpad_zone.h
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
             point_xform.h pointsrc.h drmband.h menu_window.h menu_window_paint.h \
             side_zone.h side_paint.h rbp_vu.h prompt_zone.h prompt_paint.h \
             fx_zone.h fx_paint.h fxpad_zone.h fxpad_paint.h
# menu_window_paint.o takes menu_paint.h for the palette and the view, and
# menu_window.h for the rectangles it must draw the marks exactly inside -- a
# dependency that is not written down is a rebuild that silently does not happen.
menu_window.o: menu_window.c menu_window.h menu_zone.h menu_keyboard.h
menu_window_paint.o: menu_window_paint.c menu_window_paint.h menu_window.h \
                     menu_paint.h
# menu_keyboard.o is pure and knows nothing but its own table: it draws nothing
# (menu_window_paint.c does) and it holds no state that crosses a thread, so its
# one header is its whole dependency.
menu_keyboard.o: menu_keyboard.c menu_keyboard.h
# side_zone.o and side_paint.o are pure, and their headers carry the whole
# interface: side_zone.h for the geometry and the gesture, side_paint.h for the
# view it draws over. side_paint.o takes side_zone.h as well because every
# rectangle it draws is a SZ_* literal from it -- a dependency that is not written
# down is a rebuild that silently does not happen.
side_zone.o: side_zone.c side_zone.h menu_zone.h
side_paint.o: side_paint.c side_paint.h side_zone.h menu_paint.h menu_font.h
# util_zone.o is pure in the same way and takes menu_zone.h only for the MZ_FEED_*
# vocabulary its one door answers in -- it links no rbp symbol and reads no address,
# which is why the state it needs arrives in a struct and not from rbp. Its own
# header carries the geometry, the constants and the gesture rules in full.
util_zone.o: util_zone.c util_zone.h menu_zone.h
# wave_zone.o is the fourth of that family, and it does not take menu_zone.h at all:
# it answers with a SIGNED STEP COUNT rather than with the MZ_FEED_* vocabulary,
# because it never takes a report -- that is the whole correction of the pinch it
# replaces. It reads no rbp address either: whether a selector rotation sent right now
# would reach the waveform zoom or the beat grid changes under the gesture, so the
# caller computes it per report and hands it over. Its header carries the whole feature,
# including why the on-screen ZOOM GRID control is an indicator and not a control, the
# measured geometry of the wave rect, and the regression the additive rule answers.
wave_zone.o: wave_zone.c wave_zone.h
# prompt_zone.o is the fifth of the pure family and is built to side_zone.o's exact
# shape: menu_zone.h is on the line for the MZ_FEED_* codes and MZ_LOGICAL_W/H it
# answers in, and for nothing else -- it links no rbp symbol and reads no address,
# which is why rbp's answer about the two devices arrives in a struct prompt_state
# and not from a pointer. prompt_paint.o takes prompt_zone.h as well because every
# rectangle it draws is a PR_* literal from it, the same dependency side_paint.o has
# on side_zone.h. menu_font.h is on prompt_zone.o's line since the buttons started
# carrying a device's own name: how wide a name draws is what decides how much of it
# fits, and the atlas is where that number lives. It is a compile-time table and
# nothing else, so it costs this object none of its purity.
prompt_zone.o: prompt_zone.c prompt_zone.h menu_zone.h menu_font.h
prompt_paint.o: prompt_paint.c prompt_paint.h prompt_zone.h menu_paint.h menu_font.h
# fx_zone.o is the sixth of the pure family and is built to prompt_zone.o's exact
# shape: menu_zone.h is on the line for the MZ_FEED_* codes and MZ_LOGICAL_W/H it
# answers in, and for nothing else -- it links no rbp symbol and reads no address,
# which is why rbp's answer about its Beat FX channel is read by pointsrc.c and
# arrives here as an argument. fx_paint.o takes fx_zone.h as well because every
# rectangle it draws is an FX_* literal from it, the same dependency side_paint.o
# has on side_zone.h.
fx_zone.o: fx_zone.c fx_zone.h menu_zone.h
fx_paint.o: fx_paint.c fx_paint.h fx_zone.h menu_paint.h menu_font.h
# hc_zone.o is the seventh of the pure family and is fx_zone.o's shape exactly, right
# down to the include line: hc_zone.h brings menu_zone.h in for the MZ_FEED_* codes and
# the logical space it answers in, and rbp_abi.h is taken for BROWSE_MODE_PLAY and
# K_PAD1 and for NOTHING ELSE -- rbp's pad mode and its answer about whether a cue is
# registered are read by pointsrc.c and arrive here as arguments. That is why the test
# rule below can pin all 365 checks with no Pi, no panel and no player.
hc_zone.o: hc_zone.c hc_zone.h menu_zone.h rbp_abi.h
# fxpad_zone.o is the eighth of the pure family and is fx_zone.o's shape a fifth time:
# fxpad_zone.h brings menu_zone.h in for the MZ_FEED_* codes, and rbp_abi.h is NOT on
# this line -- the pad's whole point is that rbp's own values arrive as a struct and
# leave as a struct, so this object names no rbp address at all. The offsets it is filled
# from are pointsrc.c's business (rbp_abi.h) and the test rule below pins all 411 checks
# with no Pi, no panel and no player.
fxpad_zone.o: fxpad_zone.c fxpad_zone.h menu_zone.h
# fxpad_paint.o takes fxpad_zone.h for the cell's rect and menu_paint.h for the palette,
# exactly as fx_paint.o takes fx_zone.h's -- and it takes menu_zone.h through
# menu_paint.h for MZ_LOGICAL_W/H, the space fxpad_paint_w/h scale out of.
fxpad_paint.o: fxpad_paint.c fxpad_paint.h fxpad_zone.h menu_paint.h menu_zone.h
# browser_link.o is the one object in the menu that opens a file, so its header is
# where the three paths and the frame's own header layout live -- nothing else in
# the shim names /tmp/rbwin.*.
browser_link.o: browser_link.c browser_link.h
# drmband.o is the only object that talks to the DRM device, and it does it with
# raw ioctls through syscalls.h's real_* wrappers -- the shim interposes open and
# ioctl, so the raw path is not optional.
drmband.o: drmband.c drmband.h syscalls.h pointsrc.h

# The one rule for the objects above. -Wall -Wextra is on, so a genuinely
# unused helper fails the build rather than lingering.
%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

gpioshim.so: gpioshim.c
	$(CC) $(CFLAGS) -shared -o $@ $< -lc $(LDFLAGS)

tscshim.so: tscshim.c $(COMPAT)/libc_nonshared.a $(COMPAT)/libpthread_nonshared.a
	$(CC) $(CFLAGS) -shared -o $@ $< -lpthread -lm -lc $(LDFLAGS)

crashcatch.so: crashcatch.c
	$(CC) $(CFLAGS) -shared -o $@ $< $(RX3LIB)/libdl.so.2 -lc $(LDFLAGS)

# netalias.o holds the whole policy of the network alias: which ioctl requests
# may be retargeted at all, what to do with a `system` command line, and which
# interface to answer for. Pure -- no syscall, no kernel header, no clock -- which
# is what lets test_netalias.c pin every verdict on the host. See netalias.h.
#
# HIDDEN, like rbp_key.o, and for the same reason: this library is loaded into the
# same global scope as rbp and four other shims, and nothing outside netshim.so
# has any business calling na_* by name. The interposers themselves (ioctl,
# if_nametoindex, system) are netshim.o's and are deliberately NOT hidden -- a
# hidden interposer interposes nothing. `nm -D netshim.so | grep ' na_'` is
# silent; if it ever is not, this rule stopped applying.
netalias.o: netalias.c netalias.h
	$(CC) $(CFLAGS) -fvisibility=hidden -c -o $@ $<

# netshim.so: rbp's Pro DJ Link stack, over the interface that is actually up.
# See netshim.c's header for the whole argument; what the rule has to get right
# is the same thing crashcatch.so's does -- dlsym is in libdl, NOT libc, on this
# rootfs, so libdl.so.2 is named explicitly and netshim.c pins the symbol version.
#
# -lpthread is for the Link bring-up's poll thread, and it is the same shape as
# knobshim.so's: the vendor rootfs carries no libpthread_nonshared.a, so the
# empty stub above is a prerequisite too. pthread_create resolves against the
# libpthread rbp itself is already using -- juce::Thread is pthreads underneath
# -- so this adds a dependency rbp has satisfied since it started.
netshim.so: netshim.c netalias.o netalias.h envutil.h \
            $(COMPAT)/libc_nonshared.a $(COMPAT)/libpthread_nonshared.a
	$(CC) $(CFLAGS) -shared -o $@ netshim.c netalias.o \
	    $(RX3LIB)/libdl.so.2 -lpthread -lc $(LDFLAGS)

# The witness's test subject. Must NOT be -static: LD_PRELOAD needs a dynamic
# loader, which is exactly why this is not one of TESTS. Run it in the chroot,
# where this rootfs's loader and soft-float ABI apply -- see docs/13 S3.7.
ccexit: ccexit.c
	$(CC) $(CFLAGS) -o $@ $< -lc $(LDFLAGS)

# netshim.so's drill, and for ccexit's reason the same constraint applies: it is
# the LD_PRELOAD *subject*, so it must be dynamic. Not in TESTS -- `make test`
# runs its suite statically and this cannot run that way. What it adds over
# test_netalias is that it is not a test of the RULES but of the shim: it links
# the real netshim.so through the loader, so the constructor, the RTLD_NEXT
# chain and the name restore are all exercised, and a rewrite that never happens
# reads as a wrong address rather than as a silent pass.
netcheck: netcheck.c
	$(CC) $(CFLAGS) -o $@ $< -lc $(LDFLAGS)

seqinject2: seqinject2.c
	$(CC) -O2 -march=armv5t -mfloat-abi=soft -static -o $@ $<

udplog: udplog.c
	$(CC) -O2 -march=armv5t -mfloat-abi=soft -static -o $@ $<

# controllers_cli is the controller table asked from a shell -- the half of the
# sibling's controllers.py that is not imported. It links the PRODUCTION
# controllers.c, so `doctor.sh` and `install.sh` read the same rows the shims use
# instead of keeping a copy in shell, where nothing would check it. Static and in
# STATIC because it is a diagnostic that must run on the unit whatever state the
# deploy root's shared libraries are in -- the moment it is wanted is the moment
# something else is wrong. Installed by install.sh beside the other artifacts.
controllers_cli: controllers_cli.c controllers.c controllers.h
	$(CC) -O2 -march=armv5t -mfloat-abi=soft -static -o $@ \
	    controllers_cli.c controllers.c

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
         test_evdev test_cursor_dev test_menu test_side test_window test_keyboard \
         test_menu_dev test_util test_wave test_prompt test_fx test_hc test_fxpad \
         test_led_table test_plinn_scan test_controllers test_usblabel test_netalias

test_point: test_point.c tscfake.c point_xform.c touch_zone.c $(FBSHIM_HDRS)
	$(CC) -O2 -march=armv5t -mfloat-abi=soft -fno-stack-protector \
	    -Wall -Wextra -static -o $@ test_point.c tscfake.c point_xform.c \
	    touch_zone.c

# test_menu links the PRODUCTION menu_zone.c and menu_paint.c -- not copies, and
# not the compiled objects either, so the test needs no cross-built .o to run. Its
# framebuffer is a local array: there is no device, no thread and no rbp anywhere
# in it, which is the whole reason both modules are pure. What it stops is a
# gesture rule or a column boundary that is only ever checked by a finger.
#
# side_zone.c is on the link line because menu_zone.c's closed-entry arm now asks
# side_any_open() -- the ONE SURFACE gate -- so the band does not compile without
# the drawer's state. It is the REAL object, not a stub: a stub would answer "no
# drawer is ever open" and quietly make the test agree with a gate that does not
# work.
test_menu: test_menu.c menu_zone.c menu_paint.c side_zone.c menu_zone.h \
           menu_paint.h menu_font.h side_zone.h
	$(CC) -O2 -march=armv5t -mfloat-abi=soft -fno-stack-protector \
	    -Wall -Wextra -static -o $@ test_menu.c menu_zone.c menu_paint.c \
	    side_zone.c

# test_side links the PRODUCTION side_zone.c and side_paint.c -- the shipped gesture,
# the shipped geometry and the shipped pixels -- and menu_zone.c with them, because
# the ONE SURFACE rule is a statement about two modules at once: the drawer must not
# arm while the band is open, and the band must not arm while a drawer is open. Driving
# both real feeders is the only way to pin that. Its surface is a local array: no
# framebuffer, no plane, no thread, no device. What it stops is a hit boundary, a sweep
# threshold, a mirror or a fader mapping that is only ever checked by a finger.
test_side: test_side.c side_zone.c side_paint.c menu_zone.c menu_paint.c \
           side_zone.h side_paint.h menu_zone.h menu_paint.h menu_font.h
	$(CC) -O2 -march=armv5t -mfloat-abi=soft -fno-stack-protector \
	    -Wall -Wextra -static -o $@ test_side.c side_zone.c side_paint.c \
	    menu_zone.c menu_paint.c

# test_window links the PRODUCTION menu_window.c and menu_window_paint.c, plus
# menu_paint.c because the chrome's palette comes from menu_pixel() -- one table,
# so the window cannot drift into colours the panel does not have -- and
# menu_zone.c for MZ_LOGICAL_* and MZ_STRIP_Y1, which the geometry test checks the
# window against. Its surface is a local array: no framebuffer, no plane, no
# thread, no browser. What it stops is a state machine or a hit test that is only
# ever exercised by a finger -- and, in the geometry test, a window that grows over
# the menu's own way back.
#
# menu_keyboard.c is here because menu_window.c calls it: the URL field raises the
# keyboard, a cap types into the buffer and OK hides it, so the window's own tests
# cannot be built without it. It is the PRODUCTION source, not a copy, so what the
# window's tests exercise is the table that ships.
#
# side_zone.c is the same story one level down: menu_zone.c -- linked for
# MZ_LOGICAL_* and MZ_STRIP_Y1 -- calls side_any_open() from its closed-entry arm,
# so the band's source cannot link without the drawer's state. Real object, not a
# stub; see test_menu's note.
test_window: test_window.c menu_window.c menu_window_paint.c menu_paint.c \
             menu_zone.c menu_keyboard.c side_zone.c menu_window.h \
             menu_window_paint.h menu_paint.h menu_zone.h menu_font.h \
             menu_keyboard.h side_zone.h
	$(CC) -O2 -march=armv5t -mfloat-abi=soft -fno-stack-protector \
	    -Wall -Wextra -static -o $@ test_window.c menu_window.c \
	    menu_window_paint.c menu_paint.c menu_zone.c menu_keyboard.c \
	    side_zone.c

# test_keyboard pins the popup keyboard ALONE: the 46 character keys are exactly the
# atlas's keyboard alphabet with no duplicate, the four specials are where the layout
# puts them, the key rects tile the keyboard with no gap and no overlap, a hit is -1
# while the keyboard is down, and SHIFT is consumed by a character and merely shown by
# a cap. menu_font.c does not exist -- menu_font.h is the atlas and its coverage is a
# table in the header -- so this needs menu_keyboard.c and nothing else, which is
# exactly what "pure" was supposed to buy.
test_keyboard: test_keyboard.c menu_keyboard.c menu_keyboard.h menu_font.h
	$(CC) -O2 -march=armv5t -mfloat-abi=soft -fno-stack-protector \
	    -Wall -Wextra -static -o $@ test_keyboard.c menu_keyboard.c

# test_util pins the UTILITY screen's gesture. It links the PRODUCTION util_zone.c and
# nothing else: menu_zone.h is #defines and declarations, so the module needs no other
# object -- which is the same "pure" that test_keyboard's header note is about. rbp is
# not present and cannot be: everything util_zone.c would ask rbp arrives in a
# struct util_state the test fills in by hand, and everything it wants sent comes back
# as an act and a signed count. What it stops is a row boundary, a coalescing rule, a
# tap's travel -- and, above all, the two refusals that protect the operator: a drag
# never taps, and a drag NEVER ROTATES WHILE EDITING, where rbp's rotate would rewrite
# a setting instead of scrolling. rbp_abi.h is linked in as well, on purpose, for the
# one check that keeps this module's private copy of the browse mode honest.
test_util: test_util.c util_zone.c util_zone.h menu_zone.h rbp_abi.h
	$(CC) -O2 -march=armv5t -mfloat-abi=soft -fno-stack-protector \
	    -Wall -Wextra -static -o $@ test_util.c util_zone.c

# test_wave pins the waveform swipe, and links the PRODUCTION wave_zone.c for the same
# reason test_util.c links util_zone.c. rbp_abi.h is linked in on purpose and it is the
# part that matters most: this gesture's whole safety is that a step is only sent where
# rbp would route a selector rotation to its waveform zoom and not to its beat grid, and
# the test pins that rule against the macro rbp_abi.h derives from the binary rather than
# against a second copy of the arithmetic. It also pins this module's own copies of the
# scale ladder and of the rect against rbp_abi.h and the measurements the header carries.
# The module itself reads no address -- whether the rotation would zoom arrives in struct
# wave_state, per report, because it changes under the gesture.
test_wave: test_wave.c wave_zone.c wave_zone.h rbp_abi.h
	$(CC) -O2 -march=armv5t -mfloat-abi=soft -fno-stack-protector \
	    -Wall -Wextra -static -o $@ test_wave.c wave_zone.c

# test_led_table links the PRODUCTION led_table.c. It is the one pure piece of the
# LED bridge, which is exactly why it was split out: what it decides -- whether a
# read of rbp's table is a state rbp could have had, and how fast a blink goes --
# would otherwise only ever be exercised on a unit. It pins the torn-read filter
# (including that it FIRES, and that a torn entry with a history keeps it rather
# than blinking an LED dark), the (id, channel) lookup that every other reader in
# rbp_led.c now goes through, and the half-duty phase -- on both sides of the
# 32-bit millisecond boundary, because `long` is four bytes on this target and
# this tree has already paid for a wrapped clock once.
test_led_table: test_led_table.c led_table.c led_table.h rbp_abi.h
	$(CC) -O2 -march=armv5t -mfloat-abi=soft -fno-stack-protector \
	    -Wall -Wextra -static -o $@ test_led_table.c led_table.c

# test_plinn_scan links the PRODUCTION plinn_scan.c, for the same reason
# test_led_table links led_table.c: the object scan in rbp_bridge.c is linked into
# no test, so the rule that decides which vtable match is a deck's innards -- and
# the counts a failed scan reports -- would otherwise only ever be exercised on a
# unit. It is the rule that shipped with an off-by-one on 2026-10-01, so the exact
# deck-1 case is the first thing pinned here. Note the shorter list of inputs than
# test_led_table's: this module reads no address of rbp's, so it does not need
# rbp_abi.h, and the day it wants to is the day the split has gone wrong.
test_plinn_scan: test_plinn_scan.c plinn_scan.c plinn_scan.h
	$(CC) -O2 -march=armv5t -mfloat-abi=soft -fno-stack-protector \
	    -Wall -Wextra -static -o $@ test_plinn_scan.c plinn_scan.c

# test_controllers links the PRODUCTION controllers.c, for the same reason the
# two above do. The table is the anti-drift artifact: one physical device used to
# be named by the map, the port-name hint, the ALSA card id and a USB id in four
# files that never referenced each other, so the tests pin them TOGETHER and pin
# what is deliberately NOT known (the JP21's three NULLs) as tightly as what is.
# It reads no address of rbp's, so no rbp_abi.h on the line -- the day it wants
# one is the day this module has stopped being pure. It is also the source for
# controllers_cli.c below, which doctor.sh and install.sh read instead of keeping
# their own copy of the list.
test_controllers: test_controllers.c controllers.c controllers.h
	$(CC) -O2 -march=armv5t -mfloat-abi=soft -fno-stack-protector \
	    -Wall -Wextra -static -o $@ test_controllers.c controllers.c

# test_usblabel links the PRODUCTION usb_label.c, for the same reason the four
# tests above link their modules: the rules it holds are the ones that would
# otherwise only ever be exercised by a stick in the slot and a screenshot. It
# reads no address of rbp's and opens no file -- the label's SOURCE and its
# DESTINATION are both ctrlshim.c's business -- so it needs nothing but its own
# two files, which is exactly what "pure" was supposed to buy.
test_usblabel: test_usblabel.c usb_label.c usb_label.h
	$(CC) -O2 -march=armv5t -mfloat-abi=soft -fno-stack-protector \
	    -Wall -Wextra -static -o $@ test_usblabel.c usb_label.c

# test_netalias links the PRODUCTION netalias.c, for the same reason the tests
# above link their modules: the verdicts it holds are the ones that would
# otherwise only ever be read off a Link screen that stayed empty -- or, in the
# one case that matters more, off a unit the operator can no longer reach.
#
# It includes <net/if.h> and <sys/ioctl.h> and asserts the module's own written-
# out request numbers, interface flags and struct layout against them, so the
# whitelist cannot silently decay into a list of numbers no caller ever sends.
# The first thing it walks is the getter/setter PAIRS: every SIOCGIF* this shim
# retargets has a SIOCSIF* twin that writes, and setIpAddr @0x39dfb4 is one of
# the twelve functions that name eth0. No device, no interface, no rootfs.
test_netalias: test_netalias.c netalias.c netalias.h
	$(CC) -O2 -march=armv5t -mfloat-abi=soft -fno-stack-protector \
	    -Wall -Wextra -static -o $@ test_netalias.c netalias.c

# test_prompt links the PRODUCTION prompt_zone.c and prompt_paint.c, and menu_paint.c
# with them because the box's palette comes from menu_pixel() -- one table, so the
# chooser cannot drift into colours the band and the drawers do not have. menu_zone.c is
# on the line for the same reason it is on test_side's: menu_paint.c's own layout code
# calls menu_label() and menu_button_x0/x1(), which live there, and menu_zone.c's closed-
# entry arm calls side_any_open() in turn. Its surface is a local array: no framebuffer,
# no plane, no thread, no rbp. What it stops is a row boundary, a dead-device row that
# answers anyway, and above all a box that closes itself while a finger is down -- the
# release that would then reach rbp with no press behind it.
test_prompt: test_prompt.c prompt_zone.c prompt_paint.c menu_paint.c menu_zone.c \
             side_zone.c prompt_zone.h prompt_paint.h menu_paint.h menu_zone.h \
             menu_font.h side_zone.h
	$(CC) -O2 -march=armv5t -mfloat-abi=soft -fno-stack-protector \
	    -Wall -Wextra -static -o $@ test_prompt.c prompt_zone.c \
	    prompt_paint.c menu_paint.c menu_zone.c side_zone.c

# test_fx links the PRODUCTION fx_zone.c and fx_paint.c, and menu_paint.c and
# menu_zone.c with them for exactly the reasons test_prompt names: the picker's palette
# comes from menu_pixel(), so the sixth drawn surface cannot drift into colours the
# other five do not have, and menu_paint.c's layout code calls menu_label() and
# menu_button_x0/x1(), which live in menu_zone.c. side_zone.c is on the line because
# menu_zone.c's closed-entry arm calls side_any_open() in turn -- the same chain
# test_prompt and test_side both carry.
#
# Its surface is a local array: no framebuffer, no plane, no thread, no rbp. What it
# stops is a tap on rbp's own panel firing the wrong control, a picker that answers a
# row the finger never anchored to, and above all a box that closes itself or a release
# that reaches rbp with no press behind it.
test_fx: test_fx.c fx_zone.c fx_paint.c menu_paint.c menu_zone.c side_zone.c \
         fx_zone.h fx_paint.h menu_paint.h menu_zone.h menu_font.h side_zone.h
	$(CC) -O2 -march=armv5t -mfloat-abi=soft -fno-stack-protector \
	    -Wall -Wextra -static -o $@ test_fx.c fx_zone.c \
	    fx_paint.c menu_paint.c menu_zone.c side_zone.c

# test_hc links the PRODUCTION hc_zone.c -- not a copy and not the cross-built .o,
# so the test needs no arm object to run. hc_zone.c is pure: rbp's pad mode and its
# answer about whether a cue is registered are arguments here, exactly as they are
# parameters there, which is what lets the gate be pinned on a host with no rbp. The
# rbp_abi.h prerequisite is for the two constants hc_zone.c takes from it (K_PAD1 and
# BROWSE_MODE_PLAY) -- the keycode arithmetic is rbp's own, read off the instruction
# stream, and pinning it against a number the test typed itself would pin nothing.
test_hc: test_hc.c hc_zone.c hc_zone.h menu_zone.h rbp_abi.h
	$(CC) -O2 -march=armv5t -mfloat-abi=soft -fno-stack-protector \
	    -Wall -Wextra -static -o $@ test_hc.c hc_zone.c

# test_fxpad links the PRODUCTION fxpad_zone.c AND fxpad_paint.c -- test_fx.c and
# test_side.c carry their feature's painter for the same reason: the HUD's two marks are
# drawn over rbp's own pixels, and "which pixels of the operator's readout survive" is
# exactly the property a copy of the code would get wrong without anyone noticing. Neither
# is a copy and neither is the cross-built .o, so the test needs no arm object to run.
# fxpad_zone.c is pure: rbp's level, rung and effect state are arguments here, exactly as
# they are parameters there, which is what lets a whole gesture -- press, climb, release,
# unwind -- be driven against a simulated rbp on a host. The fx_zone.h prerequisite is for
# the three neighbouring rects the cell must not overlap; fx_zone.c is NOT linked, because
# nothing here calls it. menu_paint.c and menu_zone.c come with fxpad_paint.c for the
# reasons test_fx names -- the palette is menu_pixel()'s, and menu_paint.c's layout calls
# into menu_zone.c, which in turn calls side_any_open().
test_fxpad: test_fxpad.c fxpad_zone.c fxpad_paint.c menu_paint.c menu_zone.c \
            side_zone.c fxpad_zone.h fxpad_paint.h fx_zone.h menu_paint.h \
            menu_zone.h menu_font.h side_zone.h
	$(CC) -O2 -march=armv5t -mfloat-abi=soft -fno-stack-protector \
	    -Wall -Wextra -static -o $@ test_fxpad.c fxpad_zone.c fxpad_paint.c \
	    menu_paint.c menu_zone.c side_zone.c

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
# drmband.o is here as the REAL object and not a stub, and that is deliberate: on
# a host with no /dev/dri/card1 drm_band_setup() fails the way it fails on a unit
# whose plane cannot be had, so every assertion below is made against the FALLBACK
# route -- the fb0 path this shipped with. That is exactly the path the tests were
# written for, and it has to keep working.
# menu_window.o and menu_window_paint.o are here because menu_draw.o now calls
# them: the window's plane sync is part of the driver this pins. They are the REAL
# objects for the same reason drmband.o is -- the states and the pixels have to be
# the shipped ones, or the fallback route below is being tested against a fiction.
# menu_keyboard.o and browser_link.o are here for the same reason one level down:
# menu_window.o calls the keyboard, and menu_draw.o's tick calls browser_link.o. The
# link has to resolve them, and on a host with no /tmp/rbwin.* every browser_link
# call takes its absent-file arm -- which is the route the window must survive on a
# unit where the host browser is not running.
# side_zone.o, side_paint.o and fader_state.o are here because menu_draw.o now calls
# the drawers: the plane handover and the repaint gate are part of the driver this
# pins. side_zone.o and side_paint.o are the REAL objects for the same reason
# drmband.o is -- the shipped geometry, the shipped gesture and the shipped pixels --
# and fader_state.o is the one default-visibility definition of g_fader that the
# drawer's handle reads in place, exactly as the shim does.
# prompt_zone.o and prompt_paint.o are here for the same reason one step further on:
# menu_draw.o now presents the USB STOP chooser on a plane of its own, so the box's
# state machine and its image are part of the driver this pins. Both are the REAL
# objects, so what is pinned is the shipped box.
# fx_zone.o and fx_paint.o are here because menu_draw.o's tick now syncs the BEAT FX
# picker's plane as well. They are linked for the same reason and are the REAL objects;
# unlike the chooser's there is no scenario that arms a picker plane, so there is no
# --wrap for fxlist_paint -- what the tick pins here is that a CLOSED picker costs the
# sync nothing, which is the state every scenario in this file is in.
# fxpad_zone.o and fxpad_paint.o are here for the third box: menu_draw.o now presents the
# momentary pad's HUD on a plane of its own, so the pad's own liveness -- fxpad_busy(),
# which is what decides whether that plane is up at all -- is part of the driver this
# pins. Both are the REAL objects, so what is pinned is the shipped pad; and like the
# picker's there is no scenario that engages a finger on the cell, so what the tick pins
# is that an IDLE pad costs the sync nothing.
MENU_TEST_OBJS := test_menu_dev.o menu_draw.o menu_paint.o menu_zone.o \
                  menu_window.o menu_window_paint.o menu_keyboard.o \
                  browser_link.o point_xform.o drmband.o side_zone.o \
                  side_paint.o prompt_zone.o prompt_paint.o fx_zone.o fx_paint.o \
                  fxpad_zone.o fxpad_paint.o fader_state.o rbp_transport.o

# The drawer's wrappers, and they are the ONE exception to "drmband.o is the real
# object". --wrap=drm_band_* lets scenario_drawer() hand the module a plane that
# exists, so the handover and the repaint gate can be pinned at all -- with no
# /dev/dri they are both unreachable, and a gate that has degenerated into "always"
# is invisible when the fallback paints every tick anyway. The wrapper is ARMED only
# inside that one scenario; everywhere else it delegates to the real drmband.o and
# the fallback route is untouched. --wrap=side_paint is there for the same scenario:
# the gate's claim is a COUNT. --wrap=prompt_paint is there for scenario_prompt(), the
# same claim about the chooser's box, whose plane is a FOURTH band and so has to be
# classified by the wrapper rather than assumed to be one of the two it already knows.
test_menu_dev: $(MENU_TEST_OBJS)
	$(CC) -O2 -march=armv5t -mfloat-abi=soft -fno-stack-protector \
	    -Wall -Wextra -static -o $@ $(MENU_TEST_OBJS) \
	    -Wl,--wrap=syscall -Wl,--wrap=malloc -Wl,--wrap=usleep \
	    -Wl,--wrap=drm_band_setup -Wl,--wrap=drm_band_show \
	    -Wl,--wrap=drm_band_hide -Wl,--wrap=drm_band_teardown \
	    -Wl,--wrap=side_paint -Wl,--wrap=prompt_paint \
	    -lpthread -lm -lc

test_menu_dev.o: test_menu_dev.c menu_draw.h menu_paint.h menu_zone.h \
                 point_xform.h pointsrc.h fbdev.h side_zone.h side_paint.h \
                 rbp_vu.h
	$(CC) -O2 -march=armv5t -mfloat-abi=soft -fno-stack-protector \
	    -Wall -Wextra -c -o $@ $<

test: $(TESTS)
	@for t in $(TESTS); do echo "-- $$t"; qemu-arm ./$$t || exit 1; done
	@echo "-- test_menu_dev (padded page stride)"
	@qemu-arm ./test_menu_dev 8 || exit 1

# Fail the build if any GLIBC version above 2.7 or a hard-float tag sneaks in, and
# fail it if any shim asks for a symbol nothing in the process can supply.
#
# The exit status is the point.  This printed the version list and left the
# reading to a human, which is how a GLIBC_2.34 reference (pthread/dlfcn, which
# glibc folded into libc at 2.34) can sit in a shim that then refuses to load in
# the chroot.  objdump -T is used rather than nm because nm -D prints a
# versioned symbol as name@GLIBC_x.y, so a name-based grep matches only the
# unversioned references and never sees a version at all.
#
# THE SECOND BLOCK WAS DEAD FOR AS LONG AS IT HAS EXISTED, and how it was dead is
# worth keeping, because the fix looks like a simplification.  It tested
# `$4=="*UND*"`.  objdump -T prints
#
#     address   flags   *UND*   value   version   name
#
# so the marker is field 3 and field 4 is the value column: the test was never true,
# `und` was always empty, and every shim reported "all resolved".  It was written
# against `nm -D`, where the line is `U name`, and the nm -> objdump -T rewrite moved
# the columns out from under it -- unnoticed, because the tree happened to be clean
# the day of the rewrite.  Proven 2026-10-08 by building fbshim.so WITHOUT
# rbp_transport.o: objdump -T then showed `U rbp_transport_get` and
# `U rbp_transport_want` in its dynamic symbol table, and this check printed
# "fbshim.so: all resolved" and exited 0.  A check that cannot fail is not a check:
# before trusting a green line here, break the thing it claims to watch.
#
# The rule it applies now is the one a preloaded library actually obeys.  Not "is it
# in the vendor libc" -- that draft is wrong in both directions.  A symbol may also
# come from this shim's OWN definitions, or from a shim loaded BEFORE it: RB_LD_PRELOAD
# is an order, and a later library cannot satisfy an earlier one's relocation.  So:
#
#     vendor libc  +  this shim's own definitions  +  the shims ahead of it
#
# That is what makes audioshim's g_master_gain/g_mirror_gain/g_vu_peak legal -- shmstate.o
# is deliberately NOT linked into audioshim, knobshim defines them, and knobshim is
# loaded first.  And it is what makes the launch failure this check exists for a
# FAILURE: rbp_led_transport_want was undefined in fbshim.so and defined -- hidden --
# in knobshim.so, which fbshim is loaded BEFORE.  See rbp_transport.h.
#
# Symbols a shim is ALLOWED to leave undefined.  Printed on every run; never silent.
# One entry, and it is a hazard rather than a design:
#
#   fstat -- browser_link.o calls it in bl_frame_map() (browser_link.c:61,73).  The
#   vendor libc exports no plain `fstat`, only __fxstat/__fxstat64, so this reference
#   binds to nothing and fires "symbol lookup error" the first time that path runs --
#   which is the moment the abandoned browser pane finds a /tmp/rbwin.frame to look at.
#   It has never fired because that workstream is abandoned and the file is never
#   there.  It is the last survivor of the class rbp-vendor-libc-has-no-stat records
#   (stat() and time() were removed from the same file on 2026-10-04); it wants the
#   same treatment, or the pane wants dropping, before that work resumes.
KNOWN_DANGLING := fstat
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
	done; \
	echo "== undefined symbols nothing loaded at or before it can supply =="; \
	have=""; for l in $(RX3)/lib/libc.so.6 $(RX3)/lib/libdl.so.2 $(RX3)/lib/librt.so.1 \
	                  $(RX3)/lib/libm.so.6 $(RX3)/lib/libpthread.so.0; do \
	  have="$$have $$($(CROSS)objdump -T $$l 2>/dev/null | awk '{print $$NF}' \
	         | sed 's/@.*//' | tr '\n' ' ')"; \
	done; \
	earlier=""; \
	for f in $(PRELOAD); do \
	  und=$$($(CROSS)objdump -T $$f 2>/dev/null | awk '/\*UND\*/ {print $$NF}' | sort -u); \
	  own=$$($(CROSS)nm -D --defined-only $$f 2>/dev/null | awk '{print $$NF}' \
	        | sort -u | tr '\n' ' '); \
	  miss=""; \
	  for s in $$und; do \
	    case $$s in _ITM_*|__gmon_start__) continue;; esac; \
	    case " $(KNOWN_DANGLING) " in *" $$s "*) \
	      echo "   -- $$s: allowed to dangle in $$f -- see KNOWN_DANGLING above"; continue;; esac; \
	    echo " $$have $$own $$earlier " | grep -q " $$s " || miss="$$miss $$s"; \
	  done; \
	  if [ -n "$$miss" ]; then \
	    echo "-- $$f: !! FAIL -- nothing loaded at or before it defines:$$miss"; \
	    echo "   (this links fine and then kills rbp at startup: symbol lookup error)"; rc=1; \
	  else \
	    echo "-- $$f: all resolved"; \
	  fi; \
	  earlier="$$earlier $$own"; \
	done; \
	for f in $(SHARED); do \
	  case " $(PRELOAD) " in *" $$f "*) ;; \
	    *) echo "-- $$f: !! FAIL -- built, but not placed in PRELOAD, so its load order is unknown"; rc=1;; \
	  esac; \
	done; \
	echo "== the shared state both shims must define =="; \
	smiss=""; for n in rbp_transport_get rbp_transport_set rbp_transport_clear \
	                    rbp_transport_want rbp_transport_wanted; do \
	  for f in fbshim.so knobshim.so; do \
	    $(CROSS)nm -D --defined-only $$f 2>/dev/null | grep -q " $$n$$" || smiss="$$smiss $$n($$f)"; \
	  done; \
	done; \
	if [ -n "$$smiss" ]; then \
	  echo "!! FAIL -- not defined by BOTH fbshim.so and knobshim.so:$$smiss"; \
	  echo "   (the edge drawer reads what the LED thread writes: see rbp_transport.h)"; rc=1; \
	else \
	  echo "-- rbp_transport: default visibility, defined by both"; \
	fi; \
	exit $$rc

clean:
	rm -f $(SHARED) $(STATIC) $(REFERENCE) $(TESTS) pcmprobe ccexit netcheck *.o
# ccexit and netcheck are named literally because they are deliberately not in
# TESTS (see their rules above): both must stay dynamic, so `make test` cannot
# run them under the static suite. They are still build outputs, and leaving them
# here would make `clean` incomplete -- which is how a stale one survived a
# rebuild once already.

# The always-out-of-date prerequisite that keeps the two fixture-baking objects
# above from being reused across working directories.
.PHONY: FORCE
FORCE:
