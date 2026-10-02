/*
 * rbp_vu.c -- the meter bridge.
 *
 * rbp's meter and a panel's are different instruments: rbp has 11 segments and
 * meters PRE-fader, and a panel may have any height, may meter either side of
 * the fader, and may want its level in a shape of its own. The hook in
 * rbp_bridge.c captures rbp's own numbers (in g_meter_bits) and this module
 * rescales them onto the panel's meter and pushes the result out as MIDI CCs --
 * a panel's meters are CC-driven, not note-driven, so they need their own path.
 *
 * EVERY PANEL-SPECIFIC NUMBER IS IN THE MAP, not here. The channel base, the CC,
 * the value encoding, whether the panel meters before or after the fader, and
 * where its master meter lives are all read from the surface's `led_notes`
 * (ctrl_map.h) -- the same table that says which note lights which button, and
 * for the same reason: the SC Live 4's meter and the FLX4's disagree about all
 * of it, so a number hardcoded here is wrong for one of them by construction.
 * A surface whose table declares no meter (`n_meter_cc < 0`) gets no hook
 * installed and no traffic at all.
 *
 * The scaling is deliberately rbp's numbers rather than the audio shim's peak:
 * both meters then move together, and a level that looks wrong on the panel is
 * the same level that is wrong in rbp.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <poll.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <math.h>
#include <sys/mman.h>
#include <sound/asequencer.h>

/* State shared with the audio shim. This shim is first in LD_PRELOAD, so it is
 * the one that defines them (via shmstate.o) and owns them. */
#include "shmstate.h"

#include "syscalls.h"

#include "shimutil.h"
#include "rbp_abi.h"
#include "rbp_bridge.h"
#include "midi_io.h"
#include "rbp_vu.h"
#include "ctrl_map.h"    /* the surface's meter row -- see the header */

/* Last channel-fader position (10-bit, index 1/2 = deck 1/2).  rbp's channel
 * meter is PRE-fader, so the VU bridge scales it by this to behave like the
 * SC Live 4 (Engine OS meters are post-fader). */
int g_fader[3] = { 1023, 1023, 1023 };
int g_fader_seen[3];      /* set once the panel has reported a fader */

/* rbp's own meter's segment count, as the rescale below assumes it. 11 is what
 * this rbp build lights (LED_TABLE = (1<<n)-1 for n = 0..11) and what
 * RBP_METER_SEGMENTS in rbp_abi.h documents; LED_VU_SEGMENTS overrides it for a
 * target whose meter has a different height. Read once, by vu_thread. */
static int vu_rbp_segments = RBP_METER_SEGMENTS;

/* The selected surface's LED table, which is where its meter row lives. NULL is
 * "no surface, so nothing to drive" and is treated exactly like a table with no
 * meter row: silent, and no hook installed. Set once, at the top of vu_thread,
 * before anything reads it. */
static const struct led_notes *vu_leds;

/* The meter bridge speaks two units internally: a segment COUNT 0..6 (how many
 * of the panel's meter steps are lit) and the VALUE the panel wants on the
 * wire. Everything above the encoder works in counts; the map decides the rest.
 * The panel's six steps are the SC Live 4's (4 white + 1 blue + 1 orange) and
 * every panel this build drives has been rescaled onto them -- which is a
 * choice, not a fact about the FLX4, whose own meter is 8 lamps: six steps is
 * what the rescale has always produced and what the thresholds below describe,
 * so a taller panel simply lights its top lamps at the same moments instead of
 * gaining resolution it has nothing to gain it from. */

/* The panel's meter thresholds, in dB. From JP21_Controller_Assignments.qml
 * (Master VUMeter); one shared table so the channel and master paths cannot
 * drift apart. */
static const double vu_th[7] = { -45.8, -25.8, -12.3, -7.3, -4.3, -0.2, 20.0 };

/* How many of the panel's steps a dB reading lights, 0..6. */
static int vu_db_steps(double db)
{
     int n = 0, i;
     for (i = 0; i < 7; i++)
          if (db >= vu_th[i])
               n = i + 1;
     return (n > 6) ? 6 : n;          /* ledCCValues has 6 usable segments */
}

/* Map a linear master peak (S24 full scale) to a step count. Used only for the
 * fallback path, for a panel whose master meter reads the audioshim's peak
 * because rbp's own master meter was never captured. */
