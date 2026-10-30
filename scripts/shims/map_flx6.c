/*
 * map_flx6.c -- two-deck DDJ-FLX6 input map for the RX3 clone.
 *
 * Input addresses are taken from the operator's Pioneer-DDJ-FLX6.midi.xml.
 * That file is authoritative for addresses and descriptions, but not for MIDI
 * output: LEDs, meters, device SysEx and keepalive are intentionally disabled
 * until measured on the controller. Decks 3/4 and extra pad modes are outside
 * this first port.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <sound/asequencer.h>

#include "shimutil.h"
#include "shmstate.h"
#include "rbp_abi.h"
#include "rbp_bridge.h"
#include "rbp_vu.h"
#include "pitch_state.h"
#include "ctrl_map.h"

enum {
     CH_DECK1 = 0, CH_DECK2 = 1,
     CH_FXA = 4, CH_FXB = 5, CH_MIX = 6,
     CH_PADS1 = 7, CH_PADS1S = 8, CH_PADS2 = 9, CH_PADS2S = 10,
};

enum {
     N_PLAY = 0x0b, N_CUE = 0x0c, N_LOOPIN = 0x10, N_LOOPOUT = 0x11,
     N_MODE_HOT = 0x1b, N_MODE_PFX1 = 0x1e,
     N_MODE_JUMP = 0x20, N_MODE_SMPL = 0x22,
     N_JOG_TOUCH = 0x36, N_SLIP = 0x3d, N_KEYLOCK = 0x3e,
     N_SHIFT = 0x3f, N_RELOOP = 0x4d, N_CH_CUE = 0x54,
     N_SYNC = 0x58, N_MASTER = 0x5c, N_QUANTIZE = 0x5d,
     N_TEMPO_RANGE = 0x60,
     N_BROWSE_PUSH = 0x41, N_BROWSE_BACK = 0x42,
     N_SOURCE = 0x65, N_MENU = 0x66, N_PLAYLIST = 0x68, N_VIEW = 0x7a,
     N_LOAD1 = 0x46, N_LOAD2 = 0x47,
     N_FX_BEATPREV = 0x06, N_FX_BEATNEXT = 0x07,
     N_FX_CH1 = 0x10, N_FX_CH2 = 0x11, N_FX_MASTER = 0x14,
     N_FX_NEXT = 0x63, N_FX_PREV = 0x64,
};

enum {
     CC_TEMPO_MSB = 0x00, CC_TEMPO_LSB = 0x20,
     CC_TRIM = 0x04, CC_EQH = 0x07, CC_EQM = 0x0b,
     CC_EQL = 0x0f, CC_FADER = 0x13,
     CC_JOG_SIDE = 0x21, CC_JOG_VINYL = 0x22,
     CC_JOG_NOVINYL = 0x23, CC_JOG_SHIFT = 0x29,
     CC_HP_MIX = 0x0c, CC_CFX1 = 0x17, CC_CFX2 = 0x18,
     CC_XFADER = 0x1f, CC_BROWSE = 0x40,
};

static int knob_scale = 1;
static int jog_scale = 1;
static int jog_ppr = 720; /* provisional: verify FLX6 sensitivity on hardware */
static int jog_rev;
static int jog_idle_ms = 120;
static int jog_verbose;
static int tempo_verbose;
static int tempo_rev;
static int fx_pos = -1;

struct menu_tap {
     int note, key, pressed;
     const char *name;
};
static struct menu_tap menu_taps[] = {
     { N_SOURCE,   K_SOURCE,   0, "SOURCE" },
     { N_MENU,     K_MENU,     0, "MENU" },
     { N_PLAYLIST, K_PLAYLIST, 0, "PLAYLIST" },
     { N_VIEW,     K_BROWSE,   0, "BROWSE" },
};

struct jog_ctrl {
     int rch, sch, started;
     unsigned long long last_ms;
     unsigned int vpos;
     int moving;
};
static struct jog_ctrl jog_state[2] = {
     { CH_DECK1, 1, 0, 0, 0, 0 },
     { CH_DECK2, 2, 0, 0, 0, 0 },
};

