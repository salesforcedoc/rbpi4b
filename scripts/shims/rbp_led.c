/*
 * rbp_led.c -- the panel LED bridge.
 *
 * On the SC Live 4 every front-panel LED is driven by MIDI (Note On/Off to the
 * Control Surface), while rbp drives the XDJ-RX3's micons over /dev/subucom_spi*
 * -- dead FIFOs here. So rbp computes all of its LED state and none of it
 * reaches a panel; this module reads that state out of rbp's own LedStat table
 * and engine singletons and mirrors it onto the panel's notes.
 *
 * It knows rbp's state and NO PANEL NUMBERS. The notes it transmits come from
 * the selected surface's table (ctrl_map.h's struct led_notes, reached through
 * ctrl_sel_leds()), and the only ids left in this file are rbp's own LedStat ids,
 * which belong to rbp_abi.h. That is the split ctrl_map.h states as doctrine: a
 * bridge holds rbp ids and never learns a note number, a map holds note numbers
 * and never contains an rbp address. It used to be the other way round here --
 * this file carried the SC Live 4's notes, which is why the bridge had to be
 * switched off for a target whose panel they were not.
 *
 * A row the surface marks -1 means "this panel has no such LED", and NOTHING is
 * sent for it. That is the load-bearing case, not an edge case: it is what lets
 * the bridge run against an FLX4 whose note numbers are still unmeasured, and
 * what keeps the previous target's numbers from being transmitted at a panel
 * they would land on as phantom controls.
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
#include "ctrl_map.h"   /* struct led_notes, and ctrl_sel_leds() */

/* "loop-in armed" latch per deck (0 = deck 1), driven by the LOOP IN/OUT keys;
 * the LED bridge turns it into the SC Live 4 blink pattern. */
int led_loop_armed[2];

/* The deck LED slots, as this file's own bookkeeping. The panel note for each
 * comes from the surface's table; "which of the nine is this" is rbp-side (each
 * slot maps to a different predicate below), so the index lives here. */
enum {
     L_SYNC, L_CUE, L_PLAY, L_KEYLOCK, L_VINYL, L_SLIP,
     L_LOOPIN, L_LOOPOUT, L_ALOOP, L_SLOT_COUNT
};

static int led_verbose = 0;
static int led_disabled = 0;
static int led_debug_loop = 0;
static int led_dump = 0;
static int led_pads = 1;      /* LED_PADS=0: no pad output (rbp's state is
                               * still mirrored for the other LEDs) */
int led_sweep = 0;
static unsigned long led_tick = 0;        /* 50 ms ticks, for blink */
static signed char led_last[2][L_SLOT_COUNT]; /* [deck][slot] -1 = unknown */
static int led_prev_looping[2];
static int led_dbg_last[2];               /* last logged loop-state bitmask */
static int led_blink_phase;               /* current blink phase (0/1) */
/* How many mixer strips this bridge can carry per-deck state for. It is a bound
 * on the ARRAY, not a statement about any surface: strip_count comes out of a
 * map's table, and a table that says 8 would run off the end of a [2] here. The
 * loop below clamps to this, and test_flx4.c pins that no table exceeds it. */
#define LED_STRIP_MAX 2
static signed char led_pfl_last[LED_STRIP_MAX] = { -1, -1 };  /* strip CUE LED */
static signed char led_mc_last = -1;              /* master CUE LED */

/* The surface's LED table. ctrl_sel_leds() reads the front end's selection, which
 * cannot change after start-up, so one lookup is enough -- but it is resolved
 * lazily rather than cached as "there is none", because the front end fills it
 * from another thread and this one may get here first. NULL means "this selection
 * has no panel to light" and is not an error. */
static const struct led_notes *led_sel;
static int led_sel_known;

static const struct led_notes *led_notes_sel(void)
{
     if (!led_sel_known) {
          led_sel = ctrl_sel_leds();
          led_sel_known = 1;
     }
     return led_sel;
}

