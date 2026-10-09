/*
 * ctrlshim.c -- the control-surface front end: the module that decides what an
 * event is for, without knowing what a note number means.
 *
 * It reads its configuration from the environment, picks a map (MIDI_MAP), starts
 * every thread, waits for rbp's KeyManager, opens the sequencer and hands each
 * event to the map. It also owns the constructor, and the two things that are
 * about the shim rather than about a surface: the MIDI_DUMP recorder and the
 * MIDI_REPLAY player.
 *
 * The surfaces themselves are in map_*.c: which note is PLAY, which CC pair is
 * the jog, what a pad's colour byte looks like. Everything rbp-specific is behind
 * the module headers beside this file -- a map asks rbp_bridge.h for meaning and
 * never sees an address, which is what makes a new controller a table edit rather
 * than a rewrite.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <sound/asequencer.h>

/* State shared with the audio shim. This shim is first in LD_PRELOAD, so it is
 * the one that defines them (via shmstate.o) and owns them. */
#include "shmstate.h"

#include "shimutil.h"
#include "rbp_bridge.h"
#include "rbp_led.h"
#include "rbp_vu.h"
#include "midi_io.h"
#include "mididump.h"
#include "keylog.h"        /* the key dump's format; keylog_state.c writes it */
#include "keylog_state.h"
#include "evdev_io.h"
#include "ctrl_map.h"
#include "controllers.h"   /* the surface table: the default map, the keepalive */
#include "usb_label.h"     /* a stick's own name, for rbp's device row */
#include "syscalls.h"      /* real_open/real_read: rbp interposes open itself */

/* KNOB_VERBOSE. Defined here because this is the module that reads the
 * environment; declared in shimutil.h for everyone whose logging it gates. */
int verbose = 0;

/* The two selected maps. Set once, before any thread can dispatch an event, and
 * read-only afterwards. They are TWO selections and not one -- a controller on
 * the sequencer and a keyboard on evdev are different devices with different
 * event sources, and the only reason they used to be mutually exclusive was that
 * one struct had to answer for both. MIDI_MAP picks the first, EVDEV_MAP the
 * second (see ctrl_map.h). They may name the same map, and then it is built,
 * started and ticked once. */
static const struct ctrl_map *g_midi;
static const struct ctrl_map *g_evdev;

/* Set when the map has been built, so a replay cannot start before there are
 * bindings to replay into -- a dump fed to empty tables looks like a broken map. */
static volatile int maps_ready;

/* The LED table the bridge should drive, or NULL. This is the one place that
 * knows the selection (ctrl_map.h), and the MIDI side only: the panel belongs to
 * the controller, so a keyboard selection lights nothing. NULL until the map has
 * been built, which is why rbp_led.c asks for this rather than caching a pointer
 * of its own -- a NULL here means "nothing to drive yet", and the bridge treats
 * it exactly as it treats a surface with no LEDs. */
const struct led_notes *ctrl_sel_leds(void)
{
     return g_midi ? g_midi->leds : NULL;
}

/* Both halves of "is there a table yet" -- see the declaration in ctrl_map.h for
 * why the NULL from ctrl_sel_leds() is not enough on its own. */
int ctrl_sel_ready(void)
{
     return g_midi != NULL;
}

/* MIDI_DUMP */
static FILE *dump_f;
static unsigned long long dump_t0;
static int dump_started;

/* The maps this build knows, one table per selection. `kbd` is in BOTH on
 * purpose: MIDI_MAP=kbd has meant "no controller, keyboard only" since before
 * the two selections existed, and that is still exactly what it says here (the
 * evdev side resolves to the same map, so it is built once and the MIDI side
 * consumes no sequencer events). Keeping the name out of the MIDI table would
 * turn `MIDI_MAP=kbd` into a typo warning and a silent fall back to the FLX4,
 * which is the opposite of what an operator writing it wants. */
static const struct ctrl_map *const midi_maps[] = {
     &map_flx4, &map_jp21, &map_kbd, &map_none,
};
static const struct ctrl_map *const evdev_maps[] = {
     &map_kbd, &map_none,
};

