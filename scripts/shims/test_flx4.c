/*
 * test_flx4.c -- the DDJ-FLX4 map, end to end, with no hardware.
 *
 * The sibling of test_midi.c, for the surface this port targets. It replays
 * tests/midi_flx4.dump through the REAL FLX4 map -- map_flx4.o, ctrl_map.o,
 * mididump.o, shimutil.o and shmstate.o are the objects that ship, not copies --
 * and pins the keycodes that come out.
 *
 * READ THIS BEFORE TRUSTING A PASS. test_midi.c's tables are extracted from a
 * unit that has been driven; this file's are not. Every number in map_flx4.c
 * comes from Pioneer's published FLX4 MIDI message list, and this fixture was
 * written from the same reading of it. So:
 *
 *   - The assertions below are pins on THAT TABLE. They prove the map dispatches
 *     what its tables say, that a repeat is dropped, that a latch latches, that
 *     a knob is quantised. They cannot prove the table matches an FLX4, because
 *     they would pass just as happily if both the map and this file were wrong in
 *     the same way -- a mis-read channel number is the obvious example, and it
 *     would make every control on a deck do nothing at all on real hardware.
 *   - Nothing here has been near an FLX4. The evidence that would replace this
 *     is a dump of the unit; docs/15-flx4-midi.md is the procedure, and it lists
 *     the values that are still marked TODO: unverified in the map (JOG_PPR, the
 *     pitch polarity, the pad base+pad encoding, the FX CH SELECT resting
 *     position).
 *
 * Three things are checked, kept separate because they fail for different
 * reasons:
 *
 *   1. The FORMAT round-trips. Every line of the fixture is parsed and
 *      re-rendered and the two must be identical, so a change to the writer the
 *      reader does not follow is caught here rather than by a dump nobody can
 *      replay.
 *
 *   2. MIDI_REPLAY reaches the map. The dispatch path below is the two steps
 *      ctrlshim.c's ctrl_dispatch() performs: the KeyManager gate, then the map.
 *
 *   3. The map produces the keycodes its tables say it should -- plus the state
 *      that is not a keycode at all (the headphone gains the audio shim reads,
 *      the fader positions the meters read, the mixer engine's cue flags).
 *
 * What is NOT checked here, and cannot be:
 *
 *   - flx4_startup(). It writes two absolute rbp addresses (the mixer input
 *     routing), and under qemu-arm an unmapped address is a segfault, not a
 *     failed check. What it does -- the routing, the Sound Color FX type, the
 *     master level -- has to be confirmed on the device.
 *   - Anything that is timing. The fixture's gaps are recorded so a paced replay
 *     is exercised too, but its wall-clock behaviour is not asserted. The jog's
 *     speed is derived from real elapsed time, so only its sign is pinned (inside
 *     the +-8 that rbp clamps to), not its magnitude. The one timing assertion is
 *     the idle edge at the end, and it waits for real time to pass rather than
 *     pretending the fixture's timestamps are seconds.
 *   - The LED and meter bridge. RB_LED_VU=0 for this target and this map drives
 *     no meter; rbp_led.c is still shaped for the previous unit's channels and
 *     the FLX4's own LED note numbers are not in the vendor list, so there is
 *     nothing here to assert.
 *
 * One side effect worth knowing about: klog() is unconditional, and the map logs
 * every FX SELECT step, FX CH SELECT move and master-cue toggle, so running this
 * appends a few lines to /tmp/knobshim.log on the build host.
 *
 * Build + run (static, so no rootfs is needed to load it):
 *     make test
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sound/asequencer.h>

#include "rbp_abi.h"       /* the K_..., OP_... and CH_... names the map speaks */
#include "rbp_bridge.h"    /* the keycode calls, and the signatures to match */
#include "shmstate.h"      /* the gains the mixer knobs write */
#include "rbp_vu.h"        /* g_fader, g_fader_seen */
#include "ctrl_map.h"
#include "mididump.h"

#ifndef FLX4_FIXTURE
#error "FLX4_FIXTURE must name the dump to replay (-DFLX4_FIXTURE=...); see the Makefile"
#endif

static int checks, failures;

#define CHECK(cond, ...) do {                                       \
        checks++;                                                   \
        if (!(cond)) {                                              \
            failures++;                                             \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);             \
            printf(__VA_ARGS__);                                    \
            printf("\n");                                           \
        }                                                           \
    } while (0)

/* ==========================================================================
 * The globals the map writes, defined here because the modules that own them
 * (rbp_vu.c, ctrlshim.c) are not part of this link.
 *
 * Each is declared by the header included above rather than re-declared, so a
 * change to one of those types fails to compile here instead of quietly
 * disagreeing about the size of an array. aloop_enabled and led_loop_armed are
 * NOT here: this map never calls the beat-loop or LED path (see the header), so
 * nothing in this link refers to them.
 * ========================================================================== */

