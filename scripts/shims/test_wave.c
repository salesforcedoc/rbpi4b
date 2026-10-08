/*
 * test_wave.c -- the performance screen's waveform swipe, with no Pi, no device, no
 * rbp and no screen anywhere in this file.
 *
 * It links the PRODUCTION wave_zone.c, for the reason test_menu.c, test_side.c and
 * test_util.c link theirs: what is pinned here -- a rect boundary, a step size, a
 * ratchet -- is exactly the kind that gets quietly rewritten in a copy.
 *
 * wave_zone.c is deliberately dumb. It cannot see rbp, so everything rbp would tell it
 * arrives in a struct wave_state the tests hand it, and everything it wants sent comes
 * back as a signed step count. That is what makes the things with real consequences
 * testable without a unit:
 *
 * 1. THE GEOMETRY, as arithmetic and as pinned numbers. The rect must sit inside the
 *    waveform canvas, clear of rbp's own DECK panels on the left, of BEAT FX on the
 *    right (rbp binds TOUCH to that one and it must stay his), of the hot-cue pad rows
 *    below, and of the shim's own swipe-down band above -- which OPENS on any press
 *    inside its first 56 rows, so a rect that reached into it could open the band out
 *    from under the gesture.
 *
 * 2. THE GATE, which is rbp's own dispatch decision. A rotation sent on the performance
 *    screen goes to CursorWaveZoom only when rbp's own branch would have sent it there;
 *    the other branch of the same test adjusts the BEAT GRID, which rewrites the
 *    analysis of the operator's track. The rule is pinned against the macro rbp_abi.h
 *    derives from the binary, not against a second copy of the arithmetic, and it is
 *    checked to be re-read on EVERY report -- the screen can go away under a press that
 *    is already down, and a gate cached at the down edge would let one rotation land on
 *    whatever screen replaced it.
 *
 * 3. THE ADDITIVE RULE, which is the correction of the pinch this replaces. The module
 *    cannot take a report and cannot withhold a press: every call answers with a count,
 *    a press it does not own answers 0 forever, and a down on the wave is 0 as well --
 *    there is no vocabulary here with which to swallow anything.
 *
 * 4. THE FEEL, clause by clause. Up is zoom IN (a bigger scale is zoomed in); a step is
 *    crossed exactly at a multiple of WAVE_STEP_PX; a resting finger re-sends nothing;
 *    a reversal pays back exactly what it overran; and the ratchet is kept inside rbp's
 *    own 0..4 ladder, so an overshoot past the end is not remembered and paid back later.
 *
 * 5. THE ONE RULE THAT IS NOT ABOUT ZOOM. A drag ALONG the wave is the operator
 *    scrubbing or searching the track -- rbp's own gesture on this very canvas -- and it
 *    must not zoom. The vertical must dominate, tested against the anchor so it is a
 *    property of the swipe and not of one sample of it.
 *
 * Build + run (static, so no rootfs is needed to load it):
 *     make test
 */
#define _GNU_SOURCE
#include "wave_zone.h"
#include "menu_zone.h"      /* MZ_STRIP_Y1: the band the rect must stay clear of */
#include "rbp_abi.h"        /* WAVE_SCALE_MIN/MAX: pinned equal to the module's copy */

#include <stdio.h>

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
 * The harness. wave_feed() is the module's one door, so every test is a sequence of
 * reports fed exactly as pointsrc.c's funnel feeds them, with rbp's state standing in
 * a plain struct the test sets by hand.
 *
 * X is 600 throughout -- inside the rect and clear of both edges -- because nothing in
 * this module depends on x except the dominance test, and the tests that care about x
 * say so.
 * ------------------------------------------------------------------------- */

static struct wave_state S;
static const int X = 600;

static void begin(int zoom_ok, int scale)
{
    wave_reset();
    S.zoom_ok = zoom_ok;
    S.scale = scale;
}

static int feed(int down, int x, int y)
{
    return wave_feed(&S, down, x, y);
}