/* The surface to fall back to when the environment says nothing, or says
 * something this build has never heard of.
 *
 * THE MIDI SIDE'S FALLBACK IS NOW A LOOKUP, not a pointer to map_flx4. It is the
 * map that the controller table's default row selects, so "which controller this
 * port targets" is written down in exactly one place -- controllers.c -- and a
 * table row naming a map this build does not have is caught twice over (by
 * test_controllers.c, and by the log line below). The fallback's REASON is
 * unchanged: the FLX4 is the surface this port targets and the value rb.conf
 * ships, so a shim started by hand with no environment and a typo'd name both end
 * up where they would have been anyway. The previous target's map is one word
 * away (MIDI_MAP=jp21) -- the point of the fallback is that a mistake is LOUD,
 * not that it is impossible.
 *
 * The evdev side is a plain pointer to map_kbd and stays one: it is the only
 * non-MIDI surface there is, it is the value rb.conf ships, and asking for no
 * keyboard on purpose has a name of its own (EVDEV_MAP=none), so the fallback
 * does not have to be one. It is not a controller and has no row.
 */
static const struct ctrl_map *default_midi_map(void)
{
     const struct controller *c = controllers_default();
     unsigned i;

     for (i = 0; i < sizeof midi_maps / sizeof midi_maps[0]; i++)
          if (strcmp(midi_maps[i]->name, c->map_name) == 0)
               return midi_maps[i];

     klog("knobshim2: the default controller '%s' selects map '%s', which this "
          "build does not have; falling back to '%s'\n",
          c->id, c->map_name, midi_maps[0]->name);
     return midi_maps[0];
}

static const struct ctrl_map *const default_evdev_map = &map_kbd;

/* Resolve one selection. The warning names the environment variable rather than
 * only the bad value, because with two selections "not a map this build has" no
 * longer says which of them was wrong. */
static const struct ctrl_map *pick_map(const char *env_name,
                                       const struct ctrl_map *const *table,
                                       unsigned n,
                                       const struct ctrl_map *dflt)
{
     const char *want = env_text(env_name, dflt->name);

     for (unsigned i = 0; i < n; i++)
          if (strcmp(table[i]->name, want) == 0)
               return table[i];
     klog("knobshim2: %s '%s' is not a map this build has; "
          "falling back to '%s'\n", env_name, want, dflt->name);
     return dflt;
}

/* ---- MIDI_DUMP / MIDI_REPLAY ---- */

static void dump_open(void)
{
     const char *path = env_text("MIDI_DUMP", NULL);

     if (!path)
          return;
     dump_f = fopen(path, "w");
     if (!dump_f) {
          klog("knobshim2: MIDI_DUMP %s: %s\n", path, strerror(errno));
          return;
     }
     fputs(MIDIDUMP_HEADER "\n", dump_f);
     klog("knobshim2: recording every sequencer event to %s\n", path);
}

static void dump_event(const struct snd_seq_event *ev)
{
     unsigned long long now_us;

     if (!dump_f)
          return;
     now_us = shim_now_ms() * 1000ULL;
     if (!dump_started) {
          dump_started = 1;
          dump_t0 = now_us;
     }
     mididump_write(dump_f, ev, now_us - dump_t0);
     /* Flushed per event: a bring-up dump is read after something has gone
      * wrong, which is exactly when the process may not have exited cleanly. */
     fflush(dump_f);
}

/* The one place an event becomes a keycode. Everything before the map runs here,
 * including for a replayed dump, so a replay takes the same path a live event
 * does -- except the MIDI_DUMP recorder, which stays live: with both set, a
 * replay is recorded with fresh timestamps, which is a legitimate way to
 * re-time a dump. */
static void ctrl_dispatch(const struct snd_seq_event *ev)
{
     /* Recorded above the KeyManager gate, on purpose: the events that arrive
      * before rbp's KeyManager exists are exactly the ones a bring-up dump is
      * for, and they are exactly the ones the gate below throws away. */
     dump_event(ev);

     if (!get_key_manager())
          return;
     /* The key dump's label for whatever this event turns into. Set here rather
      * than in the map because a map is a table and this is a property of the
      * path the event arrived on. */
     keylog_from("midi");
     if (g_midi->event)
          g_midi->event(ev);
}