int verbose = 0;          /* every klog() in the map is gated on this */

int g_fader[3] = { 1023, 1023, 1023 };
int g_fader_seen[3];

/* ==========================================================================
 * The bridge: every call the map makes that would otherwise reach rbp or the
 * panel.
 * ========================================================================== */

/* ---- the output of the shim, recorded rather than sent ------------------- */

struct sent {
     int kind;        /* 0 = send_rx_key, 1 = send_rx_key_f, 2 = send_rx_key_fl */
     int key;
     int op;
     int ch;
     long param;
     float fval;
     long lval;
};

/* The fixture produces 81 keycodes (see the table), and the second, paced pass
 * produces roughly as many again; 256 is slack, not a measured bound. */
#define SENT_MAX 256
static struct sent sent[SENT_MAX];
static int sent_n;

static void record(int kind, int key, int op, int ch, long param, float fval,
                   long lval)
{
     if (sent_n >= SENT_MAX) {
          printf("FAIL: the fixture sent more than %d keycodes\n", SENT_MAX);
          exit(1);
     }
     sent[sent_n].kind = kind;
     sent[sent_n].key = key;
     sent[sent_n].op = op;
     sent[sent_n].ch = ch;
     sent[sent_n].param = param;
     sent[sent_n].fval = fval;
     sent[sent_n].lval = lval;
     sent_n++;
}

void send_rx_key(int keycode, int op, int ch, long param)
{
     record(0, keycode, op, ch, param, 0.0f, 0);
}

void send_rx_key_f(int keycode, int op, int ch, long param, float fval)
{
     record(1, keycode, op, ch, param, fval, 0);
}

void send_rx_key_fl(int keycode, int op, int ch, long param, float fval,
                    long lval)
{
     record(2, keycode, op, ch, param, fval, lval);
}

/* ---- rbp ---------------------------------------------------------------- */

/* A non-NULL sentinel: the dispatcher's gate only asks whether rbp's KeyManager
 * exists, and the map must be reached. */
void *get_key_manager(void)
{
     return (void *)&checks;
}

/* cc_to_10bit() is the one stub that is a formula rather than a no-op, and it is
 * copied from rbp_bridge.c on purpose: linking that object would put rbp's
 * absolute addresses into this process. The alternative -- asserting nothing
 * about the quantisation -- would leave the map's most-used conversion
 * unexamined. It is also why the expectation table below spells out 516 and 258:
 * a change to the real conversion fails a row here rather than passing quietly. */
int cc_to_10bit(int v)
{
     if (v < 0) v = 0;
     if (v > 127) v = 127;
     return (v << 3) | (v >> 4);
}

/* The map only asks for the engine pointer while logging (verbose is 0 here), and
 * the mixer calls below are stubbed outright, so NULL is never dereferenced. */
void *mixer_engine(void)             { return NULL; }

/* The channel CUE and MASTER CUE buttons have no rbp keycode: the engine call is
 * the whole of their effect, so it is asserted rather than merely counted. The
 * stub keeps a real per-channel flag, which is what makes the LATCH testable --
 * the map decides its edge from me_get_cue()'s answer, exactly as the live shim
 * does against the real engine. */
static int cue_state[2];
static int cue_calls, cue_last = -1, cue_last_ch = -1;
int me_get_cue(int idx)              { return (idx >= 0 && idx < 2) ? cue_state[idx] : -1; }
void me_set_cue(int idx, int on)
{
     cue_calls++;
     cue_last = on;
     cue_last_ch = idx;
     if (idx >= 0 && idx < 2)
          cue_state[idx] = on;
}

static int master_cue_calls, master_cue_last = -1;
int me_get_master_cue(void)          { return 0; }
void me_set_master_cue(int on)       { master_cue_calls++; master_cue_last = on; }

/* The map has no split-cue control and must never touch the stereo type:
 * rbp_vu.c's vu_thread() asserts it for every target, and two writers for one
 * piece of engine state is the thing this stub exists to catch. */
static int stereo_calls;
void me_set_stereo(int type)         { (void)type; stereo_calls++; }

/* ==========================================================================
 * The expectation table: the keycode stream the fixture must produce.
 *
 * One row per keycode, in dispatch order. The comment on each row names what
 * produces it, so a row that moves is traceable to one gesture rather than to
 * "the map changed somewhere".
 *
 * A row is what the map's tables say, NOT what an FLX4 says -- see the file
 * header. `fval_sign` marks the rows whose float is derived from elapsed real
 * time (the jog's speed), where only the sign is a contract.
 * ========================================================================== */

struct expect {
     int kind, key, op, ch;
     long param;
     float fval;
     long lval;
     int fval_sign;   /* compare the sign, not the value */
};