struct pitch_ctrl { int sch, have_hi, pos; };
static struct pitch_ctrl tempo[2] = {
     { 1, 0, 0x2000 }, { 2, 0, 0x2000 },
};

static void flx6_note(int ch, int note, int on)
{
     /* The operator's FLX6 reports these menu buttons as 0x96 <note> 0x00:
      * zero-velocity Note Ons with no preceding nonzero edge. Accept normal
      * pairs too, but turn a lone zero edge into the complete tap rbp expects. */
     for (unsigned int i = 0; ch == CH_MIX && i < sizeof menu_taps / sizeof menu_taps[0]; i++) {
          struct menu_tap *b = &menu_taps[i];
          int lone_release = 0;
          if (note != b->note)
               continue;
          if (on) {
               if (!b->pressed)
                    send_rx_key(b->key, OP_PRESS, CH_GLOBAL, 0);
               b->pressed = 1;
          } else if (b->pressed) {
               send_rx_key(b->key, OP_RELEASE, CH_GLOBAL, 0);
               b->pressed = 0;
          } else {
               lone_release = 1;
               send_rx_key(b->key, OP_PRESS, CH_GLOBAL, 0);
               send_rx_key(b->key, OP_RELEASE, CH_GLOBAL, 0);
          }
          if (verbose)
               klog("knobshim2: FLX6 %s note 0x%02x on=%d -> key 0x%04x%s\n",
                    b->name, note, on, b->key,
                    lone_release ? " synthetic tap" : "");
          return;
     }

     if ((ch == CH_DECK1 || ch == CH_DECK2) && note == N_CH_CUE) {
          if (on) {
               int cur = me_get_cue(ch);
               me_set_cue(ch, cur == 0);
          }
          return;
     }

     if ((ch == CH_PADS1S || ch == CH_PADS2S) && note >= 0 && note < 8) {
          if (on)
               hotcue_delete(ch == CH_PADS1S ? 0 : 1, note + 1);
          return;
     }

     /* These are direct, meaningful equivalents in RX3's one-target Beat FX. */
     if ((ch == CH_FXA || ch == CH_FXB) &&
         (note == N_FX_CH1 || note == N_FX_CH2 || note == N_FX_MASTER)) {
          if (on) {
               int target = note == N_FX_CH1 ? 0 :
                            note == N_FX_CH2 ? 1 : BFX_CH_MASTER;
               send_rx_key(K_BFXCH, OP_VALUE, CH_GLOBAL, target);
          }
          return;
     }
     if (ch == CH_FXA && (note == N_FX_NEXT || note == N_FX_PREV)) {
          if (on) {
               int step = note == N_FX_NEXT ? 1 : -1;
               if (fx_pos < 0) {
                    int t = rbp_beatfx_type();
                    fx_pos = (t > 0 && t <= BFX_TYPE_POSITIONS &&
                              bfx_type_to_pos[t] >= 0) ? bfx_type_to_pos[t] : 0;
               }
               fx_pos = (fx_pos + step + BFX_TYPE_POSITIONS) % BFX_TYPE_POSITIONS;
               send_rx_key(K_BFXTYPE, OP_VALUE, CH_GLOBAL, fx_pos);
          }
          return;
     }
     if (ch == CH_FXA && (note == N_FX_BEATPREV || note == N_FX_BEATNEXT)) {
          if (on) {
               int prev = note == N_FX_BEATPREV;
               long d = prev ? -1 : 1;
               send_rx_key_fl(prev ? K_BEATPREV : K_BEATNEXT,
                              OP_PRESS, CH_GLOBAL, d, 0.0f, d);
          }
          return;
     }

     for (int i = 0; i < note_map_n; i++) {
          struct note_ctrl *n = &note_map[i];
          if (n->rch != ch || n->note != note)
               continue;
          if (!n->key)
               return;
          if (on && !n->pressed) {
               n->pressed = 1;
               send_rx_key(n->key, OP_PRESS, n->sch, 0);
               if (n->repeat)
                    send_rx_key(n->key, OP_REPEAT, n->sch, 0);
          } else if (!on && n->pressed) {
               n->pressed = 0;
               send_rx_key(n->key, OP_RELEASE, n->sch, 0);
          }
          return;
     }
     if (verbose)
          klog("knobshim2: FLX6 unmapped ch%d note%d on=%d\n", ch, note, on);
}

