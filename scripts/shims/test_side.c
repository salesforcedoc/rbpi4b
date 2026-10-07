/*
 * test_side.c -- the edge drawers' unit test: no Pi, no device, no rbp, no screen.
 *
 * It links the PRODUCTION side_zone.c, side_paint.c, menu_zone.c and menu_paint.c,
 * for the reason test_menu.c links the production gesture and image: the rules
 * pinned here -- a hit boundary, a sweep's threshold, a mirror -- are exactly the
 * kind that get quietly rewritten in a copy. Its surface is a local array: there is
 * no framebuffer, no plane, no thread and no device anywhere in this file.
 *
 * Six things are pinned, and each has a failure it is standing in front of.
 *
 * 1. THE GEOMETRY, as arithmetic rather than as a picture. The two panels have to
 *    be disjoint and mirror images of one another; every rectangle has to be the
 *    one the header says, inclusive at both ends; the controls have to be in the
 *    operator's own order (SYNC on top, CUE and PLAY at the bottom) and every box
 *    has to be disjoint from the fader's grab lane, or a drag past a button would
 *    fire it; and SZ_LINE -- the header's local copy of the font's line box, kept a
 *    literal so the module stays dependency free -- has to still equal
 *    MENU_FONT_LINE. The failure behind that last one is a font change that moves
 *    every title and readout by a row and says nothing.
 *
 * 2. THE MIRROR. The module works in PANEL-LOCAL x (0 is the outer edge on both
 *    sides), and pointsrc.c reflects x at the wire (1279 - x) *after* it has
 *    spoken, so what it is fed is logical, pre-reflection. Get the mirror wrong and
 *    every tap on one drawer lands on the control mirroring the one aimed at --
 *    invisible to any test that only ever feeds one edge. Every hit test below is
 *    therefore run twice, once per side, at mirrored points.
 *
 * 3. THE USB STOP GATE. The right drawer's entry column (local 0..55) is absolute
 *    1224..1279 -- EXACTLY the band's seventh column, whose USB STOP cell sits at
 *    the strip's rows. If the drawer armed there, a press aimed at the safe eject
 *    would be swallowed and the operator's media would not stop. SZ_ENTRY_Y0 is the
 *    whole defence and it is checked at the boundary, one row either side.
 *
 * 4. THE GESTURE, clause by clause: the opening swipe needs exactly SZ_SWIPE_PX AND
 *    a dominant axis; a press that starts outside is never ours even when it
 *    wanders in; a button fires only on a release that is still on its own box; an
 *    outward swipe on the background closes it mid-press; a press in the entry
 *    column that never travelled far enough comes back as MZ_FEED_TAP so rbp is
 *    handed it late rather than never; and, since the relayout, NO BUTTON CLOSES THE
 *    DRAWER -- using SYNC, a nudge and PLAY in a row leaves the panel out, and the
 *    ONE thing that puts it away is the outward sweep on the drawer's own background.
 *    A press on the glass rbp draws for itself is not ours at all and comes back
 *    MZ_FEED_NONE, on both edges.
 *
 * 5. BOTH DRAWERS AT ONCE, and the routing that makes it work: the second panel
 *    opens while the first is out, both answer for their own channel, and a press
 *    off every open panel is handed straight back to rbp rather than dismissing
 *    anything -- the operator reversed "tap away to close", so the drawers stay up
 *    until they are swiped away. Plus the band against the drawers in both
 *    directions -- the band must not arm while a drawer is out, and a drawer must
 *    not arm while the band is open.
 *
 * 6. THE IMAGE. That a paint leaves no pixel of the panel unwritten (the plane is
 *    single-buffered and the drawer is drawn over whatever was there), that a second
 *    identical paint changes NOTHING (the repaint gate in menu_draw.c is the only
 *    thing keeping the buffer still, and it can only be that if a repeat paint is a
 *    no-op), that the pressed image differs only inside the pressed box, that the
 *    frame's thin bands are on the correct device edges on BOTH sides -- the mirror
 *    in the one place it changes pixels -- that the fader's cap is at the top of the
 *    well for 1023 and at the bottom for 0, and that the two nudge marks are
 *    actually distinguishable: a bar for '-' and a cross for '+', because the atlas
 *    has no '+' and a pair that did not match would be worse than no mark.
 *
 * Build + run (static, so no rootfs is needed to load it):
 *     make test
 */
#define _GNU_SOURCE
#include "side_zone.h"
#include "side_paint.h"
#include "menu_zone.h"
#include "menu_paint.h"
#include "menu_font.h"

#include <stdio.h>
#include <string.h>

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
 * The gesture harness. side_feed() is the pure module's one door, so every test
 * below is a sequence of reports fed to it, exactly as pointsrc.c's funnel would.
 * ------------------------------------------------------------------------- */

static void reset_all(void)
{
    menu_reset();
    side_reset_all();
}

/* Where a panel-local point is in absolute logical x -- the module's own transform,
 * used here so the tests read as "the outer edge", "56 px in" and not as arithmetic
 * that could be wrong in the same direction as the code. */
static int absx(int side, int lx)
{
    return side_abs_x(side, lx);
}

/* A press, in one call: feed the down edge. */
static int press(int side, int lx, int y, int *act, int *val)
{
    return side_feed(side, SZ_PTR_MAIN, 1, absx(side, lx), y, act, val);
}

/* A motion report within the press that is already down. */
static int move(int side, int lx, int y, int *act, int *val)
{
    return side_feed(side, SZ_PTR_MAIN, 1, absx(side, lx), y, act, val);
}

/* The lift. */
static int lift(int side, int lx, int y, int *act, int *val)
{
    return side_feed(side, SZ_PTR_MAIN, 0, absx(side, lx), y, act, val);
}

/* Open one drawer the way the operator does: a press on the outer edge, then a
 * predominantly-inward sweep of exactly SZ_SWIPE_PX. Returns the sweep's verdict. */
static int open_by_swipe(int side)
{
    int act = SZ_ACT_NONE, val = 0;

    press(side, 0, 600, &act, &val);
    return move(side, SZ_SWIPE_PX, 600, &act, &val);
}

/* The same, but through the FUNNEL -- which is where "both at once" and the "away"
 * dismissal actually live, so anything about two drawers has to go through this. */
static int open_by_funnel(int side)
{
    int act = SZ_ACT_NONE, val = 0, which = -2;

    side_feed_any(SZ_PTR_MAIN, 1, absx(side, 0), 600, &which, &act, &val);
    side_feed_any(SZ_PTR_MAIN, 1, absx(side, SZ_SWIPE_PX), 600, &which, &act, &val);
    side_feed_any(SZ_PTR_MAIN, 0, absx(side, SZ_SWIPE_PX), 600, &which, &act, &val);
    return side_is_open(side);
}

/* ---------------------------------------------------------------------------
 * 1. The geometry, as arithmetic.
 * ------------------------------------------------------------------------- */
static void test_geometry(void)
{
    CHECK(SZ_LINE == MENU_FONT_LINE,
          "SZ_LINE is %d but the font's line box is %d -- the header's local copy has"
          " drifted, and every title and readout has moved with it",
          SZ_LINE, MENU_FONT_LINE);

    /* The two panels: each a SZ_W-wide strip at its own end of the glass, disjoint,
     * and mirrored by one subtraction. */
    CHECK(SZ_RX0 == MZ_LOGICAL_W - SZ_W, "SZ_RX0 is %d, not %d",
          SZ_RX0, MZ_LOGICAL_W - SZ_W);
    CHECK(SZ_W - 1 < SZ_RX0, "the two panels overlap: left ends at %d, right starts"
          " at %d", SZ_W - 1, SZ_RX0);
    CHECK(side_abs_x(SZ_LEFT, 0) == 0, "the left panel's outer edge is not x = 0");
    CHECK(side_abs_x(SZ_RIGHT, 0) == MZ_LOGICAL_W - 1,
          "the right panel's outer edge is not the screen's last column");
    CHECK(side_abs_x(SZ_RIGHT, SZ_W - 1) == SZ_RX0,
          "the right panel's inner edge is not SZ_RX0");
    CHECK(side_abs_x(SZ_LEFT, SZ_W - 1) == SZ_W - 1, "the left panel's inner edge moved");

    /* The transform is its own inverse on both sides. */
    {
        int side, lx;

        for (side = 0; side < 2; side++)
            for (lx = 0; lx < SZ_W; lx++)
                CHECK(side_local_x(side, side_abs_x(side, lx)) == lx,
                      "the mirror is not its own inverse at side %d, lx %d", side, lx);
    }

    /* LEFT is deck 1, and it is the whole of "deck 1 on the left". */
    CHECK(side_channel(SZ_LEFT) == 1, "the left drawer is not channel 1");
    CHECK(side_channel(SZ_RIGHT) == 2, "the right drawer is not channel 2");

    /* Every rectangle, against the numbers in the header. */
    CHECK(SZ_TITLE_X0 == SZ_PAD, "the title's inset is not SZ_PAD");
    CHECK(SZ_TITLE_X1 == SZ_W - 1 - SZ_PAD, "the title overruns the panel");
    CHECK(SZ_TITLE_Y1 == SZ_TITLE_Y0 + SZ_LINE - 1, "the title box is not one line tall");
    CHECK(SZ_BTN_X0 == SZ_PAD && SZ_BTN_X1 == SZ_W - 1 - SZ_PAD,
          "the button boxes are not the panel's inner width");
    CHECK(SZ_SYNC_Y1 == SZ_SYNC_Y0 + SZ_BTN_H - 1, "the SYNC box is not SZ_BTN_H tall");
    CHECK(SZ_NUDGE_Y1 == SZ_NUDGE_Y0 + SZ_BTN_H - 1, "a nudge box is not SZ_BTN_H tall");
    CHECK(SZ_CUE_Y1 == SZ_CUE_Y0 + SZ_BTN_H - 1, "the CUE box is not SZ_BTN_H tall");
    CHECK(SZ_PLAY_Y1 == SZ_PLAY_Y0 + SZ_BTN_H - 1, "the PLAY box is not SZ_BTN_H tall");
    CHECK(SZ_READ_Y1 == SZ_READ_Y0 + SZ_LINE - 1, "the readout is not one line tall");
    CHECK(SZ_FADER_Y1 - SZ_FADER_Y0 == SZ_FADER_TRAVEL,
          "SZ_FADER_TRAVEL is not the track's own length");
    CHECK(SZ_FADER_GX0 == SZ_FADER_X0 - SZ_FADER_PAD, "the grab lane's left slack moved");
    CHECK(SZ_FADER_GX1 == SZ_FADER_X1 + SZ_FADER_PAD, "the grab lane's right slack moved");

    /* THE OPERATOR'S ORDER, top to bottom, as strict inequalities: SYNC above the
     * nudge pair above the readout above the fader above CUE above PLAY. Each of
     * these is a sentence in the request -- "a beat sync button at the top of the
     * strip", "move the play/cue button to the bottom", "add +/- buttons to nudge the
     * track" -- and each would be silently undone by a header edit that moved one
     * block past another. */
    CHECK(SZ_SYNC_Y0 > SZ_TITLE_Y1, "SYNC starts before the title ends");
    CHECK(SZ_SYNC_Y1 < SZ_NUDGE_Y0, "SYNC and the nudge pair overlap");
    CHECK(SZ_NUDGE_Y0 > SZ_SYNC_Y1, "the nudge pair starts before SYNC ends");
    CHECK(SZ_NUDGE_Y1 < SZ_READ_Y0, "the nudge pair and the readout overlap");
    CHECK(SZ_READ_Y1 < SZ_FADER_Y0, "the readout and the fader's track overlap");
    CHECK(SZ_FADER_Y1 < SZ_CUE_Y0, "the fader's track reaches into CUE");
    CHECK(SZ_CUE_Y0 > SZ_FADER_Y1, "CUE starts before the fader ends");
    CHECK(SZ_PLAY_Y0 > SZ_CUE_Y1, "PLAY and CUE overlap");
    CHECK(SZ_PLAY_Y1 < SZ_H - SZ_PAD, "PLAY ends %d rows from the panel's bottom edge,"
          " not the %d the title leaves at the top", SZ_H - 1 - SZ_PLAY_Y1, SZ_PAD);

    /* THE NUDGE PAIR: two cells of equal width with a seam between them, and the
     * seam is deliberately nobody's -- a press that straddles the middle must be
     * answered "nothing" rather than "whichever way the arithmetic rounded". */
    CHECK(SZ_NUDGE_M_X0 == SZ_BTN_X0, "the '-' cell does not start at the panel's inset");
    CHECK(SZ_NUDGE_P_X1 == SZ_BTN_X1, "the '+' cell does not reach the panel's inset");
    CHECK(SZ_NUDGE_M_X1 + 1 + SZ_NUDGE_GAP == SZ_NUDGE_P_X0,
          "the seam between the nudge cells is not SZ_NUDGE_GAP: %d..%d then %d",
          SZ_NUDGE_M_X1, SZ_NUDGE_P_X0, SZ_NUDGE_GAP);
    CHECK((SZ_NUDGE_M_X1 - SZ_NUDGE_M_X0) == (SZ_NUDGE_P_X1 - SZ_NUDGE_P_X0),
          "the two nudge cells are different widths: %d and %d",
          SZ_NUDGE_M_X1 - SZ_NUDGE_M_X0, SZ_NUDGE_P_X1 - SZ_NUDGE_P_X0);
    CHECK(SZ_NUDGE_M_X0 < SZ_NUDGE_M_X1 && SZ_NUDGE_M_X1 < SZ_NUDGE_P_X0,
          "the nudge cells are not left-to-right: '-' is x %d..%d and '+' is x %d..%d",
          SZ_NUDGE_M_X0, SZ_NUDGE_M_X1, SZ_NUDGE_P_X0, SZ_NUDGE_P_X1);

    /* THE TWO GAPS THAT KEEP THE FADER'S GRAB LANE OFF EVERY BUTTON. The lane runs
     * SZ_FADER_GY0..SZ_FADER_GY1; if it reached the nudge pair a drag could begin on
     * one, and if it reached CUE a drag could end on it. Both are checked as strict
     * inequalities on the ROWS, because that is how the hit test orders them. */
    CHECK(SZ_FADER_GY0 > SZ_NUDGE_Y1, "the fader's grab lane starts at %d, inside the"
          " nudge pair's last row %d: a drag can begin on a nudge cell",
          SZ_FADER_GY0, SZ_NUDGE_Y1);
    CHECK(SZ_FADER_GY1 < SZ_CUE_Y0, "the fader's grab lane ends at %d, inside CUE's"
          " first row %d: a drag can end on the transport", SZ_FADER_GY1, SZ_CUE_Y0);

    /* ...and the readout sits INSIDE the lane's own top slack, which is the whole of
     * why touching the number moves the fader. */
    CHECK(SZ_READ_Y0 >= SZ_FADER_GY0 && SZ_READ_Y1 <= SZ_FADER_GY1,
          "the readout at rows %d..%d has left the grab lane's %d..%d: the number is"
          " no longer a way to move the fader", SZ_READ_Y0, SZ_READ_Y1,
          SZ_FADER_GY0, SZ_FADER_GY1);
    CHECK(SZ_READ_X0 >= SZ_FADER_GX0 && SZ_READ_X1 <= SZ_FADER_GX1,
          "the readout is not within the grab lane's own x range");

    /* And the entry column, with the USB STOP gate stated as the row it is. */
    CHECK(SZ_ENTRY_Y0 == MZ_STRIP_Y1 + 1,
          "the entry column starts at row %d, not below the band's last row %d",
          SZ_ENTRY_Y0, MZ_STRIP_Y1);
    CHECK(SZ_ENTRY_W == MZ_SWIPE_PX && SZ_SWIPE_PX == SZ_CLOSE_PX,
          "the entry column and the two sweeps have come apart: %d, %d, %d",
          SZ_ENTRY_W, SZ_SWIPE_PX, SZ_CLOSE_PX);
}

