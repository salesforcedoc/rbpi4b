/*
 * map_jp21.c -- the Denon JP21 control surface (Prime GO / SC Live 4).
 *
 * This is the map the shim grew up on: the note and CC numbers below were read
 * off the hardware, and the comment on each block says what was observed rather
 * than what the datasheet promised. It is the default (MIDI_MAP absent or
 * unknown), and it is what the Pi port starts from -- the DDJ-FLX4 map is a
 * sibling of this file, not a replacement for it.
 *
 * Everything here is one controller's numbers. rbp's addresses are in rbp_abi.h
 * and reached through rbp_bridge.h's functions, so this file never contains one.
 *
 * Two things are not just tables: the beat-loop knob drives rbp's
 * execAutoBeatLoop() directly (rbp has no delta handler for it) and the SYNC
 * button is a tap-or-hold gesture (rbp has separate SYNC and MASTER keycodes and
 * the SC Live 4 has no MASTER button). Both keep state across events, which is
 * why a map has a tick() as well as an event().
 */
#define _GNU_SOURCE
#include <unistd.h>        /* usleep, access */
#include <stdint.h>
#include <pthread.h>       /* the Beat FX channel watchdog */
#include <sound/asequencer.h>

/* A map does no I/O of its own: it reads the environment through shimutil.h,
 * touches rbp through rbp_bridge.h, and reports through klog(). That is why
 * this list is short -- syscalls.h and the libc headers it wraps are for the
 * modules that talk to the kernel, and this is not one of them. */
#include "shimutil.h"
#include "shmstate.h"      /* g_speaker_gain, g_master_gain, g_speaker_on,
                            * g_split_cue, g_cue_gain, g_cue_mix */
#include "rbp_abi.h"
#include "rbp_bridge.h"
#include "rbp_led.h"       /* led_loop_armed */
#include "rbp_vu.h"        /* g_fader, g_fader_seen */
#include "ctrl_map.h"

static int knob_scale = 1;
static int jog_scale = 1;

/* clamp rotate burst to avoid flooding the key queue */
static void rot_clamped(int key, int ch, int n)
{
     if (n > 16) n = 16;
     if (n < -16) n = -16;
     for (int i = 0; i < (n < 0 ? -n : n); i++)
          send_rx_key(key, OP_ROTATE, ch, (n < 0) ? -1 : 1);
}

/* The binding tables themselves (note_map/abs_map, add_note/add_abs) are in
 * ctrl_map.c: the shapes are the same for every surface, and the absolute-value
 * query in midi_io.c has to invalidate them. */

/* jog wheel: 14-bit absolute position assembled from CC 0x37 (hi) + 0x4D (lo).
 * The RX3 wants per-move keys 0x4305/op4 carrying:
 *   f = jog speed in revolutions/sec (Player::setJogSpeed clamps to ±8)
 *   l = jog position as a wrap-around counter (JogPulse::update expects a
 *       16-bit counter wrapping at 65536, JOG_POS_TH = 0xFB7F).
 * The Prime GO jog reports an absolute 14-bit position that wraps at 16384;
 * we unwrap it into a continuous virtual u16 counter (vpos) and compute
 * speed from the delta per sample time. */
struct jog_ctrl {
     int rch;
     int pos;            /* last full 14-bit position */
     int have_hi;
     int have_lo;
     int prev;           /* previous full position for delta */
     int ready;          /* saw at least one full sample */
     unsigned long long last_ms;  /* monotonic ms of previous completed sample */
     unsigned int vpos;  /* continuous virtual jog counter (u16 space) */
     int moving;         /* jog currently moving / nonzero speed sent */
     float speed;        /* last computed speed (rev/s) */
     int sch;
};
static struct jog_ctrl jog_state[2] = { {4,0,0,0,0,0,0,0,0,0.0f,1},
                                        {5,0,0,0,0,0,0,0,0,0.0f,2} };

static int jog_ppr = 128;      /* Prime GO counts per revolution (calibrate) */
static int jog_rev = 0;        /* invert jog direction */
static int jog_idle_ms = 120;
static int jog_verbose = 0;

/* pitch fader: 14-bit, CC 0x1F (hi) + 0x4B (lo), inverted */
struct pitch_ctrl {
     int rch;
     int pos;
     int have_hi;
     int have_lo;
     int ready;
};
static struct pitch_ctrl pitch_state[2] = { {4,0,0,0,0}, {5,0,0,0,0} };
static int tempo_verbose = 0;
static int tempo_rev = 0;

/* browse knob position (global CC5) */
static int knob_pos = -1;
static int shift_down = 0;

/* TIME knob push (ch15 note 25) held: turning the TIME knob then acts as the
 * RX3's BEAT < / BEAT > buttons instead of changing the time. */
static int fx_time_btn = 0;

/* SYNC hold -> MASTER (the SC Live 4 has no MASTER button).  rbp's
 * onKey_Sync only acts on release (op 2) and onKey_Master only on press
 * (op 0), so we can decide at release time without adding latency. */
#define SYNC_HOLD_MS 600
static unsigned long long sync_press_ms[2];
static volatile int sync_held[2];        /* button is currently down */
static volatile int sync_hold_fired[2];  /* MASTER already sent for this hold */

/* ---------- event handlers ---------- */

/* Source-menu -> USB1-open remap.
 * On the real RX3 the drive is opened from the Source menu by the dedicated
 * hardware USB1 source button (key 0x0209 -> BrowseUiIfDpl::onKey case 0x0209
 * -> BrowseUiIf::InputKey(UKEY_USB1=3) -> UiKey_Usb1 -> ChangeBrowseDevice(3)
 * -> DEV_SEL R232c messages -> browse list population).  The RX3 engine
 * deliberately IGNORES the browse-encoder push in the Source menu (mode 12),
 * and the Prime GO has no USB1 button, so pushing the browse knob (note 6) or
 * pressing FWD (note 4) while the Source menu is shown is remapped to key
 * 0x0209 so the drive can actually be opened. */