/* The non-MIDI path: one raw evdev triple, gated exactly like a sequencer event
 * (ctrl_dispatch above) and for the same reason -- rbp's key path does not exist
 * yet, so an event sent now would be dropped inside rbp and the map's held-key
 * state would be one press ahead of what rbp saw. The MIDI_DUMP recorder is not
 * here: a keyboard is not a sequencer event, and mididump's format has nothing
 * to record it as. */
static void ctrl_input_dispatch(int type, int code, int value)
{
     if (!get_key_manager())
          return;
     keylog_from("evdev");
     if (g_evdev->input)
          g_evdev->input(type, code, value);
}

/* MIDI_REPLAY: drive a recorded dump through ctrl_dispatch. Runs alongside the
 * live poll loop rather than instead of it, so a dump can be replayed on a bench
 * with nothing plugged in. */
static void *replay_thread(void *arg)
{
     const char *path = arg;
     double speed;
     int n;

     while (!maps_ready)
          usleep(10000);
     for (int i = 0; i < 300 && !get_key_manager(); i++)
          usleep(100000);
     if (!get_key_manager()) {
          klog("knobshim2: MIDI_REPLAY %s: KeyManager never became ready\n", path);
          return NULL;
     }
     speed = env_dnum("MIDI_REPLAY_SPEED", 1.0);
     klog("knobshim2: MIDI_REPLAY %s at speed %.2f\n", path, speed);
     n = mididump_replay(path, speed, ctrl_dispatch);
     if (n < 0)
          klog("knobshim2: MIDI_REPLAY %s: cannot open\n", path);
     else
          klog("knobshim2: MIDI_REPLAY %s: %d events\n", path, n);
     return NULL;
}

/* ---- KEY_DUMP / KEY_REPLAY ---- */

/* One recorded command, back into rbp. Straight to send_rx_key_fl(): a key dump
 * holds the keycodes themselves, so a replay needs neither a map nor a
 * sequencer nor a controller -- it drives the same call the shim's own maps do,
 * which is what makes it a replay of the commands rbp received rather than of
 * one build's opinion about which note means which control. */
static void replay_key_event(const struct keylog_event *ev)
{
     send_rx_key_fl(ev->key, ev->op, ev->ch, ev->param, ev->fval, ev->lval);
}

/* KEY_REPLAY: drive a recorded key dump through send_rx_key_fl. Runs alongside
 * the live path rather than instead of it, exactly like MIDI_REPLAY, and -- with
 * KEY_DUMP set as well -- is itself recorded with fresh timestamps, which is a
 * legitimate way to re-time a dump. */
static void *key_replay_thread(void *arg)
{
     const char *path = arg;
     double speed;
     int n;

     for (int i = 0; i < 300 && !get_key_manager(); i++)
          usleep(100000);
     if (!get_key_manager()) {
          klog("knobshim2: KEY_REPLAY %s: KeyManager never became ready\n", path);
          return NULL;
     }
     speed = env_dnum("KEY_REPLAY_SPEED", 1.0);
     klog("knobshim2: KEY_REPLAY %s at speed %.2f\n", path, speed);
     keylog_from("replay");
     n = keylog_replay(path, speed, replay_key_event);
     if (n < 0)
          klog("knobshim2: KEY_REPLAY %s: cannot open\n", path);
     else
          klog("knobshim2: KEY_REPLAY %s: %d events\n", path, n);
     return NULL;
}

/* ---- threads ---- */

