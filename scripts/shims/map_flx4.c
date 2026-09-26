/*
 * map_flx4.c -- the Pioneer/AlphaTheta DDJ-FLX4 control surface.
 *
 * The surface this port exists for: an XDJ-RX3 player driven from a DDJ-FLX4.
 * It is a sibling of map_jp21.c, not a replacement -- MIDI_MAP selects between
 * them and both stay in the build.
 *
 * ============================== PROVENANCE ================================
 *
 * Every note number, CC number and channel below comes from Pioneer DJ /
 * AlphaTheta's published **DDJ-FLX4 "List of MIDI messages", Ver 1.0** (the PDF
 * on the unit's support page), NOT from a dump of the hardware. Nothing in this
 * repository has ever been run with an FLX4 attached, so:
 *
 *   - The tables are UNVERIFIED. They are written from a vendor document, then
 *     cross-compiled and exercised by a hand-written fixture
 *     (scripts/shims/test_flx4.c). That is a statement about this file; it is
 *     not evidence about the unit. Anything here marked `TODO: unverified` is
 *     an expectation, and docs/15-flx4-midi.md's open questions list is where
 *     the ones that a dump would settle are collected.
 *
 *   - The single most load-bearing conversion is the CHANNEL. The list prints
 *     MIDI channels 1-based; struct snd_seq_event carries them 0-based, so every
 *     channel here is the list's minus one. A deck on the wrong channel makes
 *     every control on it do nothing at all, and no fixture can catch that --
 *     only a dump from the unit can.
 *
 *   - Where the list gives a value but not its meaning (a polarity, a pulse
 *     count, which of two relative-encoder conventions an encoder uses) the
 *     choice is called out in a comment at the table.
 *
 * The procedure that replaces all of this is in docs/15-flx4-midi.md: dump the
 * unit (aseqdump, then MIDI_DUMP), compare against these tables, correct them.
 * A dump is the only thing that makes any number here evidence.
 *
 * ====================== what the list changed about the plan ==============
 *
 * Two assumptions the port started with did not survive contact with the
 * published list, and both are worth knowing before reading the tables:
 *
 *   1. There are NO decks 3/4 and no deck switch. The FLX4 is a 2-channel
 *      surface: every deck control is on list channel 1 or 2 (0-based 0/1), and
 *      nothing mirrors onto anything. The only channel-selection control is the
 *      Beat FX FX CH SELECT lever (notes 16/17), which selects the effect's
 *      TARGET, not a deck. So "decks 3/4 mirror onto players 1/2" is moot.
 *
 *   2. Its toggles are NOTES, not CCs. Every CC on this unit is continuous: a
 *      14-bit MSB/LSB position pair, or a relative encoder. There is no CC used
 *      as a switch and no >=64 threshold anywhere, so this map needs neither a
 *      threshold row nor a CC-as-button entry kind -- ctrl_map.h never grew one,
 *      and docs/15's old claim that "DDJ-class controllers put toggles on CCs
 *      with a >=64 threshold" is not true of this unit.
 *
 * A third, smaller one: the jog is RELATIVE (0x40-centred), not a 14-bit
 * absolute pair. rbp's side of the contract (0x4305 / OP_ROTATE with a
 * revolutions-per-second float) is unchanged and is the same one map_jp21.c
 * drives; what differs is where the count comes from. See flx4_jog().
 *
 * ==================== what the FLX4 cannot reach on rbp ===================
 *
 * rbp is an XDJ-RX3 and the FLX4 is not an RX3. These have no control on the
 * unit at all, so they are unreachable from this map; the port's pointer path
 * (fbshim.so, docs/07-touch.md) is what reaches the UI for them:
 *
 *   - BACK, SOURCE, MENU, VIEW, INFO, TAG LIST, USB1, REKORDBOX and every other
 *     menu key (map_jp21.c has rows for several; this unit has no such buttons);
 *   - MASTER TEMPO (key lock), tempo range, VINYL mode, SLIP: on an FLX4 these
 *     are application settings, not panel controls (the VINYL MODE LED is even
 *     documented as something the *application* sets -- see the LED block);
 *   - the Sound Color FX TYPE (the unit has CFX knobs but no CFX buttons), which
 *     is why flx4_startup() picks Filter for both channels;
 *   - rbp's ALOOP/SLIPLOOP pad modes (see the pad block in flx4_build());
 *   - CUE/LOOP CALL: rbp's keycode for those buttons is not identified. They are
 *     in the table as log-only, and in docs/15's open questions.
 *
 * Everything about rbp lives in rbp_abi.h/rbp_bridge.h; the only place this file
 * touches a raw address is flx4_startup()'s two mixer-route words, which are
 * named in rbp_abi.h for exactly that reason.
 *
 * ============================== the LEDs ==================================
 *
 * Recorded here, driven nowhere yet. The unit's illumination messages are all
 * MIDI-OUT (the list's last section), and rbp_led.c is still shaped for the
 * previous target's channels and notes, so with RB_LED_DISABLE=1 (rb.conf, this
 * target's interim setting) nothing illuminates at all:
 *
 *   LOADED (track-load illumination)  note 0 / 1, list ch 16 (0-based 15)
 *   VINYL MODE (app-set, footnote *2) note 23,     list ch 1 / 2 (0-based 0/1)
 *   CH LEVEL METER                    CC 2,        list ch 1 / 2 (0-based 0/1)
 *     ... a value ramp, not a bitmask: 0x26-0x40 green1, 0x41-0x56 green2,
 *     0x57-0x64 orange1, 0x65-0x76 orange2, 0x77-0x7F red. Note that the list's
 *     type column calls this row a NOTE while its own status byte is B0, a
 *     control change; the status byte is what the wire carries.
 *
 * The transport/pad/loop LEDs are not in the list at all beyond the two rows
 * above, so their note numbers are simply unknown -- another reason the LED
 * bridge is off for this target rather than guessed. docs/15-flx4-midi.md has
 * the full table and the open question.
 */