static int vu_peak_segments(int peak)
{
     static const double th[7] = { -45.7, -25.7, -12.2, -7.2, -4.2, -0.2, 20.0 };
     double db;
     int n = 0, i;
     if (peak <= 0)
          return 0;
     db = 20.0 * log10((double)peak / 8388607.0);
     for (i = 0; i < 7; i++)
          if (db >= th[i])
               n = i + 1;
     return (n > 6) ? 6 : n;
}

static int popcount32(unsigned int v)
{
     int n = 0;
     while (v) { n += v & 1; v >>= 1; }
     return n;
}

/* Channel meter. rbp's meter has 11 segments (LED_TABLE = (1<<n)-1 for
 * n = 0..11) and the panel's six are a rescale of it (never a cap - capping
 * made the panel hit full at 6/11).
 *
 * `pre_fader` is the SURFACE's answer, not ours: rbp meters before the channel
 * fader, so a panel that also meters before it (the FLX4) is being sent rbp's
 * own number and must not have a fader applied on top -- that would show the
 * operator a meter their own panel does not have, ducking when they pull a
 * fader down while the signal is untouched. A post-fader panel (the SC Live 4)
 * needs the attenuation, 0 dB top -> -60 dB bottom. */
static int vu_channel_segments(unsigned int bits, int fader10, int pre_fader)
{
     int n_rbp = popcount32(bits);
     int n_sc;
     double db, f;
     if (n_rbp <= 0)
          return 0;
     if (n_rbp > vu_rbp_segments)
          n_rbp = vu_rbp_segments;
     n_sc = (n_rbp * 6 + 5) / vu_rbp_segments;       /* round to 0..6 */
     if (n_sc <= 0)
          return 0;
     if (n_sc > 6)
          n_sc = 6;
     if (pre_fader)
          return n_sc;
     db = vu_th[n_sc - 1];                  /* approx dB of top lit segment */
     f = (double)fader10 / 1023.0;
     if (f <= 0.0)
          return 0;
     if (f > 1.0)
          f = 1.0;
     return vu_db_steps(db - 60.0 * (1.0 - f));   /* fader attenuation */
}

/* Master meter: use rbp's OWN master meter (same source as the channel meters,
 * so they read consistently), with the Main Vol applied in dB. Falls back to
 * the audioshim peak only if rbp's master meter is not captured.
 *
 * Main Vol is applied for every surface that has a master meter at all: it is
 * the master output level and not a channel fader, so the pre-fader question
 * above does not reach it. */
static int vu_master_segments(void)
{
     int n_rbp = popcount32(g_meter_bits[0]);
     int n_sc;
     double db, g = (double)g_master_gain;
     if (n_rbp <= 0)
          return 0;
     if (n_rbp > vu_rbp_segments)
          n_rbp = vu_rbp_segments;
     n_sc = (n_rbp * 6 + 5) / vu_rbp_segments;
     if (n_sc <= 0)
          return 0;
     if (n_sc > 6)
          n_sc = 6;
     if (g <= 0.0001)
          return 0;
     if (g > 1.0)
          g = 1.0;
     db = vu_th[n_sc - 1] + 20.0 * log10(g);      /* Main Vol attenuation */
     return vu_db_steps(db);
}

/* The FLX4's meter is a LEVEL with published bands -- Green1 0x26-0x40, Green2
 * 0x41-0x56, Orange1 0x57-0x64, Orange2 0x65-0x76, Red 0x77-0x7F, and dark
 * below 0x26 (Pioneer's DDJ-FLX4 MIDI message list, item 3-15). Six steps onto
 * five bands, so each step is anchored at a band's own bottom -- which is what
 * keeps the colour changes landing where the document puts them -- with the top
 * step at full scale. Index 0 is dark, and 0 is also what the Prime sends for
 * dark, so "off" needs no special case at the call site. */
static const int vu_flx4_level[7] = {
     0x00,   /* dark */
     0x26,   /* 1: Green1 */
     0x41,   /* 2: Green2 */
     0x57,   /* 3: Orange1 */
     0x65,   /* 4: Orange2 */
     0x77,   /* 5: Red */
     0x7F,   /* 6: Red, full */
};

/* A step count as the value this surface wants on the wire. An encoding this
 * build does not know sends nothing rather than a plausible guess: a meter that
 * is wrong in an unrecognisable way is indistinguishable at the panel from one
 * that is right. */