static int source_menu_with_usb1(void)
{
     if (*(volatile uint32_t *)0x326f8b8 != 12)   /* browseMode != 12 */
          return 0;
     if (access("/media/usb1/sda1/PIONEER/rekordbox/export.pdb", F_OK) != 0)
          return 0;
     return 1;
}

static void handle_note(int ch, int note, int on)
{
     /* SC Live 4 mixer PFL buttons (strips 1/2 = ch 0/1, note 13): toggle
      * rbp's headphone cue directly (rbp has no PFL keycode).  Latching. */
     if ((ch == 0 || ch == 1) && note == 13) {
          if (on) {
               int cur = me_get_cue(ch);
               int want = (cur == 0) ? 1 : 0;
               me_set_cue(ch, want);
               if (verbose)
                    klog("knobshim2: PFL ch%d -> cue=%d (engine=%p)\n",
                         ch + 1, want, mixer_engine());
          }
          return;
     }

     /* SC Live 4 built-in monitor on/off switch (ch15 note 41): gates the
      * ch6/7 speaker output only (rbp/booth/monitors unaffected). */
     if (ch == 15 && note == 41) {
          g_speaker_on = on ? 1 : 0;
          klog("knobshim2: speaker switch note41 velocity-on=%d -> speakers %s\n",
               on, g_speaker_on ? "ON" : "OFF");
          return;
     }

     /* SC Live 4 has no MASTER CUE button: strips 3/4 PFL (ch 2/3 note 13)
      * toggle rbp's master cue. */
     if ((ch == 2 || ch == 3) && note == 13) {
          if (on) {
               int cur = me_get_master_cue();
               int want = (cur == 0) ? 1 : 0;
               me_set_master_cue(want);
               klog("knobshim2: master cue (strip %d) -> %s\n",
                    ch + 1, want ? "ON" : "OFF");
          }
          return;
     }

     /* SC Live 4 split-cue switch (ch15 note 11): headphones L = cue, R = main.
      * rbp's EnHeadphoneStereoType: 0 = mono split, 1 = stereo. */
     if (ch == 15 && note == 11) {
          g_split_cue = on ? 1 : 0;
          me_set_stereo(g_split_cue ? 0 : 1);
          klog("knobshim2: split cue note11 velocity-on=%d -> split %s\n",
               on, g_split_cue ? "ON" : "OFF");
          return;
     }

     if ((ch == 4 || ch == 5) && note == 28)
          shift_down = on;
     if (ch == 15 && note == 25)
          fx_time_btn = on;

     /* SC Live 4 global Sound Color FX select (ch15 notes 21..24) -> both mixer
      * channels (Filter/DubEcho/Noise/Sweep). */
     if (ch == 15 && note >= 21 && note <= 24) {
          int fxkey = 0;
          switch (note) {
          case 21: fxkey = K_FILTER;  break;  /* DualFilter */
          case 22: fxkey = K_DUBECHO; break;  /* DubEcho */
          case 23: fxkey = K_NOISE;   break;  /* NoiseSweep */
          case 24: fxkey = K_SWEEP;   break;  /* Wash */
          }
          if (fxkey) {
               send_rx_key(fxkey, on ? OP_PRESS : OP_RELEASE, 1, 0);
               send_rx_key(fxkey, on ? OP_PRESS : OP_RELEASE, 2, 0);
               if (verbose)
                    klog("knobshim2: ch15 note%d %s -> SCFX 0x%04x (both ch)\n",
                         note, on ? "on" : "off", fxkey);
               return;
          }
     }

     /* Source menu + mounted Rekordbox stick: knob push and FWD become the
      * USB1 source button.  Swallow BOTH edges so the generic SELECTOR/SOURCE
      * mapping below never fires for this gesture. */
     if (ch == 15 && (note == 4 || note == 6)) {
          if (source_menu_with_usb1()) {
               send_rx_key(K_USB1, on ? OP_PRESS : OP_RELEASE, CH_GLOBAL, 0);
               if (verbose)
                    klog("knobshim2: ch15 note%d %s -> USB1 select 0x0209\n",
                         note, on ? "on" : "off");
               return;
          }
     }

     /* SYNC (deck note 8): tap = SYNC, hold = MASTER.  The SC Live 4 has no
      * MASTER button, and rbp keeps the two keycodes distinct.
      *
      * We deliberately send NOTHING on note-on.  If we sent the SYNC press
      * early and then suppressed the release for a hold, rbp saw a stuck SYNC
      * press and ran its own long-press action - instant double, i.e. it
      * loaded the other deck's track.  Deciding on release is free because
      * rbp's onKey_Sync only acts on release (op 2) and onKey_Master only on
      * press (op 0). */
     if ((ch == 4 || ch == 5) && note == 8) {
          int d = ch - 4;
          if (on) {
               sync_press_ms[d] = shim_now_ms();
               sync_hold_fired[d] = 0;
               sync_held[d] = 1;
          } else {
               sync_held[d] = 0;
               /* if the hold already fired MASTER, the release does nothing;
                * otherwise this was a tap -> SYNC */
               if (!sync_hold_fired[d]) {
                    send_rx_key(K_SYNC, OP_PRESS, d + 1, 0);
                    send_rx_key(K_SYNC, OP_RELEASE, d + 1, 0);
               }
          }
          return;
     }

     for (int i = 0; i < note_map_n; i++) {
          if (note_map[i].rch == ch && note_map[i].note == note) {
               if (note_map[i].key == 0) {
                    if (verbose)
                         klog("knobshim2: ch%d note%d (log-only)\n", ch, note);
                    return;
               }
               int key = note_map[i].key;
               int *p = &note_map[i].pressed;
               if (on && !*p) {
                    *p = 1;
                    if (ch == 4 || ch == 5) {
                         if (key == K_LOOPIN)
                              led_loop_armed[ch - 4] = 1;
                         else if (key == K_LOOPOUT)
                              led_loop_armed[ch - 4] = 0;
                    }
                    send_rx_key(key, OP_PRESS, note_map[i].sch, 0);
                    if (verbose || key == K_FILTER || key == K_SWEEP ||
                        key == K_BFX || key == K_BEATPREV || key == K_BEATNEXT ||
                        key == K_TRFWD || key == K_TRREV ||
                        key == K_SRFWD || key == K_SRREV)
                         klog("knobshim2: ch%d note%d -> 0x%04x press (sch%d)\n",
                              ch, note, key, note_map[i].sch);
               } else if (!on && *p) {
                    *p = 0;
                    send_rx_key(key, OP_RELEASE, note_map[i].sch, 0);
                    if (verbose || key == K_FILTER || key == K_SWEEP ||
                        key == K_BFX || key == K_BEATPREV || key == K_BEATNEXT ||
                        key == K_TRFWD || key == K_TRREV ||
                        key == K_SRFWD || key == K_SRREV)
                         klog("knobshim2: ch%d note%d -> 0x%04x release (sch%d)\n",
                              ch, note, key, note_map[i].sch);
               }
               return;
          }
     }
     if (verbose)
          klog("knobshim2: unmapped ch%d note%d %s\n", ch, note, on ? "on" : "off");
}