#define _GNU_SOURCE
#include <stdint.h>         /* uint32_t, for the two mixer-route words */
#include <sound/asequencer.h>

#include "shimutil.h"
#include "shmstate.h"       /* g_cue_mix / g_cue_gain, which the knobs feed */
#include "rbp_abi.h"
#include "rbp_bridge.h"
#include "rbp_vu.h"         /* g_fader / g_fader_seen: the meter scaling input */
#include "ctrl_map.h"

/* ---- receive channels, 0-based (the list's channel minus one) -------------
 * See the PROVENANCE note above: this is the conversion a dump has to confirm. */
#define CH_DECK1  0   /* list ch 1  -- deck 1 buttons, jog, tempo, its mixer strip */
#define CH_DECK2  1   /* list ch 2  -- deck 2, the same set */
#define CH_FXA    4   /* list ch 5  -- Beat FX section, and FX CH SELECT leg A */
#define CH_FXB    5   /* list ch 6  -- FX CH SELECT leg B (and the CH2 FX leg) */
#define CH_MIX    6   /* list ch 7  -- mixer knobs, browse, LOAD, MASTER CUE */
#define CH_PADS1  7   /* list ch 8  -- deck 1 pads, current mode */
#define CH_PADS1S 8   /* list ch 9  -- deck 1 pads, +SHIFT (unbound, see build) */
#define CH_PADS2  9   /* list ch 10 -- deck 2 pads, current mode */
#define CH_PADS2S 10  /* list ch 11 -- deck 2 pads, +SHIFT (unbound) */

/* ---- deck buttons ---------------------------------------------------------
 * Every one of these has a +SHIFT variant on the same channel (a different
 * note). None of them is bound: the +SHIFT note is not sent when SHIFT is up,
 * so a bound note here can never arrive while SHIFT is held, and the list gives
 * the shift actions no names. With SHIFT held, each control therefore logs as
 * unmapped, which is the honest outcome. */
#define N_PLAY       11   /* PLAY/PAUSE         (+SHIFT 14) */
#define N_CUE        12   /* CUE                (+SHIFT 72) */
#define N_SHIFT      63   /* SHIFT -- tracked by nothing, log-only */
#define N_JOG_TOUCH  54   /* platter touch      (+SHIFT 103) */
#define N_IN         16   /* loop IN            (+SHIFT 76) */
#define N_OUT        17   /* loop OUT           (+SHIFT 78) */
#define N_4BEAT      77   /* 4 BEAT / EXIT      (+SHIFT 80) */
#define N_CALL_PREV  81   /* CUE/LOOP CALL <    (+SHIFT 62) */
#define N_CALL_NEXT  83   /* CUE/LOOP CALL >    (+SHIFT 61) */
#define N_SYNC       88   /* BEAT SYNC, sent on RELEASE (footnote *3) */
#define N_SYNC_LONG  92   /* BEAT SYNC long press */
#define N_CH_CUE     84   /* channel CUE        (+SHIFT 104) */
#define N_MODE_HOT   27   /* pad mode HOT CUE   (+SHIFT 105) */
#define N_MODE_PFX1  30   /* pad mode PAD FX 1  (+SHIFT 107) */
#define N_MODE_JUMP  32   /* pad mode BEAT JUMP (+SHIFT 109) */
#define N_MODE_SMPL  34   /* pad mode SAMPLER    (+SHIFT 111) */

/* ---- mixer / browse (all on list ch 7 unless noted) ---------------------- */
#define N_MASTER_CUE  99  /* MASTER CUE         (+SHIFT 120) */
#define N_BROWSE_PUSH 65  /* browse knob push   (+SHIFT 66 = SOURCE, see build) */
#define N_BROWSE_PUSH_SHIFT 66  /* SHIFT + browse knob push -> rbp's SOURCE screen */
#define N_LOAD1       70  /* LOAD deck 1        (+SHIFT 104) */
#define N_LOAD2       71  /* LOAD deck 2        (+SHIFT 122) */
#define N_SMART_CFX   0   /* SMART CFX          (+SHIFT 8)  -- no rbp equivalent */
#define N_SMART_FADER 1   /* SMART FADER        (+SHIFT 9)  -- no rbp equivalent */
#define N_MONO_STEREO 109 /* Android MONO/STEREO            -- no rbp equivalent */

/* ---- Beat FX (list ch 5, with a second leg on ch 6) --------------------- */
#define N_FXCH_1     16   /* FX CH SELECT leg A (ch 5) */
#define N_FXCH_2     17   /* FX CH SELECT leg B (ch 6) */
#define N_FX_SELECT  99   /* FX SELECT         (+SHIFT 100) */
#define N_FX_BEATPREV 74  /* BEAT <            (+SHIFT 102) */
#define N_FX_BEATNEXT 75  /* BEAT >            (+SHIFT 107) */
#define N_FX_ONOFF   71   /* FX ON/OFF, one leg per target channel (+SHIFT 67) */

/* ---- CCs -----------------------------------------------------------------
 * Every continuous control on this unit is a 14-bit pair: MSB on CC n, LSB on
 * CC n+0x20. Only the MSB is bound (one shared abs_map row per control, which
 * is the same 128-step resolution map_jp21.c had for its knobs); the LSB then
 * arrives as an unmapped CC and is dropped. The tempo fader is the exception --
 * rbp's K_TEMPO_SLIDER wants the full 14 bits -- and is handled in
 * flx4_pitch(). */
