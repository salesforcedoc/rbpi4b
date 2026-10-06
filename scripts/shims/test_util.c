/*
 * test_util.c -- the UTILITY screen's touch gesture, with no Pi, no device, no rbp
 * and no screen anywhere in this file.
 *
 * It links the PRODUCTION util_zone.c, for the reason test_menu.c and test_side.c
 * link theirs: what is pinned here -- a row boundary, a coalescing rule, a tap's
 * travel -- is exactly the kind that gets quietly rewritten in a copy.
 *
 * util_zone.c is deliberately dumb. It cannot see rbp, so everything rbp would tell
 * it arrives in a struct util_state the tests hand it, and everything it wants sent
 * comes back as an act and a signed count. That is what makes the two things with
 * real consequences testable without a unit:
 *
 * 1. THE GEOMETRY, as arithmetic. `row = (y - 50) / 52`, 12 rows, the list ending at
 *    y 673. Every boundary is checked a pixel either side, and the constants
 *    themselves are pinned as numbers, so a change to any of them is a deliberate
 *    act and not a drift. The last check in the section is the one that matters
 *    most: the list must not reach the deck strip rbp draws for itself at ~712, or
 *    this gesture would be sitting on top of the operator's own controls.
 *
 * 2. THE GATE, which is the operator's *"only do this in this menu"*. Everything
 *    below is answered only in mode 7 -- the number rbp's own dispatcher compares
 *    against -- and never on the calibration sub-screen, which is a touch surface
 *    rbp draws its own marks in. The gate is checked on the down edge, on the
 *    release, and (the one that is easy to forget) on the MOVE path: the screen can
 *    change under a press that is already down, because the FLX4's BACK reaches rbp
 *    without ever passing through this module.
 *
 * 3. THE SCROLL, clause by clause. Nothing is sent at the down edge; nothing is sent
 *    for a finger that has not crossed a row; one row of travel from the ANCHOR is
 *    one rotation, exactly once; a resting finger re-sends nothing; and a direction
 *    change reverses by exactly the difference rather than starting over. The cap is
 *    checked to bound one answer and to still converge on the next.
 *
 * 4. THE TWO WAYS THIS COULD HURT SOMEBODY, which is what most of the file is for.
 *    A DROP NEVER TAPS (its whole point is that the list moved under the finger, so
 *    "the row you touched" is no longer what the operator aimed at); and A DRAG
 *    NEVER ROTATES WHILE EDITING, because rbp's rotate edits the highlighted item's
 *    VALUE in that state -- measured on the live unit, one rotate turned LOAD LOCK
 *    from UNLOCK to LOCK. The tap still sends its Enter there, which is what leaves
 *    edit mode, so the gesture is always the way back out rather than a trap.
 *
 * Build + run (static, so no rootfs is needed to load it):
 *     make test
 */
#define _GNU_SOURCE
#include "util_zone.h"
#include "menu_zone.h"
#include "rbp_abi.h"        /* BROWSE_MODE_UTILITY: pinned equal to the module's copy */

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
 * The harness. util_feed() is the module's one door, so every test is a sequence of
 * reports fed to it exactly as pointsrc.c's funnel would, with rbp's state standing
 * in a plain struct the test sets by hand.
 *
 * X is 400 throughout -- inside the list (0..1279) and inside every row -- because
 * nothing in this module depends on x except the slop, and the tests that care about
 * x say so.
 * ------------------------------------------------------------------------- */

static struct util_state S;
static const int X = 400;

static void begin(int mode, int editing, int calibrating, int cursor)
{
    util_reset();
    S.mode = mode;
    S.editing = editing;
    S.calibrating = calibrating;
    S.cursor = cursor;
}

static int feed(int down, int x, int y, int *act, int *val)
{
    return util_feed(&S, down, x, y, act, val);
}

/* One report, with the answer thrown away -- for setting up a press. */
static void put(int down, int x, int y)
{
    int a, v;

    feed(down, x, y, &a, &v);
}

/* A press and its release at the same point: the whole gesture, in one call.
 * Returns the RELEASE's verdict, which is where everything interesting happens. */
static int tap_at(int y, int *act, int *val)
{
    put(1, X, y);
    return feed(0, X, y, act, val);
}

/* The y of a row's top edge. */
static int rowy(int r)
{
    return UTIL_ROW0_Y + r * UTIL_ROW_H;
}