/* The pitch fader's float, spelled the way flx4_pitch() computes it, so the two
 * cannot drift: (pos - 0x2000) / 8192, both ends exact in single precision. */
#define PITCH_NORM(pos) (((float)(pos) - 0x2000) / 8192.0f)

static const struct expect expect[] = {
     /* ---- deck 1 buttons (sch 1 = rbp player 1) ---- */
     /* PLAY, twice: the first pair, then a repeat press (dropped) and a stray
      * release (dropped), and the second pair. */
     { 0, K_PLAY,           OP_PRESS,   1, 0,    0.0f, 0, 0 },
     { 0, K_PLAY,           OP_RELEASE, 1, 0,    0.0f, 0, 0 },
     { 0, K_PLAY,           OP_PRESS,   1, 0,    0.0f, 0, 0 },
     { 0, K_PLAY,           OP_RELEASE, 1, 0,    0.0f, 0, 0 },
     { 0, K_CUE,            OP_PRESS,   1, 0,    0.0f, 0, 0 },
     { 0, K_CUE,            OP_RELEASE, 1, 0,    0.0f, 0, 0 },
     /* The platter touch, which is a button on this unit like any other. */
     { 0, K_JOG_TOUCH,      OP_PRESS,   1, 0,    0.0f, 0, 0 },
     { 0, K_JOG_TOUCH,      OP_RELEASE, 1, 0,    0.0f, 0, 0 },
     { 0, K_LOOPIN,         OP_PRESS,   1, 0,    0.0f, 0, 0 },
     { 0, K_LOOPIN,         OP_RELEASE, 1, 0,    0.0f, 0, 0 },
     { 0, K_LOOPOUT,        OP_PRESS,   1, 0,    0.0f, 0, 0 },
     { 0, K_LOOPOUT,        OP_RELEASE, 1, 0,    0.0f, 0, 0 },
     /* 4 BEAT / EXIT -> rbp's RELOOP/EXIT. */
     { 0, K_RELOOP,         OP_PRESS,   1, 0,    0.0f, 0, 0 },
     { 0, K_RELOOP,         OP_RELEASE, 1, 0,    0.0f, 0, 0 },
     /* The pad-mode button, then two pads: HOT CUE pad 1 (base 0) and BEAT JUMP
      * pad 8 (base 32), both on the deck's pad channel, both to sch 1. */
     { 0, K_HOTCUE,         OP_PRESS,   1, 0,    0.0f, 0, 0 },
     { 0, K_HOTCUE,         OP_RELEASE, 1, 0,    0.0f, 0, 0 },
     { 0, K_PAD1,           OP_PRESS,   1, 0,    0.0f, 0, 0 },
     { 0, K_PAD1,           OP_RELEASE, 1, 0,    0.0f, 0, 0 },
     { 0, K_PAD1 + 7,       OP_PRESS,   1, 0,    0.0f, 0, 0 },
     { 0, K_PAD1 + 7,       OP_RELEASE, 1, 0,    0.0f, 0, 0 },
     { 0, K_BEATJUMP,       OP_PRESS,   1, 0,    0.0f, 0, 0 },
     { 0, K_BEATJUMP,       OP_RELEASE, 1, 0,    0.0f, 0, 0 },
     /* BEAT SYNC: both keycodes on the one edge that arrives, and the other edge
      * produces nothing -- a leaked press here would be an extra row. */
     { 0, K_SYNC,           OP_PRESS,   1, 0,    0.0f, 0, 0 },
     { 0, K_SYNC,           OP_RELEASE, 1, 0,    0.0f, 0, 0 },
     /* BEAT SYNC long press -> MASTER, as a pulse. */
     { 0, K_MASTER,         OP_PRESS,   1, 0,    0.0f, 0, 0 },
     { 0, K_MASTER,         OP_RELEASE, 1, 0,    0.0f, 0, 0 },
     /* CUE/LOOP CALL < is BACK -- the surface has no BACK button, and this is
      * the spare one it was given instead. Its neighbour (83) stays log-only,
      * which is the pair of rows below: one of the two call buttons speaks and
      * the other does not, so a binding that fell on the wrong one is caught.
      * The fixture sends no NOTEOFF for it, hence the press alone. */
     { 0, K_BACK,           OP_PRESS,   1, 0,    0.0f, 0, 0 },
     /* SHIFT, the PAD FX 1 and SAMPLER pad modes and a +SHIFT note all reach no
      * keycode: they are in the table with key 0. */
     /* CH CUE produces no keycode at all -- the engine call is asserted in
      * check_state() -- and neither does the +SHIFT pad layer nor a pad in a mode
      * rbp has no equivalent for. */

     /* ---- deck 1 mixer strip and Sound Color FX ----
      * 64 -> 516 and 32 -> 258 are cc_to_10bit()'s quantisation; the repeat at
      * val=127 (a second identical CC) is dropped. */
     { 1, K_TRIM,           OP_VALUE,   1, 1023, 1.0f, 0, 0 },
     { 1, K_EQH,            OP_VALUE,   1, 516,  516.0f / 1023.0f, 0, 0 },
     { 1, K_EQM,            OP_VALUE,   1, 0,    0.0f, 0, 0 },
     { 1, K_EQL,            OP_VALUE,   1, 258,  258.0f / 1023.0f, 0, 0 },
     /* The channel fader: the pair is also what fills g_fader[1] for the meter
      * scaling, asserted below. */
     { 1, K_FADER,          OP_VALUE,   1, 1023, 1.0f, 0, 0 },
     { 1, K_FADER,          OP_VALUE,   1, 0,    0.0f, 0, 0 },
     /* The CFX knob, which arrives on the MIXER's channel but drives deck 1's
      * strip -- so this row is the one that pins that the send channel comes from
      * the table and not from the event. */
     { 1, K_COLOR,          OP_VALUE,   1, 258,  258.0f / 1023.0f, 0, 0 },

     /* ---- deck 1 jog: relative and 0x40-centred ----
      * The first message of all is spent establishing the dt baseline (its own
      * +1 is not applied), so the first row below is the second message, which
      * carries +3 counts (val 0x43 - 0x40). Then 0 counts (nothing goes out), +1,
      * -2 on the outer ring, -1 on the platter's vinyl-off CC, and +2 -- which
      * leaves the virtual position at 3, the value the idle edge later repeats. */
     { 2, K_JOG_ROT,        OP_ROTATE,  1, 0,    1.0f, 3, 1 },
     { 2, K_JOG_ROT,        OP_ROTATE,  1, 0,    1.0f, 4, 1 },
     { 2, K_JOG_ROT,        OP_ROTATE,  1, 0,   -1.0f, 2, 1 },
     { 2, K_JOG_ROT,        OP_ROTATE,  1, 0,   -1.0f, 1, 1 },
     { 2, K_JOG_ROT,        OP_ROTATE,  1, 0,    1.0f, 3, 1 },

     /* ---- deck 1 pitch: the 14-bit pair, dispatched on the LSB ----
      * +end (0x7F/0x7F), centre (0x40/0x00), -end (0x00/0x00). v10 is rbp's
      * 10-bit position and lval the raw 14-bit one; the MSB alone sends nothing
      * (a row here would be an extra entry). */
     { 2, K_TEMPO_SLIDER,   OP_VALUE,   1, 1022, PITCH_NORM(0x3FFF), 0x3FFF, 0 },
     { 2, K_TEMPO_SLIDER,   OP_VALUE,   1, 511,  PITCH_NORM(0x2000), 0x2000, 0 },
     { 2, K_TEMPO_SLIDER,   OP_VALUE,   1, 0,    PITCH_NORM(0x0000), 0x0000, 0 },

     /* ---- deck 2: the same shapes against rbp player 2 (sch 2) ---- */
     { 0, K_PLAY,           OP_PRESS,   2, 0,    0.0f, 0, 0 },
     { 0, K_PLAY,           OP_RELEASE, 2, 0,    0.0f, 0, 0 },
     /* This deck's BEAT SYNC arrives as a RELEASE only (footnote *3 read the
      * other way round), and it must still be the whole gesture. */
     { 0, K_SYNC,           OP_PRESS,   2, 0,    0.0f, 0, 0 },
     { 0, K_SYNC,           OP_RELEASE, 2, 0,    0.0f, 0, 0 },
     { 1, K_TRIM,           OP_VALUE,   2, 1023, 1.0f, 0, 0 },
     { 0, K_HOTCUE,         OP_PRESS,   2, 0,    0.0f, 0, 0 },
     { 0, K_HOTCUE,         OP_RELEASE, 2, 0,    0.0f, 0, 0 },
     /* Deck 2's pads are on list ch 10 (rch 9), not deck 1's ch 8; the +SHIFT
      * pad layer on ch 11 produces nothing. */
     { 0, K_PAD1 + 3,       OP_PRESS,   2, 0,    0.0f, 0, 0 },
     { 0, K_PAD1 + 3,       OP_RELEASE, 2, 0,    0.0f, 0, 0 },
     { 1, K_FADER,          OP_VALUE,   2, 1023, 1.0f, 0, 0 },
     /* Deck 2's jog: the same baseline rule per deck, so the second of its three
      * messages carries +4 (val 0x44 - 0x40), which is the position its idle edge
      * repeats below. Deliberately a different count from deck 1's, so a send
      * that went to the wrong player's jog state cannot pass. */
     { 2, K_JOG_ROT,        OP_ROTATE,  2, 0,    1.0f, 4, 1 },
     /* Deck 2's pitch: the first LSB arrives before any MSB, so it has nothing to
      * combine and sends nothing; the MSB then sends nothing of its own; the LSB
      * after it is the one send, at the centre detent. */
     { 2, K_TEMPO_SLIDER,   OP_VALUE,   2, 511,  PITCH_NORM(0x2000), 0x2000, 0 },

     /* ---- mixer / browse / LOAD, on list ch 7 -> rch 6 ---- */
     /* The browse knob's push and its rotation. rbp's browse keys are global, so
      * the send channel is CH_GLOBAL even though the events are on ch 6. */
     { 0, K_SELECTOR,       OP_PRESS,   1, 0,    0.0f, 0, 0 },
     { 0, K_SELECTOR,       OP_RELEASE, 1, 0,    0.0f, 0, 0 },
     /* SHIFT + the same push is SOURCE instead -- a different keycode on the
      * same note's shift variant, which is the whole reason the map binds the
      * shift note separately rather than tracking a SHIFT state. */
     { 0, K_SOURCE,         OP_PRESS,   1, 0,    0.0f, 0, 0 },
     { 0, K_SOURCE,         OP_RELEASE, 1, 0,    0.0f, 0, 0 },
     { 0, K_SELECTOR,       OP_ROTATE,  1, 1,    0.0f, 0, 0 },
     { 0, K_SELECTOR,       OP_ROTATE,  1, -1,   0.0f, 0, 0 },
     /* 63 and 64: the encoder's direction split. This is the unit's only 63/64
      * boundary -- an FLX4 has no CC-as-button, so there is no threshold row
      * anywhere in this map -- and it is the closest thing to the DDJ-class
      * "toggle at >= 64" the port's early notes assumed. */
     { 0, K_SELECTOR,       OP_ROTATE,  1, 1,    0.0f, 0, 0 },
     { 0, K_SELECTOR,       OP_ROTATE,  1, -1,   0.0f, 0, 0 },
     /* The +SHIFT browse CC (100) is the same encoder. */
     { 0, K_SELECTOR,       OP_ROTATE,  1, 1,    0.0f, 0, 0 },
     /* LOAD streams on the global channel with the deck in the send channel. */
     { 0, K_LOAD,           OP_PRESS,   1, 0,    0.0f, 0, 0 },
     { 0, K_LOAD,           OP_RELEASE, 1, 0,    0.0f, 0, 0 },
     { 0, K_LOAD,           OP_PRESS,   2, 0,    0.0f, 0, 0 },
     { 0, K_LOAD,           OP_RELEASE, 2, 0,    0.0f, 0, 0 },
     /* MASTER CUE has no keycode either (asserted in check_state()). SMART CFX,
      * SMART FADER and MONO/STEREO are log-only; MASTER LEVEL and MIC LEVEL are
      * deliberately unbound; and a deck control on ch 6 reaches nothing. */
     { 1, K_XFADER,         OP_VALUE,   1, 1023, 1.0f, 0, 0 },
     /* The CFX knobs are on the mixer channel but each drives one deck's channel:
     * deck 1's strip and then deck 2's, from the same event channel. */
     { 1, K_COLOR,          OP_VALUE,   1, 516,  516.0f / 1023.0f, 0, 0 },
     { 1, K_COLOR,          OP_VALUE,   2, 0,    0.0f, 0, 0 },
     /* The headphone knobs also land in g_cue_mix / g_cue_gain, asserted below. */
     { 1, K_HPMIX,          OP_VALUE,   1, 516,  516.0f / 1023.0f, 0, 0 },
     { 1, K_HPLEVEL,        OP_VALUE,   1, 0,    0.0f, 0, 0 },

     /* ---- Beat FX ---- */
     /* FX ON/OFF, once per target channel: the unit sends one leg per channel and
      * both are bound to the same keycode. */
     { 0, K_BFX,            OP_PRESS,   1, 0,    0.0f, 0, 0 },
     { 0, K_BFX,            OP_RELEASE, 1, 0,    0.0f, 0, 0 },
     { 0, K_BFX,            OP_PRESS,   1, 0,    0.0f, 0, 0 },
     { 0, K_BFX,            OP_RELEASE, 1, 0,    0.0f, 0, 0 },
     /* FX SELECT is a button and rbp's effect switch has 14 positions, so the map
      * keeps the position and steps it -- starting at 0 on the first press. */
     { 0, K_BFXTYPE,        OP_VALUE,   1, 0,    0.0f, 0, 0 },
     { 0, K_BFXTYPE,        OP_VALUE,   1, 1,    0.0f, 0, 0 },
     /* BEAT < / > are rbp's beat-fraction keys, sent as PRESS pulses (+-1). */
     { 2, K_BEATPREV,       OP_PRESS,   1, -1,   0.0f, -1, 0 },
     { 2, K_BEATNEXT,       OP_PRESS,   1, 1,    0.0f, 1, 0 },
     /* LEVEL/DEPTH, bound on both candidate channels because the vendor table
      * disagrees with itself about which one the knob uses. */
     { 1, K_DEPTH,          OP_VALUE,   1, 516,  516.0f / 1023.0f, 0, 0 },
     { 1, K_DEPTH,          OP_VALUE,   1, 0,    0.0f, 0, 0 },
     /* The FX CH SELECT lever: CH1 -> target 0, CH2 -> target 1, CH1&CH2 -> no
      * rbp equivalent (nothing sent, and the target is left where it was), and
      * the move back to the leg that is already selected -> nothing. */
     { 0, K_BFXCH,          OP_VALUE,   1, 0,    0.0f, 0, 0 },
     { 0, K_BFXCH,          OP_VALUE,   1, 1,    0.0f, 0, 0 },
};