#define CC_TEMPO_MSB  0
#define CC_TEMPO_LSB  32
#define CC_JOG_WHEEL  33   /* outer ring, relative, 0x40-centred */
#define CC_JOG_VINYL  34   /* platter when the unit's vinyl mode is ON (default) */
#define CC_JOG_NOVINYL 35  /* platter when it is OFF (footnote *2: app-set only) */
#define CC_JOG_SHIFT  41   /* platter while SHIFT is held */
#define CC_TRIM       4
#define CC_EQH        7
#define CC_EQM        11
#define CC_EQL        15
#define CC_FADER      19
#define CC_XFADER     31
#define CC_CFX1       23   /* Sound Color FX knob, deck 1 (list ch 7) */
#define CC_CFX2       24   /* ditto, deck 2 */
#define CC_HP_MIX     12
#define CC_HP_LEVEL   13
#define CC_FX_DEPTH   2    /* Beat FX LEVEL/DEPTH */
#define CC_BROWSE     64   /* relative, 0x01/0x7F -- NOT the jog's convention */
#define CC_BROWSE_SHIFT 100

/* ---- the map's own calibration (read by value: empty means default) ------ */
static int knob_scale = 1;      /* browse steps per detent */
static int jog_scale = 1;
static int jog_ppr = 720;       /* MEASURED on the unit (docs/15 S5.1). One
                                 * counted turn of the platter is 7183 forward
                                 * counts (6509 of +1, 337 of +2, 17 back), so
                                 * a nominal 10 revolutions gives 718.3
                                 * counts/revolution. 718 would be false
                                 * precision: the plausible design value is 720
                                 * and the 0.24% shortfall is 8.6 degrees of arc
                                 * on a hand turn. This is the only number the
                                 * jog's speed depends on -- JOG_SCALE cancels
                                 * out of speed (it scales vpos only) -- so the
                                 * old guess of 128 (map_jp21.c's jog, a
                                 * different platter) made every turn 5.6x too
                                 * fast and pinned it to the 8 rev/s clamp
                                 * below. */
static int jog_rev = 0;         /* invert jog direction */
static int jog_idle_ms = 120;
static int jog_verbose = 0;
static int tempo_verbose = 0;
static int tempo_rev = 0;       /* invert pitch polarity */

/* The Beat FX type cursor: rbp's switch has 14 positions and the unit's FX
 * SELECT is a button, so the map keeps the position and steps it. It is NOT
 * seeded from rbp's current effect (map_jp21.c reads that through
 * ADDR_GET_BFX_TYPE; this map deliberately keeps rbp addresses out of itself),
 * so the first press selects switch position 0 -- whatever effect that turns out
 * to be -- rather than stepping on from what is already playing. */
static int fx_pos = -1;

/* ---- the jog -------------------------------------------------------------
 * Relative and 0x40-centred: the list's own wording is "Turn clockwise:
 * Increases from 0x41 / Turn counterclockwise: Decreases from 0x3F", so a
 * message carries (value - 0x40) counts since the previous one. That is a
 * different convention from the BROWSE knob a few rows below it (0x01/0x7F),
 * which is why flx4_browse() is a separate handler with a separate scale. */
struct jog_ctrl {
     int rch;
     int sch;
     int started;                /* one message seen: dt has a baseline */
     unsigned long long last_ms;
     unsigned int vpos;          /* 16-bit wrapping virtual position rbp counts */
     float speed;
     int moving;
};
static struct jog_ctrl jog_state[2] = {
     { CH_DECK1, 1, 0, 0, 0, 0.0f, 0 },
     { CH_DECK2, 2, 0, 0, 0, 0.0f, 0 },
};

/* ---- the pitch fader -----------------------------------------------------
 * 14-bit, CC 0 (MSB) + CC 32 (LSB) on the deck's own channel. The list gives
 * the two ends: min ("-" side) = 0x00/0x00, max ("+" side) = 0x7F/0x7F. "-" and
 * "+" describe the tempo change, and rbp wants -1.0 = slower, +1.0 = faster, so
 * the value is used WITHOUT inversion -- see flx4_pitch(). (The previous
 * target's fader was the other way up and its map inverts; that difference is
 * the reason TEMPO_REV exists.) TODO: unverified -- the mapping from those two
 * ends to the physical fader is the thing a dump plus one push of the fader
 * settles. */
struct pitch_ctrl {
     int rch;
     int sch;
     int have_hi;
     int pos;                    /* 14-bit, 0 = "-" end, 0x3FFF = "+" end */
};
static struct pitch_ctrl pitch_state[2] = {
     { CH_DECK1, 1, 0, 0x2000 },
     { CH_DECK2, 2, 0, 0x2000 },
};

/* ---- the FX CH SELECT lever ---------------------------------------------
 * Two legs, which is how the list encodes a three-position lever: leg A is
 * list ch 5 note 16, leg B is list ch 6 note 17. CH1 = A on, CH2 = B on,
 * CH1&CH2 = both. (The other two notes of that group, ch 5 note 17 and ch 6
 * note 16, are OFF in every position the list gives.) */
static int fxch_leg1, fxch_leg2;
static int fxch_last = -1;      /* last target sent to rbp, for the log only */

/* ---- the note table's dispatch ------------------------------------------ */

/* A control rbp has no code for: logged, and otherwise ignored. Used for the
 * controls that exist on the unit but have no meaning here (SHIFT, the PAD FX 1
 * and SAMPLER pad modes, CUE/LOOP CALL, SMART CFX/FADER, MONO/STEREO), so the
 * table still says the unit has them. */
static void flx4_log_only(int ch, int note)
{
     if (verbose)
          klog("knobshim2: ch%d note%d (log-only)\n", ch, note);
}

