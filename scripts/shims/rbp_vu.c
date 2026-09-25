/*
 * rbp_vu.c -- the meter bridge.
 *
 * rbp's meters and the SC Live 4's are different instruments: rbp has 11
 * segments and meters pre-fader, the panel has 6 and meters post-fader. The
 * hook in rbp_bridge.c captures rbp's own numbers (in g_meter_bits) and this
 * module rescales them, applies the fader the panel applies, and pushes the
 * result to the panel as MIDI CCs -- the panel's meters are CC-driven, not
 * note-driven, so they need their own path.
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

/* Map a linear master peak (S24 full scale) to the SC Live 4 meter bitmask.
 * dBThresholds from JP21_Controller_Assignments.qml (Master VUMeter). */
static int vu_segments(int peak)
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
     if (n > 6)
          n = 6;                    /* ledCCValues has 6 usable segments */
     return (1 << n) - 1;
}

static int popcount32(unsigned int v)
{
     int n = 0;
     while (v) { n += v & 1; v >>= 1; }
     return n;
}
/* Map a meter level (rbp's dB-ish unit) to the SC Live 4 channel-meter
 * bitmask; thresholds from the JP21 Channel VUMeter. */
static int vu_db_to_segments(long db)
{
     static const double th[7] = { -45.8, -25.8, -12.3, -7.3, -4.3, -0.2, 20.0 };
     int n = 0, i;
     for (i = 0; i < 7; i++)
          if ((double)db >= th[i])
               n = i + 1;
     if (n > 6)
          n = 6;
     return (1 << n) - 1;
}

/* Channel meter.  rbp's meter has 11 segments (LED_TABLE = (1<<n)-1 for
 * n = 0..11); the SC Live 4 panel has 6 (4 white + 1 blue + 1 orange).  So
 * rescale 11 -> 6 (never cap - capping made the panel hit full at 6/11), then
 * apply the channel fader (rbp meters pre-fader; 0 dB top -> -60 dB bottom). */
static int vu_channel_segments(unsigned int bits, int fader10)
{
     static const double th[7] = { -45.8, -25.8, -12.3, -7.3, -4.3, -0.2, 20.0 };
     int n_rbp = popcount32(bits);
     int n_sc, i, out = 0;
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
     db = th[n_sc - 1];                     /* approx dB of top lit segment */
     f = (double)fader10 / 1023.0;
     if (f <= 0.0)
          return 0;
     if (f > 1.0)
          f = 1.0;
     db -= 60.0 * (1.0 - f);                /* fader attenuation */
     for (i = 0; i < 7; i++)
          if (db >= th[i])
               out = i + 1;
     if (out > 6)
          out = 6;
     return (1 << out) - 1;
}

/* Master meter: use rbp's OWN master meter (same source as the channel meters,
 * so they read consistently), with the Main Vol applied in dB.  Falls back to
 * the audioshim peak only if rbp's master meter is not captured. */
static int vu_master_segments(void)
{
     static const double th[7] = { -45.8, -25.8, -12.3, -7.3, -4.3, -0.2, 20.0 };
     int n_rbp = popcount32(g_meter_bits[0]);
     int n_sc, i, out = 0;
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
     db = th[n_sc - 1];
     if (g <= 0.0001)
          return 0;
     if (g > 1.0)
          g = 1.0;
     db += 20.0 * log10(g);                 /* Main Vol attenuation */
     for (i = 0; i < 7; i++)
          if (db >= th[i])
               out = i + 1;
     if (out > 6)
          out = 6;
     return (1 << out) - 1;
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
     /* LED_VU=0 means "this target has no meters", and it is read here rather
      * than checked before each send: install_meter_hook() patches rbp's own
      * machine code (see rbp_bridge.c), so a target with no meters must not have
      * the patch installed at all. Nothing is then hooked and nothing polls it.
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
     if (!env_on("LED_VU", 1)) {
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
          klog("knobshim2: LED_VU=0: meter bridge not installed\n");
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
     klog("knobshim2: VU bridge up%s\n", vu_test ? " (TEST sweep)" : "");

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
          if (vu_test) {
               int seg = (int)((t / 40) % 8);      /* 0..7 segments, ~1 s/step */
               l = r = (seg > 6) ? 63 : ((1 << seg) - 1);
          } else {
               if (g_meter_bits[0]) {
                    l = r = vu_master_segments();
               } else {
                    l = vu_segments(g_vu_peak[0]);
                    r = vu_segments(g_vu_peak[1]);
               }
          }
          /* Update the cache only when the message actually went out, so a level
           * that arrives before an output route exists is sent once one does. */
          if (l != last_l && midi_cc(15, 32, l))
               last_l = l;
          if (r != last_r && midi_cc(15, 33, r))
               last_r = r;

          /* Channel meters 1/2 come straight from rbp's own meter bitmask
           * (g_meter_bits, filled by the getLedValue hook): the number of
           * segments rbp lit is scaled to the SC Live 4 6-segment meter. */
          for (int m = 1; m <= 2; m++) {
               int v = vu_channel_segments(g_meter_bits[m], g_fader[m]);
               if (v != last_c[m] && midi_cc(m - 1, 10, v))
                    last_c[m] = v;             /* CC10 on ch0 = ch1, ch1 = ch2 */
          }
          if (vu_debug && (t % 40) == 0)
               klog("knobshim2: vudbg master=%03x ch1=%03x ch2=%03x seg=%d/%d "
                    "fader=%d/%d seen=%d/%d\n",
                    g_meter_bits[0], g_meter_bits[1], g_meter_bits[2],
                    last_c[1], last_c[2],
                    g_fader[1], g_fader[2], g_fader_seen[1], g_fader_seen[2]);
          /* Re-assert the physical fader/EQ/trim positions for the first
           * ~30 s.  rbp finishes initialising its mixer well after we are
           * loaded, and any value sent before that is lost - which is why a
           * fader already up at startup played back quiet until moved once.
           * The window is deliberately short: the panel just reports where
           * the controls physically are, but we don't want to keep re-sending
           * while the user is actively working. */
          if (t < 1200 && (t % 80) == 0) {
               led_query_absolute();
               /* rbp's mixer inits after we load, so the one-shot unity master
                * level in init() is dropped - re-assert it for ~30 s. */
               send_rx_key_f(K_MASTERLVL, OP_VALUE, CH_GLOBAL, 1023, 1.0f);
          } else if ((!g_fader_seen[1] || !g_fader_seen[2]) && (t % 80) == 0)
               led_query_absolute();
          t++;
          usleep(25000);   /* 40 Hz */
     }
     return NULL;
}