static int put(int x, int y)
{
    return feed(1, x, y);
}

/* A press that lands inside the rect and is ours, with the finger left down. */
static void press(int scale)
{
    begin(1, scale);
    put(X, 400);
}

/* ---------------------------------------------------------------------------
 * 1. The geometry.
 * ------------------------------------------------------------------------- */
static void test_geometry(void)
{
    /* The constants themselves, as numbers. A change to any of them should be a
     * deliberate act with a measurement behind it, not a drift. */
    CHECK(WAVE_X0 == 200 && WAVE_X1 == 1080, "the rect's x is %d..%d", WAVE_X0, WAVE_X1);
    CHECK(WAVE_Y0 == 60 && WAVE_Y1 == 480, "the rect's y is %d..%d", WAVE_Y0, WAVE_Y1);
    CHECK(WAVE_STEP_PX == 40, "the step is %d px", WAVE_STEP_PX);

    /* The ladder, against the one rbp_abi.h derives from the binary. This is the check
     * that keeps the module's private copy honest -- and it is load-bearing, because
     * the ladder IS the module's bound on how many rotations one report can produce. */
    CHECK(WAVE_ZOOM_MIN == WAVE_SCALE_MIN, "module min %d, rbp %d",
          WAVE_ZOOM_MIN, WAVE_SCALE_MIN);
    CHECK(WAVE_ZOOM_MAX == WAVE_SCALE_MAX, "module max %d, rbp %d",
          WAVE_ZOOM_MAX, WAVE_SCALE_MAX);

    /* The corners, and one pixel outside each -- wave_in_rect() is inclusive, which is
     * what makes these four pairs exact. */
    CHECK(wave_in_rect(WAVE_X0, WAVE_Y0), "the top-left corner is not in the rect");
    CHECK(wave_in_rect(WAVE_X1, WAVE_Y1), "the bottom-right corner is not in the rect");
    CHECK(!wave_in_rect(WAVE_X0 - 1, 400), "one px left of the rect is in it");
    CHECK(!wave_in_rect(WAVE_X1 + 1, 400), "one px right of the rect is in it");
    CHECK(!wave_in_rect(600, WAVE_Y0 - 1), "one px above the rect is in it");
    CHECK(!wave_in_rect(600, WAVE_Y1 + 1), "one px below the rect is in it");
    CHECK(wave_in_rect(600, 400), "the middle of the canvas is not in the rect");

    /* ---------------------------------------------------------------------
     * AND THE FOUR THINGS IT MUST NOT REACH, which are the whole reason the
     * numbers are 200/1080/60/480 rather than the canvas's own edges.
     * ------------------------------------------------------------------- */

    /* rbp's swipe-down band takes rows 0..55 and OPENS on any press inside them. A
     * rect that reached up into that strip could open the band out from under itself,
     * so the first row of the rect must be strictly below the last dead row. */
    CHECK(WAVE_Y0 > MZ_STRIP_Y1,
          "the rect starts at y %d, inside the band's dead rows 0..%d", WAVE_Y0, MZ_STRIP_Y1);

    /* rbp's DECK 1/2 info panels (x 10..183) and BEAT FX (x 1090..1269) are drawn in
     * this same band. BEAT FX is the one that matters: rbp binds TOUCH to it. */
    CHECK(WAVE_X0 > 183, "the rect starts at x %d, inside the deck panels (10..183)", WAVE_X0);
    CHECK(WAVE_X1 < 1090, "the rect ends at x %d, inside BEAT FX (1090..1269)", WAVE_X1);

    /* The hot-cue label row is at y 499..509 and the two pad rows at y 518..567. A slap
     * on a pad is the operator's hot cue and must never be converted into a zoom. */
    CHECK(WAVE_Y1 < 499, "the rect reaches y %d, onto the hot-cue label row (499)", WAVE_Y1);
}

/* ---------------------------------------------------------------------------
 * 2. The gate.
 * ------------------------------------------------------------------------- */