static void flx4_note(int ch, int note, int on)
{
     /* CH CUE (list ch 1/2 note 84): rbp's XDJ panel has no per-channel PFL
      * keycode, so this drives the mixer engine directly -- the same call
      * map_jp21.c makes for the previous unit's PFL button. Latching: decided
      * on the press edge from the engine's own state, so the button and rbp
      * cannot disagree after a restart. */
     if ((ch == CH_DECK1 || ch == CH_DECK2) && note == N_CH_CUE) {
          if (on) {
               int cur = me_get_cue(ch);
               int want = (cur == 0) ? 1 : 0;
               me_set_cue(ch, want);
               if (verbose)
                    klog("knobshim2: CH CUE ch%d -> cue=%d (engine=%p)\n",
                         ch + 1, want, mixer_engine());
          }
          return;
     }

     /* MASTER CUE (list ch 7 note 99): same call the previous unit's strips 3/4
      * use. rbp does have a K_MASTERCUE keycode, but the engine call is the
      * path that has been verified live and needs no key manager. */
     if (ch == CH_MIX && note == N_MASTER_CUE) {
          if (on) {
               int cur = me_get_master_cue();
               int want = (cur == 0) ? 1 : 0;
               me_set_master_cue(want);
               klog("knobshim2: master cue -> %s\n", want ? "ON" : "OFF");
          }
          return;
     }

     /* BEAT SYNC (list ch 1/2 note 88). Footnote *3: the unit sends this when
      * the finger LEAVES the button, not when it lands. So the release is the
      * whole gesture, and both keycodes are sent together on whichever edge
      * arrives first -- rbp's SYNC has a long-press action of its own (instant
      * double), and a press left open is exactly the bug map_jp21.c documents.
      * TODO: unverified -- the list does not say whether the release sends the
      * ON edge, the OFF edge or both. Both are handled: whichever edge arrives
      * with the note not already down is the gesture, and the other one is
      * swallowed, so one press of the button can never send SYNC twice. */
     if ((ch == CH_DECK1 || ch == CH_DECK2) && note == N_SYNC) {
          static int sync_down[2];
          int d = ch;
          if (on) {
               if (sync_down[d])
                    return;
               sync_down[d] = 1;
          } else if (sync_down[d]) {
               sync_down[d] = 0;
               return;                 /* the release of the gesture above */
          }
          send_rx_key(K_SYNC, OP_PRESS, ch + 1, 0);
          send_rx_key(K_SYNC, OP_RELEASE, ch + 1, 0);
          if (verbose)
               klog("knobshim2: BEAT SYNC ch%d -> 0x%04x press+release\n",
                    ch + 1, K_SYNC);
          return;
     }

     /* BEAT SYNC long press (note 92): the list gives this note no name beyond
      * "Long press". rbp's K_MASTER is the closest thing the RX3 has to a
      * long-press sync action, and it is the same gesture map_jp21.c
      * implements with a 600 ms timer on the SYNC button -- this unit reports
      * it directly instead, so no timer is needed. Sent as a pulse on the press
      * edge, because rbp's MASTER acts on the press and a stuck one would latch.
      * TODO: unverified. */
     if ((ch == CH_DECK1 || ch == CH_DECK2) && note == N_SYNC_LONG) {
          if (on) {
               send_rx_key(K_MASTER, OP_PRESS, ch + 1, 0);
               send_rx_key(K_MASTER, OP_RELEASE, ch + 1, 0);
               klog("knobshim2: BEAT SYNC long press ch%d -> MASTER 0x%04x\n",
                    ch + 1, K_MASTER);
          }
          return;
     }

     /* FX CH SELECT (list ch 5/6 notes 16/17): see the leg note above. */
     if ((ch == CH_FXA || ch == CH_FXB) &&
         (note == N_FXCH_1 || note == N_FXCH_2)) {
          if (ch == CH_FXA && note == N_FXCH_1)
               fxch_leg1 = on;
          else if (ch == CH_FXB && note == N_FXCH_2)
               fxch_leg2 = on;
          else
               return;                 /* ch5 note17 / ch6 note16: always OFF */
          if (fxch_leg1 && fxch_leg2) {
               /* rbp's Beat FX has one target channel, so "both decks" has no
                * value to send. Leave rbp where it was rather than pick one.
                * TODO: unverified -- nothing on the unit says which position
                * the lever is in at connect, so this only acts on a move. */
               klog("knobshim2: FX CH SELECT CH1&CH2 has no rbp equivalent; "
                    "target left at %d\n", fxch_last);
               return;
          }
          if (!fxch_leg1 && !fxch_leg2)
               return;                 /* mid-slide: both legs momentarily off */
          int want = fxch_leg1 ? 0 : 1;   /* EnBeatEffectSelectChannel: 0/1 */
          if (want == fxch_last)
               return;                 /* the whole group is re-sent on every
                                        * move; only a change is a move */
          fxch_last = want;
          send_rx_key(K_BFXCH, OP_VALUE, CH_GLOBAL, want);
          klog("knobshim2: FX CH SELECT -> Beat FX target deck %d\n", want + 1);
          return;
     }

     /* FX SELECT (list ch 5 note 99): step rbp's 14-position effect switch. */
     if (ch == CH_FXA && note == N_FX_SELECT) {
          if (on) {
               fx_pos = (fx_pos + 1) % BFX_TYPE_POSITIONS;
               send_rx_key(K_BFXTYPE, OP_VALUE, CH_GLOBAL, fx_pos);
               klog("knobshim2: FX SELECT -> type position %d\n", fx_pos);
          }
          return;
     }

     /* BEAT < / > (list ch 5 notes 74/75): the RX3's Beat FX beat buttons. The
      * op must be PRESS: asEventCode gates 0x4490/0x4491 on (op & 0xf) == 0,
      * which is why map_jp21.c's TIME-knob combo sends them the same way. */
     if (ch == CH_FXA && (note == N_FX_BEATPREV || note == N_FX_BEATNEXT)) {
          if (on) {
               int key = (note == N_FX_BEATPREV) ? K_BEATPREV : K_BEATNEXT;
               long d = (note == N_FX_BEATPREV) ? -1 : 1;
               send_rx_key_fl(key, OP_PRESS, CH_GLOBAL, d, 0.0f, d);
               if (verbose)
                    klog("knobshim2: ch%d note%d -> 0x%04x (BEAT %s)\n",
                         ch, note, key, d < 0 ? "<" : ">");
          }
          return;
     }

     /* Everything else is a table row: a plain button, pressed and released. */
     for (int i = 0; i < note_map_n; i++) {
          if (note_map[i].rch != ch || note_map[i].note != note)
               continue;
          int key = note_map[i].key;
          if (key == 0) {
               flx4_log_only(ch, note);
               return;
          }
          int *p = &note_map[i].pressed;
          if (on && !*p) {
               *p = 1;
               send_rx_key(key, OP_PRESS, note_map[i].sch, 0);
               if (verbose)
                    klog("knobshim2: ch%d note%d -> 0x%04x press (sch%d)\n",
                         ch, note, key, note_map[i].sch);
          } else if (!on && *p) {
               *p = 0;
               send_rx_key(key, OP_RELEASE, note_map[i].sch, 0);
               if (verbose)
                    klog("knobshim2: ch%d note%d -> 0x%04x release (sch%d)\n",
                         ch, note, key, note_map[i].sch);
          }
          return;
     }
     if (verbose)
          klog("knobshim2: unmapped ch%d note%d on=%d\n", ch, note, on);
}