static void *midi_thread(void *arg)
{
     const char *replay;
     pthread_t tid;
     int n_evdev;
     (void)arg;

     if (!is_rbp_process())
          return NULL;

     verbose = env_on("KNOB_VERBOSE", 0);
     dump_open();
     /* KEY_DUMP: the other end of the same intent as MIDI_DUMP, and the one that
      * survives a map change -- see keylog.h. Opened here, before the maps, so a
      * dump from a run that died early still has its header. */
     keylog_open();

     g_midi = pick_map("MIDI_MAP", midi_maps,
                       (unsigned)(sizeof midi_maps / sizeof midi_maps[0]),
                       default_midi_map());
     g_evdev = pick_map("EVDEV_MAP", evdev_maps,
                        (unsigned)(sizeof evdev_maps / sizeof evdev_maps[0]),
                        default_evdev_map);

     /* Clear the shared tables HERE, once, before either build -- a build is
      * additive from now on (ctrl_map.h). Leaving the reset inside the builds is
      * what would make them order-dependent: map_kbd.c's puts nothing back, so a
      * keyboard built second would leave the FLX4 with no bindings at all and
      * every button on the controller dead. */
     ctrl_bindings_reset();
     g_midi->build();
     if (g_evdev != g_midi)
          g_evdev->build();
     maps_ready = 1;
     klog("knobshim2: maps: MIDI_MAP='%s' EVDEV_MAP='%s'%s; %d notes, "
          "%d abs knobs; g_speaker_gain addr=%p cue_gain=%p cue_mix=%p\n",
          g_midi->name, g_evdev->name,
          g_evdev == g_midi ? " (one map on both sides, built once)" : "",
          note_map_n, abs_map_n, (void *)&g_speaker_gain,
          (void *)&g_cue_gain, (void *)&g_cue_mix);

     /* The selections, into the key dump's header. The recorder cannot know them
      * (it is shared between the two libraries and this is knobshim's business),
      * and a dump is only interpretable beside the maps that produced its
      * keycodes' meaning -- the keycodes themselves are the same regardless, which
      * is why the dump replays without these, but a reader downstream wants to
      * know what the operator was using. */
     keylog_note("shim maps MIDI_MAP=%s EVDEV_MAP=%s", g_midi->name,
                 g_evdev->name);

     /* The evdev reader belongs to the EVDEV_MAP selection, never to the MIDI
      * one -- which is the whole of the fix for "the keyboard does nothing while
      * the FLX4 works": the map being asked about non-MIDI sources is now the
      * keyboard map even when the FLX4 is the controller. Started before the
      * KeyManager wait, so a keyboard starts working as soon as rbp's key path
      * exists, and independent of both the sequencer and any controller being
      * found. EVDEV_MAP=none (or a map that leaves devices() NULL) starts
      * nothing, which is how the old behaviour is asked for on purpose. */
     n_evdev = g_evdev->devices ? g_evdev->devices() : 0;
     if (n_evdev > 0) {
          if (evdev_start(ctrl_input_dispatch) == 0)
               klog("knobshim2: evdev map '%s': %d non-MIDI source(s) wanted; "
                    "evdev reader started\n", g_evdev->name, n_evdev);
          else
               klog("knobshim2: evdev map '%s': %d non-MIDI source(s) wanted but "
                    "the evdev reader would not start\n", g_evdev->name, n_evdev);
     }

     replay = env_text("MIDI_REPLAY", NULL);
     if (replay && pthread_create(&tid, NULL, replay_thread, (void *)replay) == 0)
          pthread_detach(tid);

     replay = env_text("KEY_REPLAY", NULL);
     if (replay && pthread_create(&tid, NULL, key_replay_thread,
                                  (void *)replay) == 0)
          pthread_detach(tid);

     for (int i = 0; i < 300; i++) {
          if (get_key_manager())
               break;
          usleep(100000);
     }
     if (!get_key_manager()) {
          klog("knobshim2: KeyManager never became ready\n");
          return NULL;
     }
     klog("knobshim2: KeyManager ready, opening sequencer...\n");

     /* Whatever each surface needs rbp to be told, told before the first event:
      * anything sent earlier is dropped by rbp's own mixer init. MIDI side first,
      * so a controller's deck routing and mixer defaults are established before a
      * keyboard adds its (empty) startup, which is the order these were written
      * in and the order their tests pin. */
     g_midi->startup();
     if (g_evdev != g_midi)
          g_evdev->startup();

     seq_setup();
     if (seq_fd < 0) {
          if (n_evdev <= 0) {
               klog("knobshim2: sequencer setup failed, giving up\n");
               return NULL;
          }
          /* Fatal without a non-MIDI source, and only then: the evdev selection
           * has a source that is not on the sequencer, and a missing
           * /dev/snd/seq is a property of the image (fix-dev.sh modprobes it
           * before launching) rather than a reason to take down a shim whose
           * keyboard works without it.
           *
           * Blocking rather than returning keeps every thread this shim owns
           * alive -- the evdev reader, the tick and the LED threads all outlive
           * this one, and returning would leave them with no midi_thread to
           * restart. The lines around this stay loud: no sequencer also means no
           * MIDI input, so "the keyboard works and nothing else does" should
           * never be a mystery. Nothing here retries seq_setup(); a Pi that
           * boots without the module needs fix-dev.sh, not a second chance. */
          klog("knobshim2: sequencer setup failed (reason above), but evdev map "
               "'%s' has a non-MIDI source: the keyboard still works and this "
               "thread stays up. There will be no MIDI input and no LED/meter "
               "output for as long as rbp runs.\n", g_evdev->name);
          for (;;)
               pause();
     }
     klog("knobshim2: reading sequencer events (MIDI map '%s')\n", g_midi->name);

     /* The sequencer is subscribed whichever map was selected -- seq_setup() runs
      * unconditionally above -- so a map with no event() is not "no controller",
      * it is "nothing done with the controller's events". Saying so is worth a
      * line: under MIDI_MAP=kbd the FLX4 is still found and its LED/meter route
      * still exists. What that map does NOT get is a panel to light: it publishes
      * leds = NULL, and ctrl_sel_leds() below returns it, so the LED bridge sends
      * nothing. (docs/16 used to claim no subscription was made, which was wrong
      * on both counts.) */
     if (!g_midi->event)
          klog("knobshim2: MIDI map '%s' has no event handler: sequencer events "
               "are read and dropped, and any controller found is subscribed "
               "with nothing on the other end of it. That is a selection, not a "
               "failure -- EVDEV_MAP handles the keyboard.\n", g_midi->name);

     midi_poll_forever(ctrl_dispatch);
     return NULL;
}

