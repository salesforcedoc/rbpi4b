/*
 * rbp_led.c -- the panel LED bridge.
 *
 * On the SC Live 4 every front-panel LED is driven by MIDI (Note On/Off to the
 * Control Surface), while rbp drives the XDJ-RX3's micons over /dev/subucom_spi*
 * -- dead FIFOs here. So rbp computes all of its LED state and none of it
 * reaches a panel; this module reads that state out of rbp's own LedStat table
 * and engine singletons and mirrors it onto the panel's notes.
 *
 * The NOTE NUMBERS in here (LED_N_*, LED_PAD_*, the LEDSTAT_* ids) are the
 * panel's, not rbp's: LEDSTAT_* are rbp's LedDef ids and live in rbp_abi.h, but
 * the notes are part of the SC Live 4 map and move to the per-controller map in
 * the next step, leaving this file a pure state-to-velocity function.
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

/* No syscalls.h: this module no longer opens or writes anything itself. The
 * panel output is midi_io.c's, which is also the only place that decides
 * whether it goes out on rawmidi or over the sequencer. */

#include "shimutil.h"
#include "rbp_abi.h"
#include "rbp_bridge.h"
#include "midi_io.h"
#include "rbp_led.h"

/* "loop-in armed" latch per deck (0 = deck 1), driven by the LOOP IN/OUT keys;
 * the LED bridge turns it into the SC Live 4 blink pattern. */
int led_loop_armed[2];

/* =====================================================================
 * SC Live 4 LED output bridge
 * ---------------------------------------------------------------------
 * On the SC Live 4 EVERY front-panel LED is driven by MIDI: Engine OS sends
 * Note On/Off to the "Control Surface" (rawmidi hw:0,0 = seq 16:0).  rbp,
 * however, drives the XDJ-RX3's EUP/SUB micons over /dev/subucom_spi*.0,
 * which fix-dev.sh creates as dead FIFOs here — so rbp computes LED state
 * (uif::LedStat, uif::panel_protocol::EupMiconTx/SubMiconTx) but it never
 * reaches the panel.
 *
 * This bridge reads rbp's own engine state through the PlayEngine singleton
 * (the object djengine::DjEngineIF::isPlaying()/isSyncOn()/... delegate to)
 * and mirrors the transport LEDs onto the SC Live 4 notes for the same
 * buttons (deck channels 4/5).
 *
 * Verified on-device: Note On ch4/note10 vel 0x7F = PLAY LED bright,
 * Note Off = dark; no loopback into the Control Surface input path.
 * ===================================================================== */
/* JP21 note numbers of the deck LEDs (same notes the buttons send) */
#define LED_N_SYNC      8
#define LED_N_CUE       9
#define LED_N_PLAY      10
#define LED_N_KEYLOCK   34
#define LED_N_VINYL     35
#define LED_N_SLIP      36
#define LED_N_LOOPIN    37
#define LED_N_LOOPOUT   38
#define LED_N_AUTOLOOP  39

#define LED_COUNT       9

static const int led_notes[LED_COUNT] = {
     LED_N_SYNC, LED_N_CUE, LED_N_PLAY, LED_N_KEYLOCK, LED_N_VINYL,
     LED_N_SLIP, LED_N_LOOPIN, LED_N_LOOPOUT, LED_N_AUTOLOOP
};

static int led_verbose = 0;
static int led_disabled = 0;
static int led_debug_loop = 0;
static int led_dump = 0;
static int led_pads = 1;      /* LED_PADS=0: no pad output (rbp's state is
                               * still mirrored for the other LEDs) */
int led_sweep = 0;
static unsigned long led_tick = 0;        /* 50 ms ticks, for blink */
static signed char led_last[2][LED_COUNT]; /* [deck][led] -1 = unknown */
static int led_prev_looping[2];
static int led_dbg_last[2];               /* last logged loop-state bitmask */
static int led_blink_phase;               /* current blink phase (0/1) */
static signed char led_pfl_last[2] = { -1, -1 };  /* mixer PFL LED state */

