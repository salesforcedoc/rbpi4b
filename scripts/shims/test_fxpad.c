/*
 * test_fxpad.c -- the BPM cell as a momentary X/Y pad, with no Pi, no panel and no rbp
 * anywhere in this file.
 *
 * It links the PRODUCTION fxpad_zone.c, for the reason test_fx.c, test_hc.c,
 * test_prompt.c and the rest link theirs: a rectangle boundary, a rounding rule and a
 * "has rbp answered yet" latch are exactly the kind of thing that gets quietly rewritten
 * in a copy.
 *
 * WHAT IS AT STAKE HERE IS NOT SYMMETRIC, and the tests are ordered to say so. This pad
 * WRITES to rbp: it sends an absolute level, climbs rbp's beat ladder and switches the
 * effect on. A pad that fails to fire is a bug report; a pad that fires and then fails to
 * put things back leaves the operator's effect at a level they never set, switched ON,
 * mid-set, and audible. So:
 *
 * 1. THE RESTORE IS EXACT, first -- it is the half of the operator's sentence that costs
 *    something when it is wrong ("when you release set it back to what the beat and level
 *    was before you pressed it"). Pinned as three equalities against a simulated rbp, in
 *    both directions of the ladder, with the on/off toggled exactly twice: once going in
 *    and once coming home.
 *
 * 2. THE TICK IS QUIET WHEN NOTHING HAS CHANGED. A held-still finger is the normal state
 *    of this pad, and it is also what lets the ladder converge -- so a tick that re-sent
 *    the level 50 times a second would be a stream of rbp key events for no reason, and a
 *    tick that re-sent the ON TOGGLE would switch the effect straight back off.
 *
 *    AND IT IS THE ON/OFF THAT IS *WATCHED*, because it is the one axis whose wire form is a
 *    TOGGLE: it can be asked for but never written, so the only honest way to reach it is to
 *    read rbp's own answer back. That read is the thing the operator's bug report of
 *    2026-10-07 landed on -- rbp's `BeatEffect+0x3c` flag reads 1 on a switched-off effect, so
 *    a pad that believed it asked for nothing and did nothing at all ("it doesn't engage").
 *    The fixture below MODELS that lie rather than being honest by construction, and
 *    `test_flag_lies_on_a_switched_off_effect` is the test the fix exists for.
 *
 * 3. THE TWO MAPPINGS, at their ends and across their whole span. The level's ends are
 *    exactly 0 and FXPAD_DEPTH_MAX and the span is monotone, which is what catches a
 *    rounding rule that loses the top column or a Y axis wired upside down. The ladder's
 *    top-of-cell-is-most-beats is pinned by the direction it climbs.
 *
 * 4. THE CLIMB, which is the part that cannot be computed: it steps one rung at a time
 *    on rbp's own answer, so it is pinned (a) from both ends, (b) against a rbp that
 *    applies each step a tick LATE -- the case where a naive implementation sends the
 *    same rung twice and overshoots -- and (c) against a rbp that never answers at all,
 *    where it must GIVE UP rather than send a key forever.
 *
 * 5. THE LATCH, the same safety property fx_zone.c and hc_zone.c keep: a press this
 *    module swallowed has been withheld from rbp for its whole life, so it must keep
 *    being OURS until it lifts, including after it has slid off the cell. And a gesture
 *    that began OUTSIDE the cell is rbp's from its first report to its last -- a finger
 *    that crossed the panel on its way somewhere else must not move the effect.
 *
 * 6. THE GEOMETRY, as pinned numbers and as arithmetic: the cell is inside the BEAT FX
 *    plate and overlaps NONE of fx_zone.h's three rects, so a tap can never be claimed
 *    by two zones at once.
 *
 * 7. THE HUD, which is fxpad_paint.c and the one thing here that is about PIXELS rather
 *    than state. The pad draws a dot over rbp's own cell, and the cell is where rbp prints
 *    the BPM, the msec and the beat count -- live, and changing as the pad moves them. So
 *    the property is not "the mark appears": it is WHICH OF THE OPERATOR'S PIXELS SURVIVE.
 *    Both routes are pinned, because they are opposite -- with a source the destination is
 *    a plane and has to be filled with rbp's pixels first; with no source it IS rbp's
 *    framebuffer and must have only the dot written into it. Getting the second wrong
 *    blacks out the readout the operator is reading.
 *
 *    A frame used to be drawn round the cell here as well. rbp repaints that border itself,
 *    so the two writes strobed, and the operator had it removed on 2026-10-07. The tests
 *    below therefore also pin its ABSENCE: an unmarked paint must now write nothing at all.
 */

#include "fxpad_zone.h"
#include "fxpad_paint.h"    /* the HUD: struct menu_view, and the palette it draws with */
#include "fx_zone.h"        /* the three NEIGHBOURING rects the cell must not overlap */
#include "menu_zone.h"      /* MZ_LOGICAL_W/H: the space the cell must fit in */

#include <stdio.h>
#include <string.h>         /* memset, in the view builders */

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

/* ---------------------------------------------------------------------------
 * A simulated rbp, of the five words the pad reads and writes. It is deliberately
 * naive: it applies what it is told, on the tick it is told, EXCEPT when `lag` or
 * `ignore_beat` says otherwise -- those two are how the late-answer and the plateau
 * cases are staged.
 *
 * `type` IS NOT DECORATION AND IT CARRIES THE LIE. Measured on the unit, 2026-10-07:
 * whenever the effect is switched off rbp points BeatEffectManager+0x08 at a
 * `BeatEffectOff`, a class whose ON/OFF virtual is compiled out, so its raw +0x3c flag
 * sits at 1 while the effect does nothing. The simulation below reproduces exactly that
 * -- `sim_live` answers `on` from the TYPE when the type is Off, not from `s->on` -- which
 * is what makes the tests that follow able to catch a pad that reads the flag alone.
 * They could not, with a fixture that was honest by construction.
 * ------------------------------------------------------------------------- */
#define SIM_TYPE_OFF   0        /* BeatEffectManager+0x50 == 0: the Off effect */
#define SIM_TYPE_REAL 10        /* any real one; the sim does not model WHICH */

struct sim {
    int type;                /* BeatEffectManager+0x50, the word that decides whether
                              * the `on` byte below may be believed at all */
    int on, depth, beat, bmin, bmax;

    /* The ladder answers in the direction it is asked, which is the module's contract;
     * whether K_BEATNEXT really raises BeatEffect+0x44 is Stage B's measurement, not
     * this test's. */
    int lag;                 /* apply a beat step one tick late */
    int ignore_beat;         /* never move off the rung at all */
    int ignore_on;           /* swallow the K_BFX toggle: rbp never flips */

    int queued;

    /* Counting, so "one send" is assertable rather than assumed. */
    int on_sends, depth_sends, beat_sends;
    int depth_last;
};

static void sim_live(const struct sim *s, struct fxpad_live *l)
{
    l->type = s->type;
    /* THE LIE, MODELLED. A BeatEffectOff has no working ON/OFF to maintain its flag, so
     * its +0x3c reads 1 whatever the operator's effect is doing. `s->on` means nothing
     * there -- which is precisely the trap the pad fell into. */
    l->on = (s->type == SIM_TYPE_OFF) ? 1 : s->on;
    l->depth = s->depth;
    l->beat = s->beat;
    l->beat_min = s->bmin;
    l->beat_max = s->bmax;
}