/* ---------------------------------------------------------------------------
 * 2. The hit test, on both sides, at every boundary and one pixel either side.
 * ------------------------------------------------------------------------- */
static void hit_both(int lx, int y, int expect, const char *what)
{
    CHECK(side_hit(SZ_LEFT, absx(SZ_LEFT, lx), y) == expect,
          "left drawer: lx %d y %d is not %s", lx, y, what);
    CHECK(side_hit(SZ_RIGHT, absx(SZ_RIGHT, lx), y) == expect,
          "right drawer: lx %d y %d is not %s -- the mirror is wrong", lx, y, what);
}

static void test_hit(void)
{
    /* Outside the panel is NONE, inside it and off every control is BG -- two
     * different answers, because only BG can start a dismissal. */
    CHECK(side_hit(SZ_LEFT, SZ_W, 400) == SZ_HIT_NONE, "lx = SZ_W is inside the panel");
    CHECK(side_hit(SZ_LEFT, -1, 400) == SZ_HIT_NONE, "lx = -1 is inside the panel");
    CHECK(side_hit(SZ_LEFT, 90, -1) == SZ_HIT_NONE, "row -1 is inside the panel");
    CHECK(side_hit(SZ_LEFT, 90, SZ_H) == SZ_HIT_NONE, "row SZ_H is inside the panel");
    CHECK(side_hit(SZ_RIGHT, SZ_RX0 - 1, 400) == SZ_HIT_NONE,
          "the right drawer reaches into the left one's side of the glass");

    /* SYNC, at the top, with its own row above and below. */
    hit_both(SZ_BTN_X0, SZ_SYNC_Y0, SZ_HIT_SYNC, "SYNC (its own top-left corner)");
    hit_both(SZ_BTN_X1, SZ_SYNC_Y1, SZ_HIT_SYNC, "SYNC (its own bottom-right corner)");
    hit_both(SZ_BTN_X0, SZ_SYNC_Y0 - 1, SZ_HIT_BG, "background (one row above SYNC)");
    hit_both(SZ_BTN_X0, SZ_SYNC_Y1 + 1, SZ_HIT_BG, "background (one row below SYNC)");
    hit_both(SZ_BTN_X0 - 1, SZ_SYNC_Y0 + 10, SZ_HIT_BG, "background (one px left of SYNC)");
    hit_both(SZ_BTN_X1 + 1, SZ_SYNC_Y0 + 10, SZ_HIT_BG, "background (one px right of SYNC)");

    /* THE NUDGE PAIR, and the seam between the cells is the interesting one: it is
     * the only place in the panel where a press is deliberately nobody's. */
    hit_both(SZ_NUDGE_M_X0, SZ_NUDGE_Y0, SZ_HIT_NUDGE_M, "'-' (its own top-left corner)");
    hit_both(SZ_NUDGE_M_X1, SZ_NUDGE_Y1, SZ_HIT_NUDGE_M, "'-' (its own bottom-right)");
    hit_both(SZ_NUDGE_P_X0, SZ_NUDGE_Y0, SZ_HIT_NUDGE_P, "'+' (its own top-left corner)");
    hit_both(SZ_NUDGE_P_X1, SZ_NUDGE_Y1, SZ_HIT_NUDGE_P, "'+' (its own bottom-right)");
    hit_both(SZ_NUDGE_M_X1 + 1, SZ_NUDGE_Y0 + 10, SZ_HIT_BG,
             "background (the seam between the nudge cells)");
    hit_both(SZ_NUDGE_P_X0 - 1, SZ_NUDGE_Y0 + 10, SZ_HIT_BG,
             "background (the other side of the seam)");
    hit_both(SZ_NUDGE_M_X0 - 1, SZ_NUDGE_Y0 + 10, SZ_HIT_BG,
             "background (one px left of the '-' cell)");
    hit_both(SZ_NUDGE_P_X1 + 1, SZ_NUDGE_Y0 + 10, SZ_HIT_BG,
             "background (one px right of the '+' cell)");
    hit_both(90, SZ_NUDGE_Y1 + 1, SZ_HIT_BG, "background (one row below the nudge pair)");

    /* ...and the two cells never answer for each other, at any lx in the row. */
    {
        int lx;

        for (lx = SZ_BTN_X0; lx <= SZ_BTN_X1; lx++) {
            int h = side_hit(SZ_LEFT, absx(SZ_LEFT, lx), SZ_NUDGE_Y0 + 10);
            int want = (lx >= SZ_NUDGE_M_X0 && lx <= SZ_NUDGE_M_X1) ? SZ_HIT_NUDGE_M :
                       (lx >= SZ_NUDGE_P_X0 && lx <= SZ_NUDGE_P_X1) ? SZ_HIT_NUDGE_P :
                       SZ_HIT_BG;

            CHECK(h == want, "at lx %d in the nudge row the hit test says %d, not %d",
                  lx, h, want);
        }
    }

    /* CUE, including the seam above it. */
    hit_both(SZ_BTN_X0, SZ_CUE_Y0, SZ_HIT_CUE, "CUE (its own top-left corner)");
    hit_both(SZ_BTN_X1, SZ_CUE_Y1, SZ_HIT_CUE, "CUE (its own bottom-right corner)");
    hit_both(SZ_BTN_X0, SZ_CUE_Y0 - 1, SZ_HIT_BG, "background (one row above CUE)");
    hit_both(SZ_BTN_X0, SZ_CUE_Y1 + 1, SZ_HIT_BG, "background (one row below CUE)");
    hit_both(SZ_BTN_X0 - 1, SZ_CUE_Y0 + 10, SZ_HIT_BG, "background (one px left of CUE)");
    hit_both(SZ_BTN_X1 + 1, SZ_CUE_Y0 + 10, SZ_HIT_BG, "background (one px right of CUE)");

    /* PLAY, at the bottom, and the seam between the two transport boxes. */
    hit_both(SZ_BTN_X0, SZ_PLAY_Y0, SZ_HIT_PLAY, "PLAY (its own top-left corner)");
    hit_both(SZ_BTN_X1, SZ_PLAY_Y1, SZ_HIT_PLAY, "PLAY (its own bottom-right corner)");
    hit_both(90, SZ_PLAY_Y1 + 1, SZ_HIT_BG, "background (one row below PLAY)");
    hit_both(90, (SZ_CUE_Y1 + SZ_PLAY_Y0) / 2, SZ_HIT_BG, "background (the seam between"
             " CUE and PLAY)");

    /* The grab lane, which is wider and taller than the track it draws. */
    hit_both(SZ_FADER_GX0, SZ_FADER_GY0, SZ_HIT_FADER, "the fader lane (its own corner)");
    hit_both(SZ_FADER_GX1, SZ_FADER_GY1, SZ_HIT_FADER, "the fader lane (its own corner)");
    hit_both(SZ_FADER_GX0 - 1, 400, SZ_HIT_BG, "background (one px left of the lane)");
    hit_both(SZ_FADER_GX1 + 1, 400, SZ_HIT_BG, "background (one px right of the lane)");
    hit_both(90, SZ_FADER_GY0 - 1, SZ_HIT_BG, "background (one row above the lane)");
    hit_both(90, SZ_FADER_GY1 + 1, SZ_HIT_BG, "background (one row below the lane)");

    /* THE READOUT IS A GRAB, not background: it sits inside the lane's own top slack,
     * and touching the number is a friendlier way to move the fader than nothing. */
    hit_both(SZ_READ_X0, SZ_READ_Y0, SZ_HIT_FADER, "the value readout (which should be"
             " a fader grab, not background)");
    hit_both((SZ_READ_X0 + SZ_READ_X1) / 2, SZ_READ_Y1, SZ_HIT_FADER,
             "the value readout's last row");
}