/* JP21 RGB performance pads: notes 15..22 per deck.  rbp's pad LEDs are
 * LedDef::ID 18..25 (confirmed in ui::Player::checkLedStat, which calls
 * checkHotCueLedState(..., 18..25)); other pad modes reuse the same ids and
 * just change state/color. */
#define LED_PAD_FIRST   18
#define LED_PAD_COUNT   8
static int led_pad_last[2][LED_PAD_COUNT];  /* last MIDI velocity, -1 unknown */

static unsigned char *ledstat_ptr(void)
{
     void *holder = *(void **)LEDMGR_HOLDER_GLOBAL;
     void *ledmgr;
     if (!holder)
          return NULL;
     ledmgr = *(void **)((char *)holder + LEDMGR_OFF_LEDSTAT);
     if (!ledmgr)
          return NULL;
     return (unsigned char *)ledmgr + LEDSTAT_OFF;
}

/* Look up rbp's own state for one (id, channel) LED.
 * Returns 0=off, 1=solid, 2=blink, 3=dim, or -1 if rbp has no such entry. */
static int ledstat_state(unsigned int id, unsigned int ch)
{
     unsigned char *ls = ledstat_ptr();
     unsigned char *arr;
     unsigned int count, i;
     if (!ls)
          return -1;
     count = *(unsigned short *)(ls + 4);
     arr = *(unsigned char **)(ls + 8);
     if (!arr || count == 0 || count > LED_DUMP_MAX)
          return -1;
     for (i = 0; i < count; i++) {
          unsigned char *e = arr + LED_ENTRY_SIZE * i;
          if (*(unsigned int *)(e + 0) == id && *(unsigned int *)(e + 4) == ch)
               return (int)*(unsigned int *)(e + 16);
     }
     return -1;
}

/* Read the stored RGB (3 bytes at Led entry +40/+41/+42; rbp's ColorLed::Rgb
 * is 0..255 per channel).  Returns 0 if rbp has no entry for (id, ch). */
static int ledstat_rgb(unsigned int id, unsigned int ch, unsigned char *out)
{
     unsigned char *ls = ledstat_ptr();
     unsigned char *arr;
     unsigned int count, i;
     if (!ls)
          return 0;
     count = *(unsigned short *)(ls + 4);
     arr = *(unsigned char **)(ls + 8);
     if (!arr || count == 0 || count > LED_DUMP_MAX)
          return 0;
     for (i = 0; i < count; i++) {
          unsigned char *e = arr + LED_ENTRY_SIZE * i;
          if (*(unsigned int *)(e + 0) == id && *(unsigned int *)(e + 4) == ch) {
               out[0] = e[40]; out[1] = e[41]; out[2] = e[42];
               return 1;
          }
     }
     return 0;
}

static void led_dump_scan(void)
{
     /* keyed by (id, channel) so table reordering does not produce false
      * changes; state 0xff = "not seen yet" */
     static unsigned char s_state[256][4];
     static int s_init = 0;
     unsigned char *ls = ledstat_ptr();
     unsigned char *arr;
     unsigned int count, i;

     if (!ls)
          return;
     if (!s_init) {
          memset(s_state, 0xff, sizeof(s_state));
          s_init = 1;
     }
     count = *(unsigned short *)(ls + 4);
     arr = *(unsigned char **)(ls + 8);
     if (!arr || count == 0 || count > LED_DUMP_MAX)
          return;

     for (i = 0; i < count; i++) {
          unsigned char *e = arr + LED_ENTRY_SIZE * i;
          unsigned int id = *(unsigned int *)(e + 0);
          unsigned int ch = *(unsigned int *)(e + 4);
          unsigned int st = *(unsigned int *)(e + 16);
          if (id >= 256 || ch >= 4 || st > 0xff)
               continue;
          if (s_state[id][ch] == (unsigned char)st)
               continue;
          klog("knobshim2: leddump id=%u ch=%u state %u -> %u rgb=%u,%u,%u\n",
               id, ch, s_state[id][ch] == 0xff ? 0 : s_state[id][ch], st,
               e[40], e[41], e[42]);
          s_state[id][ch] = (unsigned char)st;
     }
}