/* ---- CC handlers --------------------------------------------------------- */

/* Every bound CC with a 7-bit position, from abs_map[]. This mirrors
 * map_jp21.c's handle_cc_abs() -- a map owns its own dispatch -- and it has to
 * keep the same two properties, because abs_map[] is shared state: a repeat is
 * dropped, and `last` is what ctrl_abs_invalidate() resets so a panel reply
 * re-applies. */
static void flx4_cc_abs(int ch, int cc, int val)
{
     for (int i = 0; i < abs_map_n; i++) {
          if (abs_map[i].rch != ch || abs_map[i].cc != cc)
               continue;
          int v = cc_to_10bit(val);
          if (v == abs_map[i].last)
               return;
          abs_map[i].last = v;
          int key = abs_map[i].key;
          int sch = abs_map[i].sch;
          float fval = (float)v / 1023.0f;
          if (key == K_FADER && sch >= 1 && sch <= 2) {
               g_fader[sch] = v;
               g_fader_seen[sch] = 1;
          } else if (key == K_HPMIX) {
               g_cue_mix = fval;
          } else if (key == K_HPLEVEL) {
               g_cue_gain = fval;
          }
          send_rx_key_f(key, OP_VALUE, sch, v, fval);
          if (verbose)
               klog("knobshim2: ch%d cc%d -> 0x%04x val=%d f=%.3f (sch%d)\n",
                    ch, cc, key, v, (double)fval, sch);
          return;
     }
     if (verbose)
          klog("knobshim2: unmapped ch%d cc%d val=%d\n", ch, cc, val);
}

/* BROWSE (list ch 7 CC 64, +SHIFT CC 100): a relative encoder of the OTHER
 * convention -- the list says "Turn clockwise: Increases from 0x01, turn
 * counterclockwise: Decreases from 0x7F", so it is the two's-complement delta
 * map_jp21.c's browse knob also uses, not the jog's 0x40-centred one. */
static void flx4_browse(int val)
{
     if (val < 0 || val > 127)
          return;
     int delta = (val >= 64) ? (val - 128) : val;
     if (delta == 0)
          return;
     int key = K_SELECTOR;
     long n = (long)delta * (long)knob_scale;
     send_rx_key(key, OP_ROTATE, CH_GLOBAL, n < 0 ? -1 : 1);
     if (verbose)
          klog("knobshim2: browse knob v=%d delta=%ld -> 0x%04x rotate\n",
               val, n, key);
}

/* The jog: one message is one count since the last one, so nothing is
 * accumulated to find a position -- the count IS the delta. The first message
 * only establishes the dt baseline (map_jp21.c does the same with `ready`), so
 * the very first movement after a quiet period is spent on the clock; every
 * message after it is dispatched with rbp's own contract:
 *
 *   key 0x4305, op OP_ROTATE, f = revolutions/second, l = 16-bit position
 *
 * The sign follows the platter, and JOG_REV flips it. The magnitude is derived
 * from elapsed time, so it is only as good as JOG_PPR -- see the calibration
 * note there. */