/* The maps' ~20 ms heartbeat: button-hold timeouts and idle state, the things
 * that are about time rather than about an event. One thread for all of them,
 * because they are all short checks at the same cadence.
 *
 * THE CONTROLLER KEEPALIVE RIDES THIS TICK rather than getting a thread of its
 * own. 20 ms against the FLX4's 200 ms period is ten chances per period, which is
 * finer than the period itself needs, and a second thread sleeping 5 Hz would be
 * a thread to start, name and keep alive for one message every fifth of a second.
 *
 * WHAT IT SENDS AND TO WHOM is the controller table's business, not this
 * function's: the row comes from the port name of the surface actually
 * subscribed (controllers_by_hint), so it follows what is PLUGGED IN rather than
 * what MIDI_MAP selected. Under MIDI_MAP=kbd -- a map with no row -- the FLX4 is
 * still attached, still lit and still polled, which is the behaviour that
 * selection has always had. When nothing is attached the name is empty, the
 * lookup is NULL and the whole thing is one comparison. */
static void *tick_thread(void *arg)
{
     const int ka_on = env_on("CTRL_KEEPALIVE", 1);
     unsigned long long last_ka = 0;
     int ka_announced = 0;
     (void)arg;
     if (!is_rbp_process())
          return NULL;
     if (!ka_on)
          klog("knobshim2: keepalive: off (CTRL_KEEPALIVE=0); no surface is "
               "polled\n");
     for (;;) {
          unsigned long long now;
          usleep(20000);   /* 50 Hz */
          /* Both sides tick, and a shared map ticks once: a hold timeout driven
           * twice per round would expire at half the interval it was written
           * for, which is a bug that would read as a twitchy button. */
          if (g_midi && g_midi->tick)
               g_midi->tick();
          if (g_evdev && g_evdev != g_midi && g_evdev->tick)
               g_evdev->tick();

          if (!ka_on)
               continue;
          now = shim_now_ms();
          {
               const struct controller *c =
                    controllers_by_hint(midi_surface_name());

               /* `last_ka` moves only when the message actually went out, so a
                * tick that finds no route is retried on the next one instead of
                * waiting out a period that was never used. That costs nothing
                * when there is no route -- midi_sysex_out() returns without a
                * syscall -- and it is why the per-send line is gated on
                * KNOB_VERBOSE: unplugged, this arm runs every 20 ms forever. */
               if (controller_keepalive_due(c, now, last_ka) &&
                   midi_sysex_out(c->keepalive, c->keepalive_len)) {
                    last_ka = now;
                    if (!ka_announced) {
                         ka_announced = 1;
                         klog("knobshim2: keepalive: '%s' %u byte(s) every %u ms "
                              "to '%s'\n", c->name, c->keepalive_len,
                              c->keepalive_ms, midi_surface_name());
                    } else if (verbose) {
                         klog("knobshim2: keepalive sent\n");
                    }
               }
          }
     }
     return NULL;
}