static void handle_cc_abs(int ch, int cc, int val)
{
     for (int i = 0; i < abs_map_n; i++) {
          if (abs_map[i].rch == ch && abs_map[i].cc == cc) {
               int v = cc_to_10bit(val);
               if (v != abs_map[i].last) {
                    abs_map[i].last = v;
                    if (abs_map[i].key == K_FADER && abs_map[i].sch >= 1 &&
                        abs_map[i].sch <= 2) {
                         g_fader[abs_map[i].sch] = v;
                         g_fader_seen[abs_map[i].sch] = 1;
                    }
                    float fval = (float)v / 1023.0f;
                    if (abs_map[i].key == K_HPMIX)
                         g_cue_mix = fval;
                    else if (abs_map[i].key == K_HPLEVEL)
                         g_cue_gain = fval;
                    /* absolute knobs/faders use OP_VALUE; only the
                     * detented ALOOP encoder uses OP_ROTATE. */
                    int op = (abs_map[i].key == K_ALOOP) ? OP_ROTATE : OP_VALUE;
                    send_rx_key_f(abs_map[i].key, op, abs_map[i].sch, v, fval);
                    if (verbose || (abs_map[i].key == K_COLOR && tempo_verbose) ||
                        abs_map[i].key == K_DEPTH ||
                        abs_map[i].key == K_HPMIX || abs_map[i].key == K_HPLEVEL ||
                        abs_map[i].key == K_MASTERLVL)
                         klog("knobshim2: ch%d cc%d -> 0x%04x val=%d f=%.3f (sch%d)\n",
                              ch, cc, abs_map[i].key, v, (double)fval, abs_map[i].sch);
               }
               return;
          }
     }
     if (verbose)
          klog("knobshim2: unmapped ch%d cc%d val=%d\n", ch, cc, val);
}

/* browse knob (global CC5): relative delta (1 = +1 step, 127 = -1 step) */
static void handle_knob_pos(int v)
{
     if (v < 0 || v > 127)
          return;
     int delta = (v >= 64) ? (v - 128) : v;
     if (delta == 0)
          return;
     rot_clamped(K_SELECTOR, CH_GLOBAL, delta * knob_scale);
     if (verbose)
          klog("knobshim2: browse knob v=%d delta=%d\n", v, delta);
}

/* jog: assemble 14-bit pos; on each completed sample emit the RX3 jog
 * wheel key 0x4305 / op4 with f = speed (rev/s) and l = virtual position.
 * Speed sign follows the jog direction; jog_rev flips it. */
static void handle_jog(int ch, int cc, int val)
{
     int idx = -1;
     for (int i = 0; i < 2; i++)
          if (jog_state[i].rch == ch) { idx = i; break; }
     if (idx < 0)
          return;
     struct jog_ctrl *s = &jog_state[idx];
     if (cc == 0x11) {
          s->have_hi = 1;
          s->pos = (s->pos & 0x7f) | (val << 7);   /* store hi half now */
     } else if (cc == 0x31) {
          s->have_lo = 1;
          s->pos = (s->pos & 0x3f80) | val;        /* store lo half now */
     } else {
          return;
     }
     if (!s->have_hi || !s->have_lo)
          return;                  /* need both halves */
     int pos = s->pos;
     s->have_hi = s->have_lo = 0;  /* consume the pair */
     if (!s->ready) {
          s->ready = 1;
          s->prev = pos;
          s->last_ms = shim_now_ms();
          return;
     }
     int d = pos - s->prev;
     s->prev = pos;
     if (d > 8192) d -= 16384;
     if (d < -8192) d += 16384;
     if (d == 0) {
          s->last_ms = shim_now_ms();
          return;
     }
     unsigned long long t = shim_now_ms();
     float dt = (float)(long long)(t - s->last_ms) / 1000.0f;
     s->last_ms = t;
     if (dt < 0.0005f) dt = 0.0005f;
     if (jog_rev)
          d = -d;
     int dp = d * jog_scale;               /* scaled pulse delta */
     if (dp > 4096) dp = 4096;
     if (dp < -4096) dp = -4096;
     /* continuous virtual counter in 16-bit space (wrap 65536) */
     s->vpos = (unsigned int)(s->vpos + (unsigned int)dp) & 0xFFFFu;
     /* speed in rev/s of the Prime GO jog */
     float speed = (float)dp / (float)(jog_ppr * jog_scale) / dt;
     if (speed > 8.0f) speed = 8.0f;
     if (speed < -8.0f) speed = -8.0f;
     s->moving = 1;
     s->speed = speed;
     send_rx_key_fl(K_JOG_ROT, OP_ROTATE, s->sch, 0, speed, (long)s->vpos);
     if (jog_verbose)
          klog("knobshim2: jog ch%d delta=%d speed=%.2f pos=%u (sch%d)\n",
               ch, d, (double)speed, s->vpos, s->sch);
}