/* ---------------------------------------------------------------------------
 * 1. The geometry, as arithmetic.
 * ------------------------------------------------------------------------- */
static void test_geometry(void)
{
    int r;

    /* The constants are pinned as numbers, so moving one is a decision made in this
     * file and not a drift nobody notices. 50 and 52 were measured off /dev/fb0 with
     * the screen up; 12 is what rbp's own setCursor clamps to. */
    CHECK(UTIL_ROW0_Y == 50, "UTIL_ROW0_Y is %d, not the measured 50", UTIL_ROW0_Y);
    CHECK(UTIL_ROW_H == 52, "UTIL_ROW_H is %d, not the measured 52", UTIL_ROW_H);
    CHECK(UTIL_ROWS == 12, "UTIL_ROWS is %d, not the 12 rbp draws and clamps to",
          UTIL_ROWS);
    CHECK(UTIL_LIST_Y1 == 673, "UTIL_LIST_Y1 is %d, not 673", UTIL_LIST_Y1);

    /* THE ONE THAT MATTERS. Below the list is rbp's own deck strip; if the band grew
     * into it this gesture would cover controls that are rbp's, not ours. */
    CHECK(UTIL_LIST_Y1 < 712, "the list ends at y %d, which is inside rbp's deck"
          " strip (~712) -- this gesture would be sitting on the operator's own"
          " controls", UTIL_LIST_Y1);

    /* Every row, both ends inclusive and one pixel past each end. */
    for (r = 0; r < UTIL_ROWS; r++) {
        CHECK(util_row(rowy(r)) == r, "the top pixel of row %d reads as row %d",
              r, util_row(rowy(r)));
        CHECK(util_row(rowy(r) + UTIL_ROW_H - 1) == r,
              "the bottom pixel of row %d reads as row %d",
              r, util_row(rowy(r) + UTIL_ROW_H - 1));
    }
    CHECK(util_row(UTIL_ROW0_Y - 1) == -1,
          "y %d is above the list but reads as a row", UTIL_ROW0_Y - 1);
    CHECK(util_row(UTIL_LIST_Y1 + 1) == -1,
          "y %d is below the list but reads as a row", UTIL_LIST_Y1 + 1);
    CHECK(util_row(0) == -1, "y 0 reads as a row");
    CHECK(util_row(799) == -1, "the bottom of the glass reads as a row");

    /* The band, as a hit test. */
    CHECK(util_in_list(UTIL_LIST_X0, UTIL_ROW0_Y), "the list's first pixel is not on it");
    CHECK(util_in_list(UTIL_LIST_X1, UTIL_LIST_Y1), "the list's last pixel is not on it");
    CHECK(!util_in_list(UTIL_LIST_X1 + 1, UTIL_ROW0_Y), "x past the right edge is on it");
    CHECK(!util_in_list(UTIL_LIST_X0 - 1, UTIL_ROW0_Y), "x left of the list is on it");
    CHECK(!util_in_list(UTIL_LIST_X0, UTIL_ROW0_Y - 1), "y above the list is on it");
    CHECK(!util_in_list(UTIL_LIST_X0, UTIL_LIST_Y1 + 1), "y below the list is on it");

    /* The header's claim that the tap slop is under half a row, so the two tests
     * (wandered / one row of travel) cannot disagree about a deliberate tap. */
    CHECK(UTIL_TAP_SLOP * 2 < UTIL_ROW_H,
          "the tap slop (%d) is not comfortably less than half a row (%d): a finger"
          " could cross into the next row and still be a tap", UTIL_TAP_SLOP,
          UTIL_ROW_H / 2);

    /* The module's private copy of the mode against rbp's own constant: they are
     * written down twice on purpose (util_zone.h is dependency free) and this is
     * what makes the duplication safe. */
    CHECK(UTIL_MODE_UTILITY == BROWSE_MODE_UTILITY,
          "util_zone.h's UTIL_MODE_UTILITY is %d but rbp's is %d",
          UTIL_MODE_UTILITY, BROWSE_MODE_UTILITY);
}

/* ---------------------------------------------------------------------------
 * 2. The gate: "only do this in this menu".
 * ------------------------------------------------------------------------- */