/* ---------------------------------------------------------------------------
 * 3. The entry column, and the USB STOP gate under it.
 * ------------------------------------------------------------------------- */
static void entry_both(int lx, int y, int expect, const char *what)
{
    CHECK(side_entry_in(SZ_LEFT, absx(SZ_LEFT, lx), y) == expect,
          "left entry: lx %d y %d should be %s", lx, y, what);
    CHECK(side_entry_in(SZ_RIGHT, absx(SZ_RIGHT, lx), y) == expect,
          "right entry: lx %d y %d should be %s -- the mirror is wrong", lx, y, what);
}

static void test_entry(void)
{
    entry_both(0, SZ_ENTRY_Y0, 1, "in");
    entry_both(SZ_ENTRY_W - 1, 400, 1, "in");
    entry_both(SZ_ENTRY_W, 400, 0, "out (one px past the column)");
    entry_both(0, SZ_ENTRY_Y0 - 1, 0, "out (one row above it)");
    entry_both(0, SZ_H - 1, 1, "in (the panel's own last row)");

    /* THE USB STOP DEFENCE, asserted at the exact rows it protects. The right
     * drawer's entry column is absolute 1224..1279, which is the band's seventh
     * column; the strip's rows are the band's, so the drawer must not arm there. */
    CHECK(side_entry_in(SZ_RIGHT, MZ_LOGICAL_W - 1, MZ_STRIP_Y1) == 0,
          "the right drawer arms on the band's last strip row -- a press aimed at USB"
          " STOP would be swallowed");
    CHECK(side_entry_in(SZ_RIGHT, MZ_LOGICAL_W - 1, MZ_STRIP_Y1 + 1) == 1,
          "the drawer refuses the row immediately below the strip");
    CHECK(side_entry_in(SZ_LEFT, 0, MZ_STRIP_Y1) == 0,
          "the left drawer arms inside the strip's rows");
}

/* ---------------------------------------------------------------------------
 * 4a. The fader's maths.
 * ------------------------------------------------------------------------- */
static void test_fader_maths(void)
{
    int y, prev;

    CHECK(side_fader_v(SZ_FADER_Y0) == 1023, "the track's top is not full");
    CHECK(side_fader_v(SZ_FADER_Y1) == 0, "the track's bottom is not silent");
    CHECK(side_fader_v(SZ_FADER_Y0 - 1) == 1023, "a finger above the track does not clamp");
    CHECK(side_fader_v(SZ_FADER_Y1 + 1) == 0, "a finger below the track does not clamp");
    CHECK(side_fader_v(SZ_FADER_Y0 + SZ_FADER_TRAVEL / 2) == 511,
          "the middle of the track is %d, not 511",
          side_fader_v(SZ_FADER_Y0 + SZ_FADER_TRAVEL / 2));

    /* Monotone all the way down: a value that went back up somewhere would make the
     * handle jitter under a slow, straight drag. */
    prev = 1024;
    for (y = 0; y < SZ_H; y++) {
        int v = side_fader_v(y);

        CHECK(v <= prev, "the fader is not monotone at row %d (%d after %d)", y, v, prev);
        CHECK(v >= 0 && v <= 1023, "row %d gives %d, outside 0..1023", y, v);
        prev = v;
    }

    /* The inverse, which is where the handle is drawn, round-trips at both ends. */
    CHECK(side_fader_y(1023) == SZ_FADER_Y0, "the handle at full is not at the top");
    CHECK(side_fader_y(0) == SZ_FADER_Y1, "the handle at silent is not at the bottom");
    CHECK(side_fader_y(-5) == SZ_FADER_Y1, "the inverse does not clamp below");
    CHECK(side_fader_y(5000) == SZ_FADER_Y0, "the inverse does not clamp above");

    /* The readout. */
    CHECK(side_fader_pct(1023) == 100, "full is not 100");
    CHECK(side_fader_pct(0) == 0, "silent is not 0");
    CHECK(side_fader_pct(512) == 50, "512 is %d, not 50", side_fader_pct(512));
    CHECK(side_fader_pct(-1) == 0 && side_fader_pct(2048) == 100,
          "the readout does not clamp");
}

/* ---------------------------------------------------------------------------
 * 4b. The gesture.
 * ------------------------------------------------------------------------- */
static void test_gesture_shut(void)
{
    int act = 0, val = 0;

    /* A tap in the entry column: swallowed, and handed back at the lift. This is the
     * one report the drawer takes and gives to rbp, delayed by the sweep it never
     * made -- never dropped. */
    reset_all();
    act = val = -1;
    CHECK(press(SZ_LEFT, 10, 400, &act, &val) == MZ_FEED_TAKEN,
          "a press in the entry column was not swallowed");
    CHECK(act == SZ_ACT_NONE, "a press in the entry column claimed an action");
    CHECK(lift(SZ_LEFT, 10, 400, &act, &val) == MZ_FEED_TAP,
          "a tap in the entry column was not handed back to rbp");
    CHECK(!side_is_open(SZ_LEFT), "a tap opened the drawer");
    side_reset_all();

    /* A DRAG THAT IS NOT A SWEEP. One pixel short of the threshold, and exactly the
     * threshold with the finger as far down as it is in -- neither opens. */
    reset_all();
    press(SZ_LEFT, 0, 400, &act, &val);
    CHECK(move(SZ_LEFT, SZ_SWIPE_PX - 1, 400, &act, &val) == MZ_FEED_TAKEN,
          "a sweep's motion was not swallowed");
    CHECK(!side_is_open(SZ_LEFT), "one pixel short of SZ_SWIPE_PX opened the drawer");
    lift(SZ_LEFT, SZ_SWIPE_PX - 1, 400, &act, &val);
    side_reset_all();

    press(SZ_LEFT, 0, 400, &act, &val);
    move(SZ_LEFT, SZ_SWIPE_PX, 400 + SZ_SWIPE_PX, &act, &val);
    CHECK(!side_is_open(SZ_LEFT),
          "a diagonal drag opened the drawer: the axis is not dominant");
    lift(SZ_LEFT, SZ_SWIPE_PX, 400 + SZ_SWIPE_PX, &act, &val);
    CHECK(!side_is_open(SZ_LEFT), "the lifted diagonal opened the drawer");
    side_reset_all();

    /* Not ours, and never becomes ours: a press that starts outside the entry column
     * is rbp's for its whole life, even when it wanders into it. */
    reset_all();
    CHECK(side_feed(SZ_LEFT, SZ_PTR_MAIN, 1, 600, 400, &act, &val) == MZ_FEED_NONE,
          "a press in the middle of the glass was swallowed by the drawer");
    CHECK(side_feed(SZ_LEFT, SZ_PTR_MAIN, 1, 10, 400, &act, &val) == MZ_FEED_NONE,
          "a press that started outside became ours when it wandered into the entry"
          " column");
    CHECK(side_feed(SZ_LEFT, SZ_PTR_MAIN, 0, 10, 400, &act, &val) == MZ_FEED_NONE,
          "its release was swallowed");
    CHECK(!side_is_open(SZ_LEFT), "a wandering press opened the drawer");

    /* And a press INSIDE the entry column but in the strip's rows is not ours either
     * -- this is the left drawer's half of the USB STOP gate. */
    reset_all();
    CHECK(side_feed(SZ_LEFT, SZ_PTR_MAIN, 1, 10, MZ_STRIP_Y1, &act, &val) == MZ_FEED_NONE,
          "the drawer armed inside the band's strip rows");
}