/* jog idle watcher: when the wheel has not moved for jog_idle_ms, send a
 * speed-0 key so the engine ends the pitch bend / jog state.
 * Run from tick(): the wait lives in the thread, not here. */
static void jog_idle_tick(void)
{
     unsigned long long t = shim_now_ms();
     for (int i = 0; i < 2; i++) {
          struct jog_ctrl *s = &jog_state[i];
          if (!s->ready || !s->moving)
               continue;
          if ((unsigned long long)(long long)(t - s->last_ms) <
              (unsigned long long)jog_idle_ms)
               continue;
          s->moving = 0;
          s->speed = 0.0f;
          send_rx_key_fl(K_JOG_ROT, OP_ROTATE, s->sch, 0, 0.0f, (long)s->vpos);
          if (jog_verbose)
               klog("knobshim2: jog ch%d idle -> speed 0\n", s->rch);
     }
}

/* pitch fader: 14-bit, CC 0x1F (hi) + 0x4B (lo).
 * Prime GO hardware: 0x0000 = bottom (+), 0x3FFF = top (-)
 * RX3 tempo slider = key 0x4109 op 5, payload = float fader position in
 * [-1.0 .. +1.0] (0 = detent center).
 * -1.0 = slower (top), +1.0 = faster (bottom).
 */
static void handle_pitch(int ch, int cc, int val)
{
     int idx = -1;
     for (int i = 0; i < 2; i++)
          if (pitch_state[i].rch == ch) { idx = i; break; }
     if (idx < 0)
          return;
     struct pitch_ctrl *s = &pitch_state[idx];
     if (cc == 0x1F) {
          s->have_hi = 1;
          s->pos = (s->pos & 0x7f) | (val << 7);
     } else if (cc == 0x4B) {
          s->have_lo = 1;
          s->pos = (s->pos & 0x3f80) | val;
     } else {
          return;
     }
     if (!s->ready) {
          if (s->have_hi && s->have_lo)
               s->ready = 1;
          else
               return;
     }
     /* Only dispatch on CC 0x4B (the low byte, which always arrives right after 0x1F) */
     if (cc != 0x4B)
          return;

     int pos = s->pos;
     if (pos < 0) pos = 0;
     if (pos > 0x3FFF) pos = 0x3FFF;

     /* Prime GO:
      * physical top (slower): pos = 0x3FFF (16383)
      * physical detent (center): pos = ~0x2000 (8192)
      * physical bottom (faster): pos = 0x0000 (0)
      * Pioneer:
      * float: -1.0 at top (slower), 0.0 at center, +1.0 at bottom (faster)
      */
     float norm = ((float)0x2000 - (float)pos) / 8192.0f;
     if (norm > 1.0f) norm = 1.0f;
     if (norm < -1.0f) norm = -1.0f;
     if (tempo_rev)
          norm = -norm;

     /* pitch_state[0] = rch 4 (left deck) -> deck 1,
      * pitch_state[1] = rch 5 (right deck) -> deck 2 */
     int sch = idx + 1;
     int v10 = (int)((norm + 1.0f) * 511.5f);
     if (v10 < 0) v10 = 0;
     if (v10 > 1023) v10 = 1023;

     send_rx_key_fl(K_TEMPO_SLIDER, OP_VALUE, sch, (long)v10, norm, (long)pos);
     if (tempo_verbose)
          klog("knobshim2: pitch ch%d (deck %d) pos=%d -> tempo norm=%.3f v10=0x%03x\n",
               ch, sch, pos, (double)norm, v10);
}
/* g_fx_type_pos is the position rbp's SW_BFX_TYPE switch is on. It is seeded
 * from rbp's own type through rbp_abi.h's bfx_type_to_pos[] -- shared with
 * map_flx4.c, so the two maps cannot disagree about which position holds which
 * effect. The read below still calls ADDR_GET_BFX_TYPE rather than
 * rbp_bridge.h's rbp_beatfx_type() because this gesture is not in the JP21
 * fixture (its header says why), so the bridge would buy nothing here. */
static int g_fx_type_pos = -1;         /* current SW_BFX_TYPE position */
static volatile int g_bfxch_user_set;  /* user moved the assign knob */

static void handle_fx_assign(int vel)
{
     int param;
     switch (vel) {
     case 1:   param = 0; break;        /* Channel 1 -> PLAYER_0 */
     case 2:   param = 1; break;        /* Channel 2 -> PLAYER_1 */
     case 127: param = 5; break;        /* Main      -> MASTER   */
     default:                           /* Ch3 / Ch4: rbp has only 2 players */
          g_bfxch_user_set = 1;
          if (verbose)
               klog("knobshim2: fx assign pos vel=%d -> no rbp channel\n", vel);
          return;
     }
     g_bfxch_user_set = 1;
     send_rx_key(K_BFXCH, OP_VALUE, CH_GLOBAL, param);
     if (verbose)
          klog("knobshim2: fx assign vel=%d -> BFXCH %d\n", vel, param);
}