static void test_gate(void)
{
    /* Not rbp's performance screen, or the beat grid is in adjust mode, or a shim
     * surface is open: nothing at all, however far the finger travels. */
    begin(0, 2);
    CHECK(put(X, 400) == 0, "a refused press sent something at the down edge");
    CHECK(feed(1, X, 300) == 0, "a refused press zoomed");
    CHECK(feed(1, X, 100) == 0, "a refused press zoomed further");

    /* An unreadable scale is the same refusal, and it is separate from zoom_ok: the
     * caller leaves the scale at 0 when it cannot read it, so the range test is what
     * stops a latch on a value the module cannot reason about. */
    begin(1, -1);
    put(X, 400);
    CHECK(feed(1, X, 300) == 0, "a press latched on an out-of-range scale");
    begin(1, WAVE_ZOOM_MAX + 1);
    put(X, 400);
    CHECK(feed(1, X, 300) == 0, "a press latched above the ladder");

    /* THE GATE IS RE-READ EVERY REPORT, and this is the case that costs something if
     * it is not: the screen goes away under a press that is already down. */
    press(2);
    CHECK(feed(1, X, 360) == 1, "the first step of a live swipe did not go out");
    S.zoom_ok = 0;                                  /* rbp left the screen */
    CHECK(feed(1, X, 320) == 0, "a step went out after the gate closed");
    CHECK(feed(1, X, 280) == 0, "a step went out after the gate closed");
    S.zoom_ok = 1;                                  /* and the screen came back */
    CHECK(feed(1, X, 240) == 0,
          "the press became ours again when the screen came back");

    /* A press that ended while the gate was shut must not be re-anchored by the release
     * either: the next down edge is what starts a gesture. */
    press(2);
    S.zoom_ok = 0;
    feed(1, X, 300);
    feed(0, X, 300);
    S.zoom_ok = 1;
    CHECK(feed(1, X, 100) == 0, "a move after a release zoomed");
}

/* ---------------------------------------------------------------------------
 * 3. The latch, and the additive rule.
 * ------------------------------------------------------------------------- */
static void test_latch(void)
{
    /* A press that lands outside the rect is not ours, and it NEVER becomes ours when
     * it wanders in -- the same rule util_zone.c keeps, for the same reason: a gesture
     * that could claim a press halfway through is a gesture that can claim one the
     * operator aimed at rbp. */
    begin(1, 2);
    put(WAVE_X0 - 1, 400);
    CHECK(feed(1, X, 300) == 0, "a press that landed left of the rect zoomed");
    CHECK(feed(1, 600, WAVE_Y1 + 1) == 0, "a press below the rect zoomed");

    begin(1, 2);
    put(600, WAVE_Y0 - 1);                          /* in the band's dead rows */
    CHECK(feed(1, 600, 300) == 0, "a press that landed on the band strip zoomed");

    /* THE DOWN EDGE SENDS NOTHING, whatever it lands on. A tap on the wave is a tap and
     * costs one report; there is no per-report step to starve the shared input loop. */
    begin(1, 2);
    CHECK(put(X, 400) == 0, "the down edge sent a step");
    CHECK(put(X, 400) == 0, "a repeated down edge sent a step");

    /* A release carries no answer, and one with no press behind it is still nothing. */
    begin(1, 2);
    CHECK(feed(0, X, 400) == 0, "a release with no press sent a step");
    put(X, 400);
    feed(1, X, 360);
    CHECK(feed(0, X, 360) == 0, "the release itself sent a step");

    /* AND THE PRESS IS OVER. There is no such thing as a move without a press, so the
     * next report with a finger down is a NEW press -- it takes its own anchor rather
     * than inheriting the one the last press left behind. */
    CHECK(put(X, 100) == 0, "the next press did not start clean");
    CHECK(feed(1, X, 100 - WAVE_STEP_PX) == 1, "the next press did not zoom");
}