static void test_gate(void)
{
    int m, act = UTIL_ACT_SCROLL, val = 99;
    int modes_not_utility[] = { -1, 0, 1, 2, 3, 6, 8, 9, 10, 11, 99 };
    unsigned i;

    for (i = 0; i < sizeof(modes_not_utility) / sizeof(modes_not_utility[0]); i++) {
        m = modes_not_utility[i];
        begin(m, 0, 0, 4);
        CHECK(feed(1, X, rowy(2), &act, &val) == MZ_FEED_NONE,
              "a press on the list in mode %d was claimed", m);
        CHECK(act == UTIL_ACT_NONE && val == 0,
              "mode %d: act=%d val=%d after a refused press", m, act, val);
        CHECK(feed(0, X, rowy(2), &act, &val) == MZ_FEED_NONE,
              "a release in mode %d was claimed", m);
    }

    /* The calibration sub-screen is inside UTILITY and must still be refused: rbp
     * draws its own marks there and reads touches on it itself. */
    begin(UTIL_MODE_UTILITY, 0, 1, 4);
    CHECK(feed(1, X, rowy(2), &act, &val) == MZ_FEED_NONE,
          "a press during TOUCH DISPLAY CALIBRATION was claimed");
    CHECK(feed(0, X, rowy(2), &act, &val) == MZ_FEED_NONE,
          "a release during calibration was claimed");

    /* And the screen it IS for. */
    begin(UTIL_MODE_UTILITY, 0, 0, 4);
    CHECK(feed(1, X, rowy(2), &act, &val) == MZ_FEED_TAKEN,
          "a press on the list in UTILITY was NOT claimed");
    CHECK(act == UTIL_ACT_NONE && val == 0,
          "the down edge sent something: act=%d val=%d", act, val);

    /* THE MOVE PATH'S GATE, which is the one that is easy to miss: mode changes
     * mid-press arrive from the FLX4 and never touch this module, so the press is
     * dropped and rbp is handed the report. */
    begin(UTIL_MODE_UTILITY, 0, 0, 4);
    put(1, X, rowy(2));
    S.mode = 3;
    CHECK(feed(1, X, rowy(2) + 60, &act, &val) == MZ_FEED_NONE,
          "a move after the screen left UTILITY was still claimed");

    /* ...and the same on the RELEASE, with no move report in between -- the case
     * where an Enter would otherwise land on a screen this module knows nothing
     * about. */
    begin(UTIL_MODE_UTILITY, 0, 0, 4);
    put(1, X, rowy(2));
    S.mode = 0;
    CHECK(feed(0, X, rowy(2), &act, &val) == MZ_FEED_NONE,
          "a release after the screen left UTILITY was still claimed");
    CHECK(act != UTIL_ACT_TAP, "an Enter was sent onto a screen that is not UTILITY");
}

/* ---------------------------------------------------------------------------
 * 3. Latching: a press is ours for its whole life, or not at all.
 * ------------------------------------------------------------------------- */
static void test_latch(void)
{
    int act, val;

    /* A press that starts BELOW the list is never ours, even when it wanders on. */
    begin(UTIL_MODE_UTILITY, 0, 0, 3);
    CHECK(feed(1, X, UTIL_LIST_Y1 + 1, &act, &val) == MZ_FEED_NONE,
          "a press below the list was claimed");
    CHECK(feed(1, X, rowy(4), &act, &val) == MZ_FEED_NONE,
          "a press that started below the list became ours when it moved on");
    CHECK(feed(0, X, rowy(4), &act, &val) == MZ_FEED_NONE,
          "a release of a press that started below the list was claimed");

    /* The same above it, and off to the side. */
    begin(UTIL_MODE_UTILITY, 0, 0, 3);
    CHECK(feed(1, X, UTIL_ROW0_Y - 1, &act, &val) == MZ_FEED_NONE,
          "a press above the list was claimed");
    CHECK(feed(0, X, UTIL_ROW0_Y - 1, &act, &val) == MZ_FEED_NONE,
          "its release was claimed");
    begin(UTIL_MODE_UTILITY, 0, 0, 3);
    CHECK(feed(1, UTIL_LIST_X1 + 1, rowy(4), &act, &val) == MZ_FEED_NONE,
          "a press past the right edge was claimed");
    CHECK(feed(0, UTIL_LIST_X1 + 1, rowy(4), &act, &val) == MZ_FEED_NONE,
          "its release was claimed");

    /* A release with nothing behind it is never ours. */
    begin(UTIL_MODE_UTILITY, 0, 0, 3);
    CHECK(feed(0, X, rowy(4), &act, &val) == MZ_FEED_NONE,
          "a release with no press behind it was claimed");

    /* util_reset() forgets a press in flight -- the device went away -- so the NEXT
     * press does not look like a continuation of it. */
    begin(UTIL_MODE_UTILITY, 0, 0, 3);
    put(1, X, rowy(4));
    util_reset();
    CHECK(feed(0, X, rowy(4), &act, &val) == MZ_FEED_NONE,
          "a release after util_reset() was still claimed");

    /* And a completed press leaves nothing behind: the next one works normally. */
    begin(UTIL_MODE_UTILITY, 0, 0, 3);
    CHECK(tap_at(rowy(6), &act, &val) == MZ_FEED_TAKEN && act == UTIL_ACT_TAP,
          "a tap did not register");
    CHECK(tap_at(rowy(7), &act, &val) == MZ_FEED_TAKEN && act == UTIL_ACT_TAP
          && val == 4,
          "a second tap did not register, or counted from the first press's anchor"
          " (val=%d, wanted 4)", val);
}