static void test_gesture_open(void)
{
    int act = 0, val = 0;

    /* The open itself, on both sides, and that the lift leaves it out. */
    reset_all();
    CHECK(open_by_swipe(SZ_LEFT) == MZ_FEED_TAKEN, "the opening sweep was not swallowed");
    CHECK(side_is_open(SZ_LEFT), "the sweep did not open the left drawer");
    CHECK(!side_is_open(SZ_RIGHT), "opening the left drawer opened the right one too");
    act = val = -1;
    CHECK(lift(SZ_LEFT, SZ_SWIPE_PX, 600, &act, &val) == MZ_FEED_TAKEN,
          "the opening sweep's lift was not swallowed");
    CHECK(act == SZ_ACT_NONE, "the sweep that opened the drawer fired something");
    CHECK(side_is_open(SZ_LEFT), "lifting the finger put the drawer away");
    side_reset_all();

    reset_all();
    open_by_swipe(SZ_RIGHT);
    CHECK(side_is_open(SZ_RIGHT), "the sweep did not open the right drawer");
    CHECK(!side_is_open(SZ_LEFT), "opening the right drawer opened the left one too");
    side_reset_all();

    /* SYNC FIRES ON THE LIFT, and only there. */
    reset_all();
    open_by_swipe(SZ_LEFT);
    lift(SZ_LEFT, SZ_SWIPE_PX, 600, &act, &val);
    act = val = -1;
    CHECK(press(SZ_LEFT, 90, SZ_SYNC_Y0 + 10, &act, &val) == MZ_FEED_TAKEN,
          "a press on SYNC was not swallowed");
    CHECK(act == SZ_ACT_NONE, "SYNC fired on the press and not the lift");
    CHECK(side_pressed(SZ_LEFT) == SZ_HIT_SYNC, "SYNC is not highlighted while held");
    CHECK(lift(SZ_LEFT, 90, SZ_SYNC_Y0 + 10, &act, &val) == MZ_FEED_TAKEN,
          "SYNC's lift was not swallowed");
    CHECK(act == SZ_ACT_SYNC, "SYNC did not fire on the lift");
    CHECK(side_pressed(SZ_LEFT) == SZ_HIT_NONE, "a highlight outlived the press");
    /* ...AND THE DRAWER IS STILL OUT. This is the relayout's one behavioural change:
     * with a SYNC button, a nudge pair and the transport all on one panel, a drawer
     * that closed after every control would be three swipes per track. */
    CHECK(side_is_open(SZ_LEFT), "using SYNC put the drawer away");
    side_reset_all();

    /* PLAY, on the RIGHT drawer -- so the channel the caller will send on is proven
     * to come from the side and not from a constant. */
    reset_all();
    open_by_swipe(SZ_RIGHT);
    lift(SZ_RIGHT, SZ_SWIPE_PX, 600, &act, &val);
    press(SZ_RIGHT, SZ_BTN_X1 - 2, SZ_PLAY_Y0 + 10, &act, &val);
    CHECK(lift(SZ_RIGHT, SZ_BTN_X1 - 2, SZ_PLAY_Y0 + 10, &act, &val) == MZ_FEED_TAKEN,
          "PLAY's lift was not swallowed");
    CHECK(act == SZ_ACT_PLAY, "PLAY did not fire on the right drawer");
    CHECK(side_is_open(SZ_RIGHT), "using PLAY put the right drawer away");
    side_reset_all();

    /* A WHOLE PASS DOWN THE PANEL: SYNC, then a nudge, then PLAY, and the drawer is
     * still out at the end of all three. This is the operator's own sequence -- the
     * one the relayout exists to make possible without re-swiping. */
    reset_all();
    open_by_swipe(SZ_LEFT);
    lift(SZ_LEFT, SZ_SWIPE_PX, 600, &act, &val);
    press(SZ_LEFT, 90, SZ_SYNC_Y0 + 10, &act, &val);
    lift(SZ_LEFT, 90, SZ_SYNC_Y0 + 10, &act, &val);
    CHECK(act == SZ_ACT_SYNC, "SYNC did not fire in the pass");
    press(SZ_LEFT, (SZ_NUDGE_P_X0 + SZ_NUDGE_P_X1) / 2, SZ_NUDGE_Y0 + 10, &act, &val);
    CHECK(act == SZ_ACT_NUDGE_FWD, "the '+' cell did not start a forward bend");
    CHECK(side_nudging(SZ_LEFT) == 1, "side_nudging() does not see the forward bend");
    lift(SZ_LEFT, (SZ_NUDGE_P_X0 + SZ_NUDGE_P_X1) / 2, SZ_NUDGE_Y0 + 10, &act, &val);
    CHECK(act == SZ_ACT_NUDGE_STOP, "the bend did not stop on the lift");
    CHECK(side_nudging(SZ_LEFT) == 0, "side_nudging() still reports a bend after the lift");
    press(SZ_LEFT, 90, SZ_PLAY_Y0 + 10, &act, &val);
    lift(SZ_LEFT, 90, SZ_PLAY_Y0 + 10, &act, &val);
    CHECK(act == SZ_ACT_PLAY, "PLAY did not fire at the end of the pass");
    CHECK(side_is_open(SZ_LEFT), "the drawer closed somewhere in a three-control pass");
    side_reset_all();

    /* ROLL-OFF CANCELS. A press that slides from CUE to PLAY fires neither. */
    reset_all();
    open_by_swipe(SZ_LEFT);
    lift(SZ_LEFT, SZ_SWIPE_PX, 600, &act, &val);
    press(SZ_LEFT, 90, SZ_CUE_Y0 + 10, &act, &val);
    CHECK(side_pressed(SZ_LEFT) == SZ_HIT_CUE, "the highlight did not start on CUE");
    move(SZ_LEFT, 90, SZ_PLAY_Y0 + 10, &act, &val);
    CHECK(side_pressed(SZ_LEFT) == SZ_HIT_PLAY, "the highlight did not follow to PLAY");
    CHECK(act == SZ_ACT_NONE, "a slide claimed an action while still down");
    lift(SZ_LEFT, 90, SZ_PLAY_Y0 + 10, &act, &val);
    CHECK(act == SZ_ACT_NONE, "a press that slid from CUE to PLAY fired one of them");
    CHECK(side_is_open(SZ_LEFT), "a rolled-off press closed the drawer");
    side_reset_all();

    /* A ROLL-OFF OFF A NUDGE CELL IS NOT A BUTTON PRESS, but it does stop the bend --
     * which is the important half: rbp keeps bending until it is told otherwise, so a
     * bend that outlived its press would slide the track with nothing able to stop it. */
    reset_all();
    open_by_swipe(SZ_LEFT);
    lift(SZ_LEFT, SZ_SWIPE_PX, 600, &act, &val);
    press(SZ_LEFT, (SZ_NUDGE_M_X0 + SZ_NUDGE_M_X1) / 2, SZ_NUDGE_Y0 + 10, &act, &val);
    CHECK(act == SZ_ACT_NUDGE_REV, "the '-' cell did not start a backward bend");
    CHECK(side_nudging(SZ_LEFT) == -1, "side_nudging() does not see the backward bend");
    move(SZ_LEFT, 90, SZ_PLAY_Y0 + 10, &act, &val);     /* wandered down to PLAY */
    lift(SZ_LEFT, 90, SZ_PLAY_Y0 + 10, &act, &val);
    CHECK(act == SZ_ACT_NUDGE_STOP, "a wander off the nudge cell did not stop the bend");
    CHECK(act != SZ_ACT_PLAY, "a bend that wandered onto PLAY fired the transport");
    CHECK(side_nudging(SZ_LEFT) == 0, "side_nudging() still reports a bend after a wander");
    side_reset_all();

    /* THE OUTWARD SWEEP dismisses mid-press, without waiting for the lift. */
    reset_all();
    open_by_swipe(SZ_LEFT);
    lift(SZ_LEFT, SZ_SWIPE_PX, 600, &act, &val);
    press(SZ_LEFT, 10, 400, &act, &val);        /* background: x < SZ_BTN_X0 */
    CHECK(side_hit(SZ_LEFT, 10, 400) == SZ_HIT_BG, "the test's own background point is"
          " not background");
    CHECK(move(SZ_LEFT, 10, 400, &act, &val) == MZ_FEED_TAKEN,
          "a background motion was not swallowed");
    CHECK(act == SZ_ACT_NONE, "a background press claimed an action");
    lift(SZ_LEFT, 10, 400, &act, &val);
    CHECK(side_is_open(SZ_LEFT), "a tap on the drawer's own background closed it --"
          " the ONLY dismissal is the outward sweep");

    reset_all();
    open_by_swipe(SZ_LEFT);
    lift(SZ_LEFT, SZ_SWIPE_PX, 600, &act, &val);
    press(SZ_LEFT, 140, 400, &act, &val);        /* right of the lane: background */
    CHECK(side_hit(SZ_LEFT, 140, 400) == SZ_HIT_BG, "the test's own background point is"
          " not background");
    CHECK(move(SZ_LEFT, 140 - SZ_CLOSE_PX, 400, &act, &val) == MZ_FEED_TAKEN,
          "the outward sweep was not swallowed");
    CHECK(!side_is_open(SZ_LEFT), "an outward sweep of SZ_CLOSE_PX did not close it");
    CHECK(lift(SZ_LEFT, 140 - SZ_CLOSE_PX, 400, &act, &val) == MZ_FEED_TAKEN,
          "the dismissed press's lift was not swallowed");
    CHECK(act == SZ_ACT_NONE, "the dismissed press fired something on the way out");

    /* ...but the same outward sweep STARTED ON THE FADER is a fader drag, not a
     * dismissal: a finger that is riding the fader must not sweep the drawer away. */
    reset_all();
    open_by_swipe(SZ_LEFT);
    lift(SZ_LEFT, SZ_SWIPE_PX, 600, &act, &val);
    press(SZ_LEFT, 90, 400, &act, &val);
    CHECK(act == SZ_ACT_FADER, "the fader's down edge did not jump to the touch");
    move(SZ_LEFT, 90 - SZ_CLOSE_PX, 400, &act, &val);
    CHECK(side_is_open(SZ_LEFT), "an outward drag on the fader dismissed the drawer");
    side_reset_all();
}

static void test_gesture_fader(void)
{
    int act = 0, val = 0;

    reset_all();
    open_by_swipe(SZ_LEFT);
    lift(SZ_LEFT, SZ_SWIPE_PX, 600, &act, &val);

    /* JUMP TO WHERE YOU TOUCH: the landing point IS the value, sent on the down edge
     * -- a finger that comes to rest without moving must still have moved it. */
    act = val = -1;
    press(SZ_LEFT, 90, 400, &act, &val);
    CHECK(act == SZ_ACT_FADER, "a press on the lane did not claim the fader");
    CHECK(val == side_fader_v(400), "the landing value is %d, not side_fader_v(400) = %d",
          val, side_fader_v(400));

    /* And a finger that does not move sends NOTHING: the same y again is deduped. */
    act = val = -1;
    move(SZ_LEFT, 90, 400, &act, &val);
    CHECK(act == SZ_ACT_NONE, "a resting finger re-sent the same fader value");

    /* A real drag sends, and sends only when the quantised value changes. */
    act = val = -1;
    move(SZ_LEFT, 90, 500, &act, &val);
    CHECK(act == SZ_ACT_FADER, "a fader drag sent nothing");
    CHECK(val == side_fader_v(500), "the drag's value is %d, not %d", val,
          side_fader_v(500));
    CHECK(val < side_fader_v(400), "dragging DOWN raised the value");

    /* Clamped: past either end of the lane holds the end value. */
    act = val = -1;
    move(SZ_LEFT, 90, 5000, &act, &val);
    CHECK(act == SZ_ACT_FADER && val == 0, "below the lane does not clamp to silence");
    act = val = -1;
    move(SZ_LEFT, 90, -100, &act, &val);
    CHECK(act == SZ_ACT_FADER && val == 1023, "above the lane does not clamp to full");

    /* The lift ends the drag and the drawer STAYS OUT -- riding a fader is what the
     * drawer is mostly for, and a lift that dismissed it would mean a re-swipe for
     * every nudge. Then a NEW press starts its dedup over: one that lands on the value
     * the last press ended at must still send it. */
    lift(SZ_LEFT, 90, SZ_FADER_GY0, &act, &val);
    CHECK(side_is_open(SZ_LEFT), "a lift on the fader put the drawer away");
    act = val = -1;
    press(SZ_LEFT, 90, SZ_FADER_GY0, &act, &val);
    CHECK(act == SZ_ACT_FADER && val == 1023,
          "a new press at the same place as the last one's value sent nothing");
    lift(SZ_LEFT, 90, SZ_FADER_GY0, &act, &val);
    CHECK(side_is_open(SZ_LEFT), "the second fader press put the drawer away too");
    side_reset_all();
}

/* ---------------------------------------------------------------------------
 * 5. Both drawers at once, and the funnel's routing.
 * ------------------------------------------------------------------------- */