/* ---------------------------------------------------------------------------
 * 4. The feel.
 * ------------------------------------------------------------------------- */
static void test_feel(void)
{
    /* UP IS ZOOM IN. A bigger scale is zoomed in (calcParticularWave's
     * samples-per-screen ladder is {1600,800,400,200,100} for scales 0..4), so the
     * finger moving up -- y falling -- is the POSITIVE count. */
    press(2);
    CHECK(feed(1, X, 400 - WAVE_STEP_PX) == 1, "one step up was not +1");
    press(2);
    CHECK(feed(1, X, 400 + WAVE_STEP_PX) == -1, "one step down was not -1");

    /* A step is crossed exactly at the multiple, in both directions. */
    press(2);
    CHECK(feed(1, X, 400 - WAVE_STEP_PX + 1) == 0, "a step came early (up)");
    CHECK(feed(1, X, 400 + WAVE_STEP_PX - 1) == 0, "a step came early (down)");
    CHECK(feed(1, X, 400 - WAVE_STEP_PX) == 1, "the step did not arrive at the multiple");
    CHECK(feed(1, X, 400 + WAVE_STEP_PX) == -2, "the reversal was not the difference");

    /* A RESTING FINGER RE-SENDS NOTHING, and neither does a wobble that stays inside
     * the step it has already paid for. The band is the whole of it: a step is crossed
     * at a multiple of WAVE_STEP_PX, so a finger wobbling AROUND a multiple really has
     * crossed back and forth and the ratchet pays that back -- correctly. The wobble
     * this pins is a few px either side of a point in the middle of a band. */
    press(2);
    CHECK(feed(1, X, 400 - 2 * WAVE_STEP_PX) == 2, "two steps up were not +2");
    CHECK(feed(1, X, 400 - 2 * WAVE_STEP_PX) == 0, "a resting finger re-sent its steps");
    press(2);
    CHECK(feed(1, X, 400 - 60) == 1, "a step and a half up was not +1");
    CHECK(feed(1, X, 400 - 55) == 0, "a wobble sent a step");
    CHECK(feed(1, X, 400 - 65) == 0, "a wobble sent a step");

    /* A REVERSAL PAYS BACK EXACTLY WHAT IT OVERRAN -- including the case of a finger
     * that has come back to the exact point it started from, where there is no travel
     * in either axis and the dominance test must still let the payback through. */
    press(0);                                   /* the whole ladder is in reach */
    CHECK(feed(1, X, 400 - 3 * WAVE_STEP_PX) == 3, "three steps up were not +3");
    CHECK(feed(1, X, 400) == -3, "returning to the anchor did not pay back all three");
    /* ...and a finger that carries on past the anchor is asking to zoom out from the
     * FLOOR, which is the ladder's own refusal: this press began at scale 0. */
    CHECK(feed(1, X, 400 + 1 * WAVE_STEP_PX) == 0, "a press that began at the floor zoomed out");

    /* The same at the top, where the refusal is on the way in. */
    press(WAVE_ZOOM_MAX);
    CHECK(feed(1, X, 400 - WAVE_STEP_PX) == 0, "a press at the top of the ladder zoomed in");
    CHECK(feed(1, X, 400 + 2 * WAVE_STEP_PX) == -2, "a press at the top did not zoom out");

    /* THE RATCHET IS KEPT INSIDE rbp's LADDER, which is what makes the ends feel right.
     * rbp clamps silently, so without this an overshoot would be remembered and paid
     * back on the way down: drag well past the top, drag back to the anchor, and the
     * wave would jump all the way to the bottom. */
    press(3);                                   /* three steps of headroom */
    CHECK(feed(1, X, 400 - 40 * WAVE_STEP_PX) == 1, "a huge overshoot asked for more than 1");
    CHECK(feed(1, X, 400 - 41 * WAVE_STEP_PX) == 0, "the overshoot was asked for again");
    CHECK(feed(1, X, 400) == -1, "returning to the anchor paid back more than it sent");

    press(0);                                   /* and the same at the floor */
    CHECK(feed(1, X, 400 + 40 * WAVE_STEP_PX) == 0, "a huge undershoot asked below the floor");

    press(1);                                   /* and in the middle */
    CHECK(feed(1, X, 400 - 40 * WAVE_STEP_PX) == 3, "the headroom from 1 was not 3");
    CHECK(feed(1, X, 400 - 41 * WAVE_STEP_PX) == 0, "past the top was asked for again");
    CHECK(feed(1, X, 400 + 40 * WAVE_STEP_PX) == -4, "the whole ladder did not come back");
}