/* ---------------------------------------------------------------------------
 * 4. The scroll: one row of travel from the anchor is one rotation, once.
 * ------------------------------------------------------------------------- */
static void test_scroll(void)
{
    int act, val;

    begin(UTIL_MODE_UTILITY, 0, 0, 0);

    /* The down edge anchors and sends nothing. */
    CHECK(feed(1, X, rowy(2), &act, &val) == MZ_FEED_TAKEN, "the press was not claimed");
    CHECK(act == UTIL_ACT_NONE && val == 0, "the down edge sent act=%d val=%d", act, val);

    /* Under a row: still nothing. 51 px is one short, and it is also past the slop,
     * so this same press has already stopped being a tap -- a wobble in the 25..51 px
     * band neither scrolls nor taps, which is the intended outcome. */
    CHECK(feed(1, X, rowy(2) + UTIL_ROW_H - 1, &act, &val) == MZ_FEED_TAKEN,
          "the move was not claimed");
    CHECK(act == UTIL_ACT_NONE && val == 0, "a move under one row sent act=%d val=%d",
          act, val);

    /* Exactly one row, exactly once. */
    CHECK(feed(1, X, rowy(2) + UTIL_ROW_H, &act, &val) == MZ_FEED_TAKEN,
          "the one-row move was not claimed");
    CHECK(act == UTIL_ACT_SCROLL && val == 1, "one row of travel sent act=%d val=%d"
          " (wanted SCROLL +1)", act, val);

    /* A resting finger re-sends nothing. */
    CHECK(feed(1, X, rowy(2) + UTIL_ROW_H, &act, &val) == MZ_FEED_TAKEN,
          "a repeat report was not claimed");
    CHECK(act == UTIL_ACT_NONE && val == 0, "a resting finger re-sent act=%d val=%d",
          act, val);

    /* Two more rows. */
    CHECK(feed(1, X, rowy(2) + 3 * UTIL_ROW_H, &act, &val) == MZ_FEED_TAKEN,
          "the three-row move was not claimed");
    CHECK(act == UTIL_ACT_SCROLL && val == 2, "three rows total sent act=%d val=%d"
          " (wanted SCROLL +2, the difference from what was already sent)",
          act, val);

    /* DIRECTION CHANGE: back toward the anchor by a row and a bit reverses by
     * exactly one -- it does not start a new count. */
    CHECK(feed(1, X, rowy(2) + 2 * UTIL_ROW_H, &act, &val) == MZ_FEED_TAKEN,
          "the reverse move was not claimed");
    CHECK(act == UTIL_ACT_SCROLL && val == -1,
          "reversing by one row sent act=%d val=%d (wanted SCROLL -1)", act, val);

    /* Back past the anchor: the count goes negative, which rbp reads as upward
     * rotations -- the window scrolls the other way and no sign fix is needed. Two
     * rows past the anchor after two were already sent is four back. */
    CHECK(feed(1, X, rowy(2) - 2 * UTIL_ROW_H, &act, &val) == MZ_FEED_TAKEN,
          "the upward move was not claimed");
    CHECK(act == UTIL_ACT_SCROLL && val == -4, "two rows past the anchor sent act=%d"
          " val=%d (wanted SCROLL -4)", act, val);

    /* UPWARD IS THE SAME MAPPING. Truncation toward zero makes -60/52 = -1 and
     * -104/52 = -2, so an upward drag counts rows exactly as a downward one does. */
    begin(UTIL_MODE_UTILITY, 0, 0, 0);
    put(1, X, rowy(8));
    CHECK(feed(1, X, rowy(8) - UTIL_ROW_H, &act, &val) == MZ_FEED_TAKEN
          && act == UTIL_ACT_SCROLL && val == -1,
          "one row of upward travel sent act=%d val=%d", act, val);
    CHECK(feed(1, X, rowy(8) - 3 * UTIL_ROW_H, &act, &val) == MZ_FEED_TAKEN
          && act == UTIL_ACT_SCROLL && val == -2,
          "three rows of upward travel sent act=%d val=%d", act, val);

    /* A press that wanders far and comes back to the anchor has sent nothing net,
     * and still does not tap. */
    begin(UTIL_MODE_UTILITY, 0, 0, 3);
    put(1, X, rowy(4));
    put(1, X, rowy(4) + 3 * UTIL_ROW_H);
    put(1, X, rowy(4));
    CHECK(feed(0, X, rowy(4), &act, &val) == MZ_FEED_TAKEN && act != UTIL_ACT_TAP,
          "a drag that returned to its start still tapped (act=%d)", act);
}