#define EXPECT_N ((int)(sizeof(expect) / sizeof(expect[0])))

/* ==========================================================================
 * The replay
 * ========================================================================== */

/* ctrlshim.c's ctrl_dispatch(), minus the MIDI_DUMP recording. */
static void dispatch(const struct snd_seq_event *ev)
{
     if (get_key_manager() && map_flx4.event)
          map_flx4.event(ev);
}

static unsigned long long us_of(double seconds)
{
     return (unsigned long long)(seconds * 1000000.0 + 0.5);
}

/* The fixture's replayable line count, filled in by round_trip(). */
static int fixture_events;

/* Read the fixture once: every line must parse the way the format says, and
 * every re-renderable line must come back byte-identical. */
static void round_trip(void)
{
     FILE *f = fopen(FLX4_FIXTURE, "r");
     char line[256], buf[256];
     int n_lines = 0;

     CHECK(f != NULL, "cannot open the fixture %s", FLX4_FIXTURE);
     if (!f)
          return;

     while (fgets(line, sizeof line, f)) {
          struct snd_seq_event ev;
          size_t len = strlen(line);
          double t;
          int ty;

          if (len && line[len - 1] == '\n')
               line[--len] = '\0';
          n_lines++;

          if (len == 0 || line[0] == '#')
               continue;

          if (sscanf(line, "%lf", &t) != 1) {
               CHECK(0, "fixture line %d has no timestamp: %s", n_lines, line);
               continue;
          }

          /* A SKIP line is the format's record of an event no replay can
           * reconstruct: it must re-render, and it must not replay. */
          if (sscanf(line, "%lf SKIP type=%d", &t, &ty) == 2) {
               CHECK(mididump_parse(line, &ev) == 0,
                     "fixture line %d: SKIP parsed as a replayable event",
                     n_lines);
               memset(&ev, 0, sizeof ev);
               ev.type = (unsigned char)ty;
               CHECK(mididump_format(&ev, us_of(t), buf, sizeof buf) == (int)len &&
                     strcmp(buf, line) == 0,
                     "fixture line %d: SKIP re-rendered as \"%s\", expected \"%s\"",
                     n_lines, buf, line);
               continue;
          }

          if (mididump_parse(line, &ev) != 1) {
               CHECK(0, "fixture line %d did not parse: %s", n_lines, line);
               continue;
          }
          fixture_events++;
          CHECK(mididump_format(&ev, us_of(t), buf, sizeof buf) == (int)len &&
                strcmp(buf, line) == 0,
                "fixture line %d: re-rendered as \"%s\", expected \"%s\"",
                n_lines, buf, line);
     }
     fclose(f);
     CHECK(fixture_events > 0, "the fixture carries no replayable events");
}