/* rbp's RGB performance pads are LedDef::ID 18..25 (confirmed in
 * ui::Player::checkLedStat, which calls checkHotCueLedState(..., 18..25)); other
 * pad modes reuse the same ids and just change state/color. These are rbp's ids,
 * so they stay in this file; the notes are the surface's and come from its table. */
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

/* Send one LED, if this surface has it. A note or channel of -1 means the
 * surface's table says "no such LED" -- nothing is transmitted, and that is the
 * whole point of the table: an unmeasured or absent row must send nothing rather
 * than another target's number.
 *
 * `last` is the caller's own "what did I last send" cell, so a repeat is dropped
 * before it reaches midi_io.c. It is updated only when the send actually
 * happened: midi_note() returns 0 when no route is up yet, and leaving the cell
 * alone is what makes the LED go out on the next tick instead of being lost. */
static void led_send(int sch, int note, signed char *last, int on)
{
     signed char want = (signed char)(on ? 1 : 0);
     if (sch < 0 || note < 0)
          return;
     if (*last == want)
          return;
     /* The byte building lives in midi_io.c, which is also what decides whether
      * this goes out on rawmidi or over the sequencer. */
     if (!midi_note(sch, note, on ? 0x7f : 0x00))
          return;                        /* retry next tick */
     *last = want;
     if (led_verbose)
          klog("knobshim2: led sch%d note%d %s\n",
               sch, note, on ? "on" : "off");
}

/* Prefer rbp's own state for an LED; fall back to a derived value when rbp
 * has no entry for it yet.  State 2 is rbp's blink request (e.g. SYNC blinks
 * when synced but the platter was nudged off beat), so we drive the panel
 * blink ourselves at the same cadence.  State 3 (dim) has no panel equivalent
 * available here and counts as on, which is what the previous target did. */
static void led_from_table(int sch, int note, signed char *last, unsigned int id,
                           int deck, int fallback)
{
     int st = ledstat_state(id, (unsigned int)deck + 1);
     int on;
     if (st < 0)
          on = fallback;
     else if (st == 2)
          on = led_blink_phase;
     else
          on = (st != 0);
     led_send(sch, note, last, on);
}

/* SC Live 4 pad colour = Note On velocity.  Per the Engine OS Prime LED
 * convention, bits 4-5 = red, 2-3 = green, 0-1 = blue (2 bits each).  rbp
 * keeps 0..255 per channel, so take the top 2 bits.  Some builds want bit 6
 * (0x40) set for the bright range; PAD_BRIGHT=1 enables that (default: pure
 * 6-bit colour) -- a quirk of THAT encoding, so it is applied inside the
 * PRIME_6BIT case and nowhere else.
 *
 * The encoding is the surface's (ctrl_map.h's enum led_enc), not this function's:
 * a panel whose pads are not RGB asks for LED_ENC_NONE and gets a plain on. An
 * encoding this build does not know is treated as NONE rather than guessed at --
 * a wrong colour is indistinguishable from a right one at the panel. */
static int pad_bright_bit = -1;

static unsigned char led_encode(int enc, int r, int g, int b)
{
     unsigned char v;

     if (enc != LED_ENC_PRIME_6BIT)
          return 0x7f;

     v = (unsigned char)(((r >> 6) << 4) | ((g >> 6) << 2) | (b >> 6));
     /* Read once, on first use: this runs per pad per tick, and the environment
      * does not change under us. By value, so an exported empty string (see
      * shimutil.h) is off rather than a failed atoi. */
     if (pad_bright_bit < 0)
          pad_bright_bit = env_num("PAD_BRIGHT", 0) ? 1 : 0;
     if (pad_bright_bit)
          v = (unsigned char)(v | 0x40);
     return v;
}

static void led_pad_apply(int sch, int note, int *last, unsigned char vel)
{
     if (sch < 0 || note < 0)
          return;
     if (*last == (int)vel)
          return;
     if (!midi_note(sch, note, vel))
          return;                        /* retry next tick */
     *last = (int)vel;
     if (led_verbose)
          klog("knobshim2: pad sch%d note%d vel=0x%02x\n", sch, note, vel);
}

