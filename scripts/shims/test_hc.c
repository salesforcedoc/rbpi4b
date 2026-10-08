/*
 * test_hc.c -- the HOT CUE pad row, with no Pi, no panel and no rbp anywhere in this
 * file.
 *
 * It links the PRODUCTION hc_zone.c, for the reason test_fx.c, test_prompt.c,
 * test_wave.c, test_util.c, test_side.c and test_menu.c link theirs: a rectangle
 * boundary, an anchored press and a comparison against a threshold are exactly the
 * kind of thing that gets quietly rewritten in a copy.
 *
 * WHAT IS AT STAKE HERE IS NOT SYMMETRIC, and the tests are ordered to say so. A tap
 * that fires when it should not does not merely disappoint -- rbp's HOT CUE pads STORE
 * a cue on an unlit pad, so it writes a cue onto the operator's own track, at the
 * playhead, mid-set. A tap that fails to fire is a bug report. So:
 *
 * 1. THE GATE, pinned in every combination of its three terms, and pinned BY NAME on
 *    the failure direction: `registered == 0` is 0 no matter what the other two say,
 *    and every unreadable value (`pad_mode == -1`, `registered == -1`, any browse mode
 *    but the performance screen) lands on 0 as well. There is no default that fires.
 *
 * 2. THE KEYCODE ARITHMETIC. rbp reads a pad as `keycode - 0x4116`, so an off-by-one
 *    here triggers the NEIGHBOURING pad -- and on an empty neighbour, creates a cue
 *    there. All eight pinned, and pinned against 0x4116 + pad so the test is the
 *    instruction stream's arithmetic and not this module's restatement of it.
 *
 * 3. THE GEOMETRY, as arithmetic and as pinned numbers: sixteen cells that tile their
 *    two spans exactly with a 9 px gutter and no overlap, 20 rows tall, inside the
 *    screen, and clear of the BEAT FX plate the other zone owns. Every cell centre must
 *    map back to its own (deck, pad), which is what catches a swapped row or a
 *    transposed deck -- the two ways a touch lands on the wrong cue with the geometry
 *    looking fine one cell at a time.
 *
 * 4. THE LATCH, which is the same safety property fx_zone.c keeps: a press this module
 *    swallowed has been withheld from rbp for its whole life, so it must keep being
 *    OURS until it lifts, even after it has slid off. And a release with no press
 *    behind it is rbp's, not ours.
 *
 * 5. FIRE-ON-PRESS, ANCHORED, AND EXACTLY ONE KEY PER TAP. A pad fires on the UP->DOWN
 *    edge that lands on it -- a hot cue is a jump, and the operator corrected the first
 *    release-firing build ("it should trigger on the press not the release",
 *    2026-10-07). The release that follows fires NOTHING but is still swallowed. A
 *    press that begins on pad 1 and drags over pad 2 fires pad 1 and only pad 1: the
 *    cell that fires is the cell the press STARTED on, which is what stops a drag
 *    across the pad row from triggering whatever the finger crossed.
 */

#include "hc_zone.h"
#include "menu_zone.h"      /* MZ_LOGICAL_W/H: the space the grid must fit in */
#include "rbp_abi.h"        /* K_PAD1, so the keycode test is rbp's constant */

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
 * 1. The gate
 * ------------------------------------------------------------------------- */