static void led_apply(int deck, int idx, int on)
{
     signed char want = (signed char)(on ? 1 : 0);
     if (led_last[deck][idx] == want)
          return;
     /* The byte building lives in midi_io.c, which is also what decides whether
      * this goes out on rawmidi or over the sequencer. Returns 0 when no route
      * is up yet, which leaves the LED to be retried next tick. */
     if (!midi_note(4 + deck, led_notes[idx], on ? 0x7f : 0x00))
          return;                        /* retry next tick */
     led_last[deck][idx] = want;
     if (led_verbose)
          klog("knobshim2: led deck%d note%d %s\n",
               deck + 1, led_notes[idx], on ? "on" : "off");
}

/* Prefer rbp's own state for an LED; fall back to a derived value when rbp
 * has no entry for it yet.  State 2 is rbp's blink request (e.g. SYNC blinks
 * when synced but the platter was nudged off beat), so we drive the panel
 * blink ourselves at the same cadence. */
static void led_from_table(int deck, int idx, unsigned int id, int fallback)
{
     int st = ledstat_state(id, (unsigned int)deck + 1);
     int on;
     if (st < 0)
          on = fallback;
     else if (st == 2)
          on = led_blink_phase;
     else
          on = (st != 0);
     led_apply(deck, idx, on);
}

/* SC Live 4 pad colour = Note On velocity.  Per the Engine OS Prime LED
 * convention, bits 4-5 = red, 2-3 = green, 0-1 = blue (2 bits each).  rbp
 * keeps 0..255 per channel, so take the top 2 bits.  Some builds want bit 6
 * (0x40) set for the bright range; PAD_BRIGHT=1 enables that (default: pure
 * 6-bit colour). */
static int pad_bright_bit = -1;

static unsigned char pad_encode_rgb(int r, int g, int b)
{
     unsigned char v = (unsigned char)(((r >> 6) << 4) | ((g >> 6) << 2) | (b >> 6));
     /* Read once, on first use: this runs per pad per tick, and the environment
      * does not change under us. By value, so an exported empty string (see
      * shimutil.h) is off rather than a failed atoi. */
     if (pad_bright_bit < 0)
          pad_bright_bit = env_num("PAD_BRIGHT", 0) ? 1 : 0;
     if (pad_bright_bit)
          v = (unsigned char)(v | 0x40);
     return v;
}

static void led_pad_apply(int deck, int pad, unsigned char vel)
{
     if (led_pad_last[deck][pad] == (int)vel)
          return;
     if (!midi_note(4 + deck, 15 + pad, vel))
          return;                        /* retry next tick */
     led_pad_last[deck][pad] = vel;
     if (led_verbose)
          klog("knobshim2: pad deck%d pad%d vel=0x%02x\n",
               deck + 1, pad + 1, vel);
}

/* Global (channel-15) panel LEDs, driven straight from rbp's LedStat.
 * The LedStat id is LedDef::ID + 8 for this group (verified live):
 *   EffectOnOff 40 -> 48, CfxFilter 33 -> 41, CfxSweep 34 -> 42,
 *   CfxDubEcho 35 -> 43, CfxNoise 36 -> 44.
 * State 2 = rbp wants a blink (e.g. the FX ON/OFF LED blinks while the
 * effect is active), so we drive the panel blink ourselves. */
static const struct { unsigned int id; int note; const char *name; } led_g_tab[] = {
     { 48, 26, "BfxOnOff" },
     { 41, 21, "CfxFilter" },
     { 43, 22, "CfxDubEcho" },
     { 44, 23, "CfxNoise" },
     { 42, 24, "CfxSweep" },
};
#define LEDG_COUNT ((int)(sizeof(led_g_tab) / sizeof(led_g_tab[0])))
static signed char led_last_g[LEDG_COUNT];