static int vu_encode(int steps)
{
     const struct led_notes *n = vu_leds;
     if (steps <= 0 || !n || n->n_meter_cc < 0)
          return 0;
     if (steps > 6)
          steps = 6;
     switch (n->meter_enc) {
     case METER_ENC_PRIME_SEGMENTS:
          return (1 << steps) - 1;
     case METER_ENC_FLX4_LEVEL:
          return vu_flx4_level[steps];
     default:
          return 0;
     }
}

/* The values rbp needs that are NOT meter work, re-asserted for ~30 s because
 * rbp finishes initialising its mixer well after we are loaded: anything sent
 * before that is silently dropped.
 *
 * They live here, and not in a map's startup(), because they are port
 * requirements rather than surface bindings -- they have to hold on every
 * target, including one whose map binds no fader and no master level at all.
 * That is also why BOTH vu_thread paths call this: the LED_VU=0 branch returns
 * before the meter loop, and a surface whose map declares no absolute control of
 * its own is exactly the one that needs the seed. Putting the seed inside the
 * meter loop made it dead code on the targets that need it most -- and note that
 * "no meter" and "no absolute controls" are two different surfaces' properties:
 * the FLX4 has plenty of absolute controls and, since 2026-10-01, a meter too.
 *
 * t is the caller's 40 Hz tick; the work happens every other second. */
static void mixer_defaults_tick(unsigned long t)
{
     if (t < 1200 && (t % 80) == 0) {
          int asked = led_query_absolute();
          /* rbp's mixer inits after we load, so the one-shot unity master
           * level in init() is dropped - re-assert it for ~30 s. */
          send_rx_key_f(K_MASTERLVL, OP_VALUE, CH_GLOBAL, 1023, 1.0f);
          /* ...and seed the two channel faders, when nobody is going to report
           * them. rbp's mixer engine builds with them at ZERO -- the safe
           * default for a panel that reports where its faders are, which is
           * exactly what the query above is for -- so on a surface that cannot
           * answer (the FLX4: a named surface with no absolute controls and no
           * rawmidi route) rbp's master stream is digital silence while a deck
           * plays. Measured on the unit 2026-09-26: `writei #N ... peak_m=0
           * mainvol=1.000` on every block of a playing deck, and real signal on
           * the next block after one CC 19 reached the map. Audibility had been
           * open since the port began, because nothing else sets this.
           *
           * Unity, and only until that channel's own control has moved: the map
           * sets g_fader_seen the moment the physical fader reports, so a fader
           * already where the operator wants it takes over and is never fought.
           * 30 s is the master level's window, reused rather than reinvented --
           * a fader that moved inside it is already safe, and one that never
           * moves wants the seed to stop anyway. */
          if (!asked) {
               static int told;
               for (int m = 1; m <= 2; m++)
                    if (!g_fader_seen[m]) {
                         if (!told) {
                              told = 1;
                              klog("knobshim2: this surface cannot report its "
                                   "absolute controls; channel faders seeded "
                                   "at unity (rbp builds them at zero, which "
                                   "is silence)\n");
                         }
                         send_rx_key_f(K_FADER, OP_VALUE, m, 1023, 1.0f);
                    }
          }
     } else if ((!g_fader_seen[1] || !g_fader_seen[2]) && (t % 80) == 0)
          led_query_absolute();
}