static void handle_fx_select(int val)
{
     int d = (val == 127) ? -1 : (val == 1 ? 1 : 0);
     int t;
     if (!d)
          return;
     if (g_fx_type_pos < 0) {
          t = ((int (*)(void *))ADDR_GET_BFX_TYPE)(NULL);
          g_fx_type_pos = (t >= 0 && t <= 14) ? bfx_type_to_pos[t] : 0;
     }
     g_fx_type_pos += d;
     if (g_fx_type_pos < 0)
          g_fx_type_pos = BFX_TYPE_POSITIONS - 1;
     if (g_fx_type_pos >= BFX_TYPE_POSITIONS)
          g_fx_type_pos = 0;
     send_rx_key(K_BFXTYPE, OP_VALUE, CH_GLOBAL, g_fx_type_pos);
     if (verbose)
          klog("knobshim2: fx select -> position %d\n", g_fx_type_pos);
}

static void handle_fx_time(int val)
{
     int d = (val == 127) ? -1 : (val == 1 ? 1 : 0);
     if (!d)
          return;
     /* TIME-knob push (or SHIFT) + TIME knob = the RX3's BEAT < / BEAT >
      * buttons, which the SC Live 4 does not have.  rbp: onEv_BeatFxBeat(long)
      * with -1 = halve, +1 = double the beat fraction. */
     if (shift_down || fx_time_btn) {
          int key = (d < 0) ? K_BEATPREV : K_BEATNEXT;
          /* ui::Mixer::asEventCode only produces the BEAT events for
           * 0x4490/0x4491 when the op is PRESS (0):
           *   0x4490 -> 0x2015, 0x4491 -> 0x2016, both `tst op,#15; movne 0`.
           * Sending OP_VALUE made rbp drop them. */
          send_rx_key_fl(key, OP_PRESS, CH_GLOBAL, d, 0.0f, d);
          if (verbose)
               klog("knobshim2: shift+time -> %s\n",
                    (d < 0) ? "BEAT<" : "BEAT>");
          return;
     }
     /* plain turn: rbp's onEv_BeatFxTime(long) -> BeatFxTimeKnob(value,
      * absolute=false), so the argument is a ROTATION DELTA and rbp itself
      * steps/clamps (it adds value*n to the current percent).  Sending an
      * absolute position did nothing - every turn re-read the old value. */
     send_rx_key_fl(K_TIME, OP_ROTATE, CH_GLOBAL, d, 0.0f, d);
     if (verbose)
          klog("knobshim2: fx time step %d\n", d);
}
static int g_aloop_idx[2] = { -1, -1 };

/* knob turn: change the selected loop length; if a loop is running, apply it
 * immediately so the live loop changes size (otherwise just remember it) */
static void handle_aloop(int deck, int val)
{
     static int last[2] = { -1, -1 };
     void *p;
     int d;

     /* relative encoder: 127 = counter-clockwise = SHORTER, 1 = clockwise =
      * LONGER.  Index 0 is the longest loop (4 beats), index 7 the shortest
      * (1/32), so "shorter" means a higher index. */
     if (val == 127)
          d = 1;
     else if (val == 1)
          d = -1;
     else if (last[deck] < 0)
          d = 0;
     else
          d = (val > last[deck]) ? 1 : ((val < last[deck]) ? -1 : 0);
     if (val != 1 && val != 127)
          last[deck] = val;
     if (!d)
          return;

     if (g_aloop_idx[deck] < 0)
          g_aloop_idx[deck] = 0;                 /* default 4 beats */
     g_aloop_idx[deck] += d;
     if (g_aloop_idx[deck] < 0)
          g_aloop_idx[deck] = 0;
     if (g_aloop_idx[deck] >= ALOOP_POSITIONS)
          g_aloop_idx[deck] = ALOOP_POSITIONS - 1;

     if (!aloop_is_looping(deck)) {
          if (verbose)
               klog("knobshim2: beat loop deck%d select idx=%d (not looping)\n",
                    deck + 1, g_aloop_idx[deck]);
          return;
     }
     p = plinn(deck);
     if (!p)
          return;
     aloop_apply(deck, p, g_aloop_idx[deck]);
     if (verbose)
          klog("knobshim2: beat loop deck%d -> idx=%d (was looping), now=%d\n",
               deck + 1, g_aloop_idx[deck], aloop_is_looping(deck));
}

/* button push: engage a beat loop of the selected length */
static void handle_aloop_button(int deck)
{
     void *p;
     if (g_aloop_idx[deck] < 0)
          g_aloop_idx[deck] = 0;                 /* default 4 beats */
     p = plinn(deck);
     if (!p) {
          if (verbose)
               klog("knobshim2: beat loop deck%d push: no PlayerInnards\n",
                    deck + 1);
          return;
     }
     {
          int m0 = *(volatile unsigned char *)((char *)p + PLAYERINNARDS_MODE_OFF);
          aloop_apply(deck, p, g_aloop_idx[deck]);
          if (verbose)
               klog("knobshim2: beat loop deck%d push idx=%d mode_before=%d "
                    "looping=%d chan=%d\n",
                    deck + 1, g_aloop_idx[deck], m0, aloop_is_looping(deck),
                    *(volatile unsigned char *)((char *)p +
                                                PLAYERINNARDS_CHAN_OFF));
          return;
     }
}