static void led_apply_g(int idx, int note, int on)
{
     signed char want = (signed char)(on ? 1 : 0);
     if (led_last_g[idx] == want)
          return;
     if (!midi_note(15, note, on ? 0x7f : 0x00))
          return;
     led_last_g[idx] = want;
     if (led_verbose)
          klog("knobshim2: led global note%d %s (%s)\n",
               note, on ? "on" : "off", led_g_tab[idx].name);
}

static void led_refresh(void)
{
     void *pe;
     int blink;
     /* midi_out_ready() rather than a device descriptor: the output may be the
      * sequencer, in which case there is no /dev/snd/midiC*D0 open at all. */
     if (led_disabled || !midi_out_ready())
          return;
     blink = (led_tick & 8) ? 1 : 0;      /* ~400 ms on / off */
     led_blink_phase = blink;

     /* global LEDs, straight from rbp (id/ch -> panel note) */
     for (int g = 0; g < LEDG_COUNT; g++) {
          int st = ledstat_state(led_g_tab[g].id, 0);
          int on = (st < 0) ? 0 : (st == 2 ? blink : (st != 0));
          led_apply_g(g, led_g_tab[g].note, on);
     }

     /* mixer PFL LEDs (SC Live 4 strips 1/2, note 13) from rbp's cue state */
     for (int m = 0; m < 2; m++) {
          int cue = me_get_cue(m);
          if (cue < 0)
               continue;
          if (led_pfl_last[m] != (signed char)cue) {
               if (midi_note(m, 13, cue ? 0x7f : 0x00))
                    led_pfl_last[m] = (signed char)cue;
          }
     }
     /* master cue LED on strips 3/4 (note 13) */
     {
          static signed char mc_last = -1;
          int mc = me_get_master_cue();
          if (mc >= 0 && mc_last != (signed char)mc) {
               if (midi_note(2, 13, mc ? 0x7f : 0x00) &&
                   midi_note(3, 13, mc ? 0x7f : 0x00))
                    mc_last = (signed char)mc;
          }
     }

     pe = *(void **)PLAYENGINE_GLOBAL;
     if (!pe)
          return;
     for (int i = 0; i < 2; i++) {
          int playing = ((int (*)(void *, int))PE_ISPLAYING)(pe, i) != 0;
          int loaded  = ((int (*)(void *, int))PE_ISLOADED)(pe, i) != 0;
          int sync    = ((int (*)(void *, int))PE_ISSYNCON)(pe, i) != 0;
          int mt      = ((int (*)(void *, int))PE_ISMASTERTEMPO)(pe, i) != 0;
          int vinyl   = ((int (*)(void *, int))PE_ISVINYLMODE)(pe, i) != 0;
          int slip    = ((int (*)(void *, int))PE_ISSLIPMODEON)(pe, i) != 0;
          int looping = ((int (*)(void *, int))PE_ISLOOPING)(pe, i) != 0;
          int canrel  = ((int (*)(void *, int))PE_ISCANRELOOP)(pe, i) != 0;
          int aloop   = ((int (*)(void *, int))PE_ISAUTOBEATLOOP)(pe, i) != 0;
          int armed;

          /* a loop that ends clears the "loop-in armed" latch */
          if (led_prev_looping[i] && !looping)
               led_loop_armed[i] = 0;
          led_prev_looping[i] = looping;
          /* Blink only while a loop is actually running, or while a loop-in
           * point has been set but not yet closed.  A loop that still exists
           * after exit (isPossibleToReLoop) must NOT keep the LED blinking. */
          armed = led_loop_armed[i];

          /* SYNC comes from rbp's own LED state (id 4), so the three states
           * survive: off / solid (locked) / blink (synced but nudged off
           * beat).  Falls back to isSyncOn() if rbp has no entry. */
          led_from_table(i, 0, LEDSTAT_SYNC, sync);
          led_apply(i, 1, loaded && !playing);        /* CUE */
          /* PLAY: solid while playing, blinks while paused on a loaded
           * track, dark with nothing loaded. */
          led_apply(i, 2, playing ? 1 : (loaded ? blink : 0));
          led_apply(i, 3, mt);                        /* KEY LOCK */
          led_apply(i, 4, vinyl);                     /* VINYL */
          led_apply(i, 5, slip);                      /* SLIP */
          /* SC Live 4 convention (verified against Engine OS on video):
           *   idle            -> both LEDs solid ON
           *   loop-in set     -> LOOP IN blinks, LOOP OUT solid
           *   loop running    -> both blink */
          led_apply(i, 6, (looping || armed) ? blink : 1);   /* LOOP IN */
          led_apply(i, 7, looping ? blink : 1);              /* LOOP OUT */
          led_apply(i, 8, aloop);                            /* AUTO LOOP */

          /* RGB performance pads: rbp LedDef::ID 18..25 -> notes 15..22.
           * LED_PADS=0 stops here: rbp's pad state is still read by the rest of
           * this function, the pads are simply not sent. */
          for (int p = 0; led_pads && p < LED_PAD_COUNT; p++) {
               unsigned char rgb[3];
               int st = ledstat_state(LED_PAD_FIRST + (unsigned)p, (unsigned)i + 1);
               if (st <= 0 ||
                   !ledstat_rgb(LED_PAD_FIRST + (unsigned)p, (unsigned)i + 1, rgb)) {
                    led_pad_apply(i, p, 0);
               } else if (st == 2) {
                    led_pad_apply(i, p, blink
                         ? pad_encode_rgb(rgb[0], rgb[1], rgb[2]) : 0);
               } else {
                    led_pad_apply(i, p, pad_encode_rgb(rgb[0], rgb[1], rgb[2]));
               }
          }

          if (led_debug_loop) {
               int bits = (looping << 0) | (canrel << 1) | (armed << 2);
               if (bits != led_dbg_last[i]) {
                    led_dbg_last[i] = bits;
                    klog("knobshim2: loopdbg d%d loop=%d canrel=%d armed=%d "
                         "inadj=%d outadj=%d aloop=%d loaded=%d "
                         "ls49=%d ls53=%d ls55=%d\n",
                         i + 1, looping, canrel, armed,
                         ((int (*)(void *, int))PE_ISLOOPINADJ)(pe, i),
                         ((int (*)(void *, int))PE_ISLOOPOUTADJ)(pe, i),
                         aloop, loaded,
                         ledstat_state(49, (unsigned int)i + 1),
                         ledstat_state(53, (unsigned int)i + 1),
                         ledstat_state(55, (unsigned int)i + 1));
               }
          }
     }
}