static void move_beat(struct sim *s, int d)
{
    s->beat += d;
    if (s->beat < s->bmin) s->beat = s->bmin;
    if (s->beat > s->bmax) s->beat = s->bmax;
}

static void apply(struct sim *s, const struct fxpad_out *o)
{
    if (s->queued) {                 /* last tick's step, arriving now */
        move_beat(s, s->queued);
        s->queued = 0;
    }
    if (o->want_on >= 0) {
        s->on_sends++;
        if (!s->ignore_on) {
            if (o->want_on) {
                /* rbp's ON makes the active object a REAL one: whatever the type word
                 * said, asking for ON is what clears it. This is the mechanism of the
                 * operator's own workaround -- *"it does after i turn the newly selected
                 * effect on/off"* -- and it is the reason the pad's ask is enough on its
                 * own to bring a switched-off effect back. */
                s->type = SIM_TYPE_REAL;
                s->on = 1;
            } else {
                s->on = 0;
            }
        }
    }
    if (o->depth >= 0) { s->depth = o->depth; s->depth_sends++; s->depth_last = o->depth; }
    if (o->beat_step) {
        s->beat_sends++;
        if (s->ignore_beat)
            ;                        /* a plateau: the key is swallowed */
        else if (s->lag)
            s->queued = o->beat_step;
        else
            move_beat(s, o->beat_step);
    }
}

/* `n` ticks of the pad's state machine against the simulated rbp, exactly as
 * pointsrc.c's tick block will drive it: read rbp, tick, send what came back. */
static void tick_n(struct sim *s, int n)
{
    struct fxpad_live l;
    struct fxpad_out o;
    int i;

    for (i = 0; i < n; i++) {
        sim_live(s, &l);
        fxpad_tick(&l, &o);
        apply(s, &o);
    }
}

/* The same, stopping at the moment the pad goes idle. Returns the ticks spent, so a
 * test can assert that a restore is SHORT as well as correct. */
static int drive_idle(struct sim *s, int max)
{
    struct fxpad_live l;
    struct fxpad_out o;
    int i;

    for (i = 0; i < max && fxpad_busy(); i++) {
        sim_live(s, &l);
        fxpad_tick(&l, &o);
        apply(s, &o);
    }
    return i;
}

/* ---------------------------------------------------------------------------
 * 1. The restore is exact
 * ------------------------------------------------------------------------- */

static void test_restore_exact(void)
{
    struct sim s;
    struct fxpad_live l;
    struct fxpad_out o;
    int ticks, mx, my;

    /* OFF, level 400 of 1023, sitting on rung 5 of a 0..9 ladder -- the live values read
     * off the unit on 2026-10-07 (0.409 of full scale, "1 BEAT"). */
    s.type = SIM_TYPE_REAL;
    s.on = 0; s.depth = 400; s.beat = 5; s.bmin = 0; s.bmax = 9;
    s.lag = 0; s.ignore_beat = 0; s.ignore_on = 0; s.queued = 0;
    s.on_sends = s.depth_sends = s.beat_sends = 0; s.depth_last = -1;

    fxpad_reset();
    sim_live(&s, &l);

    /* A press a little above the cell's vertical centre, a little right of its middle. */
    CHECK(fxpad_feed(1, 1180, 260) == MZ_FEED_TAKEN,
          "a press inside the cell must be taken, or the pad never engages");
    CHECK(fxpad_engaged() == 1, "a press inside the cell must engage");
    CHECK(fxpad_busy() == 1, "an engaged pad is busy");

    fxpad_tick(&l, &o);
    CHECK(o.want_on == 1,
          "the effect was OFF, so the press must turn it on -- a level moved on a "
          "switched-off effect is not audible");
    CHECK(o.depth > 400,
          "the press point must set the level immediately and to the RIGHT of 400 -- "
          "this is an X/Y pad, not a knob (got %d)", o.depth);
    apply(&s, &o);
    CHECK(s.on_sends == 1, "the ON toggle must be sent exactly once, not once a tick");
    CHECK(s.depth_sends == 1, "the press point's level must be sent on the press tick");

    /* Hold the finger still and let the ladder settle. `beat_of_y(260, 0, 9)` is 6. */
    tick_n(&s, 8);
    CHECK(s.beat == 6, "the rung under the finger at y 260 of a 0..9 ladder is 6, got %d",
          s.beat);
    CHECK(s.on_sends == 1, "a held-still finger must not re-send the ON toggle");
    CHECK(s.depth_sends == 1, "a held-still finger must not re-send the level");

    /* ---- let go ---- */
    CHECK(fxpad_feed(0, 1180, 260) == MZ_FEED_TAKEN,
          "the release of a press we swallowed must still be ours");
    CHECK(fxpad_engaged() == 0, "the release ends the engagement");
    CHECK(fxpad_busy() == 1, "the release starts the unwind, so the caller keeps ticking");

    ticks = drive_idle(&s, 200);
    CHECK(fxpad_busy() == 0, "the unwind must finish, not spin");
    CHECK(ticks < 40, "the unwind of a one-rung excursion took %d ticks", ticks);
    CHECK(s.on == 0, "THE EFFECT MUST BE BACK OFF -- it was off before the press");
    CHECK(s.depth == 400, "THE LEVEL MUST BE BACK WHERE IT WAS (400), got %d", s.depth);
    CHECK(s.beat == 5, "THE RUNG MUST BE BACK WHERE IT WAS (5), got %d", s.beat);
    CHECK(s.on_sends == 2, "the effect must be toggled exactly twice: in and back out");

    CHECK(fxpad_mark(&mx, &my) == 0, "an idle pad draws no dot");
}

static void test_restore_of_a_no_op_gesture(void)
{
    struct sim s;

    /* A press that is released without the finger ever moving, at a point where all
     * three values already are what the press asks for (x 1179 is level 508, y 284 is
     * rung 5 of 0..9). Nothing was ever sent, so the tick that finds itself restoring
     * must go idle without sending anything. */
    s.type = SIM_TYPE_REAL;
    s.on = 1; s.depth = 508; s.beat = 5; s.bmin = 0; s.bmax = 9;
    s.lag = 0; s.ignore_beat = 0; s.ignore_on = 0; s.queued = 0;
    s.on_sends = s.depth_sends = s.beat_sends = 0; s.depth_last = -1;

    fxpad_reset();
    CHECK(fxpad_feed(1, 1179, 284) == MZ_FEED_TAKEN, "the press must be taken");
    tick_n(&s, 4);
    CHECK(fxpad_feed(0, 1179, 284) == MZ_FEED_TAKEN, "the release must be taken");
    drive_idle(&s, 100);
    CHECK(fxpad_busy() == 0, "the pad must go idle");
    CHECK(s.on_sends == 0, "an effect already on must not be toggled on the way in");
    CHECK(s.depth_sends == 0, "a gesture that changed no level must send no level");
    CHECK(s.beat_sends == 0, "a gesture that changed no rung must send no rung");
}