/* Stub out Pioneer PowerManager callbacks.
 * rbp's power-manager object is not present on this rig, so [UsbStorageManager+80] is NULL.
 * When a USB drive mounts/unmounts, rbp calls notifyPermissionChanged(NULL), etc. which segfaults. */
__attribute__((visibility("default"))) void _ZN3uif13IPowerManager23notifyPermissionChangedEv(void *this) { (void)this; }
__attribute__((visibility("default"))) void _ZN3uif13IPowerManager23notifyPreparedToStandbyEi(void *this, int a) { (void)this; (void)a; }
__attribute__((visibility("default"))) void _ZN3uif13IPowerManager21notifyAutoStandbyTimeEii(void *this, int a, int b) { (void)this; (void)a; (void)b; }
__attribute__((visibility("default"))) void _ZN3uif13IPowerManager22notifyScreenSaverModeEb(void *this, int a) { (void)this; (void)a; }
__attribute__((visibility("default"))) void _ZN3uif13IPowerManager12regPermitterEPNS_21IAutoStandbyPermitterE(void *this, void *a) { (void)this; (void)a; }
__attribute__((visibility("default"))) void _ZN3uif13IPowerManager15removePermitterEPNS_21IAutoStandbyPermitterE(void *this, void *a) { (void)this; (void)a; }
__attribute__((visibility("default"))) void _ZN3uif13IPowerManager12reqStandbyOnEv(void *this) { (void)this; }
__attribute__((visibility("default"))) void _ZN3uif13IPowerManager16prepareToStandyEv(void *this) { (void)this; }

/* USB stick watcher thread.
 *
 * rbp has TWO media slots and this port now feeds both of them: slot 1 from
 * /tmp/udev_usb1 -> /media/usb1/sda1 and slot 2 from /tmp/udev_usb2 ->
 * /media/usb2/sda1 (usb-watch.sh assigns them; the first stick is slot 1).
 * rbp holds all four udev FIFOs open from startup, so the second slot needs
 * nothing new on that side.
 *
 * The words below are rbp's device LIST for the UI: USB1 is kind 2 with UI
 * device number 3, USB2 is kind 3 with device number 2. The engine
 * (Total_MainUsbMessageProc) writes kind 3 by itself, so on a build with only
 * slot 1 populated the UI draws a blank second device and USB1's stick label is
 * hidden. That is the whole reason this thread exists: while slot 2 is EMPTY it
 * keeps kind 3 cleared. Slot 1's treatment is unchanged from the single-slot
 * port, write for write.
 *
 * With a stick actually in slot 2 the suppression is lifted -- the second device
 * is real, and rbp's own two UsbStorageManager objects already report it (that is
 * what the USB STOP chooser reads). Measured 2026-10-07: feeding /tmp/udev_usb2
 * takes manager 2 from media=0 to media=2 with slot 1 untouched, so rbp needs no
 * help accepting the device.
 *
 * THE NAME OF THE DEVICE. rbp draws each row's DEVICE NAME from the 64-byte
 * UTF-16 name field at the HEAD of that device's property record (its
 * ConvertBrowseUi2Gui calls GetMountInfo_DevicePropertyInfo(0, kind) and copies
 * the whole field, or -- when the field's first UTF-16 unit is zero, which is
 * every stock run -- falls back to the literals "USB1"/"USB2"). So the operator's
 * own question, "can you display the USB label name instead of USB1 USB2?", is
 * answered by putting the stick's volume label in that field, which is what
 * usb_label_apply() below does.
 *
 * The label itself is read by usb-watch.sh, on the host, from blkid -- the same
 * string `lsblk -f` prints, and the only reader here that knows FAT12/16 from
 * FAT32 (where the volume label sits at a different offset in the boot sector;
 * hand-parsing it in C was tried and landed on the wrong byte). It is left at
 * /tmp/udev_usb<slot>.label, beside that slot's FIFO and for the same reason:
 * the chroot's /tmp is the host's, which is why /tmp/udev_usb1 reaches rbp at all.
 *
 * This thread is the only writer. rbp repopulates and clears these records on
 * mount events (~9 Clr callers), so the write is not one-shot: the field is
 * compared every tick and rewritten only when it differs.
 */