static void flx6_cc_abs(int ch, int cc, int val)
{
     for (int i = 0; i < abs_map_n; i++) {
          struct abs_ctrl *a = &abs_map[i];
          if (a->rch != ch || a->cc != cc)
               continue;
          int v = cc_to_10bit(val);
          if (v == a->last)
               return;
          a->last = v;
          float f = (float)v / 1023.0f;
          if (a->key == K_FADER && a->sch >= 1 && a->sch <= 2) {
               g_fader[a->sch] = v;
               g_fader_seen[a->sch] = 1;
          } else if (a->key == K_HPMIX) {
               g_cue_mix = f;
          }
          send_rx_key_f(a->key, OP_VALUE, a->sch, v, f);
          return;
     }
     if (verbose)
          klog("knobshim2: FLX6 unmapped ch%d cc%d val=%d\n", ch, cc, val);
}

static void flx6_browse(int val)
{
     if (val < 0 || val > 127)
          return;
     int delta = val >= 64 ? val - 128 : val;
     if (delta)
          send_rx_key(K_SELECTOR, OP_ROTATE, CH_GLOBAL,
                      delta * knob_scale < 0 ? -1 : 1);
}

static void flx6_jog(int ch, int val)
{
     struct jog_ctrl *s = &jog_state[ch == CH_DECK2];
     int d = val - 0x40; /* XML describes relative input; centre needs hardware check. */
     if (!d)
          return;
     unsigned long long now = shim_now_ms();
     if (!s->started) {
          s->started = 1;
          s->last_ms = now;
          return;
     }
     if (jog_rev)
          d = -d;
     int dp = d * jog_scale;
     float dt = (float)(long long)(now - s->last_ms) / 1000.0f;
     s->last_ms = now;
     if (dt < 0.0005f)
          dt = 0.0005f;
     s->vpos = (s->vpos + (unsigned int)dp) & 0xffffu;
     float speed = (float)dp / (float)(jog_ppr * jog_scale) / dt;
     if (speed > 8.0f) speed = 8.0f;
     if (speed < -8.0f) speed = -8.0f;
     s->moving = 1;
     send_rx_key_fl(K_JOG_ROT, OP_ROTATE, s->sch, 0, speed, (long)s->vpos);
     if (jog_verbose)
          klog("knobshim2: FLX6 jog ch%d d=%d speed=%.2f\n",
               ch, d, (double)speed);
}

static void flx6_pitch(int ch, int cc, int val)
{
     int deck = ch == CH_DECK2;
     struct pitch_ctrl *s = &tempo[deck];
     if (cc == CC_TEMPO_MSB) {
          s->have_hi = 1;
          s->pos = (s->pos & 0x7f) | ((val & 0x7f) << 7);
          return;
     }
     s->pos = (s->pos & 0x3f80) | (val & 0x7f);
     if (!s->have_hi)
          return;
     float norm = ((float)s->pos - 8192.0f) / 8192.0f;
     if (norm > 1.0f) norm = 1.0f;
     if (norm < -1.0f) norm = -1.0f;
     if (tempo_rev) norm = -norm;
     int v10 = (int)((norm + 1.0f) * 511.5f);
     pitch_state_set(deck, norm);
     send_rx_key_fl(K_TEMPO_SLIDER, OP_VALUE, s->sch, v10, norm, s->pos);
     if (tempo_verbose)
          klog("knobshim2: FLX6 tempo deck%d pos=%d norm=%.3f\n",
               deck + 1, s->pos, (double)norm);
}