/* ---------------------------------------------------------------------------
 * 5. The one rule that is not about zoom.
 * ------------------------------------------------------------------------- */
static void test_along_the_wave(void)
{
    /* A drag ALONG the wave is rbp's own gesture -- scrubbing, searching -- and it must
     * not zoom. The vertical has to dominate, and it is tested against the ANCHOR, so a
     * finger that sets off along the wave and then turns up is not ours either. */
    press(2);
    CHECK(feed(1, X + 100, 400 - WAVE_STEP_PX) == 0,
          "a drag along the wave zoomed on its vertical travel");
    CHECK(feed(1, X + 200, 400 - 4 * WAVE_STEP_PX) == 0,
          "a drag along the wave zoomed once it wandered up");

    /* A pure horizontal drag is the same refusal, and so is a diagonal that leans along
     * the wave. */
    press(2);
    CHECK(feed(1, X + 200, 400) == 0, "a horizontal drag zoomed");
    press(2);
    CHECK(feed(1, X + 100, 400 - 99) == 0, "a diagonal along the wave zoomed");

    /* A finger that turns back to vertical is ours again, and the steps it is owed are
     * the steps for where it now is -- not a fresh count and not a doubled one. */
    press(0);
    CHECK(feed(1, X + 300, 400 - 2 * WAVE_STEP_PX) == 0, "a drag along the wave zoomed");
    CHECK(feed(1, X, 400 - 3 * WAVE_STEP_PX) == 3,
          "turning back to vertical did not pay out the steps owed");

    /* And a swipe that is steeply vertical passes, which is the whole gesture: the
     * operator's arm is not a ruler and a little lateral drift cannot matter. */
    press(2);
    CHECK(feed(1, X + 20, 400 - 2 * WAVE_STEP_PX) == 2, "a slightly drifting swipe was refused");
}

static void test_clearing(void)
{
    /* wave_reset() is what pointsrc.c calls when the touch device goes away. A press
     * left anchored would make the NEXT press look like a continuation of it. */
    begin(1, 2);
    put(X, 400);
    CHECK(feed(1, X, 400 - 2 * WAVE_STEP_PX) == 2, "the setup swipe did not zoom");

    wave_reset();
    CHECK(feed(1, X, 400 - 1 * WAVE_STEP_PX) == 0,
          "a move straight after a reset was measured from the old anchor");

    /* AND THE RESET RE-ANCHORS, which is the other half of what it is for. The finger is
     * still down -- there is no new down edge to wait for -- so the first report after
     * the reset becomes the new anchor and the ones behind it are measured from there. */
    CHECK(feed(1, X, 400 - 3 * WAVE_STEP_PX) == 2,
          "the re-anchored press was not measured from the new anchor");

    /* And once the finger does lift, the next press is a fresh one. */
    CHECK(feed(0, X, 400 - 3 * WAVE_STEP_PX) == 0, "the release sent a step");
    CHECK(put(X, 400) == 0, "the press after a reset sent something at the down edge");
    CHECK(feed(1, X, 400 - 1 * WAVE_STEP_PX) == 1, "the press after a reset did not zoom");
}

int main(void)
{
    test_geometry();
    test_gate();
    test_latch();
    test_feel();
    test_along_the_wave();
    test_clearing();

    printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "ok", checks, failures);
    return failures ? 1 : 0;
}