/* One event, with rbp's KeyManager already known to exist: ctrlshim.c's
 * dispatcher checks that and does the MIDI_DUMP recording before calling here,
 * because a dump is most useful during the window when rbp is not up yet. */
static void jp21_event(const struct snd_seq_event *ev)
{
     switch (ev->type) {
     case SNDRV_SEQ_EVENT_CONTROLLER: {
          int ch = ev->data.control.channel;
          int cc = ev->data.control.param;
          int val = ev->data.control.value;
          if (ch == 15 && cc == 15) {
               g_speaker_gain = (float)val / 127.0f;
               if (verbose)
                    klog("knobshim2: speaker knob cc15=%d -> gain %.3f\n",
                         val, (double)g_speaker_gain);
          }
          else if (ch == 15 && cc == 20) {
               g_master_gain = (float)val / 127.0f;
               if (verbose)
                    klog("knobshim2: master knob cc20=%d -> gain %.3f\n",
                         val, (double)g_master_gain);
          }
          else if (ch == 15 && cc == 5)
               handle_knob_pos(val);
          else if (ch == 15 && cc == 35)
               handle_fx_select(val);
          else if (ch == 15 && cc == 36)
               handle_fx_time(val);
          else if ((ch == 4 || ch == 5) && cc == 32)
               handle_aloop(ch - 4, val);      /* beat-loop knob */
          else if (cc == 0x11 || cc == 0x31)
               handle_jog(ch, cc, val);
          else if (cc == 0x1F || cc == 0x4B)
               handle_pitch(ch, cc, val);
          else
               handle_cc_abs(ch, cc, val);
          break;
     }
     case SNDRV_SEQ_EVENT_NOTEON: {
          int on = ev->data.note.velocity > 0;
          /* FX channel assign carries its position in the velocity */
          if (ev->data.note.channel == 15 && ev->data.note.note == 40) {
               if (on)
                    handle_fx_assign(ev->data.note.velocity);
               break;
          }
          /* beat-loop knob push: engage a loop of the selected length */
          if ((ev->data.note.channel == 4 || ev->data.note.channel == 5) &&
              ev->data.note.note == 39) {
               if (on)
                    handle_aloop_button(ev->data.note.channel - 4);
               break;
          }
          handle_note(ev->data.note.channel, ev->data.note.note, on);
          break;
     }
     case SNDRV_SEQ_EVENT_NOTEOFF:
          handle_note(ev->data.note.channel, ev->data.note.note, 0);
          break;
     default:
          break;
     }
}

static void *bfx_init_thread(void *arg)
{
     (void)arg;
     for (int i = 0; i < 6 && !g_bfxch_user_set; i++) {
          usleep(500000); /* 500ms */
          send_rx_key(K_BFXCH, OP_VALUE, CH_GLOBAL, BFX_CH_MASTER);
     }
     klog("knobshim2: Beat FX Channel master enforcement complete\n");
     return NULL;
}

/* The SC Live 4's panel LEDs. These numbers were read off the hardware and are
 * the same notes the map's buttons send (which is why they look like the input
 * rows above), moved here verbatim from rbp_led.c -- that file used to carry
 * them, which put one surface's notes in a bridge. Nothing about them changed in
 * the move, so the previous target's LED behaviour is byte-for-byte what it was.
 *
 * Which rbp state lights each of these is rbp_led.c's business and is not
 * recorded here; this is only what the panel is called. */
static const struct led_notes jp21_leds = {
     .deck_ch = 4,             /* deck LEDs go out on channel 4 + deck */
     .n_sync = 8, .n_cue = 9, .n_play = 10,
     .n_keylock = 34, .n_vinyl = 35, .n_slip = 36,
     .n_loopin = 37, .n_loopout = 38, .n_autoloop = 39,

     /* No pad-mode LEDs are sent here, and -1 is spelled out rather than left
      * to zero-init: `0` is a legal note, so an omitted array would light notes
      * 0..3 on the panel. The SC Live 4's own four pad-mode buttons have never
      * been measured, and a neighbouring surface's numbers are never
      * substituted -- the same rule as every other unmeasured LED in this
      * struct. */
     .n_mode = { -1, -1, -1, -1 },

     /* The pads sit on the channels after the deck LEDs (deck 1 on 4, deck 2 on
      * 5) and are notes 15..22 -- ONE base, not four, because this surface's
      * pads keep their notes across pad modes. That is what the single
      * `n_pad_first = 15` this replaced asserted, so filling all four entries
      * with it restates the old claim rather than making a new one: the SC Live
      * 4's per-mode pad bases have never been measured, and if they turn out to
      * move the way the FLX4's do, this row is where it would be corrected. */
     .pad_ch = 4,
     .pad_ch2 = 5,
     .n_pad_base = { 15, 15, 15, 15 },
     .pad_enc = LED_ENC_PRIME_6BIT,

     .strip_ch_first = 0,      /* strip m's CUE LED is channel m, note 13 */
     .strip_count = 2,
     .n_strip_cue = 13,

     .master_ch_first = 2,     /* the master CUE LED is on the strips 3/4 send */
     .master_ch_count = 2,
     .n_master_cue = 13,

     .fx_ch = 15,              /* the global FX group is on channel 15 */
     .n_fx = {
          [LED_FX_BFX_ONOFF]   = 26,
          [LED_FX_CFX_FILTER]  = 21,
          [LED_FX_CFX_DUBECHO] = 22,
          [LED_FX_CFX_NOISE]   = 23,
          [LED_FX_CFX_SWEEP]   = 24,
     },

     /* The meter, as rbp_vu.c has always driven it -- these four numbers are the
      * values that were hardcoded in that file before it read a table, so this
      * row is a transcription and not a change. The Prime's meter is POST-fader
      * (rbp's is not, which is why meter_pre_fader is what tells rbp_vu.c to
      * attenuate by the channel fader) and it takes a segment bitmask rather
      * than a level. Master is the FX channel's CC 32/33 pair; this surface has
      * no separate master meter note. */
     .meter_ch_first = 0,      /* decks 1/2 -> channels 0/1 */
     .n_meter_cc = 10,
     .meter_enc = METER_ENC_PRIME_SEGMENTS,
     .meter_pre_fader = 0,
     .meter_master_ch = 15,
     .n_meter_master_l = 32,
     .n_meter_master_r = 33,
};

