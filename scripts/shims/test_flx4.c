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
 *     pitch polarity, the pad base+pad encoding, and which position the FX CH
 *     SELECT lever is in at connect -- the lever's three values themselves are
 *     measured, on rbp's own panel, docs/13 S5.6).
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
 *   - The LED and meter bridge. The FLX4's LED TABLE is checked here
 *     (check_leds()): every row is -1 until a note is measured to light, it may
 *     not carry one of the SC Live 4's numbers, and it must name a legal note on
 *     a legal channel. What no assertion here can do is see an LED light -- the
 *     note numbers come from the probe in docs/15's LED section and the
 *     operator's eye, and the assertions are there to stop a GUESS being
 *     mistaken for a measurement.
 *
 *     The table's METER row is checked beside them and is a different kind of
 *     claim: its numbers come from Pioneer's own MIDI message list (item 3-15,
 *     CC 2 on the deck channels) and NOT from a probe -- the unit does not send
 *     its meter, so unlike a note there is nothing to read back. What the checks
 *     can do is hold the row to the document and keep it off the SC Live 4's
 *     address, which is the same failure the note rows are guarded against. That
 *     the panel actually lights is the operator's eye, and the band boundaries
 *     in the value encoding are the part to watch.
 *
 *     rbp_vu.c itself is NOT linked here: it needs the whole bridge (midi_cc,
 *     install_meter_hook, rbp's meter globals) to do anything, and the point of
 *     this suite is the map. So the encoding below is asserted at the TABLE, not
 *     through the code that will read it.
 *   - The FX SELECT cursor's "rbp did not answer" fallback. That cursor is a
 *     file-static, seeded once per process, so the fixture gets one first press
 *     and it is spent on the seeded case -- the answer the feature is for. The
 *     fallback (rbp_beatfx_type() answers -1, or a type no position reaches,
 *     and the cursor starts at 0) would need a second first press, so it is
 *     read off the map rather than asserted here.
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
#include "rbp_led.h"       /* led_loop_armed */
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
 * disagreeing about the size of an array.
 *
 * aloop_enabled, led_loop_armed, plinn, aloop_is_looping and aloop_apply USED to
 * be absent from this list, on the grounds that the FLX4 map never reaches the
 * beat-loop or LED path. That is still true of map_flx4.c, which is why none of
 * them is called: the definitions below exist only because check_leds() compares
 * the FLX4's LED table against map_jp21's, and linking map_jp21.o drags in its
 * unresolved references. They are deliberately inert -- plinn() returns NULL and
 * nothing looping -- so that if a future FLX4 row does reach the beat loop, the
 * apply counters below catch it rather than the beat loop firing in a test.
 * ========================================================================== */

int verbose = 0;          /* every klog() in the map is gated on this */

int g_fader[3] = { 1023, 1023, 1023 };
int g_fader_seen[3];

/* The beat loop and the loop-arm flags, inert. See the comment above: map_jp21.o
 * is in this link for the LED-table comparison, not for these. */
int aloop_enabled = -1;
int led_loop_armed[2];
static int plinn_calls, looping_calls, apply_calls;
void *plinn(int deck)                { (void)deck; plinn_calls++; return NULL; }
int aloop_is_looping(int deck)       { (void)deck; looping_calls++; return 0; }
void aloop_apply(int deck, void *p, int idx)
{
     (void)deck; (void)p; (void)idx;
     apply_calls++;   /* reaching here with nothing looping is the destructive
                       * case rbp_bridge.c's BEATLOOP gate exists to prevent */
}

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

/* What rbp answers about its own Beat FX type. The map reads it once, on the
 * first FX SELECT press, to seed its cursor from what is already playing rather
 * than jumping to position 0 -- so this is the stub that pins the seeding, and
 * it is the reason the read goes through the bridge instead of through
 * ADDR_GET_BFX_TYPE (an unmapped address here is a segfault, not a failed
 * check).
 *
 * Type 7 is switch position 3 in rbp_abi.h's table. It is deliberately NOT a
 * type whose position is 0: the seeded first press then reads 4, where the
 * unseeded one would read 1, so the row fails if the map stops asking rbp. The
 * fixture's first FX SELECT line and the first K_BFXTYPE row spell out that
 * 3 + 1. */
int rbp_beatfx_type(void)
{
     return 7;
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

/* +SHIFT + a HOT CUE pad: the one gesture on this surface that is not a
 * keycode, and the only map case that calls into rbp without sending one. The
 * stub records the pair rather than doing anything -- what the map owes rbp is
 * the right (deck, pad), and the delete itself is rbp's, driven on the unit by
 * the pad key the map already sends with rbp's pad-handler selector set
 * (rbp_bridge.c's hotcue_delete()). plinn() above returns NULL, so the live
 * function would refuse every call; the map must still ask. */
static int hcd_calls;
static int hcd_deck[8], hcd_pad[8];
int hotcue_delete(int deck, int pad)
{
     if (hcd_calls < 8) {
          hcd_deck[hcd_calls] = deck;
          hcd_pad[hcd_calls] = pad;
     }
     hcd_calls++;
     return 0;
}

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
     /* The four pad-mode buttons, then one pad in each of the four bases they
      * select: HOT CUE pad 1 (base 0), and one in each of the other three bases
      * the unit's own pad modes carry -- base 32, base 16 and base 48. The bases
      * are the UNIT's, so they follow its labels; the KEYCODES are positional and
      * follow the RX3's, which is why the 3rd button (the unit's BEAT JUMP) sends
      * K_SLIPLOOP and the 4th (its SAMPLER) sends K_BEATJUMP. Both on the deck's
      * pad channel, both to sch 1. */
     { 0, K_HOTCUE,         OP_PRESS,   1, 0,    0.0f, 0, 0 },
     { 0, K_HOTCUE,         OP_RELEASE, 1, 0,    0.0f, 0, 0 },
     { 0, K_ALOOP,          OP_PRESS,   1, 0,    0.0f, 0, 0 },
     { 0, K_ALOOP,          OP_RELEASE, 1, 0,    0.0f, 0, 0 },
     { 0, K_BEATJUMP,       OP_PRESS,   1, 0,    0.0f, 0, 0 },
     { 0, K_BEATJUMP,       OP_RELEASE, 1, 0,    0.0f, 0, 0 },
     { 0, K_PAD1,           OP_PRESS,   1, 0,    0.0f, 0, 0 },
     { 0, K_PAD1,           OP_RELEASE, 1, 0,    0.0f, 0, 0 },
     { 0, K_PAD1 + 7,       OP_PRESS,   1, 0,    0.0f, 0, 0 },
     { 0, K_PAD1 + 7,       OP_RELEASE, 1, 0,    0.0f, 0, 0 },
     { 0, K_PAD1,           OP_PRESS,   1, 0,    0.0f, 0, 0 },
     { 0, K_PAD1,           OP_RELEASE, 1, 0,    0.0f, 0, 0 },
     { 0, K_PAD1 + 7,       OP_PRESS,   1, 0,    0.0f, 0, 0 },
     { 0, K_PAD1 + 7,       OP_RELEASE, 1, 0,    0.0f, 0, 0 },
     { 0, K_SLIPLOOP,       OP_PRESS,   1, 0,    0.0f, 0, 0 },
     { 0, K_SLIPLOOP,       OP_RELEASE, 1, 0,    0.0f, 0, 0 },
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
     /* SHIFT, CUE/LOOP CALL > and a +SHIFT note all reach no keycode: they are in
      * the table with key 0. */
     /* CH CUE produces no keycode at all -- the engine call is asserted in
      * check_state() -- and neither does the +SHIFT pad layer, nor a pad in a
      * mode rbp has no equivalent for. The fixture's base-64 pad is that last
      * assertion: it sits immediately past the four bases this map DOES bind, so
      * a base added by mistake, or a fifth one creeping in, silences a line that
      * is already expected to be silent rather than shifting the whole table. */

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
     /* SHIFT + LOAD, and the two rows are deliberately different shapes.
      * SHORTCUT (0x0210) is a plain press/release like every row above it.
      * TRACK FILTER (0x420f) carries the extra OP_REPEAT edge that rbp's
      * UiKey_Filter needs before it will open its panel -- three sends from two
      * fixture lines. rbp_abi.h's OP_REPEAT block has the derivation; the short
      * version is that state 2 (what a press produces) is measurably refused and
      * only op 1 produces the state 3 that opens it. A reader who "tidies" the
      * K_TRACKFILTER row down to two sends is asserting the opposite of what the
      * unit measured, and this CHECK is where that shows up. */
     { 0, K_TRACKFILTER,    OP_PRESS,   1, 0,    0.0f, 0, 0 },
     { 0, K_TRACKFILTER,    OP_REPEAT,  1, 0,    0.0f, 0, 0 },
     { 0, K_TRACKFILTER,    OP_RELEASE, 1, 0,    0.0f, 0, 0 },
     { 0, K_SHORTCUT,       OP_PRESS,   1, 0,    0.0f, 0, 0 },
     { 0, K_SHORTCUT,       OP_RELEASE, 1, 0,    0.0f, 0, 0 },
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
     /* BEAT < / > are rbp's beat-fraction keys, sent as PRESS pulses (+-1). */
     { 2, K_BEATPREV,       OP_PRESS,   1, -1,   0.0f, -1, 0 },
     { 2, K_BEATNEXT,       OP_PRESS,   1, 1,    0.0f, 1, 0 },
     /* LEVEL/DEPTH, bound on both candidate channels because the vendor table
      * disagrees with itself about which one the knob uses. */
     { 1, K_DEPTH,          OP_VALUE,   1, 516,  516.0f / 1023.0f, 0, 0 },
     { 1, K_DEPTH,          OP_VALUE,   1, 0,    0.0f, 0, 0 },
     /* The FX CH SELECT lever: CH1 -> target 0, CH2 -> target 1, CH1&CH2 ->
      * MASTER (5), and then back to CH2 alone -> target 1 again -- a real move,
      * even though the leg it ends on is the one it started from, because the
      * position in between was CH1&CH2 and rbp was told MASTER for it. The last
      * two lines of the fixture's group (leg B off, then the always-OFF note)
      * send nothing. */
     { 0, K_BFXCH,          OP_VALUE,   1, 0,    0.0f, 0, 0 },
     { 0, K_BFXCH,          OP_VALUE,   1, 1,    0.0f, 0, 0 },
     { 0, K_BFXCH,          OP_VALUE,   1, BFX_CH_MASTER, 0.0f, 0, 0 },
     { 0, K_BFXCH,          OP_VALUE,   1, 1,    0.0f, 0, 0 },

     /* FX SELECT, whose run is the last block of the fixture. rbp's effect switch
      * has 14 positions and the unit's FX SELECT is a button, so the map keeps
      * the position and steps it -- and it seeds that position on the first
      * press from the type rbp is already playing, so the button carries on from
      * the current effect rather than jumping to 0. The stub above answers type
      * 7, which rbp_abi.h's table makes position 3, so the first press reads 4.
      * Note 99 steps forward, SHIFT+FX SELECT (note 100) backward, and the
      * switch wraps at both ends rather than clamping. */
     { 0, K_BFXTYPE,        OP_VALUE,   1, 4,    0.0f, 0, 0 },  /* seeded: 3 + 1 */
     { 0, K_BFXTYPE,        OP_VALUE,   1, 5,    0.0f, 0, 0 },
     { 0, K_BFXTYPE,        OP_VALUE,   1, 4,    0.0f, 0, 0 },  /* SHIFT: back */
     { 0, K_BFXTYPE,        OP_VALUE,   1, 3,    0.0f, 0, 0 },
     { 0, K_BFXTYPE,        OP_VALUE,   1, 2,    0.0f, 0, 0 },
     { 0, K_BFXTYPE,        OP_VALUE,   1, 1,    0.0f, 0, 0 },
     { 0, K_BFXTYPE,        OP_VALUE,   1, 0,    0.0f, 0, 0 },
     { 0, K_BFXTYPE,        OP_VALUE,   1, 13,   0.0f, 0, 0 },  /* 0 - 1 wraps */
     { 0, K_BFXTYPE,        OP_VALUE,   1, 0,    0.0f, 0, 0 },  /* 13 + 1 wraps */
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

     /* +SHIFT + a HOT CUE pad deletes that pad's cue. The fixture carries one
      * press on each deck's shifted pad channel -- deck 1 pad 1 (ch 8 note 0)
      * and deck 2 pad 4 (ch 10 note 3) -- and those two lines were previously
      * listed as "must produce NOTHING". They still produce nothing in sent[],
      * because the gesture is not a keycode, so the assertion lives here; the
      * pairing is what matters, since a row that fired for the wrong pad would
      * delete the wrong cue. */
     CHECK(hcd_calls == 2, "the +SHIFT pads called hotcue_delete() %d times, "
           "expected 2 (deck 1 pad 1, deck 2 pad 4)", hcd_calls);
     CHECK(hcd_calls != 2 || (hcd_deck[0] == 0 && hcd_pad[0] == 1 &&
                              hcd_deck[1] == 1 && hcd_pad[1] == 4),
           "the +SHIFT pads called hotcue_delete(%d,%d) then (%d,%d), expected "
           "(0,1) then (1,4) -- deck 1 pad 1 and deck 2 pad 4",
           hcd_deck[0], hcd_pad[0], hcd_deck[1], hcd_pad[1]);

     /* The map must not touch the headphone stereo type: rbp_vu.c's vu_thread()
      * asserts it for every target, meters or not. */
     CHECK(stereo_calls == 0,
           "the map called me_set_stereo() %d times; the meter thread owns that "
           "setting", stereo_calls);
}

/* MASTER LEVEL, the one control whose whole purpose is that it reaches NOTHING
 * in rbp. That is what makes it a check of its own rather than a row in
 * check_state(): everything it does is invisible in `sent[]`, so a map that had
 * quietly also sent K_MASTERLVL would look perfect from the keycode stream, and
 * the failure would be a master attenuated twice -- once in the samples the Pi
 * feeds the FLX4, again in the unit's own output stage.
 *
 * The numbers are spelled out rather than taken from map_flx4.c's CH_MIX and
 * CC_MASTER_* names, because they are MEASUREMENTS (list ch 7 = 0-based 6, CC 8
 * MSB + CC 40 LSB, 2026-09-27) and a #define that drifted would take the test
 * with it -- which is the one thing this test exists to notice.
 *
 * The position is then mapped by flx4_mastervol_gain(), and the expectations
 * below spell that law's arithmetic out rather than calling it, for the same
 * reason the CC numbers are spelled out: a change to the law has to be a
 * deliberate change to this test too. The law is
 *
 *      gain = (pos / 16383) / 0.6,  clamped at 1.0
 *
 * -- unity at ONE O'CLOCK, flat above it, and a linear ramp below. The knee has
 * moved twice, both times on the operator's ear, and both times as an edit here
 * rather than a silent retune:
 *
 *   - 2026-09-27, first: unity at the stop left the mirror too quiet to use, so
 *     it moved to 0.5 -- the middle of the range, -6 dB at the knob's working
 *     point.
 *   - 2026-09-27, then: the operator asked for the top of the useful travel to
 *     be 1 o'clock rather than 12. The knob's travel is the whole raw range (its
 *     own MIDI dump shows both stops pinning, at raw 0 and raw 16383) and 12
 *     o'clock is raw 0.5, so 1 o'clock is 0.60-0.61 of the rotation;
 *     flx4_mastervol_gain() carries the derivation.
 *
 * So the cases below sit mostly in the ramp, where the position is injective and
 * a wrong decode is visible, and **nothing is asserted equal any more**: at 0.6
 * the middle of the range is 0.833, deliberately short of full level. What is
 * pinned instead is where unity STARTS -- raw 9829.8 -- with 9829 one count short
 * of it and 9830 the first position to reach it.
 *
 * What is NOT covered, said plainly because the rest of this file pins what it
 * claims:
 *   - the "LSB before any MSB" guard. The fixture's own MSB is dispatched before
 *     this runs, and the high-half flag is a file-static in map_flx4.c with no
 *     reset, so by here it is already set. The guard still earns its place -- it
 *     is what stops a stray CC 40 inventing a position out of nothing -- but the
 *     suite proves it exists, not that it fires.
 *   - MIRROR_GAIN_MID, and the fallback for a degenerate one. Both are read in
 *     flx4_build(), which this file deliberately does not call (the same reason
 *     flx4_startup() is out of scope), so only the default 0.6 is exercised here.
 */
static int gain_is(float got, double want)
{
     double d = (double)got - want;

     return (d < 0.0 ? -d : d) < 1e-6;
}

/* A CONTROLLER event, built through the dump format rather than by hand: the
 * map consumes exactly what mididump_parse() produces, and constructing the
 * struct directly would let this test pass against a shape the surface never
 * sends. */
static void send_cc(int ch, int num, int val)
{
     char line[64];
     struct snd_seq_event ev;

     snprintf(line, sizeof line, "0.000000 CONTROLLER ch=%d cc=%d val=%d\n",
              ch, num, val);
     CHECK(mididump_parse(line, &ev) == 1, "could not build \"%s\"", line);
     dispatch(&ev);
}

static void check_mastervol(void)
{
     int base = sent_n;
     float held;

     /* Start from a value the fixture cannot have set. The fixture's single
      * MASTER LEVEL line is an MSB with no LSB, which is exactly the unit's
      * behaviour at connect (measured: the knob's high half is announced, the
      * low half only arrives when it moves), so the gain must still be at its
      * initial 1.0 -- set explicitly so the assertion below is about the map and
      * not about what ran before it. */
     g_mirror_gain = 1.0f;

     /* Half an update is not a position. The MSB alone says nothing about where
      * the knob is, so neither it nor a later one may move the gain.
      *
      * Which also means an MSB arriving changes what the NEXT LSB completes: the
      * high half is held, not buffered per pair. So a pair asserted here has to
      * send its own MSB first -- MSB 127 followed by LSB 0 is 16256, not 0, and
      * that is the map working. */
     send_cc(6, 8, 0);
     CHECK(g_mirror_gain == 1.0f,
           "an MSB with no LSB moved the mirror gain to %.6f, expected 1.0 "
           "(half a 14-bit pair is not a value)", (double)g_mirror_gain);
     send_cc(6, 8, 127);
     CHECK(g_mirror_gain == 1.0f,
           "a second MSB with no LSB moved the mirror gain to %.6f, expected 1.0",
           (double)g_mirror_gain);

     /* Full down: 0 of 16383, and the bottom of the ramp is silence. */
     send_cc(6, 8, 0);
     send_cc(6, 40, 0);
     CHECK(gain_is(g_mirror_gain, 0.0),
           "full down (MSB 0, LSB 0) left the mirror gain at %.6f, expected 0",
           (double)g_mirror_gain);

     /* Up the ramp, MSB first: 16 << 7 = 2048 of 16383, which the law maps to
      * 2048/16383/0.6 = 0.2083460. The MSB on its own must not move it, so the
      * full-down 0 is still standing; that assertion is also what catches a map
      * latching the high half without waiting for its low half. */
     send_cc(6, 8, 16);
     CHECK(gain_is(g_mirror_gain, 0.0),
           "the MSB of a quarter-travel position moved the mirror gain to %.6f "
           "before its LSB arrived", (double)g_mirror_gain);
     send_cc(6, 40, 0);
     CHECK(gain_is(g_mirror_gain, 0.20834604976703494),
           "MSB 16 + LSB 0 left the mirror gain at %.6f, expected 0.208346 "
           "(2048 of 16383)", (double)g_mirror_gain);

     /* Where unity STARTS, which is the whole point of the knee and the one
      * number a retune must move: 16383 x 0.6 = 9829.8, so 9829 (MSB 76, LSB
      * 101) is one count short and 9830 (the same MSB, LSB 102 -- a ONE-COUNT
      * move of the low half alone) is the first position to reach full level.
      *
      * This is the pair that pins the knee. A law that reached 1.0 early, or
      * rounded the product up, passes every other case in this function. */
     send_cc(6, 8, 76);
     send_cc(6, 40, 101);
     CHECK(gain_is(g_mirror_gain, 0.9999186148243099),
           "one count below the knee (9829 of 16383) left the mirror gain at "
           "%.9f, expected 0.999918615 -- unity starts at 9829.8, not "
           "somewhere before it", (double)g_mirror_gain);
     send_cc(6, 40, 102);
     CHECK(g_mirror_gain == 1.0f,
           "the first position at the knee (9830 of 16383, its LSB alone moving "
           "one count up from 9829) left the mirror gain at %.9f, expected "
           "exactly 1.0", (double)g_mirror_gain);

     /* And the middle of the range is now BELOW full level -- 64 << 7 = 8192 of
      * 16383 reads 0.8333842, about -1.6 dB. That is the change the operator
      * asked for on 2026-09-27 (1 o'clock is the top of the useful travel, not
      * 12), and asserting it as a value rather than as a description is what
      * stops a later edit from quietly restoring the middle-as-unity law this
      * function used to assert. */
     send_cc(6, 8, 64);
     send_cc(6, 40, 0);
     CHECK(gain_is(g_mirror_gain, 0.8333841990681398),
           "the middle of the range (MSB 64, LSB 0 = 8192 of 16383) left the "
           "mirror gain at %.6f, expected 0.833384 -- the middle is deliberately "
           "not unity any more; the knee is at 1 o'clock", (double)g_mirror_gain);

     /* Above the knee it is flat, so the stop is worth no more than the knee:
      * both are exactly 1.0, never "1.0 and a bit over". The law itself has no
      * boost anywhere in it -- the room's knob stays a 0..1 attenuator, which is
      * the feel the operator confirmed. The mirror's level above unity is not
      * this function's; it is RB_AUDIO_MIRROR_BOOST_DB, multiplied in underneath
      * (audioshim.c), and it can be past 1.0 only because the mirror's samples
      * are saturated at the gain -- so a lifted sample that runs out of headroom
      * CLIPS instead of folding into the distortion S4.6 measured. */
     send_cc(6, 8, 95);
     send_cc(6, 40, 127);
     CHECK(g_mirror_gain == 1.0f,
           "three-quarters of the range (MSB 95, LSB 127 = 12287 of 16383) left "
           "the mirror gain at %.6f, expected 1.0 -- above the knee is flat",
           (double)g_mirror_gain);
     send_cc(6, 8, 127);
     send_cc(6, 40, 127);
     CHECK(g_mirror_gain == 1.0f,
           "full up (MSB 127, LSB 127 = 16383 of 16383) left the mirror gain at "
           "%.6f, expected 1.0 -- the same as the knee, so cranking the knob past "
           "1 o'clock buys nothing", (double)g_mirror_gain);

     /* And the shape the recording is actually full of: a LONE LSB, the low half
      * repeated with no MSB behind it (58 such lines in the measured dump). It
      * must combine with the high half already held -- 2048 | 64 = 2112, a move
      * of 64 counts -- not be read as a position of its own. A map that took the
      * LSB for the value would read 64/127 as the position and print 0.840 here;
      * one that clamped it would pin 1.0. Either is a mute or a blast out of the
      * HDMI while the room is playing.
      *
      * Deliberately done back down in the ramp. Above the knee the law
      * saturates, where every value is 1.0 and this case would prove nothing. */
     send_cc(6, 8, 16);
     send_cc(6, 40, 0);
     send_cc(6, 40, 64);
     CHECK(gain_is(g_mirror_gain, 0.21485686382225477),
           "a lone LSB (64) left the mirror gain at %.6f, expected 0.214857 "
           "(2112 of 16383 -- the low half combined with the high half held, not "
           "a position of its own, which would be 0.840)", (double)g_mirror_gain);

     /* Nothing else moves it: the CCs on either side of the pair, and the same
      * CC 40 on the Beat FX channel the recording also carries traffic on. */
     held = g_mirror_gain;
     send_cc(6, 9, 127);
     send_cc(6, 39, 0);
     send_cc(4, 40, 64);
     CHECK(g_mirror_gain == held,
           "a neighbour CC moved the mirror gain from %.6f to %.6f; only ch 6 "
           "CC 8/40 is MASTER LEVEL", (double)held, (double)g_mirror_gain);

     /* The two properties the mechanism exists for.
      *   - no rbp keycode, so the unit's own output stage is the only thing the
      *     knob attenuates;
      *   - g_master_gain untouched, which is the difference between a mirror
      *     that tracks the room and a master that is attenuated twice. */
     CHECK(sent_n == base,
           "MASTER LEVEL sent %d rbp keycodes; it must reach g_mirror_gain and "
           "nothing else", sent_n - base);
     CHECK(g_master_gain == 1.0f,
           "MASTER LEVEL left g_master_gain at %.6f; driving rbp's master level "
           "from this knob would attenuate the master twice",
           (double)g_master_gain);
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

/* ==========================================================================
 * The LED table.
 *
 * Be exact about what this can say, because the feature it guards is only
 * visible by eye. Nothing here can see an LED light. What it pins is the table's
 * SHAPE and the one decision that has a wrong answer:
 *
 *   - an unmeasured row must be -1, which sends nothing. That is what lets the
 *     bridge be switched on before the note numbers are known, and it is why a
 *     GUESSED note fails the suite instead of lighting the wrong control;
 *   - a row that is not -1 must be a legal MIDI note on a legal channel, so a
 *     half-filled row cannot transmit something out of range;
 *   - no row may carry a note the SC Live 4 already uses, because the JP21
 *     numbers driven at this panel land on the Beat FX section -- notes 16/17
 *     there are the FX CH SELECT legs -- so that mistake is not a dark LED, it
 *     is a phantom control.
 *
 * The third needs the previous target's table, which is why map_jp21.o is linked
 * into this suite. Comparing the two tables is the only mechanical check that
 * can catch a copy-paste from map_jp21.c, and it is worth the extra object.
 *
 * Be exact about what this suite covers today, because a check that iterates
 * over nothing is indistinguishable in the output from one that passed. The two
 * row loops below used to have no FLX4 rows to walk at all; they walk now that
 * the pads are wired, so the coverage is real on this side -- and still a
 * tautology on the SC Live 4's, where the loops' only job is to be the notes the
 * FLX4's must not land on. What runs either way is that both maps publish a
 * table, that the SC Live 4's flattened to something, and the pad pins.
 * ========================================================================== */
static void check_leds(void)
{
     const struct led_notes *n = map_flx4.leds;
     const struct led_notes *j = map_jp21.leds;
     /* (channel, note) pairs this surface sends, flattened so the comparison
      * below is one loop rather than a field-by-field argument. A -1 row is not
      * recorded at all: it is not transmitted, so it cannot collide with
      * anything, and the range checks below are about what IS transmitted.
      *
      * The capacity is not slack, it is the count: the FLX4 is the larger of the
      * two at 9 deck LEDs x 2 + 8 pads x 4 MODES x 2 + 2 strips + 1 master = 89,
      * and the SC Live 4 at 9 x 2 + 8 x 2 + 2 + 2 = 42. It was 64 until the pads
      * were wired -- 18 + 64 + 3 = 85 -- and PAIR, which used to write whatever
      * count it was given, walked off the end into `p` and made the collision
      * check below compare the FLX4's rows against the FLX4's own rows. The
      * failure it produced named real notes on channel 4 -- a channel this
      * surface does not use -- which is the tell that the list, not the map, was
      * wrong. */
     #define LED_ROWS_MAX 128
     struct pair { int sch, note; } f[LED_ROWS_MAX], p[LED_ROWS_MAX];
     int nf = 0, np = 0;

     CHECK(n != NULL, "the FLX4 map publishes no LED table");
     CHECK(j != NULL, "the SC Live 4 map publishes no LED table");
     if (!n || !j)
          return;

     /* Written out rather than looped over the struct, because the fields are
      * named and a loop would index them by position -- the coupling the field
      * names exist to remove.
      *
      * The channel bases are guarded here exactly as rbp_led.c guards them
      * (ctrl_map.h says -1 means "this surface has no such LED"): a -1 base must
      * not be allowed to arithmetically land on channel 0 for deck 2, which is a
      * real channel on both surfaces. */
     /* The parameters are deliberately NOT named sch/note: the preprocessor
      * substitutes inside the whole body, so a parameter sharing a name with the
      * struct member rewrites `(vec)[cnt].sch` into a computed member access --
      * which GCC accepts for a plain argument and rejects for an expression, so
      * the mistake shows up on the JP21 half of the loop rather than the FLX4
      * one. The trailing underscores are the whole reason this compiles. */
     /* The bounds guard is the point of the macro, not decoration: it counts
      * every row but records only what fits, so an overflow leaves the count
      * above LED_ROWS_MAX and the CHECK after the fills reports it -- instead of
      * writing into `p` and turning the collision check into a comparison of the
      * FLX4's rows against themselves, which is what happened when the pads took
      * this surface past the old 64 and read as a map bug. */
     #define PAIR(vec, cnt, ch_, nt_) do {                        \
          if ((ch_) >= 0 && (nt_) >= 0) {                         \
               if ((cnt) < LED_ROWS_MAX) {                        \
                    (vec)[(cnt)].sch = (ch_);                     \
                    (vec)[(cnt)].note = (nt_);                    \
               }                                                  \
               (cnt)++;                                           \
          }                                                       \
     } while (0)

     for (int d = 0; d < 2; d++) {
          int sch = (n->deck_ch < 0) ? -1 : n->deck_ch + d;
          PAIR(f, nf, sch, n->n_sync);
          PAIR(f, nf, sch, n->n_cue);
          PAIR(f, nf, sch, n->n_play);
          PAIR(f, nf, sch, n->n_keylock);
          PAIR(f, nf, sch, n->n_vinyl);
          PAIR(f, nf, sch, n->n_slip);
          PAIR(f, nf, sch, n->n_loopin);
          PAIR(f, nf, sch, n->n_loopout);
          PAIR(f, nf, sch, n->n_autoloop);
          /* All four bases, because this surface re-addresses its pads per mode
           * and every one of them is a note it really sends. A collision with
           * the other target's rows is what this list is for, and a base left
           * out of it would be a base left unchecked. */
          if (n->pad_ch >= 0 && n->n_pad_base[0] >= 0) {
               int pch = d ? n->pad_ch2 : n->pad_ch;
               for (int pm = 0; pm < 4; pm++)
                    for (int pd = 0; pd < 8; pd++)
                         PAIR(f, nf, pch, n->n_pad_base[pm] + pd);
          }
     }
     for (int m = 0; m < n->strip_count; m++)
          PAIR(f, nf, n->strip_ch_first + m, n->n_strip_cue);
     for (int k = 0; k < n->master_ch_count; k++)
          PAIR(f, nf, n->master_ch_first + k, n->n_master_cue);
     for (int g = 0; g < LED_FX_COUNT; g++)
          PAIR(f, nf, n->fx_ch, n->n_fx[g]);

     for (int d = 0; d < 2; d++) {
          int sch = (j->deck_ch < 0) ? -1 : j->deck_ch + d;
          PAIR(p, np, sch, j->n_sync);
          PAIR(p, np, sch, j->n_cue);
          PAIR(p, np, sch, j->n_play);
          PAIR(p, np, sch, j->n_keylock);
          PAIR(p, np, sch, j->n_vinyl);
          PAIR(p, np, sch, j->n_slip);
          PAIR(p, np, sch, j->n_loopin);
          PAIR(p, np, sch, j->n_loopout);
          PAIR(p, np, sch, j->n_autoloop);
          /* Base 0 only: this surface's pads keep their notes across modes, so
           * all four entries are the same number and listing them four times
           * would be the same 16 rows repeated -- and this list's whole job is
           * to be the other target's notes, once each. */
          if (j->pad_ch >= 0 && j->n_pad_base[0] >= 0) {
               int pch = d ? j->pad_ch2 : j->pad_ch;
               for (int pd = 0; pd < 8; pd++)
                    PAIR(p, np, pch, j->n_pad_base[0] + pd);
          }
     }
     for (int m = 0; m < j->strip_count; m++)
          PAIR(p, np, j->strip_ch_first + m, j->n_strip_cue);
     for (int k = 0; k < j->master_ch_count; k++)
          PAIR(p, np, j->master_ch_first + k, j->n_master_cue);
     for (int g = 0; g < LED_FX_COUNT; g++)
          PAIR(p, np, j->fx_ch, j->n_fx[g]);

     #undef PAIR

     /* Neither list may have filled its array, or the guards above have been
      * dropping rows and the two loops below are walking a truncated one. Raised
      * with the row count so the fix is arithmetic rather than a guess. */
     CHECK(nf <= LED_ROWS_MAX && np <= LED_ROWS_MAX,
           "the flattened LED lists overflowed: the FLX4 sends %d rows and the "
           "SC Live 4 %d, and LED_ROWS_MAX is %d", nf, np, LED_ROWS_MAX);

     /* The previous target's table has to be non-empty, or the comparison below
      * is a tautology. If this fails, the JP21 move has gone wrong and
      * test_midi.c's exact pins name the row. */
     CHECK(np > 0, "the SC Live 4 table flattened to no rows at all");

     /* A row that IS sent must name a real note on a real channel. Every row is
      * -1 today; this is the check that has to keep holding as they are filled
      * in, and it is the one that catches a half-filled row. */
     for (int i = 0; i < nf; i++) {
          CHECK(f[i].note >= 0 && f[i].note <= 127,
                "LED row %d names note %d, outside 0..127", i, f[i].note);
          CHECK(f[i].sch >= 0 && f[i].sch <= 15,
                "LED row %d (note %d) is sent on channel %d, outside 0..15",
                i, f[i].note, f[i].sch);
     }

     /* And it must not be one of the previous target's, on a channel that target
      * uses. This assertion exists for the operator's sake rather than mine: a
      * wrong note is not a dark LED on this unit, it is a phantom control, and
      * the two are indistinguishable from the log. */
     for (int i = 0; i < nf; i++) {
          for (int k = 0; k < np; k++) {
               CHECK(!(f[i].sch == p[k].sch && f[i].note == p[k].note),
                     "the FLX4 sends note %d on channel %d, which is the SC "
                     "Live 4's number for a different control -- this unit's own "
                     "note is not measured yet and must stay -1",
                     f[i].note, f[i].sch);
          }
     }

     /* The encoding must be one this build implements. An unknown value falls
      * through to a plain on in rbp_led.c, which is survivable for a panel whose
      * pads are not RGB but is not what a pad row means to say. */
     CHECK(n->pad_enc == LED_ENC_NONE || n->pad_enc == LED_ENC_PRIME_6BIT,
           "the FLX4's pad encoding is %d, which is not one this build has",
           n->pad_enc);
     /* And it must never claim Engine OS's colours. That is settled rather than
      * pending: this unit's pads are not RGB, so the encoding it wants is NONE
      * (Pioneer's list gives all 350 LED rows as OFF=0x00/ON=0x7F), and a row
      * changed to PRIME_6BIT would be borrowing a neighbouring surface's colour
      * layout for a panel that has no colour to show. */
     CHECK(n->pad_enc != LED_ENC_PRIME_6BIT,
           "the FLX4 claims Engine OS pad colours, but its pads have no colour "
           "(Pioneer's list: every row is OFF=0x00, ON=0x7F)");

     /* Pad rows, now that this surface lights them. `0` is a legal base, so the
      * zero-init trap ctrl_map.h warns about reads as "HOT CUE pads on notes
      * 0..7" rather than as a surface with no pads -- a row that simply forgot
      * the array would light eight notes it never chose and pass every other
      * check here. And the two channels must be named and different: both decks
      * on one channel is what `pad_ch + deck` would have produced for the FLX4,
      * where deck 2's pads would have landed on deck 1's shift channel. */
     for (int m = 0; m < 4; m++)
          CHECK(n->n_pad_base[m] >= 0 && n->n_pad_base[m] + 7 <= 127,
                "the FLX4's pad base for mode %d is %d: 0 is a legal base, so an "
                "omitted array reads as a real one, and base+7 must stay a note",
                m, n->n_pad_base[m]);
     CHECK(n->pad_ch >= 0 && n->pad_ch2 >= 0 && n->pad_ch != n->pad_ch2,
           "the FLX4's pad channels are %d and %d; both must be named and "
           "different", n->pad_ch, n->pad_ch2);
     /* And the four bases must be four DIFFERENT values, because that is the
      * claim: the unit moves its pads when the mode button is pressed. One value
      * repeated would light the same eight notes in every mode -- which is what
      * a surface that does NOT re-address its pads looks like, and then the
      * clearing of the previous base in rbp_led.c would be darking the notes it
      * is about to light again, every single tick. */
     for (int m = 1; m < 4; m++)
          CHECK(n->n_pad_base[m] != n->n_pad_base[m - 1],
                "the FLX4's pad bases for modes %d and %d are both %d, but this "
                "unit re-addresses its pads per mode (measured 2026-10-01)",
                m - 1, m, n->n_pad_base[m]);

     /* A count with no channel base is the one shape that sends on a channel
      * nobody chose. rbp_led.c computes the strip channel as base + m, so a -1
      * base means strip 1 lands on channel 0 -- a real channel on every surface --
      * and once a strip note has been measured the -1 note guard no longer covers
      * it. The bridge clamps the loop; this is what stops a table being written
      * that way in the first place. */
     CHECK(n->strip_count == 0 || n->strip_ch_first >= 0,
           "the FLX4 says it has %d strips but its channel base is %d: strip 1 "
           "would be sent on channel 0", n->strip_count, n->strip_ch_first);
     CHECK(n->master_ch_count == 0 || n->master_ch_first >= 0,
           "the FLX4 says it sends master CUE on %d channels but its channel base "
           "is %d", n->master_ch_count, n->master_ch_first);
     /* The same question for the array the state is kept in: LED_STRIP_MAX in
      * rbp_led.c is a bound on that array, and a table above it is a write past
      * the end of a module the tests do not link. Spelled out as a literal rather
      * than included from rbp_led.c, for the same reason the rest of this file is:
      * the test must not import the thing it is checking. */
     CHECK(n->strip_count >= 0 && n->strip_count <= 2,
           "the FLX4's strip_count is %d, above the 2 strips the bridge keeps "
           "state for", n->strip_count);
     CHECK(n->master_ch_count >= 0 && n->master_ch_count <= 16,
           "the FLX4's master_ch_count is %d, which is not a channel count",
           n->master_ch_count);

     /* ---- the meter row ----------------------------------------------------
      *
      * A different kind of claim from every check above: the note rows are
      * MEASURED on the unit, this one is PUBLISHED (Pioneer's list item 3-15),
      * because the unit never sends its meter and so there is nothing to read
      * back. What can be checked mechanically is that the row is complete, in
      * range, and not the other panel's address. */

     /* Complete: a CC with no channel base is the meter version of the strip
      * trap above -- meter_ch_first + 0 would be channel 0, a real channel, and
      * the level would go out on somebody else's deck. */
     CHECK(n->n_meter_cc < 0 || n->meter_ch_first >= 0,
           "the FLX4 says its meter is CC %d but its channel base is %d: deck 1 "
           "would be sent on channel 0", n->n_meter_cc, n->meter_ch_first);

     if (n->n_meter_cc >= 0) {
          CHECK(n->n_meter_cc <= 127,
                "the FLX4's meter CC is %d, outside 0..127", n->n_meter_cc);
          CHECK(n->meter_ch_first + 1 <= 15,
                "the FLX4's meter base is channel %d, so deck 2 would be sent on "
                "channel %d", n->meter_ch_first, n->meter_ch_first + 1);
          /* An encoding this build does not implement makes vu_encode() send
           * nothing at all, which reads at the panel exactly like a meter that
           * is wired but silent -- the failure this suite exists to catch. */
          CHECK(n->meter_enc == METER_ENC_PRIME_SEGMENTS ||
                n->meter_enc == METER_ENC_FLX4_LEVEL,
                "the FLX4's meter encoding is %d, which is not one this build "
                "has", n->meter_enc);
          /* Published, and the number in the document: deck 1 on CC 2. Pinned
           * exactly rather than range-checked, because a meter that lights at
           * the wrong address on the FLX4 lands on the deck's own controls. */
          CHECK(n->n_meter_cc == 2 && n->meter_ch_first == 0,
                "the FLX4's meter row is CC %d on channel base %d; Pioneer's "
                "list gives deck 1 `B0 02 hh`",
                n->n_meter_cc, n->meter_ch_first);
          /* PRE-fader, from the manual's own words ("before it passes through
           * the channel faders") and matching rbp. Applying the fader here
           * would duck a meter the operator's own panel does not duck. */
          CHECK(n->meter_pre_fader == 1,
                "the FLX4's meter is declared post-fader, but both rbp and this "
                "unit meter before the channel fader");
     }

     /* No master meter, and that is a finding rather than a gap: the unit's two
      * meters show channel or master only by the operator's [Level Meter]
      * switch, and Pioneer's list gives no master address. Lighting ch 15 CC
      * 32/33 like the SC Live 4 would be inventing one. */
     CHECK(n->meter_master_ch < 0 && n->n_meter_master_l < 0 &&
           n->n_meter_master_r < 0,
           "the FLX4 claims a master meter at CC %d/%d on channel %d, but the "
           "published list gives it no master meter address",
           n->n_meter_master_l, n->n_meter_master_r, n->meter_master_ch);

     /* And, as for the notes: the meter must not be the SC Live 4's address. The
      * Prime's is CC 10 on the same channel base, so this catches exactly the
      * copy-paste that the old hardcoded bridge was. */
     CHECK(j->n_meter_cc < 0 ||
           n->n_meter_cc != j->n_meter_cc ||
           n->meter_ch_first != j->meter_ch_first,
           "the FLX4's meter is CC %d on channel base %d, which is the SC Live "
           "4's address for its own meter", n->n_meter_cc, n->meter_ch_first);
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

     /* build() is ADDITIVE -- ctrl_bindings_reset() is the front end's, called
      * once before either surface is built -- so every count asserted below is
      * only the map's own rows because this is the first and only build in a
      * virgin process. Building a second surface here without resetting first
      * would fold its rows into these numbers and the failure would read as a
      * wrong count rather than as a missing reset. */
     map_flx4.build();

     /* The tables' sizes, spelled out rather than merely "non-empty": a row that
      * silently falls off the end of add_note()/add_abs() is a control that stops
      * working, and it would otherwise only show up as an unmapped event.
      * Deck 1/2 contribute 13 notes each (11 bound: play, cue, jog touch, loop
      * in, loop out, 4 beat, and the four pad modes hot cue, auto beat loop, beat
      * jump and slip beat loop -- plus 2 log-only: shift and the other call
      * button), 8 pads x 4 bases each, and 5 absolute controls; the mixer
      * contributes 9 notes and 5 knobs, and the Beat FX 2 notes and 2 depth
      * rows. */
     CHECK(note_map_n == 101,
           "the map built %d note rows, expected 101 (2 x (13 + 32) + 9 + 2)",
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
     check_mastervol();
     check_idle();
     check_leds();

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