static void test_gate(void)
{
    /* The one combination that fires. */
    CHECK(hc_may_fire(BROWSE_MODE_PLAY, HC_PAD_MODE_HOT, 1) == 1,
          "the performance screen, HOT CUE mode and a registered cue must fire");

    /* THE DANGEROUS DIRECTION, named. An empty pad -- which is what rbp answers 0
     * for -- must never fire, whatever the screen or the mode says. If one of these
     * ever passes as 1, a tap on an unlit pad writes a hot cue. */
    CHECK(hc_may_fire(BROWSE_MODE_PLAY, HC_PAD_MODE_HOT, 0) == 0,
          "AN EMPTY PAD FIRED -- this creates a hot cue on the operator's track");
    CHECK(hc_may_fire(BROWSE_MODE_PLAY, HC_PAD_MODE_HOT, -1) == 0,
          "an unreadable registration fired -- must fail towards doing nothing");
    CHECK(hc_may_fire(BROWSE_MODE_PLAY, HC_PAD_MODE_HOT, -1000) == 0,
          "a negative registration fired");

    /* rbp's pad keycodes mean whatever the CURRENT pad mode makes them mean, so a
     * deck in any other mode -- or one whose mode could not be read -- must not be
     * sent one. AUTO BEAT LOOP would engage a loop. */
    CHECK(hc_may_fire(BROWSE_MODE_PLAY, 1, 1) == 0,
          "a deck in AUTO BEAT LOOP mode fired -- this engages a loop");
    CHECK(hc_may_fire(BROWSE_MODE_PLAY, 2, 1) == 0, "a deck in SLIP BEAT LOOP fired");
    CHECK(hc_may_fire(BROWSE_MODE_PLAY, 3, 1) == 0, "a deck in BEAT JUMP fired");
    CHECK(hc_may_fire(BROWSE_MODE_PLAY, -1, 1) == 0,
          "an unreadable pad mode fired -- must fail towards doing nothing");

    /* And off the performance screen the grid is not even drawn at those rows. */
    CHECK(hc_may_fire(BROWSE_MODE_UTILITY, HC_PAD_MODE_HOT, 1) == 0,
          "the UTILITY screen fired a pad");
    CHECK(hc_may_fire(0, HC_PAD_MODE_HOT, 1) == 0, "browse mode 0 fired a pad");
    CHECK(hc_may_fire(-1, HC_PAD_MODE_HOT, 1) == 0, "an unreadable browse mode fired");
}

/* ---------------------------------------------------------------------------
 * 2. The keycodes
 * ------------------------------------------------------------------------- */

static void test_keycodes(void)
{
    /* rbp's own arithmetic: onHotCueEvent @0x2f5720 computes `keycode - 0x4116` as
     * the pad number and range-tests `keycode - 0x4117` in 0..7. */
    for (int pad = 1; pad <= HC_PADS; pad++) {
        int k = hc_pad_keycode(pad);

        CHECK(k == 0x4116 + pad, "pad %d -> keycode %#x, not %#x (rbp reads pad as "
              "keycode - 0x4116)", pad, k, 0x4116 + pad);
        CHECK(k == K_PAD1 + (pad - 1), "pad %d is not K_PAD1 + %d", pad, pad - 1);
        CHECK(k - 0x4116 == pad, "the keycode for pad %d reads back as pad %d",
              pad, k - 0x4116);
    }

    /* Pad 1 IS K_PAD1, and pad 0 is not a pad at all. K_BEATJUMP 0x4116 sits one
     * below the range and is a pad-MODE key, not a pad. */
    CHECK(hc_pad_keycode(1) == K_PAD1, "pad 1 must be K_PAD1 itself");
    CHECK(hc_pad_keycode(0) == 0, "pad 0 answered a keycode");
    CHECK(hc_pad_keycode(9) == 0, "pad 9 answered a keycode");
    CHECK(hc_pad_keycode(-1) == 0, "a negative pad answered a keycode");

    /* The mode keys are NOT reachable from a pad number -- a one-off would put the
     * hottest keycode in the file on a pad. */
    for (int pad = 1; pad <= HC_PADS; pad++)
        CHECK(hc_pad_keycode(pad) != K_BEATJUMP && hc_pad_keycode(pad) != K_HOTCUE &&
              hc_pad_keycode(pad) != K_ALOOP && hc_pad_keycode(pad) != K_SLIPLOOP,
              "pad %d produced a pad-MODE keycode", pad);
}