static void test_tap_between_two_ticks_sends_nothing(void)
{
    struct sim s;

    /* A flick faster than the 20 ms tick: the press and the release both arrive, and no
     * tick ever saw the pad engaged. Nothing was sent, so there is nothing to put back
     * and nothing to send -- and the pad must not hang busy waiting for a snapshot that
     * was never taken. */
    s.type = SIM_TYPE_REAL;
    s.on = 0; s.depth = 200; s.beat = 3; s.bmin = 0; s.bmax = 9;
    s.lag = 0; s.ignore_beat = 0; s.ignore_on = 0; s.queued = 0;
    s.on_sends = s.depth_sends = s.beat_sends = 0; s.depth_last = -1;

    fxpad_reset();
    fxpad_feed(1, 1180, 260);
    fxpad_feed(0, 1180, 260);
    tick_n(&s, 5);
    CHECK(fxpad_busy() == 0, "a tap that lived between two ticks must not leave it busy");
    CHECK(s.on_sends == 0 && s.depth_sends == 0 && s.beat_sends == 0,
          "a tap no tick ever saw must send NOTHING -- rbp's effect stays untouched");
}

/* ---------------------------------------------------------------------------
 * 2. The quiet tick
 * ------------------------------------------------------------------------- */

static void test_quiet_tick(void)
{
    struct sim s;
    struct fxpad_live l;
    struct fxpad_out o;

    s.type = SIM_TYPE_REAL;
    s.on = 0; s.depth = 400; s.beat = 5; s.bmin = 0; s.bmax = 9;
    s.lag = 0; s.ignore_beat = 0; s.ignore_on = 0; s.queued = 0;
    s.on_sends = s.depth_sends = s.beat_sends = 0; s.depth_last = -1;

    fxpad_reset();
    fxpad_feed(1, 1180, 260);
    sim_live(&s, &l);
    fxpad_tick(&l, &o);
    apply(&s, &o);
    tick_n(&s, 8);

    /* Everything is where the finger asks for it. One more tick must be three no-ops. */
    sim_live(&s, &l);
    fxpad_tick(&l, &o);
    CHECK(o.want_on == -1, "a converged tick must not touch the on/off (got %d)", o.want_on);
    CHECK(o.depth == -1, "a converged tick must not re-send the level (got %d)", o.depth);
    CHECK(o.beat_step == 0, "a converged tick must not step the ladder (got %d)",
          o.beat_step);
}

/* ---------------------------------------------------------------------------
 * 2b. The on/off is asked for AND WATCHED
 *
 * The one axis whose wire form is a TOGGLE, and the one the operator reported broken:
 * *"when i change effects it doesn't seem to remember to trigger when i press until i
 * turn it on off"* (2026-10-07). A single blind toggle is indistinguishable from no
 * toggle at all, so these three tests are what tells the two apart -- a flip rbp did not
 * make is re-asked, a flip it will never make is not asked forever, and an effect it
 * switches off UNDER the finger is asked for again.
 * ------------------------------------------------------------------------- */

static void sim_init(struct sim *s, int on, int depth, int beat)
{
    s->type = SIM_TYPE_REAL;
    s->on = on; s->depth = depth; s->beat = beat; s->bmin = 0; s->bmax = 9;
    s->lag = 0; s->ignore_beat = 0; s->ignore_on = 0; s->queued = 0;
    s->on_sends = s->depth_sends = s->beat_sends = 0; s->depth_last = -1;
}

/* A DROPPED TOGGLE IS ASKED AGAIN, and then let go. rbp swallows every K_BFX here, so
 * the only thing that can end is the bound -- and the pad must still be a pad, holding
 * the level, at the end of it. */
static void test_on_retries_a_dropped_toggle(void)
{
    struct sim s;

    sim_init(&s, 0, 400, 5);
    s.ignore_on = 1;

    fxpad_reset();
    fxpad_feed(1, 1180, 260);
    tick_n(&s, 1);
    CHECK(s.on_sends == 1, "the first ask is on the press tick (got %d)", s.on_sends);

    tick_n(&s, 200);
    CHECK(s.on_sends == FXPAD_ON_TRIES,
          "a toggle rbp never answers is asked FXPAD_ON_TRIES times and no more (got %d)",
          s.on_sends);
    CHECK(s.on_sends > 1, "a single dropped toggle must not leave the press silent");

    /* And the pad is still the pad: still engaged, still holding the level. */
    CHECK(fxpad_engaged() == 1, "giving up on the on/off does not end the gesture");
    CHECK(s.depth_sends > 0, "the level is still being driven while the on/off is dead");

    /* Letting go with rbp still refusing: the restore gives up too rather than spinning,
     * and the effect is left where it is -- rbp's own answer is the only truth here. */
    fxpad_feed(0, 1180, 260);
    CHECK(drive_idle(&s, 400) < 400, "the restore of a refused on/off still finishes");
    CHECK(fxpad_busy() == 0, "and it finishes, rather than holding the pad busy forever");
    CHECK(s.on == 0, "the effect is where rbp left it, not where the shim wished it");
}

/* AN EFFECT rbp SWITCHES OFF UNDER THE FINGER IS ASKED FOR AGAIN. This is the reported
 * case: the effect reads ON at the press, so there is nothing to ask for -- and then it
 * goes off underneath, and a pad that had simply latched "I asked for on" would leave the
 * rest of the gesture silent. */
static void test_on_rearms_mid_hold(void)
{
    struct sim s;

    sim_init(&s, 1, 400, 5);

    fxpad_reset();
    fxpad_feed(1, 1180, 260);
    tick_n(&s, 6);
    CHECK(s.on_sends == 0,
          "an effect that was ALREADY ON is not toggled -- a toggle would switch it off");

    /* The effect goes off underneath: a type change, or rbp's own housekeeping. */
    s.on = 0;
    tick_n(&s, 1);
    CHECK(s.on_sends == 1, "an effect that goes off under the finger is asked for again");
    CHECK(s.on == 1, "and the effect is on again");

    tick_n(&s, 20);
    CHECK(s.on_sends == 1, "once it is back on, it is not asked for a third time");
}

/* THE OPERATOR'S REPORT, 2026-10-07, AND THE ONE TEST THIS WHOLE FIX EXISTS FOR:
 *
 *   *"when i switch an effect and then press the x/y pad it doesn't engage, but it does
 *   after i turn the newly selected effect on/off"*
 *
 * A type change while the effect is off leaves rbp's type word at 0 and swaps in a
 * `BeatEffectOff`, whose raw ON/OFF flag reads 1 because nothing maintains it. So the pad
 * saw "already on", found nothing to ask for, sent no toggle -- and every other send it
 * made, the level and the rung, was a no-op on a class that ignores both. The press did
 * literally nothing. Pressing rbp's own ON/OFF re-points +0x08 at the selected type's REAL
 * object, which is why the operator's workaround worked.
 *
 * The fixture models the lie, so what is pinned here is exactly what a flag-only read gets
 * wrong. Everything below would have passed before the fix EXCEPT the sends, which is the
 * point: the pad was not crashing, it was agreeing with itself.
 */