/* full table snapshot (for sweep attribution) */
static void led_dump_full(const char *tag)
{
     unsigned char *ls = ledstat_ptr();
     unsigned char *arr;
     unsigned int count, i;
     if (!ls)
          return;
     count = *(unsigned short *)(ls + 4);
     klog("knobshim2: snapshot %s count=%u\n", tag, count);
     arr = *(unsigned char **)(ls + 8);
     if (!arr || count == 0 || count > LED_DUMP_MAX)
          return;
     for (i = 0; i < count; i++) {
          unsigned char *e = arr + LED_ENTRY_SIZE * i;
          klog("knobshim2: snap %s id=%u ch=%u state=%u\n", tag,
               *(unsigned int *)(e + 0), *(unsigned int *)(e + 4),
               *(unsigned int *)(e + 16));
     }
}

/* Discovery sweep: drive rbp's own keycodes and watch which LedStat entries
 * change, so each LED id can be mapped to its JP21 note unambiguously. */
static const struct { int key; const char *name; } led_sweep_tab[] = {
     { 0x4101, "PLAY" },   { 0x4102, "CUE" },    { 0x410c, "LOOPIN" },
     { 0x410d, "LOOPOUT" },{ 0x4112, "SYNC" },   { 0x4114, "ALOOP" },
     { 0x4110, "SLIP" },   { 0x4108, "MT" },     { 0x4104, "VINYL" },
     { 0x4113, "HOTCUE" }, { 0x4115, "SLIPLOOP" },{ 0x4116, "BEATJUMP" },
     { 0x410e, "RELOOP" }, { 0x410f, "REV" },    { 0x4111, "MASTER" },
};