static void test_both_open(void)
{
    int act = 0, val = 0, which = -2;

    /* THE SECOND PANEL OPENS WHILE THE FIRST IS ALREADY OUT -- the operator's "allow
     * for both side panels to be visable at the same time and to accept input", and
     * the one thing the old one-plane design could not do. */
    reset_all();
    CHECK(open_by_funnel(SZ_LEFT), "the left drawer did not open through the funnel");
    CHECK(!side_any_open() == 0, "side_any_open() does not see the left drawer");

    CHECK(open_by_funnel(SZ_RIGHT), "the right drawer did not open while the left was"
          " already out -- the two panels are not independent");
    CHECK(side_is_open(SZ_LEFT) && side_is_open(SZ_RIGHT),
          "the second open put the first one away");

    /* Both answer, and each on its OWN channel -- which is the whole reason the
     * funnel has to say which drawer took the report. */
    which = -2;
    CHECK(side_feed_any(SZ_PTR_MAIN, 1, absx(SZ_LEFT, 90), SZ_CUE_Y0 + 10, &which, &act, &val)
          == MZ_FEED_TAKEN, "the left panel did not take a press on its own CUE");
    CHECK(which == SZ_LEFT, "the funnel named %d for a press on the LEFT panel", which);
    side_feed_any(SZ_PTR_MAIN, 0, absx(SZ_LEFT, 90), SZ_CUE_Y0 + 10, &which, &act, &val);
    CHECK(act == SZ_ACT_CUE, "CUE did not fire on the left drawer with both out");

    which = -2;
    CHECK(side_feed_any(SZ_PTR_MAIN, 1, absx(SZ_RIGHT, 90), SZ_PLAY_Y0 + 10, &which, &act, &val)
          == MZ_FEED_TAKEN, "the right panel did not take a press on its own PLAY");
    CHECK(which == SZ_RIGHT, "the funnel named %d for a press on the RIGHT panel", which);
    side_feed_any(SZ_PTR_MAIN, 0, absx(SZ_RIGHT, 90), SZ_PLAY_Y0 + 10, &which, &act, &val);
    CHECK(act == SZ_ACT_PLAY, "PLAY did not fire on the right drawer with both out");

    /* The left panel's own controls still answer first for a left press, and the
     * right's for a right press -- proven by the channel the funnel names, which is
     * the only thing that would be wrong if the order were reversed. */
    which = -2;
    side_feed_any(SZ_PTR_MAIN, 1, absx(SZ_LEFT, 90), SZ_SYNC_Y0 + 10, &which, &act, &val);
    CHECK(which == SZ_LEFT, "a press on the left panel was routed to the right drawer");
    side_feed_any(SZ_PTR_MAIN, 0, absx(SZ_LEFT, 90), SZ_SYNC_Y0 + 10, &which, &act, &val);

    /* A PRESS ON THE GLASS IS NOT OURS ANY MORE. The operator reversed the earlier
     * "tap away to close" -- "the side bar should stay up until i swipe them away" --
     * so a press between two open panels is handed back untouched on BOTH edges: it
     * belongs to no drawer and to no dismissal. That is what keeps rbp's own centre
     * screen alive while a drawer is out, and it is why nothing here latches. */
    which = -2; act = SZ_ACT_CUE; val = -1;
    CHECK(side_feed_any(SZ_PTR_MAIN, 1, 600, 400, &which, &act, &val) == MZ_FEED_NONE,
          "a press on the glass between the two drawers was swallowed");
    CHECK(which == -1, "a press on the glass named a drawer");
    CHECK(act == SZ_ACT_NONE && val == 0, "a press on the glass fired an action");
    CHECK(side_feed_any(SZ_PTR_MAIN, 0, 600, 400, &which, &act, &val) == MZ_FEED_NONE,
          "the lift of a press on the glass was swallowed");
    CHECK(side_is_open(SZ_LEFT) && side_is_open(SZ_RIGHT),
          "a tap on the glass put a drawer away -- only the outward sweep may");
    side_reset_all();

    /* ...and a press that began on the glass cannot become a drawer's by wandering
     * onto it. It was offered to the shut right drawer's entry column at its down
     * edge and declined (the latch rule), so the motion arriving at the right edge is
     * a motion of a press that drawer never took. */
    reset_all();
    open_by_funnel(SZ_LEFT);
    which = -2;
    side_feed_any(SZ_PTR_MAIN, 1, 600, 400, &which, &act, &val);
    CHECK(side_feed_any(SZ_PTR_MAIN, 1, absx(SZ_RIGHT, 5), 400, &which, &act, &val) == MZ_FEED_NONE,
          "a motion from the glass onto the right edge was claimed");
    CHECK(!side_is_open(SZ_RIGHT),
          "a press that began on the glass armed the right drawer on the way past");
    side_feed_any(SZ_PTR_MAIN, 0, absx(SZ_RIGHT, 5), 400, &which, &act, &val);
    CHECK(side_is_open(SZ_LEFT), "the lift of a glass press put the left drawer away");
    CHECK(!side_is_open(SZ_RIGHT), "a glass press opened the right drawer at its lift");
    side_reset_all();
}

static void test_one_surface(void)
{
    int act = 0, val = 0, b = 0;
    int ex = (MZ_ENTRY_X0 + MZ_ENTRY_X1) / 2;

    /* A drawer must not arm while the BAND is open: the band overlaps the drawers at
     * the corners of the glass, so it owns the plane while it is up and must also
     * answer first for the press that dismisses it. */
    reset_all();
    menu_feed(1, ex, 10, &b);
    CHECK(menu_feed(1, ex, 10 + MZ_SWIPE_PX, &b) == MZ_FEED_TAKEN,
          "the band's own opening sweep failed -- the comparison below proves nothing");
    CHECK(menu_is_open(), "the band is not open -- the comparison below proves nothing");
    CHECK(side_feed(SZ_LEFT, SZ_PTR_MAIN, 1, absx(SZ_LEFT, 10), 400, &act, &val) == MZ_FEED_NONE,
          "a drawer armed while the top band was open");
    CHECK(side_feed(SZ_RIGHT, SZ_PTR_MAIN, 1, absx(SZ_RIGHT, 10), 400, &act, &val) == MZ_FEED_NONE,
          "the right drawer armed while the top band was open");
    CHECK(!side_any_open(), "a drawer opened while the top band was open");
    reset_all();

    /* The band must not arm while a drawer is out -- menu_zone.c's half of the same
     * rule, driven through the real menu_feed(). */
    reset_all();
    open_by_swipe(SZ_LEFT);
    CHECK(side_is_open(SZ_LEFT), "the left drawer did not open -- the comparison below"
          " proves nothing");
    CHECK(menu_feed(1, ex, 10, &b) == MZ_FEED_NONE,
          "the band armed its opening swipe while a drawer was out");
    CHECK(!menu_is_open(), "the band opened while a drawer was out");
    side_reset_all();
    menu_reset();

    /* Both shut: the funnel arms the drawer whose column was pressed, and says so. A
     * press in the middle of the glass is nobody's, and with nothing open it is not
     * even ours to swallow. */
    reset_all();
    {
        int which = -2;

        CHECK(side_feed_any(SZ_PTR_MAIN, 1, absx(SZ_LEFT, 10), 400, &which, &act, &val)
              == MZ_FEED_TAKEN, "the left entry column was not armed");
        CHECK(which == SZ_LEFT, "the funnel named the wrong drawer for a left press");
        side_feed_any(SZ_PTR_MAIN, 0, absx(SZ_LEFT, 10), 400, &which, &act, &val);
        reset_all();

        CHECK(side_feed_any(SZ_PTR_MAIN, 1, absx(SZ_RIGHT, 10), 400, &which, &act, &val)
              == MZ_FEED_TAKEN, "the right entry column was not armed");
        CHECK(which == SZ_RIGHT, "the funnel named the wrong drawer for a right press");
        side_feed_any(SZ_PTR_MAIN, 0, absx(SZ_RIGHT, 10), 400, &which, &act, &val);

        CHECK(side_feed_any(SZ_PTR_MAIN, 1, 600, 400, &which, &act, &val) == MZ_FEED_NONE,
              "the funnel claimed a press in the middle of the glass with nothing open");
        CHECK(which == -1, "the funnel named a drawer for a press it did not take");
    }
    reset_all();

    /* The device going away takes the drawers with it: a drawer left out by a finger
     * that vanished with the panel would be a drawer nothing can dismiss. */
    open_by_swipe(SZ_LEFT);
    CHECK(side_is_open(SZ_LEFT), "the drawer did not open before the reset");
    side_reset_all();
    CHECK(!side_any_open(), "side_reset_all() left a drawer open");
    CHECK(side_pressed(SZ_LEFT) == SZ_HIT_NONE, "side_reset_all() left a highlight");
}

/* ---------------------------------------------------------------------------
 * 5b. The nudge's own state, which is the one thing here rbp holds rather than us.
 * ------------------------------------------------------------------------- */
static void test_nudging(void)
{
    int act = 0, val = 0;

    /* Nothing is bending on a quiet system, on either side. */
    reset_all();
    CHECK(side_nudging(SZ_LEFT) == 0 && side_nudging(SZ_RIGHT) == 0,
          "side_nudging() reports a bend with nothing down");

    /* Only a SHUT drawer's entry column has no bend: the state is a function of a
     * press ON a nudge cell, and a press in the entry column is not one. */
    press(SZ_LEFT, 10, 400, &act, &val);
    CHECK(side_nudging(SZ_LEFT) == 0, "an entry-column press reported a bend");
    lift(SZ_LEFT, 10, 400, &act, &val);
    side_reset_all();

    /* Held: the bend is visible for the whole press, not just at its edges -- which is
     * exactly what the caller's reconciler reads in the device-loss path. */
    reset_all();
    open_by_swipe(SZ_LEFT);
    lift(SZ_LEFT, SZ_SWIPE_PX, 600, &act, &val);
    press(SZ_LEFT, (SZ_NUDGE_P_X0 + SZ_NUDGE_P_X1) / 2, SZ_NUDGE_Y0 + 10, &act, &val);
    CHECK(side_nudging(SZ_LEFT) == 1, "a held '+' does not read as a forward bend");
    CHECK(side_nudging(SZ_RIGHT) == 0, "the left drawer's bend showed on the right");
    move(SZ_LEFT, (SZ_NUDGE_P_X0 + SZ_NUDGE_P_X1) / 2, SZ_NUDGE_Y0 + 40, &act, &val);
    CHECK(side_nudging(SZ_LEFT) == 1, "the bend was lost on a later motion report");

    /* THE SAFETY NET: side_reset_all() is what the touch device going away calls, and
     * the caller's side_bend_sync() reads this to decide what to send. A reset must
     * therefore report "no bend" or a vanished finger would leave the track sliding. */
    side_reset_all();
    CHECK(side_nudging(SZ_LEFT) == 0,
          "side_reset_all() left a bend on -- a lost touch device would slide the track");
    CHECK(!side_any_open(), "side_reset_all() left the drawer out");
}

/* ---------------------------------------------------------------------------
 * 6. The image.
 * ------------------------------------------------------------------------- */

/* One drawer's plane, exactly as menu_draw.c builds one: its own buffer, origin 0,0,
 * pitch = width. */
#define SB_MAX_W 400
#define SB_MAX_H 1280
static unsigned short fb[SB_MAX_W * SB_MAX_H];

static struct menu_view mkview(int pw, int ph)
{
    struct menu_view v;

    memset(&v, 0, sizeof v);
    v.pix   = fb;
    v.pitch = pw;
    v.fb_w  = pw;
    v.fb_h  = ph;
    v.bpp   = 16;
    v.dw    = pw;
    v.dh    = ph;
    return v;
}

static void fill(unsigned int sentinel, int pw, int ph)
{
    int i;

    for (i = 0; i < pw * ph; i++)
        fb[i] = (unsigned short)sentinel;
}

/* The device column a panel-local lx lands in, and the row a logical y lands in --
 * the same arithmetic side_paint.c does, written here so the test can name a pixel
 * without asking the module where it drew. The mirror is in spx() and not devx(),
 * because a check that names a local point on the right drawer has to go through it
 * or it samples the mirrored cell. */
static int devx(int pw, int lx)
{
    return (lx * pw) / SZ_W;
}

static int spx(int pw, int side, int lx)
{
    if (side == SZ_RIGHT)
        lx = SZ_W - 1 - lx;
    return (lx * pw) / SZ_W;
}

/* A local cell to its half-open device rect, exactly as side_paint.c's sp_rect does
 * -- the two x ends sorted, because the mirror makes the first the larger on the
 * right drawer. */
static void cell_rect(int pw, int side, int lx0, int lx1, int *fx0, int *fx1)
{
    int a = spx(pw, side, lx0), b = spx(pw, side, lx1);

    *fx0 = a < b ? a : b;
    *fx1 = (a < b ? b : a) + 1;
}

static int devy(int ph, int y)
{
    return (y * ph) / SZ_H;
}