static void test_flag_lies_on_a_switched_off_effect(void)
{
    struct sim s;
    struct fxpad_live l;

    /* THE CONJUNCTION ITSELF, before any gesture is involved. */
    l.type = SIM_TYPE_OFF;  l.on = 1;
    CHECK(fxpad_live_on(&l) == 0,
          "A LIE IS NOT AN ANSWER: type 0 with the flag at 1 must read OFF");
    l.type = SIM_TYPE_REAL; l.on = 1;
    CHECK(fxpad_live_on(&l) == 1, "a real type with the flag at 1 is on");
    l.on = 0;
    CHECK(fxpad_live_on(&l) == 0, "a real type with the flag at 0 is off");
    CHECK(fxpad_live_on(NULL) == 0, "no state, no answer");

    /* ---- and the gesture: the operator's own two steps ---- */

    /* The effect is ON; the operator switches type while it is off, and rbp answers with
     * the Off object and a flag that says on. */
    sim_init(&s, 1, 400, 5);
    s.type = SIM_TYPE_OFF;

    sim_live(&s, &l);
    CHECK(l.on == 1, "the simulated BeatEffectOff reports the flag at 1, as rbp's does");
    CHECK(fxpad_live_on(&l) == 0, "and the pad reads that as OFF");

    fxpad_reset();
    fxpad_feed(1, 1180, 260);
    tick_n(&s, 1);
    CHECK(s.on_sends == 1,
          "THE PRESS MUST ASK FOR THE EFFECT -- this is the K_BFX that never went out "
          "(got %d sends)", s.on_sends);
    CHECK(s.on == 1, "and the effect is running");
    CHECK(s.type != SIM_TYPE_OFF, "the type word is no longer Off");

    /* Everything else the pad drives now lands on a REAL object. */
    tick_n(&s, 20);
    CHECK(s.depth_sends == 1 && s.beat_sends == 1,
          "with a real effect under the finger the level and the rung move (%d, %d)",
          s.depth_sends, s.beat_sends);
    CHECK(s.beat == 6, "the rung under y 260 of a 0..9 ladder is 6, got %d", s.beat);

    /* And letting go puts the effect back OFF, which is where the operator had it -- the
     * snapshot was taken on the press tick, when it was genuinely off. */
    fxpad_feed(0, 1180, 260);
    CHECK(drive_idle(&s, 400) < 400, "the unwind finishes");
    CHECK(s.on_sends == 2, "one ask in and one back out, got %d sends", s.on_sends);
    CHECK(s.on == 0, "THE RELEASE PUTS IT BACK OFF, where the operator had it");
    CHECK(s.depth == 400, "and the level back to 400, got %d", s.depth);
    CHECK(s.beat == 5, "and the rung back to 5, got %d", s.beat);
}

/* THE RESTORE WAITS FOR THE ANSWER, NOT FOR THE ASK. The level is absolute and the
 * ladder is climbed, but the on/off is the one that used to be declared home the moment
 * it had been sent -- so a dropped OFF toggle left the operator's effect running with the
 * pad already idle, and the `unwound` line reporting a state rbp had never been in. */
static void test_restore_waits_for_the_answer(void)
{
    struct sim s;
    int ticks;

    /* THE EFFECT WAS OFF, so the pad turned it on, so the release owes it an OFF. */
    sim_init(&s, 0, 400, 5);

    fxpad_reset();
    fxpad_feed(1, 1180, 260);
    tick_n(&s, 6);
    CHECK(s.on_sends == 1 && s.on == 1, "the press turned the effect on");

    /* The finger goes, and rbp refuses every OFF that should follow. The restore must
     * WAIT for the answer it asked for, retry a bounded number of times, and then stop --
     * not declare itself home on the tick it sent a toggle nobody applied. */
    s.ignore_on = 1;
    fxpad_feed(0, 1180, 260);
    ticks = drive_idle(&s, 400);

    CHECK(ticks < 400, "a refused on/off restore still ends");
    CHECK(fxpad_busy() == 0, "the pad goes idle rather than busy forever");
    CHECK(s.on_sends == 1 + FXPAD_ON_TRIES,
          "the OFF is asked for FXPAD_ON_TRIES times after the ON, got %d sends",
          s.on_sends);
    CHECK(s.on == 1, "and the effect is left as rbp answered -- on -- not as we wished");
}

/* THE COUNTERPART: when rbp DOES answer, all of that costs one tick and one send. This is
 * the case the operator will actually see, and it is the reason the wait is bounded by
 * rbp's answer rather than by a timer. */
static void test_restore_asks_once_when_rbp_answers(void)
{
    struct sim s;
    int ticks;

    sim_init(&s, 0, 400, 5);

    fxpad_reset();
    fxpad_feed(1, 1180, 260);
    tick_n(&s, 6);
    CHECK(s.on_sends == 1, "one ask turns the effect on");

    fxpad_feed(0, 1180, 260);
    ticks = drive_idle(&s, 400);
    CHECK(s.on_sends == 2, "and exactly one more puts it back (%d sends)", s.on_sends);
    CHECK(s.on == 0, "the effect is off again, as it was before the press");
    CHECK(ticks < 20, "with nothing to wait for, the restore is not slowed down (%d ticks)",
          ticks);
}

/* ---------------------------------------------------------------------------
 * 3. The two mappings
 * ------------------------------------------------------------------------- */

/* Run one full press/one-tick/release cycle at a point and answer the level rbp ends
 * up holding. The tick is the press tick, so this is the mapping read through the
 * pad's own public surface rather than through a copy of the arithmetic. */
static int depth_under(int x, int y)
{
    struct sim s;
    struct fxpad_live l;
    struct fxpad_out o;

    s.type = SIM_TYPE_REAL;
    s.on = 1; s.depth = 0; s.beat = 3; s.bmin = 0; s.bmax = 9;
    s.lag = 0; s.ignore_beat = 0; s.ignore_on = 0; s.queued = 0;
    s.on_sends = s.depth_sends = s.beat_sends = 0; s.depth_last = -1;

    fxpad_reset();
    fxpad_feed(1, x, y);
    sim_live(&s, &l);
    fxpad_tick(&l, &o);
    apply(&s, &o);
    return s.depth;
}

/* The same, dragging the finger to a point that may be off the cell before the tick
 * that reads the level. */
static int depth_after_drag(int dx, int dy)
{
    struct sim s;
    struct fxpad_live l;
    struct fxpad_out o;

    s.type = SIM_TYPE_REAL;
    s.on = 1; s.depth = 0; s.beat = 3; s.bmin = 0; s.bmax = 9;
    s.lag = 0; s.ignore_beat = 0; s.ignore_on = 0; s.queued = 0;
    s.on_sends = s.depth_sends = s.beat_sends = 0; s.depth_last = -1;

    fxpad_reset();
    fxpad_feed(1, 1179, 284);
    fxpad_feed(1, dx, dy);
    sim_live(&s, &l);
    fxpad_tick(&l, &o);
    apply(&s, &o);
    return s.depth;
}

static void test_level_mapping(void)
{
    int x, prev;

    CHECK(depth_under(FXPAD_X0, 284) == 0,          "the cell's left edge must be level 0, got %d", depth_under(FXPAD_X0, 284));
    CHECK(depth_under(FXPAD_X1, 284) == FXPAD_DEPTH_MAX,
          "the cell's right edge must be full scale (%d), got %d",
          FXPAD_DEPTH_MAX, depth_under(FXPAD_X1, 284));

    /* Monotone across the whole span, and never out of range: this is what catches a
     * rounding rule that drops the last column or steps backwards. */
    prev = -1;
    for (x = FXPAD_X0; x <= FXPAD_X1; x++) {
        int d = depth_under(x, 284);

        CHECK(d >= 0 && d <= FXPAD_DEPTH_MAX, "level %d out of range at x %d", d, x);
        CHECK(d >= prev, "the level must not fall as x rises: %d after %d at x %d",
              d, prev, x);
        prev = d;
    }

    /* The middle is the middle. */
    CHECK(depth_under((FXPAD_X0 + FXPAD_X1) / 2, 284) > 480
          && depth_under((FXPAD_X0 + FXPAD_X1) / 2, 284) < 545,
          "the cell's centre must be near half scale");
}