/* The global (FX) panel LEDs, driven straight from rbp's LedStat. The LedStat id
 * is LedDef::ID + 8 for this group (verified live):
 *   EffectOnOff 40 -> 48, CfxFilter 33 -> 41, CfxSweep 34 -> 42,
 *   CfxDubEcho 35 -> 43, CfxNoise 36 -> 44.
 * Those ids are rbp's, so they stay in this file; the note for each row is the
 * surface's and comes from its table by the same enum led_fx that indexes this
 * one. State 2 = rbp wants a blink (e.g. the FX ON/OFF LED blinks while the
 * effect is active), so we drive the panel blink ourselves. */
static const struct { unsigned int id; const char *name; } led_g_tab[LED_FX_COUNT] = {
     [LED_FX_BFX_ONOFF]   = { 48, "BfxOnOff" },
     [LED_FX_CFX_FILTER]  = { 41, "CfxFilter" },
     [LED_FX_CFX_DUBECHO] = { 43, "CfxDubEcho" },
     [LED_FX_CFX_NOISE]   = { 44, "CfxNoise" },
     [LED_FX_CFX_SWEEP]   = { 42, "CfxSweep" },
};
static signed char led_last_g[LED_FX_COUNT];

static void led_apply_g(int idx, int sch, int note, int on)
{
     signed char want = (signed char)(on ? 1 : 0);
     if (sch < 0 || note < 0)
          return;
     if (led_last_g[idx] == want)
          return;
     if (!midi_note(sch, note, on ? 0x7f : 0x00))
          return;
     led_last_g[idx] = want;
     if (led_verbose)
          klog("knobshim2: led global note%d %s (%s)\n",
               note, on ? "on" : "off", led_g_tab[idx].name);
}

/* Send the same note on a run of consecutive channels, all-or-nothing. The
 * previous target lights its master CUE LED on two strips at once; a surface
 * that puts it on one just passes count 1. Nothing is remembered unless every
 * send in the run succeeded, so a half-lit pair is retried next tick rather than
 * frozen. */
static void led_send_run(int sch_first, int count, int note,
                         signed char *last, int on)
{
     signed char want = (signed char)(on ? 1 : 0);
     if (sch_first < 0 || note < 0 || count <= 0)
          return;
     if (*last == want)
          return;
     for (int k = 0; k < count; k++)
          if (!midi_note(sch_first + k, note, on ? 0x7f : 0x00))
               return;                   /* retry next tick */
     *last = want;
     if (led_verbose)
          klog("knobshim2: led sch%d-%d note%d %s\n",
               sch_first, sch_first + count - 1, note, on ? "on" : "off");
}