/* The lines the parser must refuse, and the shapes it must accept. The refusals
 * matter as much as the acceptances: every one of them is a line that appears in
 * a dump that is still being written, or in a dump of a unit doing something the
 * format has no room for. */
static void parser_tolerance(void)
{
     static const struct { const char *line; int want; } t[] = {
          { "",                                     0 },  /* blank */
          { "\n",                                   0 },
          { "# just a comment\n",                   0 },
          { "not a dump line at all\n",             0 },
          { "0.500000 SKIP type=3\n",               0 },  /* recorded, not replayable */
          { "0.500000 NOTEON ch=0 note=1\n",        0 },  /* no velocity: no event */
          { "0.500000 CONTROLLER ch=0\n",           0 },
          { "0.500000 NOTEON ch=0 note=36 vel=127\n",   1 },
          { "0.500000 NOTEOFF ch=0 note=36 vel=0\n",    1 },
          { "0.500000 CONTROLLER ch=4 cc=17 val=64\n",  1 },
     };
     struct snd_seq_event ev;
     char buf[64], want[64];
     unsigned i;

     for (i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
          int got = mididump_parse(t[i].line, &ev);
          CHECK(got == t[i].want, "\"%s\" parsed as %d, expected %d",
                t[i].line, got, t[i].want);
     }

     /* The one field the format renders from the event rather than from a switch
      * arm: a type it cannot carry keeps its number, so a dump says what arrived
      * even when nothing can be done with it. */
     memset(&ev, 0, sizeof ev);
     ev.type = (unsigned char)SNDRV_SEQ_EVENT_CLOCK;
     snprintf(want, sizeof want, "0.000000 SKIP type=%d",
              (int)SNDRV_SEQ_EVENT_CLOCK);
     CHECK(mididump_format(&ev, 0, buf, sizeof buf) == (int)strlen(want) &&
           strcmp(buf, want) == 0,
           "an unreplayable type rendered as \"%s\", expected \"%s\"", buf, want);

     /* And a destination too small for the line is refused, not truncated. */
     CHECK(mididump_format(&ev, 0, buf, 4) < 0,
           "a too-small destination was not refused");
}