/* Run a press cycle at a point, hold until the ladder settles, and answer the rung rbp
 * ends up on. */
static int beat_under(int x, int y, int bmin, int bmax)
{
    struct sim s;

    s.type = SIM_TYPE_REAL;
    s.on = 1; s.depth = 100; s.beat = (bmin + bmax) / 2; s.bmin = bmin; s.bmax = bmax;
    s.lag = 0; s.ignore_beat = 0; s.ignore_on = 0; s.queued = 0;
    s.on_sends = s.depth_sends = s.beat_sends = 0; s.depth_last = -1;

    fxpad_reset();
    fxpad_feed(1, x, y);
    tick_n(&s, 40);
    return s.beat;
}

/* Press inside the cell, drag to a point that may be off it, hold until the ladder
 * settles, and answer the rung rbp ends up on. The drag is the case that must clamp:
 * a press that BEGINS off the cell is rbp's and never engages at all, so a "dragged
 * past the edge" test has to start on the pad. */
static int beat_after_drag(int dx, int dy, int bmin, int bmax)
{
    struct sim s;

    s.type = SIM_TYPE_REAL;
    s.on = 1; s.depth = 100; s.beat = (bmin + bmax) / 2; s.bmin = bmin; s.bmax = bmax;
    s.lag = 0; s.ignore_beat = 0; s.ignore_on = 0; s.queued = 0;
    s.on_sends = s.depth_sends = s.beat_sends = 0; s.depth_last = -1;

    fxpad_reset();
    fxpad_feed(1, 1179, 284);
    fxpad_feed(1, dx, dy);
    tick_n(&s, 40);
    return s.beat;
}

static void test_beat_mapping(void)
{
    CHECK(beat_under(1179, FXPAD_Y0, 0, 9) == 9,
          "the TOP of the cell must be the MOST beats (9), got %d",
          beat_under(1179, FXPAD_Y0, 0, 9));
    CHECK(beat_under(1179, FXPAD_Y1, 0, 9) == 0,
          "the BOTTOM of the cell must be the fewest beats (0), got %d",
          beat_under(1179, FXPAD_Y1, 0, 9));
    CHECK(beat_under(1179, 284, 0, 9) == 5,
          "the exact centre of a 0..9 ladder is rung 5 (nine gaps, so the half-way "
          "rounds up), got %d", beat_under(1179, 284, 0, 9));

    /* A ladder that does not start at zero, which is what a per-type range means. */
    CHECK(beat_under(1179, FXPAD_Y0, 2, 12) == 12,
          "the top of a 2..12 ladder must be 12, got %d", beat_under(1179, FXPAD_Y0, 2, 12));
    CHECK(beat_under(1179, FXPAD_Y1, 2, 12) == 2,
          "the bottom of a 2..12 ladder must be 2, got %d",
          beat_under(1179, FXPAD_Y1, 2, 12));

    /* A drag past the edges pins at the ends rather than running off the mapping. */
    CHECK(beat_after_drag(1179, FXPAD_Y0 - 500, 0, 9) == 9,
          "a drag above the cell pins at the top, got %d",
          beat_after_drag(1179, FXPAD_Y0 - 500, 0, 9));
    CHECK(beat_after_drag(1179, FXPAD_Y1 + 500, 0, 9) == 0,
          "a drag below the cell pins at the floor, got %d",
          beat_after_drag(1179, FXPAD_Y1 + 500, 0, 9));

    /* And the same for the level, on the axis that is absolute. */
    CHECK(depth_after_drag(FXPAD_X0 - 500, 284) == 0,
          "a drag left of the cell pins at level 0");
    CHECK(depth_after_drag(FXPAD_X1 + 500, 284) == FXPAD_DEPTH_MAX,
          "a drag right of the cell pins at full scale");
}

/* ---------------------------------------------------------------------------
 * 4. The climb
 * ------------------------------------------------------------------------- */

static void test_climb_both_directions(void)
{
    struct sim s;

    /* Down the ladder: rung 9 to rung 0, one step per answer, never overshooting. */
    s.type = SIM_TYPE_REAL;
    s.on = 1; s.depth = 100; s.beat = 9; s.bmin = 0; s.bmax = 9;
    s.lag = 0; s.ignore_beat = 0; s.ignore_on = 0; s.queued = 0;
    s.on_sends = s.depth_sends = s.beat_sends = 0; s.depth_last = -1;
    fxpad_reset();
    fxpad_feed(1, 1179, FXPAD_Y1);
    tick_n(&s, 40);
    CHECK(s.beat == 0, "a drag to the floor must land on rung 0, got %d", s.beat);
    CHECK(s.beat_sends == 9, "9 rungs down must be 9 key sends, got %d", s.beat_sends);

    /* Up the ladder, the same arithmetic with the sign flipped. */
    s.beat = 0; s.beat_sends = 0; s.queued = 0;
    fxpad_reset();
    fxpad_feed(1, 1179, FXPAD_Y0);
    tick_n(&s, 40);
    CHECK(s.beat == 9, "a drag to the top must land on rung 9, got %d", s.beat);
    CHECK(s.beat_sends == 9, "9 rungs up must be 9 key sends, got %d", s.beat_sends);
}

static void test_climb_survives_a_late_answer(void)
{
    struct sim s;

    /* THE CASE A NAIVE IMPLEMENTATION GETS WRONG. rbp applies each step a tick late, so
     * on the tick after a step its answer still reads the OLD rung. A module that
     * compared the target against that stale answer every tick would send the same step
     * twice and overshoot; this one waits for rbp to move off the rung it stepped from.
     * The send count is therefore exactly the number of rungs, not twice it. */
    s.type = SIM_TYPE_REAL;
    s.on = 1; s.depth = 100; s.beat = 5; s.bmin = 0; s.bmax = 9;
    s.lag = 1; s.ignore_beat = 0; s.ignore_on = 0; s.queued = 0;
    s.on_sends = s.depth_sends = s.beat_sends = 0; s.depth_last = -1;
    fxpad_reset();
    fxpad_feed(1, 1179, FXPAD_Y0);
    tick_n(&s, 40);
    CHECK(s.beat == 9, "a late-answering rbp must still reach rung 9, got %d", s.beat);
    CHECK(s.beat_sends == 4, "5->9 is FOUR rungs, so four sends -- got %d", s.beat_sends);
}