static void test_paint_refusals(void)
{
    struct menu_view v;

    /* A drawer the size a 1280x800 page gives it, and the two larger pages. */
    v = mkview(180, 800);
    CHECK(side_paint_ok(&v), "a 1280x800 page's drawer was refused");
    v = mkview(270, 1080);
    CHECK(side_paint_ok(&v), "a 1920x1080 page's drawer was refused");
    v = mkview(144, 768);
    CHECK(side_paint_ok(&v), "a 1024x768 page's drawer was refused");

    /* THE THREE REFUSALS. A box too short to host the font's line box, a panel too
     * narrow for its widest label, and a panel too small for the drawn nudge cross --
     * all are the band's own rule (menu_view_ok()) at the drawer's scale, and all
     * three would be a smear (or a silently clipped mark) rather than a control. */
    v = mkview(180, 100);
    CHECK(!side_paint_ok(&v), "a drawer 100 device rows tall was accepted: its button"
          " boxes cannot host a %d-px line box", MENU_FONT_LINE);
    v = mkview(20, 800);
    CHECK(!side_paint_ok(&v), "a drawer 20 device px wide was accepted: it cannot host"
          " its own labels");
    v = mkview(60, 800);
    CHECK(!side_paint_ok(&v), "a drawer 60 device px wide was accepted: a nudge cell"
          " in it is %d px and the cross is %d",
          (SZ_NUDGE_M_X1 - SZ_NUDGE_M_X0 + 1) * 60 / SZ_W, 2 * SP_SIGN_ARM + 1);

    /* And nothing draws into one that was refused. */
    fill(0xa5a5u, 180, 100);
    v = mkview(180, 100);
    side_paint(&v, SZ_LEFT, 0, 512);
    {
        int i, touched = 0;

        for (i = 0; i < 180 * 100; i++)
            if (fb[i] != 0xa5a5u)
                touched++;
        CHECK(touched == 0, "a refused drawer wrote %d pixels anyway", touched);
    }

    /* Non-16/32 bpp is refused before anything is written. */
    v = mkview(180, 800);
    v.bpp = 24;
    CHECK(!side_paint_ok(&v), "a 24 bpp view was accepted");
}

static void test_paint_covers(void)
{
    struct menu_view v;
    int x, y, unwritten = 0;

    fill(0xa5a5u, 180, 800);
    v = mkview(180, 800);
    side_paint(&v, SZ_LEFT, 0, 512);

    for (y = 0; y < 800; y++)
        for (x = 0; x < 180; x++)
            if (fb[y * 180 + x] == 0xa5a5u)
                unwritten++;
    CHECK(unwritten == 0, "%d drawer pixels were never written", unwritten);
}

static void test_paint_idempotent(void)
{
    static unsigned short first[180 * 800];
    struct menu_view v = mkview(180, 800);

    /* A second identical paint changes nothing -- the whole reason menu_draw.c's gate
     * can be a comparison of three ints instead of a counter: if a repeat paint were
     * not a no-op, the gate would be holding back work that was needed. */
    fill(0xa5a5u, 180, 800);
    side_paint(&v, SZ_LEFT, 0, 512);
    memcpy(first, fb, sizeof first);
    side_paint(&v, SZ_LEFT, 0, 512);
    CHECK(memcmp(first, fb, sizeof first) == 0,
          "a second identical paint changed the image");

    /* And a DIFFERENT state changes it, so the gate cannot be satisfied by a painter
     * that draws the same thing whatever it is told. */
    fill(0xa5a5u, 180, 800);
    side_paint(&v, SZ_LEFT, 0, 512);
    side_paint(&v, SZ_LEFT, 0, 0);
    CHECK(memcmp(first, fb, sizeof first) != 0,
          "moving the fader to zero did not change the image");

    fill(0xa5a5u, 180, 800);
    side_paint(&v, SZ_LEFT, 0, 512);
    side_paint(&v, SZ_LEFT, SZ_HIT_SYNC, 512);
    CHECK(memcmp(first, fb, sizeof first) != 0,
          "pressing SYNC did not change the image");

    fill(0xa5a5u, 180, 800);
    side_paint(&v, SZ_LEFT, 0, 512);
    side_paint(&v, SZ_LEFT, SZ_HIT_CUE, 512);
    CHECK(memcmp(first, fb, sizeof first) != 0,
          "pressing CUE did not change the image");

    fill(0xa5a5u, 180, 800);
    side_paint(&v, SZ_LEFT, 0, 512);
    side_paint(&v, SZ_LEFT, SZ_HIT_NUDGE_M, 512);
    CHECK(memcmp(first, fb, sizeof first) != 0,
          "pressing the '-' nudge cell did not change the image");

    fill(0xa5a5u, 180, 800);
    side_paint(&v, SZ_LEFT, 0, 512);
    side_paint(&v, SZ_RIGHT, 0, 512);
    CHECK(memcmp(first, fb, sizeof first) != 0,
          "the two drawers paint the same image -- the mirror is not in the pixels");
}

/* The pressed image must differ ONLY inside the pressed box -- otherwise a press
 * repaints ink the gate has no reason to expect, and the drawer would smear whatever
 * it overlapped. */
static void test_paint_pressed_is_local(void)
{
    static unsigned short plain[180 * 800];
    struct menu_view v = mkview(180, 800);
    int x, y, outside = 0;
    int cx0 = devx(180, SZ_BTN_X0), cx1 = devx(180, SZ_BTN_X1);
    int cy0 = devy(800, SZ_CUE_Y0), cy1 = devy(800, SZ_CUE_Y1);

    fill(0xa5a5u, 180, 800);
    side_paint(&v, SZ_LEFT, 0, 512);
    memcpy(plain, fb, sizeof plain);
    side_paint(&v, SZ_LEFT, SZ_HIT_CUE, 512);

    for (y = 0; y < 800; y++)
        for (x = 0; x < 180; x++) {
            int in = (x >= cx0 && x <= cx1 && y >= cy0 && y <= cy1);

            if (!in && fb[y * 180 + x] != plain[y * 180 + x])
                outside++;
        }
    CHECK(outside == 0, "pressing CUE changed %d pixels outside its own box", outside);

    /* ...and something inside it DID change, or the check above is vacuous. */
    CHECK(memcmp(plain, fb, sizeof plain) != 0, "pressing CUE changed nothing at all");
}

/* The mirror, and the absent frame. The mirror is the one thing this panel's drawing
 * can get wrong without any hit test noticing -- side_paint() works in PANEL-LOCAL x
 * and the plane is in SCREEN order -- and until 2026-10-06 the frame's thin bands were
 * what proved it, because they were the only asymmetric mark in the picture. The frame
 * is gone (*"you don't need a white border on the side swipe menus"*), so the proof
 * moves to the next mark out: SYNC's own outline, which is 16 px in from the outer edge
 * on both sides and therefore lands on device column 16 on the left and 163 on the
 * right. The frame's absence is asserted in the same loop, because the change that
 * removed it is exactly the change that could have left one band behind. */
static void test_paint_mirror(void)
{
    struct menu_view v = mkview(180, 800);
    unsigned int bed  = menu_pixel(16, MENU_FILL);
    unsigned int ink  = menu_pixel(16, MENU_LABEL);
    int side;

    CHECK(bed != ink, "the bed and the outline are the same pixel value");

    for (side = 0; side < 2; side++) {
        /* SYNC's outline, as device columns, on this side's own edges. */
        int outer = side == SZ_LEFT ? SZ_BTN_X0 : (SZ_W - 1 - SZ_BTN_X0);
        int inner = side == SZ_LEFT ? SZ_BTN_X1 : (SZ_W - 1 - SZ_BTN_X1);
        int y     = SZ_SYNC_Y0 + SZ_BTN_H / 2;

        fill(0xa5a5u, 180, 800);
        side_paint(&v, side, 0, 512);

        CHECK(fb[y * 180 + outer] == ink,
              "side %d: SYNC's outer edge is not outlined", side);
        CHECK(fb[y * 180 + inner] == ink,
              "side %d: SYNC's inner edge is not outlined", side);
        /* ...and the panel's own edges are the bed, on BOTH sides of the panel on BOTH
         * drawers. A frame that survived on one edge is the failure this catches. */
        CHECK(fb[y * 180 + 0] == bed,
              "side %d: the panel's first column is still framed", side);
        CHECK(fb[y * 180 + (SZ_W - 1)] == bed,
              "side %d: the panel's last column is still framed", side);
        CHECK(fb[0 * 180 + 90] == bed,
              "side %d: the panel's top edge is still framed", side);
        CHECK(fb[(SZ_H - 1) * 180 + 90] == bed,
              "side %d: the panel's bottom edge is still framed", side);
    }
}

/* The fader's cap follows the value: at the top of the well for 1023, at the bottom
 * for 0. This is the drawer's whole answer to "where is my channel fader", and it is
 * the one mark whose position is a function of a number rather than of a rectangle. */
static void test_paint_fader_cap(void)
{
    struct menu_view v = mkview(180, 800);
    unsigned int face = menu_pixel(16, MENU_BTN);
    unsigned int well = menu_pixel(16, MENU_DIV);
    /* The cap's own centre at each end: sampling its top edge would sample the one-px
     * frame the cap is outlined with, which is ink and not the face. The centre column
     * is well clear of that frame's left and right edges. */
    int cx  = devx(180, (SZ_FADER_X0 + SZ_FADER_X1) / 2);
    int top = devy(800, SZ_FADER_Y0 + SP_HANDLE_H / 2);
    int bot = devy(800, SZ_FADER_Y1 - SP_HANDLE_H / 2);

    /* If the cap and the well were the same colour this whole test would be vacuous. */
    CHECK(face != well, "the cap and the well are the same pixel value");
    /* The cap has to fit inside the well, or the clamp in sp_fader() is hiding a track
     * shorter than the handle it draws. */
    CHECK(SP_HANDLE_H < SZ_FADER_TRAVEL,
          "the fader's cap is %d rows and its track is %d: the cap cannot travel",
          SP_HANDLE_H, SZ_FADER_TRAVEL);

    fill(0xa5a5u, 180, 800);
    side_paint(&v, SZ_LEFT, 0, 1023);
    CHECK(fb[top * 180 + cx] == face,
          "at full the top of the well is not the cap: the handle is not at the top");
    CHECK(fb[bot * 180 + cx] == well,
          "at full the bottom of the well is the cap: the handle has not moved");

    side_paint(&v, SZ_LEFT, 0, 0);
    CHECK(fb[bot * 180 + cx] == face,
          "at zero the bottom of the well is not the cap");
    CHECK(fb[top * 180 + cx] == well, "at zero the top of the well is still the cap");

    /* The readout above the well is drawn at every value, and it is ink and not the
     * bed -- a blank readout is the failure this is standing in front of. */
    {
        int x, y, ink = 0;
        unsigned int label = menu_pixel(16, MENU_LABEL);
        int rx0 = devx(180, SZ_READ_X0), rx1 = devx(180, SZ_READ_X1);
        int ry0 = devy(800, SZ_READ_Y0), ry1 = devy(800, SZ_READ_Y1);

        for (y = ry0; y <= ry1; y++)
            for (x = rx0; x <= rx1; x++)
                if (fb[y * 180 + x] == label)
                    ink++;
        CHECK(ink > 0, "the value readout is blank");
    }
}

/* THE TWO NUDGE MARKS HAVE TO BE DISTINGUISHABLE, and this is the only test that can
 * say so: the atlas has no '+' (menu_font.h), so both marks are drawn as bars by
 * side_paint.c and a mistake there would leave two identical '-' cells -- a control
 * pair whose halves the operator could not tell apart. It samples the two pixels that
 * differ: the centre column above the bar, which is ink on the cross and the button
 * face on the plain bar. */
