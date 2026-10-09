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
 *   - the unit's extra pad modes: rbp has four and this section has more, so
 *     KEYBOARD, PAD FX 2, BEAT LOOP and KEY SHIFT reach nothing at all (see the
 *     pad block in flx4_build());
 *   - CUE/LOOP CALL: rbp's keycode for those buttons is not identified. They are
 *     in the table as log-only, and in docs/15's open questions.
 *
 * Everything about rbp lives in rbp_abi.h/rbp_bridge.h; the only place this file
 * touches a raw address is flx4_startup()'s two mixer-route words, which are
 * named in rbp_abi.h for exactly that reason.
 *
 * ============================== the LEDs ==================================
 *
 * The unit's illumination messages are all MIDI-OUT (the list's last section).
 * Only three kinds are in that list, and NONE of them is a transport or pad LED:
 *
 *   LOADED (track-load illumination)  note 0 / 1, list ch 16 (0-based 15)
 *   VINYL MODE (app-set, footnote *2) note 23,     list ch 1 / 2 (0-based 0/1)
 *   CH LEVEL METER                    CC 2,        list ch 1 / 2 (0-based 0/1)
 *     ... a value ramp, not a bitmask: 0x26-0x40 green1, 0x41-0x56 green2,
 *     0x57-0x64 orange1, 0x65-0x76 orange2, 0x77-0x7F red. Note that the list's
 *     type column calls this row a NOTE while its own status byte is B0, a
 *     control change; the status byte is what the wire carries.
 *
 * So the note numbers for PLAY, CUE, BEAT SYNC, the channel-strip CUE, the pads
 * and the loops are NOT published anywhere -- and they are not guessable either.
 * The hypothesis worth testing first is Pioneer's usual one, that a button's LED
 * is the same note as its input (which was true of the previous target, whose
 * numbers are in map_jp21.c), and the input notes ARE known from the S5.1
 * capture: PLAY 11, CUE 12, CH CUE 84, BEAT SYNC 88, MASTER CUE 99, pad 1 = note
 * 0. But "worth testing first" is not "known", and the difference is not
 * cosmetic here: the SC Live 4's numbers, driven at this panel, land on the Beat
 * FX section, where notes 16/17 are the FX CH SELECT legs. A wrong guess is not
 * a dark LED, it is a phantom control.
 *
 * So every note below is -1 -- "this surface has no such LED as far as we know"
 * -- and the probe in docs/15's LED section is what fills them in. -1 sends
 * NOTHING (rbp_led.c), which is why the bridge can be switched on
 * (RB_LED_DISABLE=0) before the measurement: the machinery runs, the panel stays
 * dark, and each row becomes a one-line edit once its note has been seen to
 * light. Do not fill a row from the vendor list or from the JP21 table.
 *
 * Which rows the unit will ultimately have is a separate question from which are
 * measured, and two of them are already settled by what the panel physically
 * is: KEY LOCK and SLIP are absent (no button on the unit, so no LED -- see the
 * "cannot reach" block above), and so are the four Sound Color FX rows (the unit
 * has CFX knobs, not CFX buttons). Those four stay -1 permanently rather than
 * pending. The loops, the Beat FX ON/OFF LED and VINYL MODE are the opposite
 * case: the unit does have those controls, so their rows are pending a
 * measurement rather than absent. VINYL MODE additionally has a documented note
 * (23, above) but no button, and the list footnotes it as application-set -- a
 * display-only decision to take deliberately rather than as a side effect.
 */
#define _GNU_SOURCE
#include <stdint.h>         /* uint32_t, for the two mixer-route words */
#include <sound/asequencer.h>

#include "shimutil.h"
#include "shmstate.h"       /* g_cue_mix / g_cue_gain, which the knobs feed */
#include "rbp_abi.h"
#include "rbp_bridge.h"
#include "rbp_vu.h"         /* g_fader / g_fader_seen: the meter scaling input */
#include "pitch_state.h"    /* g_pitch_norm: what the edge drawers' nudge moves from */
#include "ctrl_map.h"

/* ---- receive channels, 0-based (the list's channel minus one) -------------
 * See the PROVENANCE note above: this is the conversion a dump has to confirm. */
#define CH_DECK1  0   /* list ch 1  -- deck 1 buttons, jog, tempo, its mixer strip */
#define CH_DECK2  1   /* list ch 2  -- deck 2, the same set */
#define CH_FXA    4   /* list ch 5  -- Beat FX section, and FX CH SELECT leg A */
#define CH_FXB    5   /* list ch 6  -- FX CH SELECT leg B (and the CH2 FX leg) */
#define CH_MIX    6   /* list ch 7  -- mixer knobs, browse, LOAD, MASTER CUE */
#define CH_PADS1  7   /* list ch 8  -- deck 1 pads, current mode */
#define CH_PADS1S 8   /* list ch 9  -- deck 1 pads, +SHIFT (HOT CUE only, see dispatch) */
#define CH_PADS2  9   /* list ch 10 -- deck 2 pads, current mode */
#define CH_PADS2S 10  /* list ch 11 -- deck 2 pads, +SHIFT (HOT CUE only) */

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
/* The pad-mode buttons, and the four rbp modes they select. **The binding is
 * POSITIONAL, and this unit's own printed labels are deliberately ignored.**
 *
 * rbp's UI is an XDJ-RX3's, and the RX3's four pad-mode buttons run HOT CUE,
 * BEAT LOOP, SLIP LOOP, BEAT JUMP -- which is rbp's modes 0, 1, 2, 3 in that
 * order, and is the order map_jp21.c binds its own four in for the same reason.
 * This unit's row is HOT CUE, PAD FX 1, BEAT JUMP, SAMPLER, so each button
 * selects the mode that occupies ITS POSITION on the RX3: PAD FX 1 stands in for
 * BEAT LOOP, and the 3rd and 4th buttons give SLIP BEAT LOOP and BEAT JUMP --
 * the opposite way round from what those two labels suggest.
 *
 * The operator asked for exactly this on 2026-10-01, in their own words:
 * "ignore the names on the FLX4, it should just map to the way the RX3 behaves by
 * position for muscle memory". Muscle memory is the whole argument: the hand goes
 * to the third button for a slip loop because that is where the RX3 keeps one,
 * and a binding that reads well in a table but sits on the wrong button defeats
 * the point of driving a familiar UI.
 *
 * The four SHIFT variants of these buttons (KEYBOARD, PAD FX 2, BEAT LOOP and
 * KEY SHIFT, on notes 105/107/109/111) reach nothing: rbp has no such modes and
 * no keycodes for them. rbp has no PAD FX and no sampler pads at all -- see the
 * pad block in flx4_build() and docs/15-flx4-midi.md. */