/* ---------------------------------------------------------------------------
 * 5. The cap: one answer is bounded, and the count still converges.
 * ------------------------------------------------------------------------- */
static void test_cap(void)
{
    int act, val;
    int far = rowy(0) + 17 * UTIL_ROW_H;   /* seventeen rows: past UTIL_ROT_MAX */

    begin(UTIL_MODE_UTILITY, 0, 0, 0);
    put(1, X, rowy(0));
    CHECK(feed(1, X, far, &act, &val) == MZ_FEED_TAKEN, "the long move was not claimed");
    CHECK(act == UTIL_ACT_SCROLL && val == UTIL_ROT_MAX,
          "a seventeen-row jump sent act=%d val=%d (wanted SCROLL +%d)", act, val,
          UTIL_ROT_MAX);

    /* The remainder is not lost: the next report carries it, so a fast flick still
     * scrolls as far as the finger went. */
    CHECK(feed(1, X, far, &act, &val) == MZ_FEED_TAKEN, "the follow-up was not claimed");
    CHECK(act == UTIL_ACT_SCROLL && val == 1,
          "the remaining row sent act=%d val=%d (wanted SCROLL +1)", act, val);

    /* And a long reverse is capped the same way. Seventeen rows down sent +16 (the
     * cap); the finger then goes seventeen rows back UP, which is a net travel of
     * -17 from the anchor against 16 already sent -- thirty-three rotations owed, and
     * one answer carries the cap's worth of them. */
    begin(UTIL_MODE_UTILITY, 0, 0, 0);
    put(1, X, rowy(11));
    put(1, X, rowy(11) + 17 * UTIL_ROW_H);
    CHECK(feed(1, X, rowy(11) - 17 * UTIL_ROW_H, &act, &val) == MZ_FEED_TAKEN,
          "the long reverse was not claimed");
    CHECK(act == UTIL_ACT_SCROLL && val == -UTIL_ROT_MAX,
          "a long reverse sent act=%d val=%d (wanted SCROLL -%d)", act, val,
          UTIL_ROT_MAX);

    /* The header's claim that a tap can never need more than the cursor's own
     * range -- so the cap only ever bites on a drag. */
    CHECK(UTIL_ROT_MAX >= UTIL_ROWS - 1,
          "UTIL_ROT_MAX (%d) is smaller than the cursor's own range (%d): a tap"
          " across the whole window could not be sent in one answer",
          UTIL_ROT_MAX, UTIL_ROWS - 1);
}

/* ---------------------------------------------------------------------------
 * 6. The tap: the row you touched, and nothing else.
 * ------------------------------------------------------------------------- */