static void check_stream(void)
{
     int i;

     CHECK(sent_n == EXPECT_N,
           "the fixture produced %d keycodes, the table expects %d",
           sent_n, EXPECT_N);
     for (i = 0; i < EXPECT_N && i < sent_n; i++) {
          const struct sent *s = &sent[i];
          const struct expect *e = &expect[i];

          CHECK(s->kind == e->kind && s->key == e->key && s->op == e->op &&
                s->ch == e->ch,
                "keycode %d: got kind%d key=0x%04x op=%d ch=%d, expected "
                "kind%d key=0x%04x op=%d ch=%d",
                i, s->kind, s->key, s->op, s->ch,
                e->kind, e->key, e->op, e->ch);
          CHECK(s->param == e->param,
                "keycode %d (0x%04x): param %ld, expected %ld",
                i, e->key, s->param, e->param);
          if (e->fval_sign)
               CHECK((s->fval > 0.0f) == (e->fval > 0.0f) &&
                     s->fval <= 8.0f && s->fval >= -8.0f,
                     "keycode %d (0x%04x): fval %.3f, expected the sign of "
                     "%.1f inside rbp's +-8 clamp",
                     i, e->key, (double)s->fval, (double)e->fval);
          else
               CHECK(s->fval == e->fval,
                     "keycode %d (0x%04x): fval %.6f, expected %.6f",
                     i, e->key, (double)s->fval, (double)e->fval);
          CHECK(s->lval == e->lval,
                "keycode %d (0x%04x): lval %ld, expected %ld",
                i, e->key, s->lval, e->lval);
     }
}