#define N_MODE_HOT   27   /* 1st -> HOT CUE        (RX3 position 1) (+SHIFT 105) */
#define N_MODE_PFX1  30   /* 2nd -> AUTO BEAT LOOP (RX3 position 2) (+SHIFT 107) */
#define N_MODE_JUMP  32   /* 3rd -> SLIP BEAT LOOP (RX3 position 3) (+SHIFT 109) */
#define N_MODE_SMPL  34   /* 4th -> BEAT JUMP      (RX3 position 4) (+SHIFT 111) */

/* ---- mixer / browse (all on list ch 7 unless noted) ---------------------- */
#define N_MASTER_CUE  99  /* MASTER CUE         (+SHIFT 120) */
#define N_BROWSE_PUSH 65  /* browse knob push   (+SHIFT 66 = SOURCE, see build) */
#define N_BROWSE_PUSH_SHIFT 66  /* SHIFT + browse knob push -> rbp's SOURCE screen */
#define N_LOAD1       70  /* LOAD deck 1        (+SHIFT 104) */
#define N_LOAD2       71  /* LOAD deck 2        (+SHIFT 122) */
/* The two SHIFT + LOAD notes. PROVENANCE: the note NUMBERS 104/122 started out
 * published-only; the operator's own presses settled them on 2026-09-30, and
 * both ways at once -- SHIFT + LOAD 1 and SHIFT + LOAD 2 each brought up their
 * screen on the glass, AND RB_MIDI_DUMP caught both edges on the wire as
 * `NOTEON ch=6 note=104 vel=127` and `note=122 vel=127` in a file with no
 * vel=100 event in it at all (seqinject2 sends 100, the panel 127). The screen
 * is the corroboration rather than the proof: a wrong note number could not have
 * fired anything, because each of these two keycodes is bound by exactly one row
 * in this map, both on CH_MIX.
 *
 * Note 104 means two different things and the CHANNEL is the whole of the
 * separation: on the deck channels it is SHIFT + channel CUE (N_CH_CUE above,
 * 84 -> 104), and on the mixer's list channel it is SHIFT + LOAD 1. Neither row
 * may be moved to the other's channel. 122 is used nowhere else in this map. */
#define N_LOAD1_SHIFT 104 /* SHIFT + LOAD deck 1 -> rbp's TRACK FILTER panel */
#define N_LOAD2_SHIFT 122 /* SHIFT + LOAD deck 2 -> rbp's SHORTCUT view */
#define N_SMART_CFX   0   /* SMART CFX          (+SHIFT 8)  -- no rbp equivalent */
#define N_SMART_FADER 1   /* SMART FADER        (+SHIFT 9)  -- no rbp equivalent */
#define N_MONO_STEREO 109 /* Android MONO/STEREO            -- no rbp equivalent */

/* ---- Beat FX (list ch 5, with a second leg on ch 6) --------------------- */
#define N_FXCH_1     16   /* FX CH SELECT leg A (ch 5) */
#define N_FXCH_2     17   /* FX CH SELECT leg B (ch 6) */
#define N_FX_SELECT  99   /* FX SELECT         (+SHIFT 100, see below) */
#define N_FX_SELECT_SHIFT 100  /* SHIFT + FX SELECT: the same step backwards */
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
/* MASTER LEVEL, the unit's own output volume (list ch 7 -> CH_MIX). 14-bit like
 * every other continuous control here: MSB CC 8, LSB CC 40 (= 8 + 32). MEASURED
 * on the unit 2026-09-27, after the dump showed cc8 sweeping 40,41,39,37,32,28,
 * 24,21,15,11,7,2,0 on ch 6 with a companion cc40 whose values looked random --
 * which they are not: combining them as cc8*128+cc40 traces a smooth monotonic
 * ramp down to 0 and back up (412 non-zero steps, 30 sign flips), so cc40 is the
 * low 7 bits. Two things this is NOT, both worth stating because the raw dump
 * suggests them: it is not the FX LEVEL/DEPTH knob (that is CC 2, and the dump
 * shows it separately on ch 4), and cc40 is not idle chatter. Neither the map's
 * table nor this comment previously carried the LSB.
 *
 * Deliberately NOT routed to rbp's master level or g_master_gain: the knob is
 * downstream of the USB audio the FLX4 is fed, so driving either from it would
 * attenuate the master twice. It feeds g_mirror_gain -- the HDMI mirror only --
 * which is how the HDMI copy comes to track the room. See flx4_mastervol(). */
#define CC_MASTER_MSB 8
#define CC_MASTER_LSB 40
#define CC_FX_DEPTH   2    /* Beat FX LEVEL/DEPTH */
#define CC_BROWSE     64   /* relative, 0x01/0x7F -- NOT the jog's convention */
#define CC_BROWSE_SHIFT 100

/* Where the knob's travel reaches unity, as a fraction of the raw 14-bit range:
 * 0.6, which is ONE O'CLOCK on this knob. Measured from the knob's own stops, not
 * assumed -- see flx4_mastervol_gain(). */