#define USB_LABEL_FILE1  "/tmp/udev_usb1.label"
#define USB_LABEL_FILE2  "/tmp/udev_usb2.label"

/* The two name fields: 4 bytes past the detect word this thread already writes
 * (kind 2 / USB1 -> 0x03256888, kind 3 / USB2 -> 0x03256944). Measured live
 * 2026-10-07 by writing "AAAA1111"/"BBBB2222222" into them and reading both back
 * off the SOURCE screen, in full and with no corruption of the other row. They
 * are ordinary writable .bss -- no mprotect, and no code patched to reach them. */
#define USB1_NAME_FIELD  0x0325688Cu
#define USB2_NAME_FIELD  0x03256948u

/* One label file, read whole. Answers its length -- 0 when the file is absent,
 * empty or unreadable -- and always NUL-terminates. The raw syscall is not
 * optional: rbp's own open/read are interposed by fbshim in this same process. */
static int label_file_read(const char *path, char *buf, int max)
{
     int fd, n;

     if (max <= 0)
          return 0;
     fd = real_open(path, O_RDONLY, 0);
     if (fd < 0)
          return 0;
     n = (int)real_read(fd, buf, (size_t)(max - 1));
     real_close(fd);
     if (n < 0)
          return 0;
     buf[n] = '\0';
     return n;
}

/* Put one slot's label where rbp will draw it, or hand rbp's own name back.
 *
 * `present` is whether the slot holds a stick at all; `owned` is this thread's
 * memory of whether it has ever written this field. A field the shim has never
 * named is left COMPLETELY alone -- not even cleared -- which is what keeps a
 * unit running an older usb-watch.sh (one that writes no label files) byte-for-
 * byte the shim it was before this existed. Once the shim has owned the field it
 * keeps it equal to the label, empty included, so a stick that goes takes its
 * name with it.
 *
 * The whole 64 bytes are compared and rewritten only on a difference: rbp
 * repopulates these records itself, and a blind store every 100 ms would fight it
 * for the field on every tick. */
static void usb_label_apply(unsigned long addr, const char *path, int present,
                            int *owned)
{
     volatile unsigned char *field = (volatile unsigned char *)(uintptr_t)addr;
     unsigned char want[USB_LABEL_FIELD];
     char raw[USB_LABEL_FIELD];
     char text[USB_LABEL_MAX + 1];
     int n, i;

     n = 0;
     if (present) {
          n = label_file_read(path, raw, (int)sizeof raw);
          n = usb_label_sanitize(raw, n, text, USB_LABEL_MAX);
     }

     /* No stick, or a stick with no label of its own: rbp's "USB1"/"USB2" is the
      * right answer, and if the field was never ours there is nothing to undo. */
     if (n == 0 && !*owned)
          return;

     if (n > 0)
          usb_label_pack(text, want, USB_LABEL_FIELD);
     else
          memset(want, 0, sizeof want);

     for (i = 0; i < USB_LABEL_FIELD; i++)
          if (field[i] != want[i])
               break;
     if (i < USB_LABEL_FIELD) {
          for (i = 0; i < USB_LABEL_FIELD; i++)
               field[i] = want[i];
          /* The rows are cached; without this the column keeps drawing the old
           * name until something else happens to make it repaint. */
          *(volatile uint32_t *)0x326e128 = 1;
          klog("knobshim2: device name 0x%08lx <- '%s'\n", addr,
               n > 0 ? text : "(rbp's own)");
     }

     *owned = (n > 0);
}