static void test_climb_gives_up_on_a_plateau(void)
{
    struct sim s;
    struct fxpad_live l;
    struct fxpad_out o;
    int i;

    /* rbp never moves off the rung. A pad that kept climbing would emit a key every tick
     * for as long as the operator held their finger down. It must give up, and it must
     * still unwind cleanly afterwards. */
    s.type = SIM_TYPE_REAL;
    s.on = 1; s.depth = 100; s.beat = 5; s.bmin = 0; s.bmax = 9;
    s.lag = 0; s.ignore_beat = 1; s.queued = 0;
    s.on_sends = s.depth_sends = s.beat_sends = 0; s.depth_last = -1;
    fxpad_reset();
    fxpad_feed(1, 1179, FXPAD_Y0);
    tick_n(&s, 60);
    CHECK(s.beat_sends == 1,
          "a ladder rbp will not move off must be asked ONCE, got %d sends", s.beat_sends);

    /* And it is quiet from then on. */
    for (i = 0; i < 5; i++) {
        sim_live(&s, &l);
        fxpad_tick(&l, &o);
        CHECK(o.beat_step == 0, "a given-up climb must stay quiet");
    }

    /* Releasing must not hang: the restore wants the rung it is already on. */
    fxpad_feed(0, 1179, FXPAD_Y0);
    CHECK(drive_idle(&s, 100) < 100, "the unwind must finish even after a give-up");
    CHECK(fxpad_busy() == 0, "the pad must be idle");
    CHECK(s.beat == 5, "the rung must be back where it started, got %d", s.beat);
}

/* ---------------------------------------------------------------------------
 * 5. The gesture and the latch
 * ------------------------------------------------------------------------- */

static void test_latch(void)
{
    fxpad_reset();

    /* A press off the cell is rbp's -- and every report of that gesture is, including
     * one that crosses the cell on the way. */
    CHECK(fxpad_feed(1, 500, 300) == MZ_FEED_NONE, "a press off the cell must reach rbp");
    CHECK(fxpad_feed(1, 1180, 260) == MZ_FEED_NONE,
          "A GESTURE THAT BEGAN ELSEWHERE MUST NOT BE ADOPTED on a slide");
    CHECK(fxpad_engaged() == 0, "a slide over the cell must not engage the pad");
    CHECK(fxpad_feed(0, 1180, 260) == MZ_FEED_NONE, "its release is rbp's too");
    CHECK(fxpad_busy() == 0, "and it leaves nothing behind");

    /* A release with no press behind it is rbp's. */
    CHECK(fxpad_feed(0, 1180, 260) == MZ_FEED_NONE, "a bare release must reach rbp");

    /* A press inside is ours, and stays ours for the whole gesture -- including after
     * it has slid off, which is what stops rbp going deaf to the release. */
    CHECK(fxpad_feed(1, 1180, 260) == MZ_FEED_TAKEN, "a press inside the cell is ours");
    CHECK(fxpad_feed(1, 1180, 262) == MZ_FEED_TAKEN, "a run of downs is ours");
    CHECK(fxpad_feed(1, 900, 700) == MZ_FEED_TAKEN,
          "A SLIDE OFF THE CELL IS STILL OURS -- rbp must not see the release alone");
    CHECK(fxpad_engaged() == 1, "sliding off does not un-engage it");
    CHECK(fxpad_feed(0, 900, 700) == MZ_FEED_TAKEN, "and its release is ours");
    fxpad_reset();

    /* On the edges themselves, both inclusive. */
    CHECK(fxpad_feed(1, FXPAD_X0, FXPAD_Y0) == MZ_FEED_TAKEN, "the top-left corner is in");
    fxpad_reset();
    CHECK(fxpad_feed(1, FXPAD_X1, FXPAD_Y1) == MZ_FEED_TAKEN, "the bottom-right corner is in");
    fxpad_reset();
    CHECK(fxpad_feed(1, FXPAD_X0 - 1, FXPAD_Y0 - 1) == MZ_FEED_NONE, "one past the corner is out");
    fxpad_reset();
    CHECK(fxpad_feed(1, FXPAD_X1 + 1, FXPAD_Y1 + 1) == MZ_FEED_NONE, "one past the corner is out");
    fxpad_reset();

    /* fxpad_reset drops a gesture in flight, which is what pointsrc.c needs when the
     * pointer device goes away. */
    fxpad_feed(1, 1180, 260);
    fxpad_reset();
    CHECK(fxpad_busy() == 0, "fxpad_reset must drop an engaged pad");
    CHECK(fxpad_feed(0, 1180, 260) == MZ_FEED_NONE,
          "after a reset the release belongs to rbp again");
}

static void test_mark(void)
{
    int mx, my;

    fxpad_reset();
    CHECK(fxpad_mark(&mx, &my) == 0, "an idle pad has no dot");

    fxpad_feed(1, 1180, 260);
    CHECK(fxpad_mark(&mx, &my) == 1, "an engaged pad draws a dot");
    CHECK(mx == 1180 && my == 260, "the dot sits under the finger (%d,%d)", mx, my);

    /* Clamped with the value, so the dot never leaves the cell. */
    fxpad_feed(1, 5000, -5000);
    CHECK(fxpad_mark(&mx, &my) == 1 && mx == FXPAD_X1 && my == FXPAD_Y0,
          "a finger past the corner pins the dot to it (%d,%d)", mx, my);

    /* NULL is allowed: the caller may only want the answer. */
    CHECK(fxpad_mark(NULL, NULL) == 1, "fxpad_mark must take NULL");
}

/* ---------------------------------------------------------------------------
 * 6. The geometry
 * ------------------------------------------------------------------------- */

static void test_geometry(void)
{
    int x0, y0, x1, y1;

    fxpad_rect(&x0, &y0, &x1, &y1);
    CHECK(x0 == FXPAD_X0 && y0 == FXPAD_Y0 && x1 == FXPAD_X1 && y1 == FXPAD_Y1,
          "fxpad_rect must answer the pad's own rect");
    CHECK(x0 == 1100 && x1 == 1259 && y0 == 216 && y1 == 352,
          "THE CELL MOVED: it is a measurement of rbp's ink (1100..1259, 216..352)");
    CHECK(FXPAD_W == 160 && FXPAD_H == 137, "the cell is 160 x 137");

    /* Inside the plate rbp draws it in, and inside the screen. */
    CHECK(x0 >= FX_PANEL_X0 && x1 <= FX_PANEL_X1 && y0 >= FX_PANEL_Y0 && y1 <= FX_PANEL_Y1,
          "the cell must sit inside the BEAT FX plate");
    CHECK(x1 < MZ_LOGICAL_W && y1 < MZ_LOGICAL_H, "the cell must be on the screen");

    /* AND IT MUST OVERLAP NONE OF THE THREE CONTROLS fx_zone.c OWNS. Two zones claiming
     * one point is a tap whose owner depends on the order of the ladder's rungs. */
    CHECK(x1 < FX_CH_X0 || x0 > FX_CH_X1 || y1 < FX_CH_Y0 || y0 > FX_CH_Y1,
          "THE CELL OVERLAPS THE CH SELECT BOX");
    CHECK(x1 < FX_HEADER_X0 || x0 > FX_HEADER_X1 || y1 < FX_HEADER_Y0 || y0 > FX_HEADER_Y1,
          "THE CELL OVERLAPS THE BEAT FX HEADER BAR");
    CHECK(x1 < FX_NAME_X0 || x0 > FX_NAME_X1 || y1 < FX_NAME_Y0 || y0 > FX_NAME_Y1,
          "THE CELL OVERLAPS THE EFFECT-NAME CELL");

    /* The nearest neighbour is the CH box above it, and there is plate between them. */
    CHECK(FXPAD_Y0 > FX_CH_Y1, "the CH box must end above the cell, not run into it");
    CHECK(fxpad_hit(1180, 284) == 1, "the cell centre is in the pad");
    CHECK(fxpad_hit(1180, FX_CH_Y1) == 0, "a point in the CH box is not in the pad");
}