static void led_sweep_run(void)
{
     unsigned int n = sizeof(led_sweep_tab) / sizeof(led_sweep_tab[0]);
     unsigned int i;
     int w;

     /* wait until rbp's LED table is populated and stable */
     for (w = 0; w < 150; w++) {
          unsigned char *ls = ledstat_ptr();
          if (ls && *(unsigned short *)(ls + 4) >= 8)
               break;
          usleep(200000);
     }
     usleep(1500000);

     for (int pass = 0; pass < 2; pass++) {
     for (i = 0; i < n; i++) {
          char tag[32];
          snprintf(tag, sizeof(tag), "p%d%s-0", pass, led_sweep_tab[i].name);
          led_dump_full(tag);
          klog("knobshim2: sweep >>> p%d %s 0x%04x\n", pass,
               led_sweep_tab[i].name, led_sweep_tab[i].key);
          send_rx_key(led_sweep_tab[i].key, OP_PRESS, 1, 0);
          usleep(300000);
          send_rx_key(led_sweep_tab[i].key, OP_RELEASE, 1, 0);
          usleep(1800000);
          snprintf(tag, sizeof(tag), "p%d%s-1", pass, led_sweep_tab[i].name);
          led_dump_full(tag);
     }
     }
     klog("knobshim2: sweep done\n");
}

void *led_thread(void *arg)
{
     (void)arg;
     if (!is_rbp_process())
          return NULL;
     led_verbose = env_on("LED_VERBOSE", 0);
     led_disabled = env_on("LED_DISABLE", 0);
     led_debug_loop = env_on("LED_DEBUG_LOOP", 0);
     led_dump = env_on("LED_DUMP", 0);
     led_sweep = env_on("LED_SWEEP", 0);
     led_pads = env_on("LED_PADS", 1);
     if (led_disabled)
          return NULL;

     /* Wait up to 30 s for an output route. midi_io.c is what opens it and is
      * the only place that knows which one it is (a sequencer port, or a
      * rawmidi node), so this waits on the route rather than on a device. Not
      * fatal if none appears: led_refresh() does nothing until there is one, and
      * the poll loop picks a controller up whenever it is plugged in. */
     for (int i = 0; i < 300 && !midi_out_ready(); i++)
          usleep(100000);
     if (!midi_out_ready())
          klog("knobshim2: no LED/meter output route yet; "
               "sending will begin when one comes up\n");
     memset(led_last, -1, sizeof(led_last));
     memset(led_pad_last, -1, sizeof(led_pad_last));
     /* dump rbp's mixer channel -> EnMixerInput map once the engine exists */
     for (int w = 0; w < 100 && !mixer_engine(); w++)
          usleep(100000);
     {
          void *me = mixer_engine();
          klog("knobshim2: mixer engine=%p chans=%d inputs=", me, me_channel_count());
          for (int i = 0; i < 8; i++) {
               int in = me ? me_channel_input(me, i) : -1;
               if (in >= 0)
                    klog("%d ", in);
          }
          klog("\n");
     }
     klog("knobshim2: LED bridge up\n");

     for (;;) {
          if (led_dump)
               led_dump_scan();
          led_refresh();
          led_tick++;
          usleep(50000);   /* 20 Hz */
     }
     return NULL;
}

void *led_sweep_thread(void *arg)
{
     (void)arg;
     if (!is_rbp_process())
          return NULL;
     usleep(4000000);          /* let the UI settle first */
     led_sweep_run();
     return NULL;
}