void *vu_thread(void *arg)
{
     unsigned long t = 0;
     int last_l = -1, last_r = -1;
     int last_c[3] = { -1, -1, -1 };
     int vu_test, vu_debug;
     (void)arg;
     if (!is_rbp_process())
          return NULL;
     /* THE SURFACE'S OWN TABLE decides whether there is a meter here, and
      * LED_VU=0 remains the manual over-ride on top of it. Both are read here
      * rather than checked before each send: install_meter_hook() patches rbp's
      * own machine code (see rbp_bridge.c), so a target with no meters must not
      * have the patch installed at all. Nothing is then hooked and nothing polls
      * it.
      *
      * Reading it from the map is what makes the FLX4's meter exist: its CH
      * LEVEL METER is host-driven (Pioneer's list item 3-15, CC 2 on the deck
      * channels) and the old hardcoded CC 10 was one of four numbers that are
      * simply another panel's -- so the belief that "the DDJ-FLX4 has no meters
      * to light", which had this target gated off, was a consequence of the
      * hardcoding and not an observation about the unit.
      *
      * The engine defaults in the branch below are NOT meter work and must not
      * be skipped along with the meters. rbp has no PFL keycode, so the cue bus
      * is not reachable from this shim by keycode at all and asserting it is
      * the shim's job on every target: the headphone bus should carry the
      * master before the operator touches anything. A meterless target
      * therefore still gets them -- on its own wait for rbp's mixer -- and only
      * then leaves.
      *
      * It is a startup DEFAULT and not a claim about the panel. The SC Live 4
      * has no master-cue button (strips 3/4's PFL toggles it, per map_jp21.c);
      * the FLX4 does have one and map_flx4.c binds it -- to me_get_master_cue()
      * as a toggle, so pressing it after startup cannot disagree with what is
      * set here. Neither map asserts this itself: a second writer for one piece
      * of engine state is the thing being avoided, not a conflict with the
      * button. */
     /* WAIT FOR THE SELECTION FIRST, and this is measured rather than defensive.
      * The MIDI surface is picked on midi_thread, and every thread is started
      * together from the constructor, so this one gets here first: the first run
      * of this code on the unit (2026-10-01 11:33) logged "no meter here (the
      * selected surface declares none)" while MIDI_MAP=flx4, whose table does
      * declare one -- and with the gate closed there is no hook and no meter, so
      * the failure would have looked exactly like a surface that has none.
      *
      * The bound and the cadence are the key-manager wait's, below. It is
      * ctrl_sel_ready() and not a NULL check on ctrl_sel_leds(), because NULL is
      * also what a surface with no LEDs answers forever. */
     for (int i = 0; i < 300 && !ctrl_sel_ready(); i++)
          usleep(100000);
     vu_leds = ctrl_sel_leds();
     if (!env_on("LED_VU", 1) || !vu_leds || vu_leds->n_meter_cc < 0) {
          /* Wait at 5 Hz rather than on a deadline: setting the cue before the
           * engine exists is silently dropped (mixer_engine() is how the loop
           * below knows it exists), and when rbp's mixer appears is not ours to
           * bound. The thread exits once they are set, so this costs nothing
           * after that. */
          for (;;) {
               if (mixer_engine()) {
                    me_set_master_cue(1);
                    me_set_stereo(1);     /* default: stereo (0 = mono split) */
                    klog("knobshim2: master cue enabled at startup\n");
                    break;
               }
               usleep(200000);
          }
          klog("knobshim2: no meter here (%s): meter bridge not installed\n",
               !env_on("LED_VU", 1) ? "LED_VU=0"
                                    : "the selected surface declares none");
          /* Leaving here is what left this target silent: the absolute-value
           * defaults below are not meter work, and the target that cannot
           * report its absolute controls is the one that needs them most. Run
           * the same 30 s window the meter path runs, then leave as before. */
          for (t = 0; t < 1200; t++) {
               mixer_defaults_tick(t);
               usleep(25000);   /* 40 Hz, as below */
          }
          return NULL;
     }
     vu_test = env_on("VU_TEST", 0);
     vu_debug = env_on("VU_DEBUG", 0);
     vu_rbp_segments = env_num("LED_VU_SEGMENTS", RBP_METER_SEGMENTS);
     if (vu_rbp_segments < 1)
          vu_rbp_segments = RBP_METER_SEGMENTS;   /* the rescale divides by it */
     /* No wait for an output route here: whether the panel is there yet has
      * nothing to do with whether rbp's meters should be hooked, and every send
      * below is retried until one lands. */
     install_meter_hook();
     /* Ask for the physical control positions only once rbp can accept them:
      * if the reply lands before rbp's mixer exists, the values are dropped and
      * rbp initialises the faders/EQs to their defaults (the fader then reads
      * "down" until it is moved once). */
     for (int i = 0; i < 300 && !get_key_manager(); i++)
          usleep(100000);
     led_query_absolute();
     klog("knobshim2: VU bridge up: channels CC %d on ch %d/%d%s, "
          "master CC %d/%d on ch %d%s\n",
          vu_leds->n_meter_cc,
          vu_leds->meter_ch_first, vu_leds->meter_ch_first + 1,
          vu_leds->meter_pre_fader ? " pre-fader" : "",
          vu_leds->n_meter_master_l, vu_leds->n_meter_master_r,
          vu_leds->meter_master_ch,
          vu_leds->meter_master_ch >= 0 ? "" : " (absent on this surface)");
     if (vu_test)
          klog("knobshim2: VU bridge up (TEST sweep)\n");

     for (;;) {
          int l, r;
          /* Enable rbp's master cue once the engine exists, so the headphone
           * bus contains the master (no panel we target has a MASTER CUE
           * button). The LED_VU=0 branch above does the same before it returns;
           * this one is the meters-present path, kept here so the order of
           * startup work on that path is unchanged. */
          static int mc_init = 0;
          if (!mc_init && mixer_engine()) {
               mc_init = 1;
               me_set_master_cue(1);
               me_set_stereo(1);     /* default: stereo (0 = mono split) */
               klog("knobshim2: master cue enabled at startup\n");
          }
          int master_here = (vu_leds->meter_master_ch >= 0);
          /* VU_TEST is the WIRING test, and it drives the channel meters as well
           * as the master pair -- which is what makes it usable on a surface
           * that has no master meter at all. The FLX4 is that surface, and
           * without this its only meter could not be exercised without audio:
           * the sweep would light nothing, and a wiring fault would be
           * indistinguishable from a silent deck. It repeats forever, which is
           * what lets the operator read it off the panel whenever they look. */
          int test_steps = vu_test ? (int)((t / 40) % 8) : -1;  /* ~1 s/step */
          if (test_steps >= 0) {
               l = r = vu_encode(test_steps);
          } else if (!master_here) {
               l = r = 0;         /* nothing to send: this surface has no master
                                   * meter -- the FLX4's two meters show channel
                                   * or master only by the operator's own
                                   * [Level Meter] switch, and the list gives no
                                   * master address of its own */
          } else if (g_meter_bits[0]) {
               l = r = vu_encode(vu_master_segments());
          } else {
               l = vu_encode(vu_peak_segments(g_vu_peak[0]));
               r = vu_encode(vu_peak_segments(g_vu_peak[1]));
          }
          /* Update the cache only when the message actually went out, so a level
           * that arrives before an output route exists is sent once one does. */
          if (master_here && l != last_l && midi_cc(vu_leds->meter_master_ch,
                                                    vu_leds->n_meter_master_l, l))
               last_l = l;
          if (master_here && r != last_r && midi_cc(vu_leds->meter_master_ch,
                                                    vu_leds->n_meter_master_r, r))
               last_r = r;

          /* Channel meters 1/2 come straight from rbp's own meter bitmask
           * (g_meter_bits, filled by the getLedValue hook): the number of
           * segments rbp lit is rescaled to the panel's six steps. */
          int vu_sent = -1, vu_sent_v = -1;
          for (int m = 1; m <= 2; m++) {
               int v = vu_encode(test_steps >= 0
                                     ? test_steps
                                     : vu_channel_segments(g_meter_bits[m],
                                                           g_fader[m],
                                                           vu_leds->meter_pre_fader));
               if (v != last_c[m]) {
                    int ok = midi_cc(vu_leds->meter_ch_first + (m - 1),
                                     vu_leds->n_meter_cc, v);
                    if (ok)
                         last_c[m] = v;
                    if (m == 1) {       /* what deck 1's meter was told */
                         vu_sent = ok;
                         vu_sent_v = v;
                    }
               }
          }
          /* Labelled in halves, and it is worth the space: this line used to
           * print rbp's own bitmasks as `ch1`/`ch2` and the values that went out
           * as `seg`, which reads as though the first pair were what the panel
           * was told. It cost a session -- the encoder was working perfectly
           * while `ch1=000` (rbp's deck meter, dark because nothing was playing)
           * looked like a channel that had never been sent to. */
          if (vu_debug && (t % 40) == 0)
               klog("knobshim2: vudbg rbp[m=%03x c1=%03x c2=%03x] "
                    "sent[master=%d/%d ch=%d/%d] fader=%d/%d seen=%d/%d "
                    "steps=%d enc=%d cc=%d/%d wire=%d/%s\n",
                    g_meter_bits[0], g_meter_bits[1], g_meter_bits[2],
                    last_l, last_r, last_c[1], last_c[2],
                    g_fader[1], g_fader[2], g_fader_seen[1], g_fader_seen[2],
                    test_steps, vu_leds->meter_enc,
                    vu_leds->n_meter_cc, vu_leds->meter_ch_first,
                    vu_sent_v, vu_sent < 0 ? "not-called"
                                           : (vu_sent ? "ok" : "REFUSED"));
          /* Re-assert the physical fader/EQ/trim positions for the first
           * ~30 s.  See mixer_defaults_tick(): it is shared with the LED_VU=0
           * branch above, which returns before this loop. */
          mixer_defaults_tick(t);
          t++;
          usleep(25000);   /* 40 Hz */
     }
     return NULL;
}