static void test_paint_signs(void)
{
    struct menu_view v = mkview(180, 800);
    unsigned int face = menu_pixel(16, MENU_BTN);
    unsigned int ink  = menu_pixel(16, MENU_LABEL);
    int cell[2][2] = { { SZ_NUDGE_M_X0, SZ_NUDGE_M_X1 },
                       { SZ_NUDGE_P_X0, SZ_NUDGE_P_X1 } };
    int i, side, cy = devy(800, (SZ_NUDGE_Y0 + SZ_NUDGE_Y1) / 2);
    int up = cy - SP_SIGN_ARM / 2;      /* above the bar, inside the cross */

    CHECK(face != ink, "the button face and the label ink are the same pixel value");

    for (side = 0; side < 2; side++) {
        fill(0xa5a5u, 180, 800);
        side_paint(&v, side, 0, 512);

        for (i = 0; i < 2; i++) {
            int fx0, fx1, cx, want = i ? ink : face;

            cell_rect(180, side, cell[i][0], cell[i][1], &fx0, &fx1);
            cx = (fx0 + fx1) / 2;

            /* The horizontal bar itself is ink on BOTH cells, well inside the frame. */
            CHECK(fb[cy * 180 + (cx - SP_SIGN_ARM + 1)] == ink,
                  "side %d cell %d: the horizontal bar is missing on the left",
                  side, i);
            CHECK(fb[cy * 180 + (cx + SP_SIGN_ARM - 1)] == ink,
                  "side %d cell %d: the horizontal bar is missing on the right",
                  side, i);
            /* ...and the vertical stroke is the whole difference between them. */
            CHECK(fb[up * 180 + cx] == want,
                  "side %d cell %d: the centre column above the bar is %#06x, expected"
                  " %#06x -- the two nudge marks are not distinguishable",
                  side, i, fb[up * 180 + cx], want);

            /* THE MARK FITS ITS CELL. `arm` is in device pixels and the cell is
             * SCALED, so a smaller page is where a bar drawn past its frame would be
             * silently cut -- exactly the failure side_paint_ok() refuses a narrow
             * panel for, checked here on the panel it accepts. Inclusive at both ends:
             * side_paint_ok()'s rule is `scaled cell width >= 2*arm+1`, so on the
             * tightest page it accepts the mark spans the cell's columns exactly, to
             * the frame, which is the honest boundary and not a clipping. */
            CHECK(cx - SP_SIGN_ARM >= fx0 && cx + SP_SIGN_ARM <= fx1 - 1,
                  "side %d cell %d: the %d-px mark does not fit its %d-px cell",
                  side, i, 2 * SP_SIGN_ARM + 1, fx1 - fx0);
        }
    }

    /* ...and on the page side_paint_ok() draws at its tightest: the mark still fits
     * the scaled cells, on both drawers. 65 px is the first width that check passes
     * (a nudge cell in it is 26 px, the mark 25), so this is the exact boundary the
     * refusal is drawn at rather than an arbitrary small number. */
    {
        struct menu_view small = mkview(65, 800);
        int fx0, fx1, cx;

        CHECK(side_paint_ok(&small), "a 65 px page is refused -- this test's own"
              " boundary assumption is wrong, so the checks below prove nothing");

        fill(0xa5a5u, 65, 800);
        side_paint(&small, SZ_LEFT, 0, 512);
        cell_rect(65, SZ_LEFT, SZ_NUDGE_M_X0, SZ_NUDGE_M_X1, &fx0, &fx1);
        cx = (fx0 + fx1) / 2;
        CHECK(cx - SP_SIGN_ARM >= fx0 && cx + SP_SIGN_ARM <= fx1 - 1,
              "at 65 px the left drawer's '-' mark does not fit its %d-px cell",
              fx1 - fx0);

        fill(0xa5a5u, 65, 800);
        side_paint(&small, SZ_RIGHT, 0, 512);
        cell_rect(65, SZ_RIGHT, SZ_NUDGE_P_X0, SZ_NUDGE_P_X1, &fx0, &fx1);
        cx = (fx0 + fx1) / 2;
        CHECK(cx - SP_SIGN_ARM >= fx0 && cx + SP_SIGN_ARM <= fx1 - 1,
              "at 65 px the right drawer's '+' mark does not fit its %d-px cell",
              fx1 - fx0);
    }
}

/* ---------------------------------------------------------------------------
 * 5c. THE ENTRY COLUMN MUST SURVIVE A PRESS IT DECLINED. This is the one bug the
 * relayout's routing produced and the only test here that stands in front of a
 * SILENT death: a press the drawer was offered and refused still records the down
 * edge (the latch rule), so unless its release is delivered to that drawer too, the
 * drawer believes a finger is still down and reads the NEXT press on its entry
 * column as a motion of a press that ended long ago. Nothing draws wrong, nothing
 * logs, and the drawer is simply dead until the process restarts.
 * ------------------------------------------------------------------------- */
static void test_entry_survives_declined(void)
{
    int act = 0, val = 0, which = -2;

    /* A tap in the middle of the glass with NOTHING open: offered to both shut
     * drawers' entry columns, declined by both, and it must go to rbp untouched. */
    reset_all();
    CHECK(side_feed_any(SZ_PTR_MAIN, 1, 600, 400, &which, &act, &val) == MZ_FEED_NONE,
          "a tap on the bare glass was swallowed with no drawer open");
    CHECK(which == -1, "a declined tap named a drawer");
    CHECK(side_feed_any(SZ_PTR_MAIN, 0, 600, 400, &which, &act, &val) == MZ_FEED_NONE,
          "a declined tap's release was swallowed with no drawer open");

    /* ...and the entry column still works afterwards, on BOTH sides. */
    CHECK(open_by_funnel(SZ_LEFT),
          "the left entry column was dead after a press it had declined");
    side_reset_all();
    CHECK(open_by_funnel(SZ_RIGHT),
          "the right entry column was dead after a press it had declined");
    side_reset_all();

    /* The same class of death, with a drawer OPEN: a press on the glass is refused by
     * both open panels (step 1 needs a hit on a panel) and by the shut drawer's entry
     * column, comes back NONE on both edges, and the drawer stays up. The right entry
     * must live through it. */
    CHECK(open_by_funnel(SZ_LEFT), "the left drawer did not open for the glass-press test");
    CHECK(side_feed_any(SZ_PTR_MAIN, 1, 600, 400, &which, &act, &val) == MZ_FEED_NONE,
          "a press on the glass with a drawer open was swallowed");
    CHECK(side_feed_any(SZ_PTR_MAIN, 0, 600, 400, &which, &act, &val) == MZ_FEED_NONE,
          "the release of a press on the glass with a drawer open was swallowed");
    CHECK(side_is_open(SZ_LEFT), "a press on the glass closed the drawer");
    CHECK(open_by_funnel(SZ_RIGHT),
          "the right entry column was dead after a press on the glass");
    side_reset_all();

    /* ...and a whole press in the entry column that NEVER SWIPED (so it came back as
     * a tap) leaves the column usable too -- the same class of leak, reached the
     * ordinary way rather than through a dismissal. */
    reset_all();
    press(SZ_LEFT, 10, 400, &act, &val);
    CHECK(lift(SZ_LEFT, 10, 400, &act, &val) == MZ_FEED_TAP,
          "the entry tap was not handed back to rbp");
    CHECK(open_by_funnel(SZ_LEFT),
          "the left entry column was dead after a tap it had taken and returned");
    side_reset_all();
}

/* The same three edges for the SECOND contact -- the panel's other finger. */
static int press2(int side, int lx, int y, int *act, int *val)
{
    return side_feed(side, SZ_PTR_ALT, 1, absx(side, lx), y, act, val);
}

static int lift2(int side, int lx, int y, int *act, int *val)
{
    return side_feed(side, SZ_PTR_ALT, 0, absx(side, lx), y, act, val);
}

/* ---------------------------------------------------------------------------
 * 5d. THE TWO HANDS. *"if i try to drag both volume meters it gets confused and
 * only one of them changes"* (2026-10-06).
 *
 * TWO CHANNEL FADERS, TWO FINGERS, ONE AT A TIME ON EACH. The two contacts used to
 * be ONE press: the reader had one pair of coordinates, so the second finger's
 * position arrived as the first pointer moving, and the funnel's step 0 -- "a
 * latched press owns all of its own reports" -- handed every one of them to the
 * drawer that latched first, which is the LEFT edge, always, because that loop runs
 * i = 0 first. Measured on the unit before the repair: slot 0 down on the left lane,
 * slot 1 dragged alone on the right, and the only line the log produced was
 * `side left fader ch1 -> 198`.
 *
 * Every assertion below is a sentence that defect made false.
 * ------------------------------------------------------------------------- */