/* ---------------------------------------------------------------------------
 * 7. The HUD
 *
 * The pad draws two marks over rbp's OWN pixels, and the property that matters is not
 * that the marks appear -- it is WHICH PIXELS OF THE OPERATOR'S READOUT SURVIVE. The
 * cell is where rbp prints the BPM, the msec and the beat count, all of it live, and a
 * copy of this code that got the copy wrong would cover the very numbers the operator is
 * reading while they drag. So the page here is filled with a value no palette entry can
 * be, and every test below is about how much of it is still there afterwards.
 *
 * The plane route and the page route are the two things being pinned and they are
 * opposite: with a `page` the destination is a separate buffer that has to be filled with
 * rbp's pixels first, and without one the destination IS rbp's pixels, so the marks go
 * down and NOTHING is filled. That second case is the one an implementer gets wrong, and
 * the failure would be a black cell where the operator's readout used to be.
 * ------------------------------------------------------------------------- */

#define HUD_MAXW 400
#define HUD_MAXH 400

/* No palette value: every write this module makes is detectable by its difference from
 * this, which is what makes "how many pixels did the HUD touch" a count and not a guess. */
#define HUD_SENTINEL 0x1234u

static unsigned short hud_page[HUD_MAXW * HUD_MAXH];
static unsigned short hud_dst[HUD_MAXW * HUD_MAXH];

static void hud_view(struct menu_view *v, void *buf, int w, int h, int pitch)
{
    memset(v, 0, sizeof *v);
    v->pix   = buf;
    v->pitch = pitch;
    v->fb_w  = w;
    v->fb_h  = h;
    v->dw    = w;
    v->dh    = h;
    v->bpp   = 16;
}

static unsigned int hud_at(void *buf, int pitch, int x, int y)
{
    return ((unsigned short *)buf)[y * pitch + x];
}

static void hud_fill(void *buf, int pitch, int w, int h, unsigned int v)
{
    int x, y;

    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            ((unsigned short *)buf)[y * pitch + x] = (unsigned short)v;
}

/* How many pixels of `got` differ from `want`, at the same stride and size. */
static int hud_diff(const void *got, const void *want, int pitch, int w, int h)
{
    int x, y, n = 0;

    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            if (((const unsigned short *)got)[y * pitch + x] !=
                ((const unsigned short *)want)[y * pitch + x])
                n++;
    return n;
}

static void test_hud_sizes(void)
{
    /* The cell at the page's own scale, and at a bigger one -- the plane is created at
     * exactly this size and the copy reads exactly this rectangle, so the two have to
     * come out of one function. */
    CHECK(fxpad_paint_w(1280) == FXPAD_W && fxpad_paint_h(800) == FXPAD_H,
          "at 1280x800 the cell is its own logical size (%d x %d)",
          fxpad_paint_w(1280), fxpad_paint_h(800));
    CHECK(fxpad_paint_w(1920) == 240, "at 1920 wide the cell scales to 240 (%d)",
          fxpad_paint_w(1920));
    CHECK(fxpad_paint_h(1080) == 184, "at 1080 tall the cell scales to 184 (%d)",
          fxpad_paint_h(1080));
    /* Never zero, whatever the page -- a plane of height 0 is not a plane. */
    CHECK(fxpad_paint_w(1) >= 1 && fxpad_paint_h(1) >= 1, "a tiny page never yields 0");
}

static void test_hud_refusals(void)
{
    struct menu_view v;

    hud_view(&v, hud_dst, FXPAD_W, FXPAD_H, FXPAD_W);
    CHECK(fxpad_paint_ok(&v) == 1, "the real cell is a view the HUD can draw into");

    CHECK(fxpad_paint_ok(NULL) == 0, "no view, no paint");

    hud_view(&v, NULL, FXPAD_W, FXPAD_H, FXPAD_W);
    CHECK(fxpad_paint_ok(&v) == 0, "no pixels, no paint");

    hud_view(&v, hud_dst, FXPAD_W, FXPAD_H, FXPAD_W);
    v.bpp = 8;
    CHECK(fxpad_paint_ok(&v) == 0, "an unknown depth is refused");

    hud_view(&v, hud_dst, FXPAD_W, FXPAD_H, FXPAD_W);
    v.pitch = FXPAD_W - 1;
    CHECK(fxpad_paint_ok(&v) == 0, "a stride narrower than the cell is refused");

    /* A cell so small the dot's two ends would be the same picture. */
    hud_view(&v, hud_dst, 6, 6, 6);
    CHECK(fxpad_paint_ok(&v) == 0, "a cell with no room for the marks is refused");
}

/* THE COPY, and the fact that with nothing marked it is the whole of the paint. This is
 * also the regression guard for the frame: it was drawn here, on the cell's own edge, and
 * an unmarked paint must now write rbp's pixels byte for byte and add nothing. */
static void test_hud_copy_only(void)
{
    struct menu_view dst, src;
    int x, y, bad = 0;

    /* A source with a stride of its own, so a copy that assumes pitch == width reads the
     * wrong rows and is caught here rather than on the glass -- and a page that is NOT
     * uniform, so a copy that reads the wrong pixel shows up as a wrong value instead of
     * passing on a constant. */
    for (y = 0; y < FXPAD_H; y++)
        for (x = 0; x < FXPAD_W + 7; x++)
            ((unsigned short *)hud_page)[y * (FXPAD_W + 7) + x] =
                (unsigned short)(HUD_SENTINEL + y * 13 + x);
    hud_fill(hud_dst, FXPAD_W, FXPAD_W, FXPAD_H, 0);

    hud_view(&dst, hud_dst, FXPAD_W, FXPAD_H, FXPAD_W);
    hud_view(&src, hud_page, FXPAD_W, FXPAD_H, FXPAD_W + 7);

    fxpad_paint(&dst, &src, 0, 0, 0);

    /* THE READOUT SURVIVES, EVERY PIXEL OF IT, at the row and column it came from. This is
     * the whole reason the module copies instead of filling, and with no mark asked for
     * there is nothing else it is allowed to write. */
    for (y = 0; y < FXPAD_H; y++)
        for (x = 0; x < FXPAD_W; x++)
            if (hud_at(hud_dst, FXPAD_W, x, y) != (HUD_SENTINEL + (unsigned)y * 13 + (unsigned)x)) {
                if (bad == 0)
                    CHECK(0, "the copy put %04x at %d,%d, not rbp's own %04x",
                          hud_at(hud_dst, FXPAD_W, x, y), x, y,
                          HUD_SENTINEL + (unsigned)y * 13 + (unsigned)x);
                bad++;
            }
    CHECK(bad == 0, "every pixel of the cell is rbp's own, and no frame is added (%d wrong)",
          bad);
}