static void jp21_build(void)
{
     /* ADDITIVE, deliberately -- see the note on ctrl_bindings_reset() in
      * ctrl_map.h. The front end clears the shared tables once, before either
      * selection is built; a reset here would wipe the other surface's. */

     /* The map's own calibration. Read here, by value, so an empty export from
      * start-rb.sh's SHIM_VARS loop means "use the default" rather than 0 --
      * which for JOG_PPR silently produced a jog 128x too fast. */
     knob_scale = env_num("KNOB_SCALE", 1);
     if (knob_scale < 1) knob_scale = 1;
     jog_scale = env_num("JOG_SCALE", 1);
     if (jog_scale < 1) jog_scale = 1;
     jog_ppr = env_num("JOG_PPR", 128);
     if (jog_ppr < 1) jog_ppr = 1;
     jog_rev = env_on("JOG_REV", 0);
     jog_idle_ms = env_num("JOG_IDLE_MS", 120);
     if (jog_idle_ms < 10) jog_idle_ms = 10;
     jog_verbose = env_on("JOG_VERBOSE", 0);
     tempo_verbose = env_on("TEMPO_VERBOSE", 0);
     tempo_rev = env_on("TEMPO_REV", 0);
     aloop_enabled = env_on("BEATLOOP", 0);

     /* ---- global (ch 15) — send ch 1 ---- */
     /* LOAD streams on the GLOBAL channel (verified live: deck 1 LOAD =
      * ch15 note1; Prime GO is the same).  It is NOT on the deck channel. */
     add_note(15, 1, K_LOAD, 1);               /* deck 1 LOAD */
     add_note(15, 2, K_LOAD, 2);               /* deck 2 LOAD */
     add_note(15, 3,  K_BACK, CH_GLOBAL);      /* BACK */
     add_note(15, 4,  K_SOURCE, CH_GLOBAL);    /* FWD -> source */
     add_note(15, 6,  K_SELECTOR, CH_GLOBAL);  /* browse knob push */
     add_note(15, 13, K_MENU, CH_GLOBAL);      /* MENU */
     add_note(15, 14, K_BROWSE, CH_GLOBAL);    /* VIEW */
     add_abs(15, 14, K_XFADER, CH_GLOBAL);     /* crossfader */
     add_abs(15, 18, K_HPMIX, CH_GLOBAL);      /* cue mix */
     add_abs(15, 19, K_HPLEVEL, CH_GLOBAL);    /* cue gain */
     /* CC20 (Main Vol) is handled in the CC dispatch as g_master_gain (ch0/1
      * only) - NOT sent to rbp's master level, which would affect everything. */

     /* ---- decks (rch 4 = left -> deck1, rch 5 = right -> deck2) ---- */
     for (int d = 0; d < 2; d++) {
          int rch = 4 + d;
          int sch = 1 + d;
          /* CENSOR (ch4/ch5 note1) is remapped to the RX3 RELOOP/EXIT key:
           * exits a running loop, and re-enters it when a loop is stored. */
          add_note(rch, 1, K_RELOOP, sch);
          add_note(rch, 8,  K_SYNC, sch);
          add_note(rch, 9,  K_CUE, sch);
          add_note(rch, 10, K_PLAY, sch);
          add_note(rch, 11, K_HOTCUE, sch);    /* mode CUES/STEMS */
          add_note(rch, 12, K_ALOOP, sch);     /* mode LOOPS/AUTO */
          add_note(rch, 13, K_SLIPLOOP, sch);  /* mode ROLL/SAMPLER */
          add_note(rch, 14, K_BEATJUMP, sch);  /* mode SLICER -> rbp BEAT JUMP */
          for (int p = 0; p < 8; p++)
               add_note(rch, 15 + p, K_PAD1 + p, sch);
          add_note(rch, 29, K_TEMPO_RANGE, sch); /* pitch bend - -> tempo range */
          add_note(rch, 30, K_MT, sch);          /* pitch bend + -> master tempo */
          add_note(rch, 33, K_JOG_TOUCH, sch); /* jog touch */
          add_note(rch, 34, K_MT, sch);        /* key lock */
          add_note(rch, 35, K_VINYL, sch);
          add_note(rch, 36, K_SLIP, sch);
          add_note(rch, 37, K_LOOPIN, sch);    /* manual loop in */
          add_note(rch, 38, K_LOOPOUT, sch);   /* manual loop out */
          add_note(rch, 39, K_ALOOP, sch);     /* auto loop push */
          add_note(rch, 4, K_TRREV, sch);      /* track skip <: XDJ TRACK REV */
          add_note(rch, 5, K_TRFWD, sch);      /* track skip >: XDJ TRACK FWD */
          add_note(rch, 6, K_SRREV, sch);      /* beat jump <:  XDJ SEARCH REV */
          add_note(rch, 7, K_SRFWD, sch);      /* beat jump >:  XDJ SEARCH FWD */
          /* CC 32 (auto/beat-loop knob) is handled by handle_aloop(), which
           * drives rbp's execAutoBeatLoop() directly - rbp's onKey_AutoBeatLoop
           * rejects any op but PRESS, so a rotate-based mapping is dead. */
     }

     /* ---- mixer channels ----
      * The SC Live 4 has 4 strips but rbp is a 2-channel mixer, so only strips
      * 1/2 drive decks 1/2.  Mapping strips 3/4 onto the SAME decks (the old
      * ch1->deck1, ch2->deck2, ch3->deck1, ch4->deck2) made two strips fight
      * over one rbp channel: setting strip 1's fader was immediately
      * overwritten by strip 3's position (and vice versa), which also broke
      * the startup absolute-control re-assert. */
     for (int m = 0; m < 2; m++) {
          int rch = m;
          int sch = 1 + m;         /* strip 1 -> deck 1, strip 2 -> deck 2 */
          add_abs(rch, 3,  K_TRIM, sch);
          add_abs(rch, 4,  K_EQH, sch);
          add_abs(rch, 6,  K_EQM, sch);
          add_abs(rch, 8,  K_EQL, sch);
          add_abs(rch, 14, K_FADER, sch);
          add_abs(rch, 11, K_COLOR, sch);      /* sweep fx knob -> Color knob */
          add_note(rch, 13, 0, sch);           /* PFL: no RX3 code, log-only */
     }

     /* ---- DJ FX (global ch 15) ----
      * (turn-encoders CC35/CC36/CC40 need relative-encoder handling and are
      * handled separately below.) */
     add_note(15, 26, K_BFX, CH_GLOBAL);       /* FX activate (0x448d) */
     add_abs(15, 4,  K_DEPTH, CH_GLOBAL);      /* FX wet/dry knob (0x448f) */
     /* ch15 note 25 (TIME-knob push) is deliberately NOT mapped to TAP: it is
      * used as the modifier for BEAT < / BEAT > instead (see handle_fx_time).
      * Both BEAT buttons need op == press: asEventCode gates 0x4490/0x4491
      * (events 0x2015/0x2016) on (op & 0xf) == 0. */
}