/* ---------------------------------------------------------------------------
 * 3. The geometry
 * ------------------------------------------------------------------------- */

/* Every cell's own rect, from the module rather than from the macros -- so these
 * checks are about what hc_cell() tests, not about what the header says. */
static void cell(int deck, int pad, int *x0, int *y0, int *x1, int *y1)
{
    hc_cell_rect(deck, pad, x0, y0, x1, y1);
}

static void test_geometry(void)
{
    for (int d = 0; d < 2; d++) {
        for (int p = 1; p <= HC_PADS; p++) {
            int x0, y0, x1, y1;

            cell(d, p, &x0, &y0, &x1, &y1);
            CHECK(x1 - x0 + 1 == HC_CELL_W, "deck %d pad %d is %d px wide, not %d",
                  d + 1, p, x1 - x0 + 1, HC_CELL_W);
            CHECK(y1 - y0 + 1 == 20, "deck %d pad %d is %d rows tall, not 20",
                  d + 1, p, y1 - y0 + 1);
            CHECK(x0 >= 0 && x1 < MZ_LOGICAL_W && y0 >= 0 && y1 < MZ_LOGICAL_H,
                  "deck %d pad %d runs off the screen", d + 1, p);
            CHECK(y1 < 581, "deck %d pad %d reaches into the DECK strip", d + 1, p);

            /* The four cells of a row step by the pitch and leave exactly the gutter. */
            if (p < 4 || (p > 4 && p < 8)) {
                int ax0, ay0, ax1, ay1, bx0, by0, bx1, by1;

                cell(d, p, &ax0, &ay0, &ax1, &ay1);
                cell(d, p + 1, &bx0, &by0, &bx1, &by1);
                CHECK(ax1 < bx0, "deck %d pads %d and %d overlap", d + 1, p, p + 1);
                CHECK(bx0 - ax1 - 1 == HC_CELL_PITCH - HC_CELL_W,
                      "deck %d's gutter between pads %d and %d is %d, not %d",
                      d + 1, p, p + 1, bx0 - ax1 - 1, HC_CELL_PITCH - HC_CELL_W);
            }

            /* The two rows are the same rect moved down, and they do not touch. */
            if (p <= 4) {
                int bx0, by0, bx1, by1;

                cell(d, p + 4, &bx0, &by0, &bx1, &by1);
                CHECK(bx0 == x0 && bx1 == x1, "deck %d: pads %d and %d are not in "
                      "the same column", d + 1, p, p + 4);
                CHECK(y1 < by0, "deck %d's two rows overlap at pad %d", d + 1, p);
                CHECK(by0 - y1 - 1 == 10, "deck %d's row gap is %d, not 10",
                      d + 1, by0 - y1 - 1);
            }
        }

        /* The deck's span, end to end: first cell at the deck's own origin, last
         * cell ending at the screen's margin. */
        {
            int x0, y0, x1, y1;
            int dx0, dy0, dx1, dy1;

            cell(d, 1, &x0, &y0, &x1, &y1);
            cell(d, 4, &dx0, &dy0, &dx1, &dy1);
            CHECK(x0 == (d ? HC_DECK2_X0 : HC_DECK1_X0),
                  "deck %d's first cell starts at %d", d + 1, x0);
            CHECK(dx1 == (d ? 1269 : 629),
                  "deck %d's last cell ends at %d, not the measured %d",
                  d + 1, dx1, d ? 1269 : 629);
        }
    }

    /* The two decks' rows are the SAME rows: a cell in deck 1 and a cell in deck 2
     * share their y, which is what makes the two grids read as one band. */
    for (int p = 1; p <= HC_PADS; p++) {
        int ax0, ay0, ax1, ay1, bx0, by0, bx1, by1;

        cell(0, p, &ax0, &ay0, &ax1, &ay1);
        cell(1, p, &bx0, &by0, &bx1, &by1);
        CHECK(ay0 == by0 && ay1 == by1, "pad %d sits at a different row on deck 2", p);
        CHECK(ax1 < bx0, "the two decks' pad %d cells overlap or touch", p);
    }

    /* The decks' cells are 148 px apart at the join -- 630..650 -- so the two grids
     * cannot be mistaken for one another by a point between them. */
    {
        int ax0, ay0, ax1, ay1, bx0, by0, bx1, by1;

        cell(0, 4, &ax0, &ay0, &ax1, &ay1);
        cell(1, 1, &bx0, &by0, &bx1, &by1);
        CHECK(ax1 == 629 && bx0 == 651, "the deck join moved: %d..%d", ax1, bx0);
        CHECK(HC_DECK_SPLIT > ax1 && HC_DECK_SPLIT <= bx0,
              "the deck split %d is not in the gap %d..%d",
              HC_DECK_SPLIT, ax1 + 1, bx0 - 1);
    }

    /* The grid is below the label row and clear of the BEAT FX plate the other zone
     * owns (x 1090..1269, y 47..490) -- the two zones can never both answer a point. */
    {
        int x0, y0, x1, y1;

        cell(0, 1, &x0, &y0, &x1, &y1);
        CHECK(y0 > 509, "the pad row reaches into the HOT CUE label row");
        CHECK(y1 < 490 || x1 < 1090,
              "the pad row reaches into the BEAT FX panel's rectangle");
    }
}