static void test_tap(void)
{
    int act, val, r;

    /* Every row against every cursor, which is the whole of "the window cancels".
     * A travel of `row - cursor` is asked for whatever the window is showing. */
    for (r = 0; r < UTIL_ROWS; r++) {
        int c;

        for (c = 0; c < UTIL_ROWS; c++) {
            begin(UTIL_MODE_UTILITY, 0, 0, c);
            CHECK(tap_at(rowy(r), &act, &val) == MZ_FEED_TAKEN && act == UTIL_ACT_TAP,
                  "tapping row %d with the cursor on %d did not register", r, c);
            CHECK(val == r - c, "tapping row %d with the cursor on %d asked for %d"
                  " rotations, wanted %d", r, c, val, r - c);
        }
    }

    /* A tap on the row the cursor is already on is a REAL answer, not a no-op: it
     * still sends its Enter, and the count is zero. */
    begin(UTIL_MODE_UTILITY, 0, 0, 5);
    CHECK(tap_at(rowy(5), &act, &val) == MZ_FEED_TAKEN && act == UTIL_ACT_TAP && val == 0,
          "tapping the cursor's own row sent act=%d val=%d (wanted TAP 0)", act, val);

    /* The whole row is one target: the name cell and the value cell are the same
     * row, so a tap anywhere across the width means the same thing. */
    begin(UTIL_MODE_UTILITY, 0, 0, 0);
    put(1, 10, rowy(7));
    CHECK(feed(0, 10, rowy(7), &act, &val) == MZ_FEED_TAKEN && val == 7,
          "a tap in the name cell asked for %d", val);
    begin(UTIL_MODE_UTILITY, 0, 0, 0);
    put(1, 1100, rowy(7));
    CHECK(feed(0, 1100, rowy(7), &act, &val) == MZ_FEED_TAKEN && val == 7,
          "a tap in the value cell asked for %d", val);

    /* A finger that wobbles a little is still a tap: within the slop the count is
     * unchanged and the release still taps. */
    begin(UTIL_MODE_UTILITY, 0, 0, 1);
    put(1, X, rowy(6));
    put(1, X + 5, rowy(6) + UTIL_TAP_SLOP);
    CHECK(feed(0, X + 5, rowy(6) + UTIL_TAP_SLOP, &act, &val) == MZ_FEED_TAKEN
          && act == UTIL_ACT_TAP && val == 5,
          "a wobbling tap sent act=%d val=%d (wanted TAP +5)", act, val);

    /* The tap counts from where the finger LANDED, not from where it was when the
     * report came: a wobble down onto the next row's edge and back to the anchor is
     * still the row it started on. */
    begin(UTIL_MODE_UTILITY, 0, 0, 0);
    put(1, X, rowy(3));
    put(1, X, rowy(3) + UTIL_TAP_SLOP);
    CHECK(feed(0, X, rowy(3), &act, &val) == MZ_FEED_TAKEN && val == 3,
          "a wobble-and-return tap asked for %d, wanted 3", val);

    /* A DRAG NEVER TAPS, even one that ends back on the anchor: the list moved under
     * it, so "the row you touched" is no longer what the operator was aiming at. */
    begin(UTIL_MODE_UTILITY, 0, 0, 0);
    put(1, X, rowy(4));
    put(1, X, rowy(4) + UTIL_TAP_SLOP + 1);
    put(1, X, rowy(4));
    CHECK(feed(0, X, rowy(4), &act, &val) == MZ_FEED_TAKEN && act == UTIL_ACT_NONE,
          "a drag that ended on its anchor sent act=%d val=%d (wanted nothing)",
          act, val);
}

/* ---------------------------------------------------------------------------
 * 7. Edit mode, which is the one that could rewrite a setting.
 * ------------------------------------------------------------------------- */