static void test_two_hands(void)
{
    /* A local x inside BOTH drawers' grab lanes, and the two lane rows the hands
     * work at. The values are read back through the module's own maths so the test
     * cannot be wrong in the same direction as the code. */
    const int lx   = (SZ_FADER_GX0 + SZ_FADER_GX1) / 2;
    const int yL   = side_fader_y(900);
    const int yR   = side_fader_y(200);
    const int vL   = side_fader_v(yL);
    const int vR   = side_fader_v(yR);
    int act, val, which;

    CHECK(vL != vR, "the two lane rows this test uses are the same value");

    /* BOTH DRAWERS OUT, and the SECOND contact opens its own -- if it could not, two
     * faders could only ever be reached by opening them one-handed first, and the
     * operator's gesture is two hands or nothing. */
    reset_all();
    CHECK(open_by_funnel(SZ_LEFT), "the left drawer did not open");
    CHECK(side_feed_any(SZ_PTR_ALT, 1, absx(SZ_RIGHT, 0), 600, &which, &act, &val)
          != MZ_FEED_NONE,
          "the second contact was refused the shut right drawer's entry column");
    CHECK(side_feed_any(SZ_PTR_ALT, 1, absx(SZ_RIGHT, SZ_SWIPE_PX), 600, &which, &act, &val)
          != MZ_FEED_NONE,
          "the second contact's inward sweep was refused");
    side_feed_any(SZ_PTR_ALT, 0, absx(SZ_RIGHT, SZ_SWIPE_PX), 600, &which, &act, &val);
    CHECK(side_is_open(SZ_LEFT) && side_is_open(SZ_RIGHT),
          "the second contact's opening swipe did not leave both drawers out");

    /* THE PRIMARY HAND takes its own channel -- and the second hand's press must not
     * be answered by the drawer the first hand is holding. */
    CHECK(side_feed_any(SZ_PTR_MAIN, 1, absx(SZ_LEFT, lx), yL, &which, &act, &val)
          != MZ_FEED_NONE,
          "the primary hand's fader press was not taken");
    CHECK(which == SZ_LEFT && act == SZ_ACT_FADER && val == vL,
          "the primary hand's press was answered by %s (act=%d val=%d), expected the"
          " LEFT drawer and FADER/%d",
          which == SZ_LEFT ? "the left drawer" :
          which == SZ_RIGHT ? "the RIGHT drawer" : "nothing", act, val, vL);

    CHECK(side_feed_any(SZ_PTR_ALT, 1, absx(SZ_RIGHT, lx), yR, &which, &act, &val)
          != MZ_FEED_NONE,
          "the second hand's fader press was not taken by any drawer");
    CHECK(which == SZ_RIGHT && act == SZ_ACT_FADER && val == vR,
          "the second hand's press was answered by %s (act=%d val=%d), expected the"
          " RIGHT drawer and FADER/%d -- the other hand's press is owning it",
          which == SZ_LEFT ? "the LEFT drawer" :
          which == SZ_RIGHT ? "the right drawer" : "nothing", act, val, vR);

    /* THE DEFECT ITSELF. The second hand drags; the FIRST hand's channel must not
     * follow it. This is the exact line the unit's log produced wrongly. */
    CHECK(side_feed_any(SZ_PTR_ALT, 1, absx(SZ_RIGHT, lx), yL, &which, &act, &val)
          != MZ_FEED_NONE,
          "the second hand's drag was dropped");
    CHECK(which == SZ_RIGHT,
          "the second hand's drag was routed to the LEFT drawer -- one hand's press is"
          " owning the other hand's reports, which is the whole defect");
    CHECK(act == SZ_ACT_FADER && val == vL,
          "the second hand's drag sent act=%d val=%d, expected FADER/%d", act, val, vL);

    /* ...and the first hand's drag is still the left drawer's. */
    CHECK(side_feed_any(SZ_PTR_MAIN, 1, absx(SZ_LEFT, lx), yR, &which, &act, &val)
          != MZ_FEED_NONE,
          "the primary hand's drag was dropped");
    CHECK(which == SZ_LEFT && act == SZ_ACT_FADER && val == vR,
          "the primary hand's drag was answered by %s (act=%d val=%d), expected the"
          " LEFT drawer and FADER/%d",
          which == SZ_LEFT ? "the left drawer" :
          which == SZ_RIGHT ? "the RIGHT drawer" : "nothing", act, val, vR);

    /* Both hands lift, and both drawers are still out -- a lift is one press's end
     * and never the panel's. */
    lift2(SZ_RIGHT, lx, yL, &act, &val);
    lift(SZ_LEFT, lx, yR, &act, &val);
    CHECK(side_is_open(SZ_LEFT) && side_is_open(SZ_RIGHT),
          "lifting a fader put a drawer away");

    /* AND THE SECOND HAND BECOMES THE FIRST'S: with the second contact released, the
     * primary can take the channel it was holding, and the drawer it was never near
     * answers nothing. */
    CHECK(side_feed_any(SZ_PTR_MAIN, 1, absx(SZ_RIGHT, lx), yR, &which, &act, &val)
          != MZ_FEED_NONE,
          "the primary hand could not take the right channel after the second lifted");
    CHECK(which == SZ_RIGHT && act == SZ_ACT_FADER,
          "the primary hand's press on the right lane was answered by the wrong drawer");
    side_reset_all();

    /* THE SCOPE, in the negative. The second contact is the DRAWERS' and nothing
     * else: on the centre of the glass with both drawers shut it is not taken by
     * anything, on either edge. rbp has one pointer and a second press would be a
     * button it cannot interpret. */
    reset_all();
    CHECK(side_feed_any(SZ_PTR_ALT, 1, 600, 400, &which, &act, &val) == MZ_FEED_NONE,
          "the second contact was swallowed on the bare glass");
    CHECK(which == -1, "a second contact nobody took named a drawer");
    CHECK(side_feed_any(SZ_PTR_ALT, 0, 600, 400, &which, &act, &val) == MZ_FEED_NONE,
          "the second contact's release on the bare glass was swallowed");

    /* ...and a second contact on a SHUT drawer's entry column is a TAP it can take
     * and return -- the module answers TAP, and the caller drops it rather than
     * replaying, because there is no second pointer to replay into. What must NOT
     * happen is the drawer being left latched by it. */
    reset_all();
    CHECK(press2(SZ_LEFT, 10, 400, &act, &val) == MZ_FEED_TAKEN,
          "the second contact's entry-column press was not taken");
    CHECK(lift2(SZ_LEFT, 10, 400, &act, &val) == MZ_FEED_TAP,
          "the second contact's entry tap was not handed back");
    CHECK(!side_is_open(SZ_LEFT), "the second contact's entry tap opened the drawer");
    CHECK(open_by_funnel(SZ_LEFT),
          "the left entry column was dead after the second contact tapped it");
    side_reset_all();

    /* A HIGHLIGHT IS A FACT ABOUT THE PANEL, so the second hand's press must show on
     * it -- otherwise a bend would run under a panel that looked untouched. */
    reset_all();
    CHECK(open_by_funnel(SZ_LEFT), "the left drawer did not open for the highlight test");
    press2(SZ_LEFT, lx, yL, &act, &val);
    CHECK(side_pressed(SZ_LEFT) == SZ_HIT_FADER,
          "the second hand's press left the panel unhighlighted (side_pressed=%d)",
          side_pressed(SZ_LEFT));
    lift2(SZ_LEFT, lx, yL, &act, &val);
    CHECK(side_pressed(SZ_LEFT) == SZ_HIT_NONE,
          "the second hand's highlight outlived its press");

    /* ...and the same for a BEND, which is the one control where losing sight of a
     * finger slides the track: the caller reconciles side_nudging() on every report
     * and turns any change into a speed-0 send, so a bend the primary hand cannot see
     * is a bend that gets cancelled by the next report either hand makes. */
    {
        const int nx = (SZ_NUDGE_P_X0 + SZ_NUDGE_P_X1) / 2;
        const int ny = (SZ_NUDGE_Y0 + SZ_NUDGE_Y1) / 2;

        CHECK(press2(SZ_LEFT, nx, ny, &act, &val) == MZ_FEED_TAKEN,
              "the second hand's nudge press was refused");
        CHECK(act == SZ_ACT_NUDGE_FWD,
              "the second hand's '+' cell sent act=%d, expected SZ_ACT_NUDGE_FWD", act);
        CHECK(side_nudging(SZ_LEFT) == 1,
              "the second hand's bend is invisible -- the next report would cancel it");
        /* The primary hand reports something else entirely on the same panel, which is
         * a BG press at local x 170 -- outside every control, so it fires nothing. */
        move(SZ_LEFT, 170, 400, &act, &val);
        CHECK(side_nudging(SZ_LEFT) == 1,
              "the primary hand's report cancelled the second hand's bend");
        lift2(SZ_LEFT, nx, ny, &act, &val);
        CHECK(side_nudging(SZ_LEFT) == 0,
              "the second hand's bend outlived its press -- the track would keep sliding");
    }

    /* THE SAFETY NET COVERS BOTH HANDS. side_reset_all() is what the touch device
     * going away calls, and a second finger left latched by it would leave a channel
     * fader nothing can move. */
    reset_all();
    CHECK(open_by_funnel(SZ_LEFT), "the left drawer did not open before the reset");
    press(SZ_LEFT, lx, yL, &act, &val);
    press2(SZ_LEFT, lx, yR, &act, &val);
    side_reset_all();
    CHECK(!side_any_open(), "side_reset_all() left a drawer out");
    CHECK(side_pressed(SZ_LEFT) == SZ_HIT_NONE,
          "side_reset_all() left a highlight on the primary hand");
    CHECK(side_nudging(SZ_LEFT) == 0 && side_nudging(SZ_RIGHT) == 0,
          "side_reset_all() left a bend on");
    /* ...and neither pointer is still latched: a fresh press on either lane is a
     * fresh press. */
    CHECK(open_by_funnel(SZ_LEFT), "the drawer would not reopen after the reset");
    act = SZ_ACT_NONE; val = -1;
    CHECK(press2(SZ_LEFT, lx, yL, &act, &val) == MZ_FEED_TAKEN && act == SZ_ACT_FADER
          && val == vL,
          "the second pointer was still latched across side_reset_all()");
    side_reset_all();
}

/* The columns a string's ink really reaches, read from the atlas itself: walk the pens
 * with menu_font_adv() -- what every painter steps -- and for each glyph take the first
 * and last column that carries any coverage.
 *
 * BOTH ENDS MUST COME FROM THE COVERAGE, not from the glyph's metrics. A glyph's ink
 * begins at its `left` bearing (signed, and not zero for the thinned Decker bake: Decker
 * Bold's 'l' is 11 px of ink at -1) and ends at `left + w - 1` BEFORE the thinning --
 * lighten() thins each glyph a sixth of a pixel a side and the bake says outright that
 * "the outermost pixels of the box are allowed to fall to zero", so the last inked
 * column can be short of the box. A `left`-blind oracle would read every label 1-2 px
 * wrong and call a correct painter broken. */
static void ink_extent(const char *s, int pen, int *lo, int *hi)
{
    int k, col, row, first = 1;

    *lo = *hi = 0;
    for (k = 0; s[k]; k++) {
        const struct menu_glyph *g = menu_font_glyph((unsigned char)s[k]);

        for (col = 0; col < (int)g->w; col++) {
            int hit = 0;

            for (row = 0; row < (int)g->h && !hit; row++)
                hit = menu_font_cov(g, col, row) != 0;
            if (!hit)
                continue;
            if (first) {
                *lo = *hi = pen + g->left + col;
                first = 0;
            } else if (pen + g->left + col > *hi) {
                *hi = pen + g->left + col;
            }
        }
        pen += menu_font_adv((unsigned char)s[k], (unsigned char)s[k + 1]);
    }
}

/* THE DRAWER'S LABEL IS LAID DOWN BY THE PENS THE ATLAS MEASURES.
 *
 * The width sp_text_center() CENTRES with (menu_text_width) and the pens sp_text()
 * steps have to be the same numbers, and nothing else in this file can see a
 * disagreement: the drawer has no classifier to fall out with, so a walk that stepped
 * something else would draw a slightly different word, off its centre by however much
 * it got wrong -- invisible in a test that checks marks by name.
 *
 * The INK WIDTH is the tooth, and it needs no copy of sp_x()'s mirror: the extent from
 * the first inked column to the last is a pure function of the string and the atlas --
 * the pen's origin, and the mirror, both cancel out. On a bake whose pair table is live
 * it also says WHICH walk was used, because the two answer differently at every kerned
 * pair; on the shipped Decker bake the table is all zero (MENU_FONT_KERNED 0), so what
 * it pins there is the pen arithmetic itself -- that a glyph is followed by its own
 * advance, and that the last glyph's ink is not counted as an advance. The scan stays
 * inside the frame (sp_frame is 2 px) so it measures the label and not the box, and the
 * button face is MENU_BTN, which is not the bed. */
static void test_paint_label_pens(void)
{
    static const struct { int ly0, ly1; const char *s; } labels[] = {
        { SZ_SYNC_Y0, SZ_SYNC_Y1, "SYNC" },
        { SZ_CUE_Y0,  SZ_CUE_Y1,  "CUE"  },
        { SZ_PLAY_Y0, SZ_PLAY_Y1, "PLAY" }
    };
    int side, i;

    for (side = 0; side < 2; side++) {
        struct menu_view v = mkview(180, 800);

        for (i = 0; i < 3; i++) {
            const char *s = labels[i].s;
            unsigned int face = menu_pixel(16, MENU_BTN);
            int dx0, dx1, minx = 99999, maxx = -1, x, y, ix0, ix1, want;

            ink_extent(s, 0, &ix0, &ix1);
            want = ix1 - ix0 + 1;

            dx0 = devx(180, side == SZ_LEFT ? SZ_BTN_X0 : SZ_W - 1 - SZ_BTN_X1);
            dx1 = devx(180, side == SZ_LEFT ? SZ_BTN_X1 : SZ_W - 1 - SZ_BTN_X0);
            dx0 += 2;                       /* inside sp_frame's 2 px */
            dx1 -= 2;

            fill(0xa5a5u, 180, 800);
            side_paint(&v, side, 0, 512);

            for (y = labels[i].ly0 + 2; y <= labels[i].ly1 - 2; y++)
                for (x = dx0; x <= dx1; x++)
                    if (fb[y * 180 + x] != face) {
                        if (x < minx) minx = x;
                        if (x > maxx) maxx = x;
                    }
            CHECK(maxx >= 0, "side %d: %s drew no ink at all", side, s);
            if (maxx < 0)
                continue;
            CHECK(maxx - minx + 1 == want,
                  "side %d: %s is %d px wide where the atlas's own advances make it %d"
                  " -- a walk that is not stepping menu_font_adv()",
                  side, s, maxx - minx + 1, want);
        }
    }
}

int main(void)
{
    test_geometry();
    test_hit();
    test_entry();
    test_fader_maths();
    test_gesture_shut();
    test_gesture_open();
    test_gesture_fader();
    test_both_open();
    test_one_surface();
    test_nudging();
    test_two_hands();
    test_entry_survives_declined();
    test_paint_refusals();
    test_paint_covers();
    test_paint_idempotent();
    test_paint_pressed_is_local();
    test_paint_mirror();
    test_paint_fader_cap();
    test_paint_signs();
    test_paint_label_pens();

    printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "ok", checks, failures);
    return failures ? 1 : 0;
}
