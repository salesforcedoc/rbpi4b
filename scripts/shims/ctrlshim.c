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
#include "evdev_io.h"
#include "ctrl_map.h"

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
 * something this build has never heard of. The MIDI side falls back to the FLX4
 * for the same reason it always did: it is the surface this port targets, and it
 * is the value rb.conf ships, so a shim started by hand with no environment and
 * a typo'd name both end up where they would have been anyway. The previous
 * target's map is one word away (MIDI_MAP=jp21) -- the point of the fallback is
 * that a mistake is LOUD, not that it is impossible.
 *
 * The evdev side falls back to the keyboard: it is the only non-MIDI surface
 * there is, it is the value rb.conf ships, and asking for no keyboard on purpose
 * has a name of its own (EVDEV_MAP=none), so the fallback does not have to be
 * one.
 */
static const struct ctrl_map *const default_midi_map = &map_flx4;
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

     g_midi = pick_map("MIDI_MAP", midi_maps,
                       (unsigned)(sizeof midi_maps / sizeof midi_maps[0]),
                       default_midi_map);
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
 * because they are all short checks at the same cadence. */
static void *tick_thread(void *arg)
{
     (void)arg;
     if (!is_rbp_process())
          return NULL;
     for (;;) {
          usleep(20000);   /* 50 Hz */
          /* Both sides tick, and a shared map ticks once: a hold timeout driven
           * twice per round would expire at half the interval it was written
           * for, which is a bug that would read as a twitchy button. */
          if (g_midi && g_midi->tick)
               g_midi->tick();
          if (g_evdev && g_evdev != g_midi && g_evdev->tick)
               g_evdev->tick();
     }
     return NULL;
}

/* Stub out Pioneer PowerManager callbacks.
 * Prime GO lacks Pioneer's power manager hardware, so [UsbStorageManager+80] is NULL.
 * When a USB drive mounts/unmounts, rbp calls notifyPermissionChanged(NULL), etc. which segfaults. */
__attribute__((visibility("default"))) void _ZN3uif13IPowerManager23notifyPermissionChangedEv(void *this) { (void)this; }
__attribute__((visibility("default"))) void _ZN3uif13IPowerManager23notifyPreparedToStandbyEi(void *this, int a) { (void)this; (void)a; }
__attribute__((visibility("default"))) void _ZN3uif13IPowerManager21notifyAutoStandbyTimeEii(void *this, int a, int b) { (void)this; (void)a; (void)b; }
__attribute__((visibility("default"))) void _ZN3uif13IPowerManager22notifyScreenSaverModeEb(void *this, int a) { (void)this; (void)a; }
__attribute__((visibility("default"))) void _ZN3uif13IPowerManager12regPermitterEPNS_21IAutoStandbyPermitterE(void *this, void *a) { (void)this; (void)a; }
__attribute__((visibility("default"))) void _ZN3uif13IPowerManager15removePermitterEPNS_21IAutoStandbyPermitterE(void *this, void *a) { (void)this; (void)a; }
__attribute__((visibility("default"))) void _ZN3uif13IPowerManager12reqStandbyOnEv(void *this) { (void)this; }
__attribute__((visibility("default"))) void _ZN3uif13IPowerManager16prepareToStandyEv(void *this) { (void)this; }

/* USB stick auto-detection watcher thread.
 * On the Denon Prime GO there is only ONE rear USB-A port, but the Pioneer RX3
 * firmware has 2 USB ports: USB1 (kind 2 in UI, device 3) and USB2 (kind 3 in UI, device 2).
 * Pioneer's internal engine (Total_MainUsbMessageProc) writes to kind 3 (USB2), which causes
 * the UI to display a blank/phantom "USB2" and hides the USB1 stick label.
 * This thread continuously:
 *   1. Monitors for mounted Rekordbox stick (/media/usb1/sda1/PIONEER/rekordbox/export.pdb).
 *   2. Automatically redirects any kind 3 (USB2) detect flags and property info into kind 2 (USB1).
 *   3. Keeps kind 3 cleared to 0 so phantom USB2 is NEVER reported.
 *   4. Ensures USB1 detect flag = 2, uiConnectedMedia = 2 (USB1 only), browseDevice = 3 (USB1).
 *   5. Opens the Source menu on fresh attach, and clears state on detach.
 */
static void *usb_auto_thread(void *arg)
{
     (void)arg;
     int last_mounted = 0;
     for (;;) {
          usleep(100000); /* 100 ms */
          if (!get_key_manager())
               continue;

          int mounted = access("/media/usb1/sda1/PIONEER/rekordbox/export.pdb", F_OK) == 0;
          if (mounted) {
               volatile uint32_t *p_det_usb1 = (volatile uint32_t *)0x03256888;
               volatile uint32_t *p_det_usb2 = (volatile uint32_t *)0x03256944;
               volatile uint32_t *p_media    = (volatile uint32_t *)0x326f8b4;
               volatile uint32_t *p_mode     = (volatile uint32_t *)0x326f8b8;
               volatile uint32_t *p_dev      = (volatile uint32_t *)0x326f8bc;
               volatile uint32_t *p_refresh  = (volatile uint32_t *)0x326e128;

               /* Suppress phantom USB2 (kind 3) on Prime GO since it has only 1 physical port */
               if (*p_det_usb2 != 0) {
                    *p_det_usb2 = 0;
                    *p_refresh = 1;
               }

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
                    *p_det_usb2 = 0;
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
               /* Stick unplugged: clear detect flags */
               *(volatile uint32_t *)0x03256888 = 0;
               *(volatile uint32_t *)0x03256944 = 0;
               *(volatile uint32_t *)0x326f8b4 = 0;
               *(volatile uint32_t *)0x326e128 = 1;
               klog("knobshim2: USB removed\n");
          }
          last_mounted = mounted;
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