static void led_refresh(void)
{
     const struct led_notes *n;
     void *pe;
     int blink;
     /* midi_out_ready() rather than a device descriptor: the output may be the
      * sequencer, in which case there is no /dev/snd/midiC*D0 open at all. */
     if (led_disabled || !midi_out_ready())
          return;
     /* No panel to light -- MIDI_MAP=kbd/none, or the map is not built yet. Not
      * an error, and emphatically not a reason to fall back to another surface's
      * numbers: a selection with no LED table means nothing is transmitted. */
     n = led_notes_sel();
     if (!n)
          return;
     blink = (led_tick & 8) ? 1 : 0;      /* ~400 ms on / off */
     led_blink_phase = blink;

     /* global LEDs, straight from rbp (id -> whatever note this panel uses) */
     for (int g = 0; g < LED_FX_COUNT; g++) {
          int st = ledstat_state(led_g_tab[g].id, 0);
          int on = (st < 0) ? 0 : (st == 2 ? blink : (st != 0));
          led_apply_g(g, n->fx_ch, n->n_fx[g], on);
     }

     /* the mixer strips' CUE LEDs, from rbp's own cue state. The channel base and
      * the count are guarded rather than trusted: a -1 base with a non-zero count
      * would send on channel 0 for strip 1, which is a real channel on any surface
      * -- the same hazard the deck loop guards, and the note being -1 does NOT
      * cover it once a strip note has been measured. */
     for (int m = 0; n->strip_ch_first >= 0 && m < n->strip_count &&
                     m < LED_STRIP_MAX; m++) {
          int cue = me_get_cue(m);
          if (cue < 0)
               continue;
          led_send_run(n->strip_ch_first + m, 1, n->n_strip_cue,
                       &led_pfl_last[m], cue);
     }
     /* master CUE, on every channel this surface puts it on */
     if (n->master_ch_first >= 0) {
          int mc = me_get_master_cue();
          if (mc >= 0)
               led_send_run(n->master_ch_first, n->master_ch_count,
                            n->n_master_cue, &led_mc_last, mc);
     }

     pe = *(void **)PLAYENGINE_GLOBAL;
     if (!pe)
          return;
     for (int i = 0; i < 2; i++) {
          /* A surface with no deck LEDs at all (deck_ch -1) sends nothing for
           * either deck, whatever its note rows say -- the channel guard is what
           * keeps a -1 base from arithmetically landing on channel 0. */
          int sch = (n->deck_ch < 0) ? -1 : n->deck_ch + i;
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
          led_from_table(sch, n->n_sync, &led_last[i][L_SYNC], LEDSTAT_SYNC, i, sync);
          led_send(sch, n->n_cue, &led_last[i][L_CUE], loaded && !playing);
          /* PLAY: solid while playing, blinks while paused on a loaded
           * track, dark with nothing loaded. */
          led_send(sch, n->n_play, &led_last[i][L_PLAY],
                   playing ? 1 : (loaded ? blink : 0));
          led_send(sch, n->n_keylock, &led_last[i][L_KEYLOCK], mt);
          led_send(sch, n->n_vinyl, &led_last[i][L_VINYL], vinyl);
          led_send(sch, n->n_slip, &led_last[i][L_SLIP], slip);
          /* SC Live 4 convention (verified against Engine OS on video):
           *   idle            -> both LEDs solid ON
           *   loop-in set     -> LOOP IN blinks, LOOP OUT solid
           *   loop running    -> both blink */
          led_send(sch, n->n_loopin, &led_last[i][L_LOOPIN],
                   (looping || armed) ? blink : 1);
          led_send(sch, n->n_loopout, &led_last[i][L_LOOPOUT],
                   looping ? blink : 1);
          led_send(sch, n->n_autoloop, &led_last[i][L_ALOOP], aloop);

          /* RGB performance pads: rbp LedDef::ID 18..25 -> whatever note this
           * surface lights for pad p, on its own pad channel. LED_PADS=0 stops
           * here: rbp's pad state is still read by the rest of this function,
           * the pads are simply not sent. */
          if (led_pads && n->pad_ch >= 0 && n->n_pad_first >= 0) {
               for (int p = 0; p < LED_PAD_COUNT; p++) {
                    unsigned char rgb[3];
                    int pnote = n->n_pad_first + p;
                    int st = ledstat_state(LED_PAD_FIRST + (unsigned)p,
                                           (unsigned)i + 1);
                    if (st <= 0 ||
                        !ledstat_rgb(LED_PAD_FIRST + (unsigned)p,
                                     (unsigned)i + 1, rgb)) {
                         led_pad_apply(n->pad_ch + i, pnote,
                                       &led_pad_last[i][p], 0);
                    } else if (st == 2) {
                         led_pad_apply(n->pad_ch + i, pnote, &led_pad_last[i][p],
                              blink ? led_encode(n->pad_enc,
                                                 rgb[0], rgb[1], rgb[2]) : 0);
                    } else {
                         led_pad_apply(n->pad_ch + i, pnote, &led_pad_last[i][p],
                              led_encode(n->pad_enc, rgb[0], rgb[1], rgb[2]));
                    }
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
     /* Say which panel, or that there is none, once -- a dark panel has three
      * very different causes (this selection, a surface whose rows are all -1,
      * and a fault) and the log should not make them look alike. Checked here
      * rather than in led_refresh() because by now the front end has built its
      * map: the builds run before the output route this waited on. */
     {
          const struct led_notes *n = led_notes_sel();
          if (!n)
               klog("knobshim2: no LED table for this selection; "
                    "nothing will be lit (MIDI_MAP=kbd/none?)\n");
     }
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