static void flx6_build(void)
{
     knob_scale = env_num("KNOB_SCALE", 1); if (knob_scale < 1) knob_scale = 1;
     jog_scale = env_num("JOG_SCALE", 1); if (jog_scale < 1) jog_scale = 1;
     jog_ppr = env_num("JOG_PPR", 720); if (jog_ppr < 1) jog_ppr = 1;
     jog_rev = env_on("JOG_REV", 0);
     jog_idle_ms = env_num("JOG_IDLE_MS", 120); if (jog_idle_ms < 10) jog_idle_ms = 10;
     jog_verbose = env_on("JOG_VERBOSE", 0);
     tempo_verbose = env_on("TEMPO_VERBOSE", 0);
     tempo_rev = env_on("TEMPO_REV", 0);

     for (int d = 0; d < 2; d++) {
          int rch = CH_DECK1 + d, sch = 1 + d;
          add_note(rch, N_PLAY, K_PLAY, sch);
          add_note(rch, N_CUE, K_CUE, sch);
          add_note(rch, N_LOOPIN, K_LOOPIN, sch);
          add_note(rch, N_LOOPOUT, K_LOOPOUT, sch);
          add_note(rch, N_RELOOP, K_RELOOP, sch);
          add_note(rch, N_JOG_TOUCH, K_JOG_TOUCH, sch);
          add_note(rch, N_SYNC, K_SYNC, sch);
          add_note(rch, N_MASTER, K_MASTER, sch);
          add_note(rch, N_KEYLOCK, K_MT, sch);
          add_note(rch, N_QUANTIZE, K_QUANTIZE, sch);
          add_note(rch, N_SLIP, K_SLIP, sch);
          add_note(rch, N_TEMPO_RANGE, K_TEMPO_RANGE, sch);
          add_note(rch, N_MODE_HOT, K_HOTCUE, sch);
          add_note(rch, N_MODE_PFX1, K_ALOOP, sch);
          add_note(rch, N_MODE_JUMP, K_SLIPLOOP, sch);
          add_note(rch, N_MODE_SMPL, K_BEATJUMP, sch);
          add_note(rch, N_SHIFT, 0, sch);
          add_abs(rch, CC_TRIM, K_TRIM, sch);
          add_abs(rch, CC_EQH, K_EQH, sch);
          add_abs(rch, CC_EQM, K_EQM, sch);
          add_abs(rch, CC_EQL, K_EQL, sch);
          add_abs(rch, CC_FADER, K_FADER, sch);
     }
     for (int d = 0; d < 2; d++) {
          int rch = d ? CH_PADS2 : CH_PADS1;
          for (int p = 0; p < 8; p++) {
               add_note(rch, 0x00 + p, K_PAD1 + p, 1 + d);
               add_note(rch, 0x10 + p, K_PAD1 + p, 1 + d);
               add_note(rch, 0x20 + p, K_PAD1 + p, 1 + d);
               add_note(rch, 0x30 + p, K_PAD1 + p, 1 + d);
          }
     }
     add_abs(CH_MIX, CC_CFX1, K_COLOR, 1);
     add_abs(CH_MIX, CC_CFX2, K_COLOR, 2);
     add_abs(CH_MIX, CC_XFADER, K_XFADER, CH_GLOBAL);
     add_abs(CH_MIX, CC_HP_MIX, K_HPMIX, CH_GLOBAL);
     add_note(CH_MIX, N_BROWSE_PUSH, K_SELECTOR, CH_GLOBAL);
     add_note(CH_MIX, N_BROWSE_BACK, K_BACK, CH_GLOBAL);
     /* Measured on the operator's FLX6: status 0x96 (rch 6), with lone
      * zero-velocity Note Ons. flx6_note() completes those into taps. */
     add_note(CH_MIX, N_SOURCE, K_SOURCE, CH_GLOBAL);
     add_note(CH_MIX, N_MENU, K_MENU, CH_GLOBAL);
     add_note(CH_MIX, N_PLAYLIST, K_PLAYLIST, CH_GLOBAL);
     add_note(CH_MIX, N_VIEW, K_BROWSE, CH_GLOBAL);
     add_note(CH_MIX, N_LOAD1, K_LOAD, 1);
     add_note(CH_MIX, N_LOAD2, K_LOAD, 2);
}