#define MIRROR_GAIN_MID_DEFAULT 0.6

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
 * SELECT is a button, so the map keeps the position and steps it -- forward on
 * the button, backward on SHIFT+FX SELECT, the two directions the RX3's
 * FX-select rotary has.
 *
 * It is seeded from what rbp is already playing, so the first press steps on
 * from the current effect instead of jumping to position 0 -- a position that
 * holds a different effect, and on a live deck an audible one. The read is
 * rbp_bridge.h's rbp_beatfx_type() and the table is rbp_abi.h's
 * bfx_type_to_pos[], the same table map_jp21.c's rotary seeds from. Keeping
 * rbp's addresses out of this map is what makes the seeding a stub the host
 * test can drive (see test_flx4.c) rather than a call it would segfault on. */
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
 * list ch 5 note 16, leg B is list ch 6 note 17. CH1 = leg A on, CH2 = B on,
 * CH1&CH2 = both. (The other two notes of that group, ch 5 note 17 and ch 6
 * note 16, are OFF in every position the list gives.)
 *
 * The three positions do NOT map one-to-one onto what rbp can be told: rbp's
 * Beat FX has ONE target channel, so CH1 and CH2 are its two players and CH1&CH2
 * has no value of its own. That position is sent as MASTER -- the output both
 * decks reach, which is the value the operator asked for and the same one
 * map_jp21.c sends for the previous unit's Main position.
 *
 * Because of that, the state kept here is the LEVER's position and not the last
 * keycode sent: stepping from CH1&CH2 to one of the single-deck positions is a
 * real move even when that deck is what rbp was told about last.
 *
 * All three values are measured on rbp's own screen rather than inferred: its
 * Beat FX panel reads **1**, **2** and **MASTER** for them (2026-09-28, docs/13
 * S5.6). rbp's `EnBeatEffectSelectChannel` enum is in rbp_abi.h. */