/* Every cell centre must map back to its own deck and pad. This is the check that
 * catches a swapped row (A..D under E..H) or a transposed deck -- both of which look
 * perfectly plausible one cell at a time and put a finger on the wrong cue. */
static void test_lookup(void)
{
    for (int d = 0; d < 2; d++) {
        for (int p = 1; p <= HC_PADS; p++) {
            int x0, y0, x1, y1, dd = -1, pp = 0;

            cell(d, p, &x0, &y0, &x1, &y1);
            CHECK(hc_cell((x0 + x1) / 2, (y0 + y1) / 2, &dd, &pp) == 1,
                  "deck %d pad %d's centre answered no cell", d + 1, p);
            CHECK(dd == d && pp == p, "deck %d pad %d's centre answered deck %d pad %d",
                  d + 1, p, dd + 1, pp);
        }
    }

    /* Row 1 is pads 1..4 and row 2 is 5..8, left to right -- the lettering on the
     * glass (A B C D / E F G H). */
    for (int c = 0; c < 4; c++) {
        int x0, y0, x1, y1, dd, pp;

        cell(0, c + 1, &x0, &y0, &x1, &y1);
        hc_cell((x0 + x1) / 2, (y0 + y1) / 2, &dd, &pp);
        CHECK(pp == c + 1, "deck 1's column %d answered pad %d, not %d", c, pp, c + 1);

        cell(0, c + 5, &x0, &y0, &x1, &y1);
        hc_cell((x0 + x1) / 2, (y0 + y1) / 2, &dd, &pp);
        CHECK(pp == c + 5, "deck 1's lower column %d answered pad %d, not %d",
              c, pp, c + 5);
    }

    /* The four corners of a cell are inside it -- inclusive at both ends. */
    {
        int x0, y0, x1, y1, dd, pp;

        cell(0, 1, &x0, &y0, &x1, &y1);
        CHECK(hc_cell(x0, y0, &dd, &pp) == 1 && dd == 0 && pp == 1,
              "the top-left corner is outside its own cell");
        CHECK(hc_cell(x1, y1, &dd, &pp) == 1 && dd == 0 && pp == 1,
              "the bottom-right corner is outside its own cell");
        CHECK(hc_cell(x0 - 1, y0, &dd, &pp) == 0, "one px left of a cell answered it");
        CHECK(hc_cell(x0, y0 - 1, &dd, &pp) == 0, "one px above a cell answered it");
        CHECK(hc_cell(x1 + 1, y0, &dd, &pp) == 0, "one px right of a cell answered it");
        CHECK(hc_cell(x0, y1 + 1, &dd, &pp) == 0, "one px below a cell answered it");
    }

    /* The gutter between two cells is nobody's. */
    {
        int ax0, ay0, ax1, ay1, bx0, by0, bx1, by1;

        cell(0, 1, &ax0, &ay0, &ax1, &ay1);
        cell(0, 2, &bx0, &by0, &bx1, &by1);
        for (int x = ax1 + 1; x < bx0; x++)
            CHECK(hc_cell(x, ay0 + 2, NULL, NULL) == 0,
                  "the gutter at x=%d answered a cell", x);
    }

    /* The gap between the decks, the label row above, the 10-row gap between the two
     * rows, and the DECK strip below are all nobody's. */
    for (int x = 630; x <= 650; x++)
        CHECK(hc_cell(x, 528, NULL, NULL) == 0,
              "the deck gap at x=%d answered a cell", x);
    for (int y = 495; y <= 517; y++)
        CHECK(hc_cell(84, y, NULL, NULL) == 0, "the label row at y=%d answered a cell", y);
    for (int y = 538; y <= 547; y++)
        CHECK(hc_cell(84, y, NULL, NULL) == 0, "the row gap at y=%d answered a cell", y);
    for (int y = 568; y <= 600; y++)
        CHECK(hc_cell(84, y, NULL, NULL) == 0, "row %d answered a pad", y);
    CHECK(hc_cell(0, 528, NULL, NULL) == 0, "the left margin answered a cell");
    CHECK(hc_cell(1279, 528, NULL, NULL) == 0, "the right margin answered a cell");
    CHECK(hc_cell(-1, 528, NULL, NULL) == 0, "a negative x answered a cell");
    CHECK(hc_cell(84, -1, NULL, NULL) == 0, "a negative y answered a cell");

    /* A NULL out-parameter is a legal question ("is this point on a cell?"). */
    CHECK(hc_cell(84, 528, NULL, NULL) == 1, "a cell centre answered 0 with NULL outs");

    /* Out-of-range cells answer an empty rect rather than a wrapped one. */
    {
        int x0, y0, x1, y1;

        hc_cell_rect(0, 0, &x0, &y0, &x1, &y1);
        CHECK(x1 < x0, "pad 0 answered a rect");
        hc_cell_rect(0, 9, &x0, &y0, &x1, &y1);
        CHECK(x1 < x0, "pad 9 answered a rect");
        hc_cell_rect(-1, 1, &x0, &y0, &x1, &y1);
        CHECK(x1 < x0, "deck -1 answered a rect");
        hc_cell_rect(2, 1, &x0, &y0, &x1, &y1);
        CHECK(x1 < x0, "deck 2 answered a rect");
    }
}