static void flx4_jog(int ch, int val)
{
     int idx = (ch == CH_DECK2) ? 1 : 0;
     struct jog_ctrl *s = &jog_state[idx];
     int d = val - 0x40;
     if (d == 0)
          return;
     unsigned long long t = shim_now_ms();
     if (!s->started) {
          s->started = 1;
          s->last_ms = t;
          return;
     }
     if (jog_rev)
          d = -d;
     int dp = d * jog_scale;
     if (dp > 4096) dp = 4096;
     if (dp < -4096) dp = -4096;
     float dt = (float)(long long)(t - s->last_ms) / 1000.0f;
     s->last_ms = t;
     if (dt < 0.0005f)
          dt = 0.0005f;
     s->vpos = (unsigned int)(s->vpos + (unsigned int)dp) & 0xFFFFu;
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

/* The jog's end-of-motion edge: rbp keeps bending until it is told the wheel
 * stopped, so a speed-0 message goes out once the wheel has been still for
 * JOG_IDLE_MS. Runs from tick(). */
static void flx4_jog_idle(void)
{
     unsigned long long t = shim_now_ms();
     for (int i = 0; i < 2; i++) {
          struct jog_ctrl *s = &jog_state[i];
          if (!s->started || !s->moving)
               continue;
          if ((unsigned long long)(t - s->last_ms) <
              (unsigned long long)jog_idle_ms)
               continue;
          s->moving = 0;
          s->speed = 0.0f;
          send_rx_key_fl(K_JOG_ROT, OP_ROTATE, s->sch, 0, 0.0f, (long)s->vpos);
          if (jog_verbose)
               klog("knobshim2: jog ch%d idle -> speed 0\n", s->rch);
     }
}

/* The pitch fader: 14-bit, dispatched once per pair on the LSB (which the list
 * shows arriving right after the MSB), so one physical change is one send.
 * rbp's tempo slider is key 0x4109 op OP_VALUE with the position as a float in
 * [-1.0 .. +1.0], -1.0 = slower, +1.0 = faster; the list's "-" end (0x0000) and
 * "+" end (0x3FFF) line up with that, so nothing is inverted here. TEMPO_REV
 * flips it for a unit that turns out to be the other way up. */
static void flx4_pitch(int ch, int cc, int val)
{
     int idx = (ch == CH_DECK2) ? 1 : 0;
     struct pitch_ctrl *s = &pitch_state[idx];
     if (cc == CC_TEMPO_MSB) {
          s->have_hi = 1;
          s->pos = (s->pos & 0x7f) | (val << 7);
          return;
     }
     if (cc != CC_TEMPO_LSB)
          return;
     s->pos = (s->pos & 0x3f80) | (val & 0x7f);
     if (!s->have_hi)
          return;                    /* no MSB yet: nothing to combine */
     int pos = s->pos;
     if (pos < 0) pos = 0;
     if (pos > 0x3FFF) pos = 0x3FFF;
     float norm = ((float)pos - 0x2000) / 8192.0f;
     if (norm > 1.0f) norm = 1.0f;
     if (norm < -1.0f) norm = -1.0f;
     if (tempo_rev)
          norm = -norm;
     int v10 = (int)((norm + 1.0f) * 511.5f);
     if (v10 < 0) v10 = 0;
     if (v10 > 1023) v10 = 1023;
     send_rx_key_fl(K_TEMPO_SLIDER, OP_VALUE, s->sch, (long)v10, norm, (long)pos);
     if (tempo_verbose)
          klog("knobshim2: pitch ch%d (deck %d) pos=0x%04x -> norm=%.3f v10=%d\n",
               ch, s->sch, pos, (double)norm, v10);
}

/* ---- the tables ---------------------------------------------------------- */

static void flx4_build(void)
{
     /* ADDITIVE, deliberately: this fills the tables and never empties them.
      * A keyboard can be live on the other selection at the same time, and the
      * front end clears the tables once before any build -- see the note on
      * ctrl_bindings_reset() in ctrl_map.h. A reset here would take map_kbd.c's
      * bindings with it, and its build puts nothing back. */

     /* Calibration, read by value so an empty export from start-rb.sh's
      * SHIM_VARS loop means "use the default" rather than 0. */
     knob_scale = env_num("KNOB_SCALE", 1);
     if (knob_scale < 1) knob_scale = 1;
     jog_scale = env_num("JOG_SCALE", 1);
     if (jog_scale < 1) jog_scale = 1;
     jog_ppr = env_num("JOG_PPR", 720);
     if (jog_ppr < 1) jog_ppr = 1;
     jog_rev = env_on("JOG_REV", 0);
     jog_idle_ms = env_num("JOG_IDLE_MS", 120);
     if (jog_idle_ms < 10) jog_idle_ms = 10;
     jog_verbose = env_on("JOG_VERBOSE", 0);
     tempo_verbose = env_on("TEMPO_VERBOSE", 0);
     tempo_rev = env_on("TEMPO_REV", 0);

     /* ---- decks (list ch 1/2 -> rch 0/1 -> rbp players 1/2) ----
      * Every control on these two channels is a button; the continuous ones
      * (jog, tempo) are handled in flx4_event() instead. */
     for (int d = 0; d < 2; d++) {
          int rch = CH_DECK1 + d;
          int sch = 1 + d;
          add_note(rch, N_PLAY, K_PLAY, sch);
          add_note(rch, N_CUE, K_CUE, sch);
          add_note(rch, N_JOG_TOUCH, K_JOG_TOUCH, sch);
          add_note(rch, N_IN, K_LOOPIN, sch);       /* manual loop in */
          add_note(rch, N_OUT, K_LOOPOUT, sch);     /* manual loop out */
          /* 4 BEAT / EXIT -> rbp's RELOOP/EXIT: it exits a running loop and
           * re-enters a stored one. Starting a fresh 4-beat loop needs rbp's
           * AUTO LOOP pad mode, which this map does not enter -- see the pad
           * block below. TODO: unverified. */
          add_note(rch, N_4BEAT, K_RELOOP, sch);
          /* The pad-mode buttons select rbp's pad mode, which is what makes the
           * pad keycodes below mean HOT CUE and BEAT JUMP. */
          add_note(rch, N_MODE_HOT, K_HOTCUE, sch);
          add_note(rch, N_MODE_JUMP, K_BEATJUMP, sch);
          /* Known, deliberately not routed (key 0): the two pad modes rbp has
           * no equivalent for (PAD FX 1/2 and SAMPLER are rekordbox features),
           * the SHIFT button, and CUE/LOOP CALL > -- rbp's keycode for the call
           * buttons is not identified. See docs/15's open questions. */
          add_note(rch, N_SHIFT, 0, sch);
          add_note(rch, N_MODE_PFX1, 0, sch);
          add_note(rch, N_MODE_SMPL, 0, sch);
          /* ...with one exception. CALL < is BACK, and it is the only spare
           * button on this unit: the FLX4 has no BACK and no SOURCE button at
           * all, and without a BACK the browse screen is a one-way door --
           * every keycode that leaves it is one this surface does not have.
           * Nothing is taken from the button by this: the call buttons' real
           * rbp keycodes are unidentified, so it was dead. "<" reads as "back
           * one level", which is what it now does. */
          add_note(rch, N_CALL_PREV, K_BACK, CH_GLOBAL);
          add_note(rch, N_CALL_NEXT, 0, sch);

          /* The deck's own mixer strip: same four knobs and one fader rbp's
           * channel has, on the deck's channel rather than a separate one. */
          add_abs(rch, CC_TRIM, K_TRIM, sch);
          add_abs(rch, CC_EQH, K_EQH, sch);
          add_abs(rch, CC_EQM, K_EQM, sch);
          add_abs(rch, CC_EQL, K_EQL, sch);
          add_abs(rch, CC_FADER, K_FADER, sch);
     }

     /* ---- pads (list ch 8/10 -> rch 7/9) ----
      * A pad's note encodes BOTH its number and the unit's current pad MODE:
      * base + pad, where base is 0 HOT CUE, 16 PAD FX 1, 32 BEAT JUMP,
      * 48 SAMPLER, 64 KEYBOARD, 80 PAD FX 2, 96 BEAT LOOP, 112 KEY SHIFT, and
      * pad is 0..7. rbp's pad keycodes K_PAD1..8 mean whatever rbp's own pad
      * mode says they mean, and that mode is set by the mode buttons above.
      *
      * So only the two ranges rbp has a mode for are bound -- HOT CUE (base 0)
      * and BEAT JUMP (base 32), which are the two modes bound above. The other
      * six are deliberately unbound: PAD FX 1/2 and KEYBOARD have no rbp mode,
      * and BEAT LOOP (base 96) and KEY SHIFT are reached by a SHIFT + mode
      * button combination whose correspondence the list does not state (it
      * gives four extra modes and four shift notes, but never pairs them), so
      * binding them would be a guess about which button puts rbp in which
      * mode. An unbound pad produces nothing and logs as unmapped.
      * TODO: unverified -- the whole base+pad encoding, and the SHIFT pairing.
      *
      * The pads' +SHIFT layer is a second channel per deck (list ch 9/11 ->
      * rch 8/10) carrying the same eight ranges. It is unbound for the same
      * reason, so shift-layer pads produce nothing. */
     for (int d = 0; d < 2; d++) {
          int rch = CH_PADS1 + 2 * d;
          int sch = 1 + d;
          for (int p = 0; p < 8; p++) {
               add_note(rch, 0 + p, K_PAD1 + p, sch);
               add_note(rch, 32 + p, K_PAD1 + p, sch);
          }
     }

     /* ---- mixer / global (list ch 7 -> rch 6) ----
      * The knobs are on this channel, not the deck channels: the unit's CFX
      * knobs and the crossfader are mixer-section controls even though they
      * drive one deck each. */
     add_abs(CH_MIX, CC_CFX1, K_COLOR, 1);        /* Sound Color FX, deck 1 */
     add_abs(CH_MIX, CC_CFX2, K_COLOR, 2);        /* ditto, deck 2 */
     add_abs(CH_MIX, CC_XFADER, K_XFADER, CH_GLOBAL);
     add_abs(CH_MIX, CC_HP_MIX, K_HPMIX, CH_GLOBAL);      /* -> g_cue_mix */
     add_abs(CH_MIX, CC_HP_LEVEL, K_HPLEVEL, CH_GLOBAL);  /* -> g_cue_gain */
     add_note(CH_MIX, N_BROWSE_PUSH, K_SELECTOR, CH_GLOBAL);
     /* SHIFT + browse push -> rbp's SOURCE screen. This is the row the whole
      * port needed: **the FLX4 has no SOURCE button and no BROWSE button**, and
      * rbp has no on-screen SOURCE target on the deck screen, so without it
      * there is no way to reach the USB stick from this surface at all -- the
      * browse knob can navigate a list but cannot open one. The pointer path
      * (fbshim.so) is what reaches rbp's other on-screen controls for them; it
      * cannot reach a control that is not drawn.
      *
      * PROVENANCE: note 66 is the +SHIFT variant of the browse push in Pioneer's
      * published list, and it is bound on that reading -- the same standing the
      * rest of this file's unmeasured rows have (see the PROVENANCE block at the
      * top). Two separate things have to be true for it to work, and they fail
      * differently: the binding (this table -> rbp) and the note number (the unit
      * -> this table). An injection of ch6 note66 through the shim's own
      * sequencer port tests the first on the unit; a press on the real button
      * with MIDI_DUMP on measures the second, and a wrong number there shows up
      * as an unmapped note in the KNOB_VERBOSE log rather than as a mystery. */
     add_note(CH_MIX, N_BROWSE_PUSH_SHIFT, K_SOURCE, CH_GLOBAL);
     /* LOAD streams on the global channel, as it does in map_jp21.c: rbp's
      * browse/Load keys are global, and the deck is in the send channel. */
     add_note(CH_MIX, N_LOAD1, K_LOAD, 1);
     add_note(CH_MIX, N_LOAD2, K_LOAD, 2);
     /* Known, deliberately not routed (key 0): rekordbox's SMART CFX and SMART
      * FADER and the Android MONO/STEREO output switch have no rbp equivalent
      * (MONO is the mono sum of both channels, not the L=cue/R=master split
      * rbp's stereo type means, so it is not me_set_stereo()). */
     add_note(CH_MIX, N_SMART_CFX, 0, CH_GLOBAL);
     add_note(CH_MIX, N_SMART_FADER, 0, CH_GLOBAL);
     add_note(CH_MIX, N_MONO_STEREO, 0, CH_GLOBAL);
     /* And the two controls this map deliberately leaves alone rather than
      * routes: MASTER LEVEL (CC 8) is the unit's own output volume, which sits
      * after the USB audio it feeds -- driving rbp's master level or
      * audioshim's g_master_gain from it would attenuate the master twice --
      * and MIC LEVEL (CC 5) has no reader, because rbp's mic input is not part
      * of this port. flx4_startup() pins rbp's master level at unity instead. */

     /* ---- Beat FX (list ch 5/6 -> rch 4/5) ---- */
     add_note(CH_FXA, N_FX_ONOFF, K_BFX, CH_GLOBAL);
     add_note(CH_FXB, N_FX_ONOFF, K_BFX, CH_GLOBAL);
     /* The LEVEL/DEPTH knob is a 14-bit pair; only the MSB is bound (one row =
      * one 128-step knob). The vendor table disagrees with itself about which
      * channel this one row uses -- its Channel column says 6 (list ch 6), its
      * own MIDI-IN status byte says B4 (list ch 5) -- so both candidates are
      * bound to the same key and the knob works either way. The dump settles
      * which one the unit actually sends. TODO: unverified. */
     add_abs(CH_FXA, CC_FX_DEPTH, K_DEPTH, CH_GLOBAL);
     add_abs(CH_FXB, CC_FX_DEPTH, K_DEPTH, CH_GLOBAL);
}

/* Everything that has to reach rbp once it exists. Runs after rbp's KeyManager
 * is up and before the first event is dispatched, which is why anything sent
 * here is not lost. */
static void flx4_startup(void)
{
     /* Route each mixer channel to the player behind it. The RX3 does this from
      * its physical DECK/LINE switches and no control surface has any, so the
      * engine's own default (channel 2 on player 0) is what playback would
      * otherwise inherit. The same two words, and the same reason, as
      * map_jp21.c's startup(); named in rbp_abi.h so this file carries no
      * literal rbp address. */
     *(volatile uint32_t *)ADDR_MIXER_ROUTE_PLAYER0 = MIXER_ROUTE_INPUT0;
     *(volatile uint32_t *)ADDR_MIXER_ROUTE_PLAYER1 = MIXER_ROUTE_INPUT1;
     klog("knobshim2: routed Mixer Ch1 -> Deck1, Ch2 -> Deck2\n");

     /* Sound Color FX needs a *type* selected behind the knob or the CFX knobs
      * move a control with no effect behind them. The FLX4 has no CFX type
      * buttons -- one knob per channel -- so the map chooses at startup, as the
      * previous map does, and centres both knobs. Filter is the choice because
      * it is the one effect a single knob can express without a parameter.
      * TODO: unverified -- with no way to change it from the unit, this is a
      * fixed choice, not something the operator can correct. */
     send_rx_key(K_FILTER, OP_PRESS, 1, 0);
     send_rx_key(K_FILTER, OP_RELEASE, 1, 0);
     send_rx_key(K_FILTER, OP_PRESS, 2, 0);
     send_rx_key(K_FILTER, OP_RELEASE, 2, 0);
     send_rx_key_f(K_COLOR, OP_VALUE, 1, 512, 0.5f);
     send_rx_key_f(K_COLOR, OP_VALUE, 2, 512, 0.5f);
     klog("knobshim2: Sound Color FX initialized to Filter on Ch1 & Ch2\n");

     /* rbp's internal master level to unity, for the reason in flx4_build()'s
      * note on MASTER LEVEL: the unit's own knob is the master volume, so rbp's
      * must sit at a fixed reference or the master is attenuated twice. */
     send_rx_key_f(K_MASTERLVL, OP_VALUE, CH_GLOBAL, 1023, 1.0f);
     klog("knobshim2: master level set to unity\n");

     /* Deliberately NOT here: me_set_master_cue() / me_set_stereo(). rbp_vu.c's
      * vu_thread() waits for rbp's mixer and asserts both on every target --
      * meters or not -- and a second writer for one piece of engine state is
      * the thing to avoid. The unit's own MASTER CUE button (in the table
      * above) is not a conflict with that assertion: it reads the engine state
      * back with me_get_master_cue() and toggles it, so it always agrees with
      * whatever startup left behind. */

     /* Also deliberately not here: forcing the Beat FX target channel to
      * MASTER, which map_jp21.c does because its assign knob is a position
      * encoder with no state of its own. This unit's FX CH SELECT is a lever,
      * and rbp gets its position from the lever's notes -- see the FX CH SELECT
      * handler. Until the lever is moved, rbp keeps whatever target it built
      * itself with. TODO: unverified -- what rbp's default target is, and
      * whether the unit reports the lever's resting position at connect. */
}

/* The one thing this map has that is about time: the jog's idle edge. */
static void flx4_tick(void)
{
     flx4_jog_idle();
}

static void flx4_event(const struct snd_seq_event *ev)
{
     switch (ev->type) {
     case SNDRV_SEQ_EVENT_CONTROLLER: {
          int ch = ev->data.control.channel;
          int cc = ev->data.control.param;
          int val = ev->data.control.value;
          if ((ch == CH_DECK1 || ch == CH_DECK2) &&
              (cc == CC_TEMPO_MSB || cc == CC_TEMPO_LSB))
               flx4_pitch(ch, cc, val);
          else if ((ch == CH_DECK1 || ch == CH_DECK2) &&
                   (cc == CC_JOG_WHEEL || cc == CC_JOG_VINYL ||
                    cc == CC_JOG_NOVINYL || cc == CC_JOG_SHIFT))
               flx4_jog(ch, val);
          else if (ch == CH_MIX && (cc == CC_BROWSE || cc == CC_BROWSE_SHIFT))
               flx4_browse(val);
          else
               flx4_cc_abs(ch, cc, val);
          break;
     }
     case SNDRV_SEQ_EVENT_NOTEON:
          flx4_note(ev->data.note.channel, ev->data.note.note,
                    ev->data.note.velocity > 0);
          break;
     case SNDRV_SEQ_EVENT_NOTEOFF:
          flx4_note(ev->data.note.channel, ev->data.note.note, 0);
          break;
     default:
          break;
     }
}

const struct ctrl_map map_flx4 = {
     "flx4",
     flx4_build,
     flx4_startup,
     flx4_event,
     flx4_tick,
     NULL,   /* devices(): a MIDI surface, so no evdev reader is started */
     NULL,   /* input(): ditto -- this map never sees an evdev triple */
};