/* Everything that has to reach rbp once it exists. Runs before the first event
 * is dispatched, because anything sent earlier is dropped by rbp's own mixer
 * init -- which is also why the master level is re-asserted by rbp_vu.c. */
static void jp21_startup(void)
{
     /* Ensure audio routing in djengine::MixerRouteMngr:
      * On real RX3, physical DECK/LINE switches assign input routing.
      * On Prime GO without subucom switches, default routes left Channel 2 to Player 0.
      * Fix: permanently route Input 0 -> Player 0 (Deck 1) and Input 1 -> Player 1 (Deck 2).
      */
     *(volatile uint32_t *)0x01149f50 = 0x01149f08; /* Input 0 -> Player 0 */
     *(volatile uint32_t *)0x01149f54 = 0x01149f10; /* Input 1 -> Player 1 */
     klog("knobshim2: routed Mixer Ch1 -> Deck1, Ch2 -> Deck2\n");

     /* Initialize Sound Color FX to Filter on both channels so Sweep FX knob works out of the box */
     send_rx_key(K_FILTER, OP_PRESS, 1, 0);
     send_rx_key(K_FILTER, OP_RELEASE, 1, 0);
     send_rx_key(K_FILTER, OP_PRESS, 2, 0);
     send_rx_key(K_FILTER, OP_RELEASE, 2, 0);
     send_rx_key_f(K_COLOR, OP_VALUE, 1, 512, 0.5f);
     send_rx_key_f(K_COLOR, OP_VALUE, 2, 512, 0.5f);
     klog("knobshim2: Sound Color FX initialized to Filter on Ch1 & Ch2\n");

     /* Always set Beat FX Channel to MASTER (channel 5) */
     send_rx_key(K_BFXCH, OP_VALUE, CH_GLOBAL, BFX_CH_MASTER);
     klog("knobshim2: Beat FX Channel set to MASTER (5)\n");

     /* Set rbp's master level to unity.  The SC Live 4's Main Vol (CC20) is
      * applied by audioshim to ch0/1 (XLR) only, and the built-in monitors /
      * headphones have their own controls, so rbp's internal master level must
      * sit at a fixed reference or the master VU/level lags the channels. */
     send_rx_key_f(K_MASTERLVL, OP_VALUE, CH_GLOBAL, 1023, 1.0f);
     klog("knobshim2: master level set to unity\n");

     pthread_t bfx_tid;
     pthread_create(&bfx_tid, NULL, bfx_init_thread, NULL);
     pthread_detach(bfx_tid);
}

/* The two things that are about time rather than about an event: the jog's
 * idle timeout, and SYNC-hold -> MASTER. Both used to own a thread; they share
 * this one because they are both short checks at the same cadence. */
static void jp21_tick(void)
{
     jog_idle_tick();

     /* Fires SYNC-hold -> MASTER as soon as the hold threshold is reached,
      * without waiting for the button to be released. */
     for (int d = 0; d < 2; d++) {
          if (sync_held[d] && !sync_hold_fired[d] &&
              shim_now_ms() - sync_press_ms[d] >= SYNC_HOLD_MS) {
               sync_hold_fired[d] = 1;
               send_rx_key(K_MASTER, OP_PRESS, d + 1, 0);
               send_rx_key(K_MASTER, OP_RELEASE, d + 1, 0);
               if (verbose)
                    klog("knobshim2: deck%d SYNC held -> MASTER 0x4111\n",
                         d + 1);
          }
     }
}

const struct ctrl_map map_jp21 = {
     "jp21",
     jp21_build,
     jp21_startup,
     jp21_event,
     jp21_tick,
     NULL,            /* devices(): a MIDI surface, so no evdev reader */
     NULL,            /* input(): ditto -- never sees an evdev triple */
     &jp21_leds,
};