/* Everything the map drives that is not a keycode. Each of these is a value the
 * audio shim reads or the meter scaling uses, so a map that stopped writing one
 * would still look fine from the keycode stream alone. */
static void check_state(void)
{
     CHECK(g_cue_mix == 516.0f / 1023.0f,
           "the cue-mix knob left g_cue_mix at %.4f, expected %.4f",
           (double)g_cue_mix, (double)(516.0f / 1023.0f));
     CHECK(g_cue_gain == 0.0f,
           "the cue-level knob left g_cue_gain at %.4f, expected 0",
           (double)g_cue_gain);

     /* Both channel faders moved, so both meter inputs are known. */
     CHECK(g_fader[1] == 0 && g_fader_seen[1] == 1,
           "the left fader is at %d (seen %d), expected 0 and seen",
           g_fader[1], g_fader_seen[1]);
     CHECK(g_fader[2] == 1023 && g_fader_seen[2] == 1,
           "the right fader is at %d (seen %d), expected 1023 and seen",
           g_fader[2], g_fader_seen[2]);

     /* The two buttons with no rbp keycode. CH CUE is driven from the engine's
      * own state, so the fixture's two presses on deck 1 are an on edge and then
      * an off edge: that pair is the latch, and it only works if the map asks the
      * engine first. Deck 2's single press is the other direction. */
     CHECK(cue_calls == 3, "the CH CUE buttons called me_set_cue() %d times, "
           "expected 3 (deck 1 twice, deck 2 once)", cue_calls);
     CHECK(cue_state[0] == 0,
           "deck 1's cue was left %d; the fixture presses CH CUE twice, so the "
           "second press must be the off edge -- if this is 2 the map sent ON "
           "twice", cue_state[0]);
     CHECK(cue_state[1] == 1,
           "deck 2's cue was left %d, expected 1", cue_state[1]);
     CHECK(cue_last_ch == 0 && cue_last == 0,
           "the last me_set_cue() was ch %d -> %d, expected ch 0 -> 0",
           cue_last_ch, cue_last);

     CHECK(master_cue_calls == 1 && master_cue_last == 1,
           "the MASTER CUE button called me_set_master_cue() %d times (last %d), "
           "expected 1 with cue on", master_cue_calls, master_cue_last);

     /* The map must not touch the headphone stereo type: rbp_vu.c's vu_thread()
      * asserts it for every target, meters or not. */
     CHECK(stereo_calls == 0,
           "the map called me_set_stereo() %d times; the meter thread owns that "
           "setting", stereo_calls);
}