static int fxch_leg1, fxch_leg2;
static int fxch_pos = -1;       /* 0 = CH1, 1 = CH2, 2 = CH1&CH2; -1 = unread */

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
          int pos;
          if (ch == CH_FXA && note == N_FXCH_1)
               fxch_leg1 = on;
          else if (ch == CH_FXB && note == N_FXCH_2)
               fxch_leg2 = on;
          else
               return;                 /* ch5 note17 / ch6 note16: always OFF */

          if (fxch_leg1 && fxch_leg2)
               pos = 2;                /* CH1&CH2 */
          else if (fxch_leg1)
               pos = 0;                /* CH1 */
          else if (fxch_leg2)
               pos = 1;                /* CH2 */
          else
               return;                 /* mid-slide: both legs momentarily off */

          if (pos == fxch_pos)
               return;                 /* the whole group is re-sent on every
                                        * move; only a change is a move */
          fxch_pos = pos;
          /* CH1&CH2 -> MASTER, the two single positions -> their own player.
           * TODO: unverified -- nothing on the unit says which position the
           * lever is in at connect, so this acts on the first message that
           * names a position, whether that is a move or the device's own
           * state. */
          int want = (pos == 2) ? BFX_CH_MASTER : pos;
          send_rx_key(K_BFXCH, OP_VALUE, CH_GLOBAL, want);
          klog("knobshim2: FX CH SELECT -> Beat FX target %s\n",
               pos == 2 ? "MASTER (5)" :
               (pos == 0 ? "deck 1 (0)" : "deck 2 (1)"));
          return;
     }

     /* FX SELECT (list ch 5 note 99) and SHIFT+FX SELECT (note 100): step rbp's
      * 14-position effect switch, forward and back -- the RX3's FX-select
      * rotary's two directions, which a button needs a modifier for. The +SHIFT
      * note is a distinct note on the same channel rather than a state this map
      * tracks (see the deck-button note at the top), so the backward step needs
      * no SHIFT bookkeeping at all. */
     if (ch == CH_FXA && (note == N_FX_SELECT || note == N_FX_SELECT_SHIFT)) {
          int step = (note == N_FX_SELECT_SHIFT) ? -1 : 1;
          if (on) {
               if (fx_pos < 0) {
                    int t = rbp_beatfx_type();
                    /* If rbp cannot be asked, or answers a type no position
                     * reaches, the cursor starts at 0 -- what every first press
                     * did before the seeding existed, and the honest answer to
                     * "nobody said". */
                    fx_pos = (t > 0 && t <= BFX_TYPE_POSITIONS &&
                              bfx_type_to_pos[t] >= 0)
                             ? bfx_type_to_pos[t] : 0;
               }
               fx_pos = (fx_pos + step + BFX_TYPE_POSITIONS)
                        % BFX_TYPE_POSITIONS;
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

     /* +SHIFT + a HOT CUE pad (list ch 9/11 -> rch 8/10): delete that pad's
      * cue. These rows are NOT in note_map[] and cannot be, because the gesture
      * is not a keycode: rbp has none for it -- its own 0x4124 deletes a fixed
      * slot nothing ever fills -- and the per-pad delete it does have is reached
      * by no key this shim can send, so hotcue_delete() calls it directly
      * (rbp_bridge.c carries both negatives and the derivation). This is
      * therefore the one shape a table row cannot express, and it is handled the
      * way this map handles the FX CH SELECT lever: a named case above the loop.
      *
      * The layer's note numbers are MEASURED, not published: the operator's own
      * press with RB_MIDI_DUMP on (2026-10-01) put the shifted pad on the wire as
      * `NOTEON ch=8 note=0 vel=127` inside a SHIFT-held window (note 63 down at
      * 610.514, released 611.118), so the shifted note REPLACES its base note --
      * there is no ch 7 note 0 beside it. vel 127 is the panel; seqinject2 sends
      * at 100, which is how the two are told apart in a dump.
      *
      * Only the HOT CUE base (notes 0..7) is bound, and that restriction is the
      * operator's own explicit instruction (2026-10-01), not an oversight: the
      * other three bases the unshifted layer carries (16 PAD FX 1, 32 BEAT JUMP,
      * 48 SAMPLER) mean nothing as a delete, so a shifted press on one of them
      * logs as unmapped rather than guessing an action. */
     if ((ch == CH_PADS1S || ch == CH_PADS2S) && note >= 0 && note < 8) {
          if (on)
               hotcue_delete(ch == CH_PADS1S ? 0 : 1, note + 1);
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
               /* The rows rbp needs a second edge for (rbp_abi.h's OP_REPEAT --
                * today only K_TRACKFILTER). It goes out on the press rather than
                * on the release because rbp's handler reads the record's *state*,
                * not which edge carried it: state 2 -> 3 opens the panel, and a
                * release after that finds the state already cleared and does
                * nothing. Sending it here also means a tap and a hold behave
                * alike, so a note-off that never arrives cannot leave the key
                * stuck half-pressed. */
               if (note_map[i].repeat)
                    send_rx_key(key, OP_REPEAT, note_map[i].sch, 0);
               if (verbose)
                    klog("knobshim2: ch%d note%d -> 0x%04x press%s (sch%d)\n",
                         ch, note, key, note_map[i].repeat ? "+repeat" : "",
                         note_map[i].sch);
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
     /* PUBLISH WHAT RBP IS BEING TOLD, and after TEMPO_REV rather than before: the
      * drawer's nudge is a move FROM the position rbp has, so the published value has
      * to be the one in the message. pitch_state.h carries the arrangement that lets
      * fbshim.so -- which loads first and cannot call in here -- read it. */
     pitch_state_set(idx, norm);
     send_rx_key_fl(K_TEMPO_SLIDER, OP_VALUE, s->sch, (long)v10, norm, (long)pos);
     if (tempo_verbose)
          klog("knobshim2: pitch ch%d (deck %d) pos=0x%04x -> norm=%.3f v10=%d\n",
               ch, s->sch, pos, (double)norm, v10);
}

/* ---- MASTER LEVEL --------------------------------------------------------
 * The unit's own output volume, 14-bit on CH_MIX: CC 8 (MSB) + CC 40 (LSB).
 * Shaped like flx4_pitch() above and for the same reason -- one physical change
 * arrives as a pair of messages, so dispatching on the LSB is what makes the
 * value complete rather than half-updated, and `have_hi` is what stops an LSB
 * that has no MSB yet from producing a number out of nothing. */
static int mastervol_have_hi = 0;
static int mastervol_pos = 0;
static double mastervol_mid = MIRROR_GAIN_MID_DEFAULT;

/* Position -> mirror level. Unity is reached at ONE O'CLOCK, and the travel above
 * it is flat.
 *
 * The travel IS the raw range, and that is a measurement rather than a
 * convenience. The unit's own MIDI dump (RB_MIDI_DUMP) was read back on
 * 2026-09-27 with a hand on the knob: the composed MSB/LSB positions span raw 0 to
 * raw 16383, and both ends are among the most-visited positions (14 samples at
 * 16383, 11 at 0), which is what a mechanical stop looks like -- the pot pins at
 * the ends of its electrical range rather than stopping short of them. So
 * `pos / 16383` is a fraction of ROTATION, and the earlier reading of that dump
 * which put the bottom stop at raw 2228 was simply a sweep that never reached the
 * bottom (the same document already said full-down had not been recorded).
 *
 * 12 o'clock is therefore raw 0.5 -- the electrical midpoint, and where the hand
 * rests: the same dump's most-visited positions cluster on 0.479..0.532, centred
 * on 0.50, which is the knob's parking spot. 1 o'clock is one hour past it, i.e.
 * 30 degrees of a 270-300 degree sweep, or 0.60-0.61 of the rotation. 0.6 is the
 * round number, and the difference between the two is 0.15 dB at 12 o'clock.
 *
 * Why not 0.5, the middle: that was the first fix and it worked -- see below --
 * but the operator's follow-up the same day (2026-09-27) was that the top of the
 * useful travel should be 1 o'clock rather than 12. So unity moved up with it,
 * and 12 o'clock is no longer full level: it reads 0.833, about -1.6 dB. Nothing
 * anywhere in the law can exceed 1.0, so this change only ever LOWERS what a given
 * position gives -- it moves the knee, it does not add level. The one mercy of the
 * travel being the whole range is that the bottom is unaffected by any of it: full
 * down is still silence, as it was under every version of this law.
 *
 * Why not the obvious `pos / 16383`: that puts unity at the END of the travel,
 * which is not where anyone leaves a volume knob. Measured 2026-09-27 -- the
 * operator had to crank the knob to the stop to get a level they liked, and at
 * the middle the HDMI was too quiet to use. Unity belongs at the working point,
 * so the travel above it becomes headroom that this path cannot spend.
 *
 * This law cannot spend it, because this law is a 0..1 attenuator and that is
 * what the room's knob should be: above the knee it is flat, and the top of the
 * travel is worth exactly the knee. The mirror's level above unity therefore
 * comes from configuration instead -- RB_AUDIO_MIRROR_BOOST_DB, applied in
 * audioshim.c as a multiplier UNDER this gain, so the knob attenuates the lifted
 * level rather than setting it. Two things are what made that lift safe, and
 * neither was true when this comment first said a boost was impossible: the
 * mirror's samples are saturated to the 24-bit domain at the gain
 * (mirror_saturate(), which counts what it caught as `clips=`), so running out
 * of headroom CLIPS rather than WRAPS -- the loud, aliased waveform S4.6
 * measured -- and rbp's master does peak below full scale (measured, the loudest
 * window of a run: 3229776 of 8388608, about -8.3 dBFS), so a +4 dB lift lands
 * its peaks near -4.3 dBFS with the knob at unity. The clamp is at the gain and
 * deliberately NOT inside s24pack(), whose modularity is a pinned contract; see
 * the S24PACK_SAMPLE_* comment in s24pack.h.
 *
 * `mid` is a fraction so the working point can be dialled by ear without a
 * rebuild (MIRROR_GAIN_MID). MID=1.0 reproduces the old law exactly, which makes
 * the knob its own rollback. */
static float flx4_mastervol_gain(int pos)
{
     double mid = mastervol_mid;
     double g;

     if (!(mid > 0.0) || mid > 1.0)
          mid = MIRROR_GAIN_MID_DEFAULT;    /* 0 would divide, >1 would boost */

     g = ((double)pos / 16383.0) / mid;
     if (g > 1.0)
          g = 1.0;
     return (float)g;
}

static void flx4_mastervol(int cc, int val)
{
     if (cc == CC_MASTER_MSB) {
          mastervol_have_hi = 1;
          mastervol_pos = (mastervol_pos & 0x7f) | (val << 7);
          return;
     }
     mastervol_pos = (mastervol_pos & 0x3f80) | (val & 0x7f);
     if (!mastervol_have_hi)
          return;                    /* no MSB yet: nothing to combine */
     int pos = mastervol_pos;
     if (pos < 0) pos = 0;
     if (pos > 0x3FFF) pos = 0x3FFF;

     /* g_mirror_gain and NOTHING ELSE. In particular no K_MASTERLVL: rbp's own
      * master level is pinned at unity by flx4_startup(), and this knob sits
      * downstream of the USB audio the Pi feeds the FLX4 -- so attenuating rbp's
      * level from it would attenuate the master twice, once in the samples the
      * Pi sends and again in the unit's analogue stage. flx4_build()'s note on
      * MASTER LEVEL left this CC unrouted for exactly that reason; the HDMI
      * mirror is the one place the value can go without that happening.
      *
      * The knob's electrical taper is neither published nor measured, so the
      * position is tracked rather than converted to the room's dB; doing that
      * would mean measuring the RCA output against knob position. What the
      * position is mapped ONTO is flx4_mastervol_gain()'s business. */
     g_mirror_gain = flx4_mastervol_gain(pos);
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
     /* Where the HDMI mirror reaches unity in the MASTER LEVEL knob's travel --
     * env_dnum because it is a fraction, not a count (shimutil.h's reason for
     * having two readers), and read here so an empty export from SHIM_VARS means
     * the default. See flx4_mastervol_gain() for what the number does, and for
     * why MID=1.0 is the rollback. */
     mastervol_mid = env_dnum("MIRROR_GAIN_MID", MIRROR_GAIN_MID_DEFAULT);
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
           * re-enters a stored one. A FRESH 4-beat loop is a pad rather than a
           * keycode: enter rbp's AUTO BEAT LOOP with the PAD FX 1 pad-mode
           * button below and press pad 5. **Not pad 1** -- MEASURED on the unit,
           * 2026-10-01, by reading the grid off rbp's own framebuffer while the
           * operator pressed all eight: rbp's AUTO BEAT LOOP bank is
           * 1/4, 1/2, 1, 2, 4, 8, 16, 32 across pads 1..8, so pad 1 is a
           * QUARTER beat and 4 beats is the fifth pad. The applied size matched
           * the label (pad 8 engaged a loop whose badge read `32`), so this is
           * the grid's own order and not a display quirk. docs/08-controls.md's
           * size table gives that bank as 4, 2, 1, 1/2, 1/4, ... -- a different
           * bank, or the same one read in the other direction; either way the
           * table is not what a fresh entry into this mode shows. */
          add_note(rch, N_4BEAT, K_RELOOP, sch);
          /* The pad-mode buttons select rbp's pad mode, which is what makes the
           * pad keycodes below mean whatever rbp's grid says they mean. The four
           * rows map to the RX3's four modes BY POSITION, not by this unit's
           * printed labels -- the constants above carry that argument, and it is
           * the one thing about this block that is not obvious from the table.
           * All four are plain press/release rows: rbp's mode setters are
           * idempotent toggles that re-read their own state, so pressing the
           * button of the mode it is already in switches that mode's SIZE BANK
           * instead of re-entering it (onKey_AutoBeatLoop's [+0x7a] and
           * friends) -- the RX3's own behaviour, which the shim must not try to
           * compute for rbp. MEASURED on the unit 2026-10-01: pressing HOT CUE
           * while already in HOT CUE takes rbp's pad-mode byte to **4**, a value
           * outside the documented 0..3, and pressing it again returns it to 0.
           * So 4 is that bank path seen from the outside, and anything reading
           * this mode must not treat 4 as a corrupt read. */
          add_note(rch, N_MODE_HOT, K_HOTCUE, sch);
          add_note(rch, N_MODE_PFX1, K_ALOOP, sch);
          add_note(rch, N_MODE_JUMP, K_SLIPLOOP, sch);
          add_note(rch, N_MODE_SMPL, K_BEATJUMP, sch);
          /* Known, deliberately not routed (key 0): the SHIFT button and
           * CUE/LOOP CALL > -- rbp's keycode for the call buttons is not
           * identified. See docs/15's open questions. */
          add_note(rch, N_SHIFT, 0, sch);
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
      * A base is therefore bound iff the map puts rbp in a mode for it, which
      * is why FOUR of the eight are bound -- and, since the mode buttons are
      * bound POSITIONALLY (see their block above), which rbp mode each base
      * carries follows that same order rather than the unit's labels: HOT CUE
      * (base 0 -> rbp mode 0), AUTO BEAT LOOP by way of the unit's PAD FX 1
      * (base 16 -> mode 1), SLIP BEAT LOOP by way of its BEAT JUMP button (base
      * 32 -> mode 2), and BEAT JUMP by way of its SAMPLER (base 48 -> mode 3).
      * The keycodes are the same eight in all four bases: rbp's K_PAD1..8 mean
      * whatever rbp's own mode says they mean, so swapping two bases' meaning is
      * a swap of LABELS here and nothing else. Without these rows the mode
      * buttons would be worse than dead: the unit really does switch its pads to
      * the new base, so PAD FX 1 would put rbp in a mode whose eight pads
      * produced nothing at all.
      *
      * The other four are deliberately unbound, because their BUTTONS reach no
      * rbp mode: KEYBOARD and PAD FX 2 have no rbp equivalent at all, and BEAT
      * LOOP (base 96) and KEY SHIFT are reached by a SHIFT + mode button
      * combination whose correspondence the list does not state (it gives four
      * extra modes and four shift notes, but never pairs them), so binding them
      * would be a guess about which button puts rbp in which mode -- and there
      * is no rbp mode for their pads to act in either way. An unbound pad
      * produces nothing and logs as unmapped.
      * MEASURED, all four bases, 2026-10-01: eight pads pressed in each of HOT
      * CUE, AUTO BEAT LOOP, the BEAT JUMP button's mode and SAMPLER's, with a
      * dump running and rbp's own pad-mode byte read back each time -- bases 0,
      * 16, 32 and 48 all arrived at base+pad as this block claims, and rbp acted
      * on every one of the 32 presses. (The pads act in whatever mode rbp is in,
      * so a press in the wrong mode is a wrong action rather than nothing.) The
      * SHIFT pairing -- the note numbers, not the bases -- is still unmeasured.
      *
      * The pads' +SHIFT layer is a second channel per deck (list ch 9/11 ->
      * rch 8/10) carrying the same eight ranges. Its HOT CUE base is bound
      * above the loop rather than here, because the action it stands for is not
      * a keycode; the other three bases are deliberately left unbound. */
     for (int d = 0; d < 2; d++) {
          int rch = CH_PADS1 + 2 * d;
          int sch = 1 + d;
          for (int p = 0; p < 8; p++) {
               add_note(rch,  0 + p, K_PAD1 + p, sch);  /* HOT CUE                    */
               add_note(rch, 16 + p, K_PAD1 + p, sch);  /* PAD FX 1 -> AUTO BEAT LOOP */
               add_note(rch, 32 + p, K_PAD1 + p, sch);  /* BEAT JUMP -> SLIP BEAT LOOP */
               add_note(rch, 48 + p, K_PAD1 + p, sch);  /* SAMPLER -> BEAT JUMP       */
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
     /* SHIFT + LOAD. The operator's own ask on the glass: "please also make shift
      * load 1 the track filter and shift load 2 the shortcut menu". Both are
      * CH_GLOBAL, like every other browse/library key in this map.
      *
      * The two are NOT the same shape, and the difference is measured rather than
      * stylistic. SHORTCUT (0x0210) opens on a plain press+release. TRACK FILTER
      * (0x420f) does not: rbp's UiKey_Filter gates on its record reaching state 3
      * and measurably refuses at state 2 -- which a press+release produces -- so
      * its row carries add_note_repeat(). rbp_abi.h's OP_REPEAT has the whole
      * derivation and the five failed drills behind it; a reader who changes this
      * row to add_note() will get 0 px and no error. */
     add_note_repeat(CH_MIX, N_LOAD1_SHIFT, K_TRACKFILTER, CH_GLOBAL);
     add_note(CH_MIX, N_LOAD2_SHIFT, K_SHORTCUT, CH_GLOBAL);
     /* Known, deliberately not routed (key 0): rekordbox's SMART CFX and SMART
      * FADER and the Android MONO/STEREO output switch have no rbp equivalent
      * (MONO is the mono sum of both channels, not the L=cue/R=master split
      * rbp's stereo type means, so it is not me_set_stereo()). */
     add_note(CH_MIX, N_SMART_CFX, 0, CH_GLOBAL);
     add_note(CH_MIX, N_SMART_FADER, 0, CH_GLOBAL);
     add_note(CH_MIX, N_MONO_STEREO, 0, CH_GLOBAL);
     /* And the control this map deliberately does not route to rbp: MASTER LEVEL
      * (CC 8 + CC 40) is the unit's own output volume, which sits after the USB
      * audio it feeds -- driving rbp's master level or audioshim's g_master_gain
      * from it would attenuate the master twice -- so flx4_startup() pins rbp's
      * master level at unity instead. Since 2026-09-27 the knob is not dropped:
      * flx4_mastervol() feeds g_mirror_gain, which scales the HDMI mirror and
      * nothing else. MIC LEVEL (CC 5) stays unrouted and has no reader, because
      * rbp's mic input is not part of this port. */

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
          else if (ch == CH_MIX && (cc == CC_MASTER_MSB || cc == CC_MASTER_LSB))
               flx4_mastervol(cc, val);
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

/* The DDJ-FLX4's panel LEDs -- **three rows measured, the rest still -1**. The
 * block at the top of this file says why they were left unguessed: this unit's
 * illumination notes for the transport, the pads, the channel CUE and the loops
 * are not published, and a wrong number here does not produce a dark LED, it
 * produces a phantom control. rbp_led.c sends nothing for a -1 row, so the
 * bridge runs with the rest unfilled and those LEDs simply stay dark until the
 * probe in docs/15's LED section has named a note for each. Each is then a
 * one-line edit -- as these three were: measured 2026-10-01, wired the same day,
 * and they are the surface's first lit LEDs.
 *
 * KEY LOCK, SLIP and the four Sound Color FX rows are -1 for a different reason:
 * the unit has no such control, so it has no such LED and never will. Recording
 * them as absent rather than omitting them is the point -- an omission is what a
 * future reader fills in by guessing.
 *
 * ONE GROUP IS NOT HERE AT ALL, and it is not an oversight: the unit's seven
 * pad-mode buttons (HOT CUE, PAD FX 1/2, BEAT JUMP, SAMPLER, BEAT LOOP, KEY
 * SHIFT) have no field in struct led_notes to be -1 in, so nothing can drive
 * them and nothing pretends to. They are also the one group with state behind
 * them -- what the light should say is *which* rbp pad mode is current -- so
 * wiring them needs a new field, the buttons' LED notes, and a source of truth
 * for that mode. The obvious candidate, `[player+0x74]` (which scan_plinn()
 * locates and logs as `padmode=%d`), is measured NOT to track the displayed pad
 * mode -- polled 1.1 M times across all four mode buttons while rbp's grid
 * verifiably switched HOT CUE -> BEAT LOOP, it never left 0 -- so that read is
 * still to be found. Until then the FLX4 lights the button that
 * was pressed on its own, which happens to be correct whenever the mode was
 * chosen from these buttons and wrong when rbp's screen moves the mode instead.
 * The operator asked for this and it is deliberately still open: see S9.9 in
 * docs/13-raspberrypi4.md. */
static const struct led_notes flx4_leds = {
     /* THE FIRST THREE ROWS EVER MEASURED ON THIS SURFACE, 2026-10-01, and they
      * are the same numbers the same buttons send: the panel's LED for a control
      * uses that control's own input note on its own input channel. Probed by
      * playing a one-note MIDI file into the FLX4 from the host with `aplaymidi
      * -p 28:0` -- the sequencer route, because amidi is EBUSY while the shim
      * holds the port -- one candidate at a time, and read off the panel by eye:
      * `84/0` lit deck 1's channel CUE, `84/1` lit deck 2's, `99/6` lit MASTER
      * CUE, and velocity 0 turned each back off (which is the value rbp_led.c
      * sends for "off", so the two agree). Wired rather than left as a note
      * because the driver for both rows was already here and already correct:
      * rbp_led.c reads rbp's own `me_get_cue(m)` and `me_get_master_cue()` every
      * tick and sends only on a change, with both caches starting `-1` so the
      * first tick sends the truth -- the lights therefore agree with rbp from
      * boot, not from the first press.
      *
      * THE DECK ROWS, MEASURED THE SAME DAY AND THE SAME WAY, with one thing the
      * first three could not show: this panel wants the message REPEATED. The
      * first pass held each candidate 1.2 s with a single message and lit one of
      * four; the second held the same four 3 s with the message re-sent every
      * 100 ms and lit all four. rbp_led.c's LED_RESEND_TICKS is that finding --
      * without it these rows would be bound and still dark. So `11/0`, `12/0`,
      * `16/0`, `17/0` and `88/0` are PLAY, CUE, LOOP IN, LOOP OUT and BEAT SYNC on
      * deck 1, and the rule above puts the same notes on ch 1 for deck 2, which is
      * what `.deck_ch = 0` encodes as `deck_ch + i`.
      *
      * `n_autoloop` (4 BEAT / EXIT, note 77) stayed dark through that same 100 ms
      * stream, and it is now settled rather than pending: Pioneer's list gives it
      * an EMPTY receive column, so it has no LED to light -- see the field's own
      * comment below. The pad group and the Beat FX group remain unwired, and
      * nothing here is a guess dressed as a measurement. */
     .deck_ch = 0,             /* MEASURED: deck 1 = ch 0, deck 2 = ch 1 */
     .n_sync = 88,             /* MEASURED: 88/0 = deck 1 BEAT SYNC */
     .n_cue = 12,              /* MEASURED: 12/0 = deck 1 CUE (the DECK cue,
                                * not the mixer strip's -- that is n_strip_cue) */
     .n_play = 11,             /* MEASURED: 11/0 = deck 1 PLAY */
     .n_keylock = -1,          /* absent: the unit has no KEY LOCK button */
     .n_vinyl = -1,            /* pending, not absent: documented note 23, no button */
     .n_slip = -1,             /* absent: the unit has no SLIP button */
     .n_loopin = 16,           /* MEASURED: 16/0 = deck 1 LOOP IN */
     .n_loopout = 17,          /* MEASURED: 17/0 = deck 1 LOOP OUT */
     .n_autoloop = -1,         /* ABSENT, and now settled: 4 BEAT / EXIT has NO
                                * LED to light. Pioneer's own "List of MIDI
                                * Messages" gives note 77 (and +SHIFT 80) an
                                * EMPTY receive column -- the button sends and
                                * receives nothing back -- and the panel agreed:
                                * 77/0 held for 1 s with the same 100 ms stream
                                * that lit every other row stayed dark. So this
                                * is not an unmeasured number to chase; leaving
                                * it -1 is the correct final state. The same
                                * empty receive column covers SHIFT (63), platter
                                * touch (54/103), CUE/LOOP CALL (81/83, +SHIFT
                                * 61/62), BEAT SYNC long-press (92) and the FX
                                * section's 74/75/99-102 on ch 4/5 -- none of
                                * those have a host-driven LED either. */

     /* The four pad-mode buttons, indexed by rbp's own mode value. MEASURED ON
      * THE GLASS 2026-10-01: each of the four was lit alone from the host and
      * the operator read the button it lit -- 27 the 1st, 30 the 2nd, 32 the
      * 3rd, 34 the 4th, in order, with deck 1's CUE (84/0) flashing as the
      * positive control in the same run. The numbers are the four buttons' own
      * INPUT notes on the deck channel, which is the LED rule this map already
      * measured twice (84/0, 84/1 and 99/6), so the rule now holds 4 for 4 on
      * this surface.
      *
      * The index is RBP's mode and not the button's printed name, and the two
      * deliberately disagree: the positional rebind of the same day makes the
      * 3rd button rbp's SLIP BEAT LOOP and the 4th its BEAT JUMP, so the 3rd
      * light comes on when rbp is in SLIP LOOP. That is the whole point of the
      * positional doctrine -- the light says which RX3 position is active, not
      * what Pioneer printed under it. */
     .n_mode = { 27, 30, 32, 34 },

     /* The eight performance pads. Their notes are their own INPUT notes -- the
      * LED rule again, which now holds 4 for 4 on this surface -- but unlike
      * every other group here they are not one note each: the unit re-addresses
      * the whole eight when a mode button is pressed, so the base belongs to the
      * MODE and there are four of them.
      *
      * MEASURED ON THE PANEL 2026-10-01, all four bases, every pad pressed with
      * the dump running: HOT CUE sent 0..7 on ch 7, PAD FX 1 sent 16..23, the
      * BEAT JUMP button 32..39 and SAMPLER 48..55, each press at vel 127 with
      * its own release and rbp acting on every one. So `16 * mode` is not a
      * guess about this unit, and the four bases are exactly the four the mode
      * rows above put rbp into -- which is what makes this a lighting of rbp's
      * modes rather than of the unit's printed labels.
      *
      * The two channels are NOT adjacent: deck 1's pads are ch 7 and deck 2's
      * are ch 9, because 8 and 10 are the same eight pads' +SHIFT layer, unbound
      * in build(). The `pad_ch + deck` this table said until now would have sent
      * deck 2's pads to the shift channel.
      *
      * A pad is LIT OR DARK here and never coloured, and that is this panel
      * rather than a gap in the wiring: rbp hands over an RGB per pad, but the
      * FLX4's pads have no colour to put it in. Pioneer's own list gives every
      * one of its 350 LED rows as `OFF=0x00, ON=0x7F`, and the ONLY row in the
      * whole document that carries a value range is the CH LEVEL METER
      * (0x26..0x7F, banded) -- which this table models separately, in
      * meter_enc. So `LED_ENC_NONE` is this row's FINAL value and not a
      * placeholder: the RGB is read and discarded, and 0x7f lights a pad.
      * (docs/15's open question 3 asked which velocity means which colour; the
      * answer is that the question does not apply to this surface.)
      *
      * What replaces the colour as the on/off signal is rbp's own "nothing
      * assigned here" flag -- see rbp_abi.h's LED_ENTRY_OFF_UNASSIGNED. It has
      * to: rbp keeps an empty hot-cue pad at State 1, so on State alone all
      * eight pads light in HOT CUE and an unassigned pad is indistinguishable
      * from a loaded one. The operator's panel showed exactly that. */
     .pad_ch = 7,              /* MEASURED: deck 1 pads, notes base + 0..7 */
     .pad_ch2 = 9,             /* MEASURED: deck 2 pads (+SHIFT is 8 and 10) */
     .n_pad_base = { 0, 16, 32, 48 },   /* indexed by rbp's mode; all measured */
     .pad_enc = LED_ENC_NONE,  /* final: the pads are on/off, they have no colour */

     .strip_ch_first = 0,      /* MEASURED: 84/0 = deck 1, 84/1 = deck 2 */
     .strip_count = 2,
     .n_strip_cue = 84,

     .master_ch_first = 6,     /* MEASURED: 99/6 lights MASTER CUE */
     .master_ch_count = 1,
     .n_master_cue = 99,

     .fx_ch = -1,              /* pending: the unit HAS Beat FX ON/OFF (note 71) */
     .n_fx = {
          [LED_FX_BFX_ONOFF]   = -1,
          [LED_FX_CFX_FILTER]  = -1,   /* absent: CFX knobs, no CFX buttons */
          [LED_FX_CFX_DUBECHO] = -1,   /* absent, ditto */
          [LED_FX_CFX_NOISE]   = -1,   /* absent, ditto */
          [LED_FX_CFX_SWEEP]   = -1,   /* absent, ditto */
     },

     /* The meter -- the one row of this table the unit does not SEND, and the
      * only row whose confirmation was a finger rather than a dump. Pioneer's
      * list, item 3-15 "CH LEVEL METER": deck 1 receives `B0 02 hh` and deck 2
      * `B1 02 hh`, with the value banded Green1 0x26-0x40, Green2 0x41-0x56,
      * Orange1 0x57-0x64, Orange2 0x65-0x76, Red 0x77-0x7F, "lights up from the
      * bottom", dark below 0x26. Zero-indexed that is CC 2 on channel 0 and
      * channel 1, which is the whole of this row.
      *
      * MEASURED ON THE PANEL 2026-10-01: with VU_TEST=1 sweeping the steps, the
      * operator's own eye confirmed both channel meters stepping, which is the
      * only thing that settles a meter -- a successful midi_cc() proves the
      * message left the shim and nothing more. So the wiring is confirmed and
      * the band boundaries above remain the list's published values; if the
      * panel lights but the top is unreachable or the bottom is dead, this is
      * still the row to correct.
      *
      * Master is deliberately absent. The manual's `[Level Meter]` switch picks
      * channel or master for the SAME two meters, and the list gives no separate
      * master meter address -- so lighting ch 15 like the Prime would be
      * inventing an address, which is the one thing this table must not do. */
     .meter_ch_first = 0,      /* decks 1/2 -> channels 0/1 */
     .n_meter_cc = 2,
     .meter_enc = METER_ENC_FLX4_LEVEL,
     .meter_pre_fader = 1,
     .meter_master_ch = -1,
     .n_meter_master_l = -1,
     .n_meter_master_r = -1,
};

const struct ctrl_map map_flx4 = {
     "flx4",
     flx4_build,
     flx4_startup,
     flx4_event,
     flx4_tick,
     NULL,   /* devices(): a MIDI surface, so no evdev reader is started */
     NULL,   /* input(): ditto -- this map never sees an evdev triple */
     &flx4_leds,
};