static void test_hud_dot(void)
{
    struct menu_view dst, src;
    unsigned int face = menu_pixel(16, MENU_FX_SEL);
    int n;

    hud_fill(hud_page, FXPAD_W, FXPAD_W, FXPAD_H, HUD_SENTINEL);
    hud_fill(hud_dst, FXPAD_W, FXPAD_W, FXPAD_H, 0);
    hud_view(&dst, hud_dst, FXPAD_W, FXPAD_H, FXPAD_W);
    hud_view(&src, hud_page, FXPAD_W, FXPAD_H, FXPAD_W);

    fxpad_paint(&dst, &src, 0, 0, 0);
    n = hud_diff(hud_dst, hud_page, FXPAD_W, FXPAD_W, FXPAD_H);

    /* The cell centre, which is where the operator will drag through most. */
    fxpad_paint(&dst, &src, 1, 1180, 284);
    CHECK(hud_at(hud_dst, FXPAD_W, 1180 - FXPAD_X0, 284 - FXPAD_Y0) == face,
          "the dot's centre is the accent colour");
    CHECK(hud_diff(hud_dst, hud_page, FXPAD_W, FXPAD_W, FXPAD_H) > n,
          "the dot is drawn at all (%d px now, %d before)",
          hud_diff(hud_dst, hud_page, FXPAD_W, FXPAD_W, FXPAD_H), n);

    /* Two radii away from the centre is rbp's own pixel again: the dot is local and does
     * not smear the readout it sits on. */
    CHECK(hud_at(hud_dst, FXPAD_W, 1180 - FXPAD_X0 + FXPAD_DOT_R + 3,
                 284 - FXPAD_Y0) == HUD_SENTINEL,
          "a few pixels to the right of the dot is still rbp's");

    /* The corner: fxpad_mark() clamps the dot into the cell, so this is the extreme the
     * operator can reach, and the dot has to land there rather than nowhere. */
    fxpad_paint(&dst, &src, 1, FXPAD_X1, FXPAD_Y0);
    CHECK(hud_at(hud_dst, FXPAD_W, FXPAD_W - 1, 0) != HUD_SENTINEL,
          "the dot reaches the cell's top-right corner");
}

/* THE PAGE ROUTE, and the trap it exists to catch. No source means the destination is
 * rbp's own framebuffer, so the only writes allowed are the dot's own pixels -- anything
 * that fills would black out the readout the operator is looking at. */
static void test_hud_page_route_fills_nothing(void)
{
    struct menu_view dst;
    unsigned int border = menu_pixel(16, MENU_BORDER);
    unsigned int face = menu_pixel(16, MENU_FX_SEL);
    int x, y, marked = 0;

    hud_fill(hud_dst, FXPAD_W, FXPAD_W, FXPAD_H, HUD_SENTINEL);
    hud_view(&dst, hud_dst, FXPAD_W, FXPAD_H, FXPAD_W);

    fxpad_paint(&dst, NULL, 1, 1180, 284);

    for (y = 0; y < FXPAD_H; y++)
        for (x = 0; x < FXPAD_W; x++) {
            unsigned int p = hud_at(hud_dst, FXPAD_W, x, y);

            if (p == HUD_SENTINEL)
                continue;
            marked++;
            CHECK(p == border || p == face,
                  "the page route wrote %04x at %d,%d -- only the marks may be written",
                  p, x, y);
        }
    CHECK(marked > 0, "the page route writes the mark");
    /* And only the dot: face plus its ring, which is what the loop above counted. The
     * frame that used to be drawn on the cell's outline is gone, so the outline here is
     * rbp's own pixel and nothing of ours lands on it. */
    CHECK(marked <= 4 * (FXPAD_DOT_R + 1) * (FXPAD_DOT_R + 1),
          "the page route writes the dot and no rectangle (%d px)", marked);

    /* Every pixel the dot did not land on is rbp's, untouched: the readout, the cell's own
     * border, and the plate around it. The cell's edge is the one to name, because that is
     * exactly where the removed frame used to write. */
    CHECK(hud_at(hud_dst, FXPAD_W, 0, FXPAD_H / 2) == HUD_SENTINEL,
          "the cell's left edge is left as rbp drew it -- no frame on the page route");
    CHECK(hud_at(hud_dst, FXPAD_W, 20, 20) == HUD_SENTINEL,
          "the readout inside the cell is untouched on the page route");
}

static void test_hud_idempotent(void)
{
    struct menu_view dst, src;
    static unsigned short once[HUD_MAXW * HUD_MAXH];
    int x, y;

    hud_fill(hud_page, FXPAD_W, FXPAD_W, FXPAD_H, HUD_SENTINEL);
    hud_view(&dst, hud_dst, FXPAD_W, FXPAD_H, FXPAD_W);
    hud_view(&src, hud_page, FXPAD_W, FXPAD_H, FXPAD_W);

    fxpad_paint(&dst, &src, 1, 1150, 300);
    for (y = 0; y < FXPAD_H; y++)
        for (x = 0; x < FXPAD_W; x++)
            once[y * FXPAD_W + x] = (unsigned short)hud_at(hud_dst, FXPAD_W, x, y);

    fxpad_paint(&dst, &src, 1, 1150, 300);
    CHECK(hud_diff(hud_dst, once, FXPAD_W, FXPAD_W, FXPAD_H) == 0,
          "painting the same page and the same finger twice writes the same bytes");
}

/* The one failure the module chooses, so that it is a decision and not an accident: a
 * source that cannot be lined up with the destination is not a source, and a plane must
 * never be left showing the dumb buffer's own contents. */
static void test_hud_mismatched_page_fills(void)
{
    struct menu_view dst, src;
    unsigned int bed = menu_pixel(16, MENU_FILL);
    int x, y, ok = 1;

    hud_fill(hud_dst, FXPAD_W, FXPAD_W, FXPAD_H, HUD_SENTINEL);
    hud_fill(hud_page, FXPAD_W, FXPAD_W, FXPAD_H, HUD_SENTINEL);
    hud_view(&dst, hud_dst, FXPAD_W, FXPAD_H, FXPAD_W);
    hud_view(&src, hud_page, FXPAD_W / 2, FXPAD_H / 2, FXPAD_W / 2);

    fxpad_paint(&dst, &src, 0, 0, 0);

    for (y = 1; y < FXPAD_H - 1 && ok; y++)
        for (x = 1; x < FXPAD_W - 1; x++)
            if (hud_at(hud_dst, FXPAD_W, x, y) != bed) {
                CHECK(0, "a mismatched page must fill, not leave %04x at %d,%d",
                      hud_at(hud_dst, FXPAD_W, x, y), x, y);
                ok = 0;
                break;
            }
    CHECK(ok, "a mismatched source fills the cell rather than showing the buffer");
}

int main(void)
{
    test_restore_exact();
    test_restore_of_a_no_op_gesture();
    test_tap_between_two_ticks_sends_nothing();
    test_quiet_tick();
    test_on_retries_a_dropped_toggle();
    test_on_rearms_mid_hold();
    test_flag_lies_on_a_switched_off_effect();
    test_restore_waits_for_the_answer();
    test_restore_asks_once_when_rbp_answers();
    test_level_mapping();
    test_beat_mapping();
    test_climb_both_directions();
    test_climb_survives_a_late_answer();
    test_climb_gives_up_on_a_plateau();
    test_latch();
    test_mark();
    test_geometry();

    test_hud_sizes();
    test_hud_refusals();
    test_hud_copy_only();
    test_hud_dot();
    test_hud_page_route_fills_nothing();
    test_hud_idempotent();
    test_hud_mismatched_page_fills();

    printf("test_fxpad: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