/* The jog's end-of-motion edge, which is the one thing this map does on the tick
 * rather than on an event. Real time has to pass for it, so this waits for it
 * instead of pretending the fixture's timestamps are seconds. */
static void check_idle(void)
{
     int base = sent_n;

     usleep(200000);              /* longer than the default JOG_IDLE_MS = 120 */
     map_flx4.tick();

     CHECK(sent_n == base + 2,
           "the jog's idle edge sent %d keycodes, expected one per deck",
           sent_n - base);
     if (sent_n != base + 2)
          return;

     /* Deck 1's last delta left its virtual position at 3, deck 2's at 4; both
      * must be repeated with a speed of exactly 0 -- rbp keeps bending until it
      * is told the wheel stopped. */
     for (int i = 0; i < 2; i++) {
          const struct sent *s = &sent[base + i];
          int want_ch = 1 + i;
          long want_lval = 3 + i;

          CHECK(s->kind == 2 && s->key == K_JOG_ROT && s->op == OP_ROTATE &&
                s->ch == want_ch && s->param == 0 && s->fval == 0.0f &&
                s->lval == want_lval,
                "deck %d's idle edge: kind%d key=0x%04x op=%d ch=%d param=%ld "
                "fval=%.3f lval=%ld, expected a speed-0 K_JOG_ROT to ch %d at "
                "position %ld",
                want_ch, s->kind, s->key, s->op, s->ch, s->param,
                (double)s->fval, s->lval, want_ch, want_lval);
     }
}

int main(void)
{
     /* The map reads its calibration from the environment, and `make test`
      * inherits whatever the shell running it exports. Pin every name it reads,
      * so the expectation table is about the code and not about the developer's
      * environment. */
     static const char *const names[] = {
          "KNOB_SCALE", "JOG_SCALE", "JOG_PPR", "JOG_REV", "JOG_IDLE_MS",
          "JOG_VERBOSE", "TEMPO_VERBOSE", "TEMPO_REV", "KNOB_VERBOSE",
     };
     unsigned i;
     int n;

     for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
          unsetenv(names[i]);

     round_trip();
     parser_tolerance();

     map_flx4.build();

     /* The tables' sizes, spelled out rather than merely "non-empty": a row that
      * silently falls off the end of add_note()/add_abs() is a control that stops
      * working, and it would otherwise only show up as an unmapped event.
      * Deck 1/2 contribute 13 notes each (9 bound: play, cue, jog touch, loop in,
      * loop out, 4 beat, hot cue mode, beat jump mode, and CUE/LOOP CALL < as
      * BACK -- plus 4 log-only: shift, pad fx, sampler, and the other call
      * button), 8 pads x 2 bases each, and 5 absolute controls; the mixer
      * contributes 7 notes and 5 knobs, and the Beat FX 2 notes and 2 depth
      * rows. */
     CHECK(note_map_n == 67,
           "the map built %d note rows, expected 67 (2 x (13 + 16) + 7 + 2)",
           note_map_n);
     CHECK(abs_map_n == 17,
           "the map built %d absolute rows, expected 17 (2 x 5 + 5 + 2)",
           abs_map_n);

     /* speed <= 0 is "no pacing": the recording's gaps are real seconds, and
      * sleeping through them would make this a test of nanosleep(). */
     n = mididump_replay(FLX4_FIXTURE, 0.0, dispatch);
     CHECK(n == fixture_events,
           "the replay dispatched %d events, the fixture has %d replayable lines",
           n, fixture_events);

     check_stream();
     check_state();
     check_idle();

     /* And once more with pacing on, which is the path MIDI_REPLAY_SPEED takes.
      * At this speed every sleep is zero-length, so only the count is asserted:
      * the map's own state (pressed rows, the jog's baseline, the pitch's MSB) was
      * consumed by the first pass, so the stream it produces here is a different
      * one and is deliberately not compared. */
     sent_n = 0;
     n = mididump_replay(FLX4_FIXTURE, 1000000.0, dispatch);
     CHECK(n == fixture_events,
           "the paced replay dispatched %d events, expected %d",
           n, fixture_events);

     printf("test_flx4: %d checks, %d failures\n", checks, failures);
     return failures == 0 ? 0 : 1;
}