static void test_editing(void)
{
    int act, val;

    /* A TAP WHILE EDITING still taps, and still sends its Enter -- that is what
     * leaves edit mode, so the gesture is the way OUT and never a trap. But the
     * count is ZERO on purpose: rotating here edits the highlighted item's value,
     * not the list. */
    begin(UTIL_MODE_UTILITY, 1, 0, 2);
    CHECK(tap_at(rowy(9), &act, &val) == MZ_FEED_TAKEN && act == UTIL_ACT_TAP,
          "a tap while editing did not register");
    CHECK(val == 0, "a tap while editing asked for %d rotations -- that would edit"
          " the highlighted item's VALUE, not scroll", val);

    /* A DRAG WHILE EDITING NOW SENDS ITS ROTATIONS, and that is the operator's own
     * correction. Measured on the live unit: one rotate in this state turned LOAD
     * LOCK from UNLOCK to LOCK -- so this IS the value edit, and the first release's
     * refusal to send anything here was the reason they could enter a value row and
     * never change it. The rotations go out exactly as they do when not editing,
     * because rbp is what decides their meaning. */
    begin(UTIL_MODE_UTILITY, 1, 0, 2);
    put(1, X, rowy(2));
    CHECK(feed(1, X, rowy(2) + UTIL_ROW_H, &act, &val) == MZ_FEED_TAKEN,
          "the editing drag was not claimed");
    CHECK(act == UTIL_ACT_SCROLL && val == 1,
          "a drag while editing sent act=%d val=%d (wanted SCROLL +1) -- that is the"
          " value edit and nothing else can do it", act, val);
    CHECK(feed(1, X, rowy(2) + 4 * UTIL_ROW_H, &act, &val) == MZ_FEED_TAKEN
          && act == UTIL_ACT_SCROLL && val == 3,
          "a longer drag while editing sent act=%d val=%d (wanted SCROLL +3)",
          act, val);
    /* Reversing must track, not replay: back to the anchor-minus-one row. */
    CHECK(feed(1, X, rowy(2) - UTIL_ROW_H, &act, &val) == MZ_FEED_TAKEN
          && act == UTIL_ACT_SCROLL && val == -5,
          "reversing an editing drag sent act=%d val=%d (wanted SCROLL -5)", act, val);
    /* And the release at the end of it is a DRAG, not a tap: it must not also send
     * an Enter, which would leave edit mode the instant the value was changed. */
    CHECK(feed(0, X, rowy(2) - UTIL_ROW_H, &act, &val) == MZ_FEED_TAKEN
          && act == UTIL_ACT_NONE,
          "the end of an editing drag sent act=%d -- an Enter there would have ended"
          " edit mode on release", act);

    /* Leaving edit mode restores the ordinary meaning of the same gesture: the very
     * next press scrolls, and a tap travels. */
    begin(UTIL_MODE_UTILITY, 1, 0, 2);
    put(1, X, rowy(2));
    put(0, X, rowy(2));
    S.editing = 0;
    put(1, X, rowy(2));
    CHECK(feed(1, X, rowy(2) + UTIL_ROW_H, &act, &val) == MZ_FEED_TAKEN
          && act == UTIL_ACT_SCROLL && val == 1,
          "after leaving edit mode a drag sent act=%d val=%d (wanted SCROLL +1)",
          act, val);
}

/* ---------------------------------------------------------------------------
 * 8. The refused report is REFUSED, not half-answered: *act and *value are cleared
 *    on every path that is not a TAKEN answer, so a stale answer cannot be acted on.
 * ------------------------------------------------------------------------- */
static void test_clearing(void)
{
    int act, val;

    begin(0, 0, 0, 4);
    act = 99; val = 99;
    feed(1, X, rowy(2), &act, &val);
    CHECK(act == UTIL_ACT_NONE && val == 0,
          "a refused press left act=%d val=%d", act, val);

    begin(UTIL_MODE_UTILITY, 0, 0, 4);
    act = 99; val = 99;
    feed(1, X, rowy(2), &act, &val);           /* latched, nothing sent */
    CHECK(act == UTIL_ACT_NONE && val == 0,
          "the down edge left act=%d val=%d", act, val);

    /* The module tolerates a caller that does not want one of the two out-params,
     * because pointsrc.c's ladder passes both but a future caller need not. */
    begin(UTIL_MODE_UTILITY, 0, 0, 4);
    put(1, X, rowy(2));
    CHECK(feed(1, X, rowy(2) + UTIL_ROW_H, NULL, NULL) == MZ_FEED_TAKEN,
          "a NULL out-param was not tolerated");
    begin(UTIL_MODE_UTILITY, 0, 0, 4);
    put(1, X, rowy(2));
    act = 99; val = 99;
    feed(1, X, rowy(2) + UTIL_ROW_H, &act, NULL);
    CHECK(act == UTIL_ACT_SCROLL, "a NULL value out-param broke the act (act=%d)", act);
}

int main(void)
{
    test_geometry();
    test_gate();
    test_latch();
    test_scroll();
    test_cap();
    test_tap();
    test_editing();
    test_clearing();

    printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "ok", checks, failures);
    return failures ? 1 : 0;
}