static void *usb_auto_thread(void *arg)
{
     (void)arg;
     int last_mounted = 0, last_mounted2 = 0;
     int label1_owned = 0, label2_owned = 0;
     for (;;) {
          usleep(100000); /* 100 ms */
          if (!get_key_manager())
               continue;

          int mounted  = access("/media/usb1/sda1/PIONEER/rekordbox/export.pdb", F_OK) == 0;
          int mounted2 = access("/media/usb2/sda1/PIONEER/rekordbox/export.pdb", F_OK) == 0;

          /* Before the early-out below, on purpose: a stick that left has to
           * have given its name up as well. */
          usb_label_apply(USB1_NAME_FIELD, USB_LABEL_FILE1, mounted, &label1_owned);
          usb_label_apply(USB2_NAME_FIELD, USB_LABEL_FILE2, mounted2, &label2_owned);

          if (!mounted && !mounted2) {
               if (last_mounted || last_mounted2) {
                    /* Both slots empty: clear the whole device list. */
                    *(volatile uint32_t *)0x03256888 = 0;
                    *(volatile uint32_t *)0x03256944 = 0;
                    *(volatile uint32_t *)0x326f8b4 = 0;
                    *(volatile uint32_t *)0x326e128 = 1;
                    klog("knobshim2: USB removed\n");
               }
               last_mounted = last_mounted2 = 0;
               continue;
          }

          volatile uint32_t *p_det_usb1 = (volatile uint32_t *)0x03256888;
          volatile uint32_t *p_det_usb2 = (volatile uint32_t *)0x03256944;
          volatile uint32_t *p_media    = (volatile uint32_t *)0x326f8b4;
          volatile uint32_t *p_mode     = (volatile uint32_t *)0x326f8b8;
          volatile uint32_t *p_dev      = (volatile uint32_t *)0x326f8bc;
          volatile uint32_t *p_refresh  = (volatile uint32_t *)0x326e128;

          /* Keep kind 3 cleared ONLY while slot 2 holds no stick: an empty
           * second device is the phantom, a populated one is a device. */
          if (!mounted2 && *p_det_usb2 != 0) {
               *p_det_usb2 = 0;
               *p_refresh = 1;
          }

          if (mounted) {
               /* When USB1 is ready (kind2=2), ensure UI knows media 2 is connected */
               if (*p_det_usb1 == 2) {
                    if (*p_media != 2) {
                         *p_media = 2;
                         *p_refresh = 1;
                    }
                    /* Ensure browse caution message is cleared so touchscreen is active */
                    volatile uint32_t *p_caution = (volatile uint32_t *)0x05a191fc;
                    if (*p_caution != 0) {
                         *p_caution = 0;
                    }
               }

               if (!last_mounted) {
                    *p_det_usb1 = 2;
                    *p_media = 2;
                    *p_dev = 3;   /* Device 3 = USB 1 */
                    *p_refresh = 1;
                    klog("knobshim2: USB1 detected -> registered (dev=3)\n");
               } else if (*p_mode == 12 && *p_dev == 0) {
                    /* On Source menu: keep USB1 (device 3) active so the stick label shows */
                    *p_media = 2;
                    *p_dev = 3;
                    *p_refresh = 1;
               }
          } else if (last_mounted) {
               /* Slot 1 emptied while slot 2 still holds a stick: drop slot 1
                * alone. Nothing about slot 2 is touched here. */
               *p_det_usb1 = 0;
               *p_media = 0;
               *p_refresh = 1;
          }

          /* Slot 2 is a device in its own right, on its own kind and its own UI
           * device number (2). Its detect word is the only thing forced. */
          if (mounted2) {
               if (*p_det_usb2 != 2) {
                    *p_det_usb2 = 2;
                    *p_refresh = 1;
               }
               if (!last_mounted2)
                    klog("knobshim2: USB2 detected -> registered (dev=2)\n");
          } else if (last_mounted2) {
               *p_det_usb2 = 0;
               *p_refresh = 1;
          }

          last_mounted = mounted;
          last_mounted2 = mounted2;
     }
     return NULL;
}

__attribute__((constructor))
static void knobshim2_init(void)
{
     pthread_t tid;
     led_sweep = env_on("LED_SWEEP", 0);        /* must be known before
                                                 * spawning the sweep thread */
     if (pthread_create(&tid, NULL, vu_thread, NULL) == 0)
          pthread_detach(tid);
     if (pthread_create(&tid, NULL, midi_thread, NULL) == 0)
          pthread_detach(tid);
     if (pthread_create(&tid, NULL, tick_thread, NULL) == 0)
          pthread_detach(tid);
     if (pthread_create(&tid, NULL, led_thread, NULL) == 0)
          pthread_detach(tid);
     if (led_sweep && pthread_create(&tid, NULL, led_sweep_thread, NULL) == 0)
          pthread_detach(tid);
     if (pthread_create(&tid, NULL, usb_auto_thread, NULL) == 0)
          pthread_detach(tid);
}