static void flx6_startup(void)
{
     *(volatile uint32_t *)ADDR_MIXER_ROUTE_PLAYER0 = MIXER_ROUTE_INPUT0;
     *(volatile uint32_t *)ADDR_MIXER_ROUTE_PLAYER1 = MIXER_ROUTE_INPUT1;
     for (int ch = 1; ch <= 2; ch++) {
          send_rx_key(K_FILTER, OP_PRESS, ch, 0);
          send_rx_key(K_FILTER, OP_RELEASE, ch, 0);
          send_rx_key_f(K_COLOR, OP_VALUE, ch, 512, 0.5f);
     }
     send_rx_key_f(K_MASTERLVL, OP_VALUE, CH_GLOBAL, 1023, 1.0f);
}

static void flx6_tick(void)
{
     unsigned long long now = shim_now_ms();
     for (int i = 0; i < 2; i++) {
          struct jog_ctrl *s = &jog_state[i];
          if (s->started && s->moving && now - s->last_ms >= (unsigned)jog_idle_ms) {
               s->moving = 0;
               send_rx_key_fl(K_JOG_ROT, OP_ROTATE, s->sch, 0, 0.0f, s->vpos);
          }
     }
}

static void flx6_event(const struct snd_seq_event *ev)
{
     if (ev->type == SNDRV_SEQ_EVENT_CONTROLLER) {
          int ch = ev->data.control.channel;
          int cc = ev->data.control.param;
          int val = ev->data.control.value;
          if ((ch == CH_DECK1 || ch == CH_DECK2) &&
              (cc == CC_TEMPO_MSB || cc == CC_TEMPO_LSB))
               flx6_pitch(ch, cc, val);
          else if ((ch == CH_DECK1 || ch == CH_DECK2) &&
                   (cc == CC_JOG_SIDE || cc == CC_JOG_VINYL ||
                    cc == CC_JOG_NOVINYL || cc == CC_JOG_SHIFT))
               flx6_jog(ch, val);
          else if (ch == CH_MIX && cc == CC_BROWSE)
               flx6_browse(val);
          else
               flx6_cc_abs(ch, cc, val);
     } else if (ev->type == SNDRV_SEQ_EVENT_NOTEON) {
          flx6_note(ev->data.note.channel, ev->data.note.note,
                    ev->data.note.velocity > 0);
     } else if (ev->type == SNDRV_SEQ_EVENT_NOTEOFF) {
          flx6_note(ev->data.note.channel, ev->data.note.note, 0);
     }
}

/* Every output address is deliberately unavailable until FLX6 MIDI receive
 * behavior is measured. Zero is a valid note, so every absent field is explicit. */
static const struct led_notes flx6_leds = {
     .deck_ch = -1,
     .n_sync = -1, .n_cue = -1, .n_play = -1, .n_keylock = -1,
     .n_vinyl = -1, .n_slip = -1, .n_loopin = -1, .n_loopout = -1,
     .n_autoloop = -1, .n_mode = { -1, -1, -1, -1 },
     .pad_ch = -1, .pad_ch2 = -1,
     .n_pad_base = { -1, -1, -1, -1 }, .pad_enc = LED_ENC_NONE,
     .strip_ch_first = -1, .strip_count = 0, .n_strip_cue = -1,
     .master_ch_first = -1, .master_ch_count = 0, .n_master_cue = -1,
     .fx_ch = -1,
     .n_fx = { -1, -1, -1, -1, -1 },
     .meter_ch_first = -1, .n_meter_cc = -1, .meter_enc = METER_ENC_NONE,
     .meter_pre_fader = 0, .meter_master_ch = -1,
     .n_meter_master_l = -1, .n_meter_master_r = -1,
};

const struct ctrl_map map_flx6 = {
     "flx6", flx6_build, flx6_startup, flx6_event, flx6_tick,
     NULL, NULL, &flx6_leds,
};