/* ---------------------------------------------------------------------------
 * 4 and 5. The latch, the anchoring, and the release rule
 * ------------------------------------------------------------------------- */

/* A report, exactly as pointsrc.c's funnel hands them out. Returns what hc_feed()
 * answered, and reports the cell it filled in (or -1). */
static int report(int down, int x, int y, int *deck, int *pad)
{
    *deck = -1;
    *pad = 0;
    return hc_feed(down, x, y, deck, pad);
}

/* A cell centre, so a test never restates the geometry it is testing. */
static void centre(int deck, int pad, int *x, int *y)
{
    int x0, y0, x1, y1;

    cell(deck, pad, &x0, &y0, &x1, &y1);
    *x = (x0 + x1) / 2;
    *y = (y0 + y1) / 2;
}

static void test_zone(void)
{
    int d, p, x, y, x2, y2;

    hc_reset();

    /* A CLEAN TAP FIRES ON THE PRESS, NOT THE LIFT -- the operator's own correction of
     * the first build ("it should trigger on the press not the release", 2026-10-07).
     * Asserted in BOTH halves, because a tap that fires twice is as wrong as one that
     * fires late: the press answers the cell and the lift that follows answers NOTHING. */
    centre(0, 3, &x, &y);
    CHECK(report(1, x, y, &d, &p) == MZ_FEED_TAKEN && d == 0 && p == 3,
          "the press did not fire deck 1 pad 3 (answered deck %d pad %d)", d + 1, p);
    CHECK(hc_latched(), "a press on a cell did not latch");
    CHECK(report(0, x, y, &d, &p) == MZ_FEED_TAKEN, "the release was not taken");
    CHECK(d == -1, "the RELEASE fired a pad -- a tap must fire once, on the press");
    CHECK(!hc_latched(), "the release left the latch set");

    /* Deck 2, the other row. */
    centre(1, 7, &x, &y);
    CHECK(report(1, x, y, &d, &p) == MZ_FEED_TAKEN && d == 1 && p == 7,
          "deck 2 pad 7 fired deck %d pad %d", d + 1, p);
    CHECK(report(0, x, y, &d, &p) == MZ_FEED_TAKEN && d == -1,
          "deck 2's release fired a pad");

    /* A RUN OF DOWNS IS THE SAME FINGER, not a second press: the pad must not fire
     * again, and the reports stay swallowed so the lift still finds a live press. */
    hc_reset();
    centre(0, 6, &x, &y);
    CHECK(report(1, x, y, &d, &p) == MZ_FEED_TAKEN && d == 0 && p == 6,
          "the press on pad 6 did not fire it");
    for (int i = 0; i < 3; i++)
        CHECK(report(1, x, y, &d, &p) == MZ_FEED_TAKEN && d == -1,
              "a run report fired the pad a second time");
    CHECK(hc_latched(), "the run lifted the latch");
    CHECK(report(0, x, y, &d, &p) == MZ_FEED_TAKEN && d == -1, "the release fired a pad");

    /* A press that MISSES every cell is rbp's, first report to last -- the module
     * must not adopt a finger that merely crossed the row. */
    hc_reset();
    CHECK(report(1, 640, 528, &d, &p) == MZ_FEED_NONE,
          "a press in the deck gap was taken");
    CHECK(!hc_latched(), "a press in the deck gap latched");
    CHECK(report(0, 640, 528, &d, &p) == MZ_FEED_NONE,
          "the release in the deck gap was taken");
    CHECK(report(1, 84, 545, &d, &p) == MZ_FEED_NONE, "a press in the row gap was taken");
    CHECK(report(1, 84, 600, &d, &p) == MZ_FEED_NONE, "a press in the DECK strip was taken");

    /* A DRAG ACROSS THE ROW FIRES THE CELL IT STARTED ON AND NOT THE ONE IT REACHED.
     * The press fires pad 1 on the way down; the report over pad 2 is a run and fires
     * nothing; the lift over pad 2 fires nothing. This is the whole reason the anchor is
     * taken from the up->down edge's own coordinates and the release never fires. */
    hc_reset();
    centre(0, 1, &x, &y);
    centre(0, 2, &x2, &y2);
    CHECK(report(1, x, y, &d, &p) == MZ_FEED_TAKEN && d == 0 && p == 1,
          "the press on pad 1 did not fire it");
    CHECK(report(1, x2, y2, &d, &p) == MZ_FEED_TAKEN && d == -1,
          "a drag onto pad 2 fired it -- the anchor must not move");
    CHECK(report(0, x2, y2, &d, &p) == MZ_FEED_TAKEN && d == -1,
          "a lift over pad 2 fired it");
    CHECK(!hc_latched(), "the drag left the latch set");

    /* A press that slid OFF the grid entirely: fired its own cell on the press, then
     * swallowed and silent, and rbp must not be handed the release alone. */
    hc_reset();
    centre(0, 5, &x, &y);
    CHECK(report(1, x, y, &d, &p) == MZ_FEED_TAKEN && d == 0 && p == 5,
          "the press on pad 5 did not fire it");
    CHECK(report(0, 84, 620, &d, &p) == MZ_FEED_TAKEN && d == -1,
          "a press that slid off the grid fired a pad");
    CHECK(!hc_latched(), "sliding off left the latch set");

    /* A GESTURE THAT BEGAN ELSEWHERE IS NEVER ADOPTED, even when it slides across the
     * pads and lifts on one. This is the hole a re-anchoring latch opens: the finger
     * started on the glass, so the module owns none of it, and firing on the lift would
     * trigger whatever it happened to finish over. */
    hc_reset();
    centre(0, 3, &x, &y);
    CHECK(report(1, 84, 620, &d, &p) == MZ_FEED_NONE,
          "a press that began off the grid was taken");
    CHECK(report(1, x, y, &d, &p) == MZ_FEED_NONE,
          "a slide onto a pad was adopted -- the anchor must not move");
    CHECK(report(0, x, y, &d, &p) == MZ_FEED_NONE && d == -1,
          "a gesture that began off the grid fired a pad when it lifted on one");
    CHECK(!hc_latched(), "a gesture that began off the grid stayed latched");

    /* A release with no press behind it is rbp's report, not ours -- handing back a
     * bare up would be an event rbp never had a down for. */
    hc_reset();
    centre(0, 4, &x, &y);
    CHECK(report(0, x, y, &d, &p) == MZ_FEED_NONE,
          "a release with no press behind it was taken");
    CHECK(d == -1, "a release with no press behind it fired a pad");

    /* A LATER press anchors to its own cell -- but only a real one: the anchor is set
     * on the up->down edge, so a release has to come between the two. */
    hc_reset();
    centre(0, 1, &x, &y);
    centre(0, 8, &x2, &y2);
    CHECK(report(1, x, y, &d, &p) == MZ_FEED_TAKEN && d == 0 && p == 1,
          "the first press did not fire pad 1");
    report(0, x, y, &d, &p);               /* lifted: the first gesture is over */
    CHECK(report(1, x2, y2, &d, &p) == MZ_FEED_TAKEN && d == 0 && p == 8,
          "the second press fired deck %d pad %d, not deck 1 pad 8", d + 1, p);
    report(0, x2, y2, &d, &p);

    /* And a press that misses clears it, so a stale anchor cannot arm the next
     * release. NOTE THE RELEASE BEFORE THE MISS: the miss is a NEW press and has to
     * arrive as the up->down edge of one, not as another down of the press on pad 2 --
     * a second `down` with no lift between is the same finger moving, and the module
     * swallows it as a run. */
    hc_reset();
    centre(0, 2, &x, &y);
    CHECK(report(1, x, y, &d, &p) == MZ_FEED_TAKEN && d == 0 && p == 2,
          "the press on pad 2 did not fire it");
    report(0, x, y, &d, &p);               /* lifted: the press on pad 2 is over */
    CHECK(report(1, 640, 528, &d, &p) == MZ_FEED_NONE, "the miss was taken");
    CHECK(!hc_latched(), "a press that missed left the earlier anchor armed");
    CHECK(report(0, x, y, &d, &p) == MZ_FEED_NONE && d == -1,
          "a release after a press that missed fired a pad");

    /* hc_reset() drops a press in flight -- pointsrc.c calls it when the pointer
     * device goes away, and a half-held pad must not survive that. */
    hc_reset();
    centre(1, 6, &x, &y);
    report(1, x, y, &d, &p);
    CHECK(hc_latched(), "the press did not latch before the reset");
    hc_reset();
    CHECK(!hc_latched(), "hc_reset() left a press latched");
    CHECK(report(0, x, y, &d, &p) == MZ_FEED_NONE,
          "a release after hc_reset() was still taken");

    hc_reset();
}

int main(void)
{
    test_gate();
    test_keycodes();
    test_geometry();
    test_lookup();
    test_zone();

    printf("test_hc: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
