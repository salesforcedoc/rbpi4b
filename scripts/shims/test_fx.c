/*
 * test_fx.c -- the BEAT FX panel's three touch controls, with no Pi, no panel, no plane
 * and no rbp anywhere in this file.
 *
 * It links the PRODUCTION fx_zone.c and fx_paint.c, for the reason test_prompt.c,
 * test_side.c, test_menu.c, test_util.c and test_wave.c link theirs: a rectangle
 * boundary, an anchored release and a timeout guard are exactly the kind of thing that
 * gets quietly rewritten in a copy.
 *
 * fx_zone.c cannot see rbp. What rbp would tell it -- which Beat FX channel its panel
 * is currently on -- never reaches this module at all: pointsrc.c reads that word and
 * decides the next one, and what it wants SENT comes back from here as an FX_ACT_* code
 * and, for the picker, as a switch position. That is what makes the things with real
 * consequences testable without a unit:
 *
 * 1. THE GEOMETRY, as arithmetic and as pinned numbers. FX_H must be the sum of the
 *    parts it claims to be made of, or the fourteenth row runs off the bottom of the
 *    box; every row rect must lie inside the box, be FX_ROW_H tall and the shared row
 *    width, and not touch its neighbour. The box itself must be on the screen, and --
 *    since the operator asked for it to be rbp's BEAT FX panel's own rectangle, drawn
 *    over the panel -- all four of its edges must EQUAL FX_PANEL_*'s, and a row must be
 *    exactly the width of rbp's own effect-name cell. Every one of those is a relation to
 *    FX_PANEL_* rather than a number, so rbp moving his panel cannot leave the copy
 *    behind. And
 *    the THREE HIT RECTS must all sit inside the grey plate rbp draws and must not
 *    overlap each other -- the header bar spans the plate's full width and the two boxes
 *    below it are inset, and the whole point of the gaps between them is that a thumb
 *    cannot straddle two controls.
 *
 * 2. THE ZONE'S LATCH, which is the whole safety property, and the same one
 *    prompt_zone.c keeps. A press this module swallowed has been withheld from rbp for
 *    its whole life; rbp is going to be given its release. So a press that began inside
 *    a rect must keep being OURS until it lifts, even after it has slid off.
 *
 * 3. FIRE-ON-RELEASE, ANCHORED, AND ONLY ON THE SAME RECT -- for the header bar and the
 *    CH SELECT box. A press that begins on the CH SELECT box and slides to the effect-name
 *    cell answers NOTHING: the rect that fires is the rect the press STARTED on, and only
 *    if the finger is still on it when it lifts. That is the same rule menu_zone.c keeps
 *    for the band, and it is what stops a drag that merely crossed the panel from cycling
 *    the operator's channel.
 *
 * 3b. FIRE-ON-PRESS, for the EFFECT-NAME CELL only. It is a power toggle, so it acts the
 *    moment the finger goes down -- the operator's own answer of 2026-10-07, and the same
 *    edge hc_zone.c's HOT CUE pads moved to. Its press answers FX_ACT_POWER and its
 *    release answers NOTHING, so a tap is exactly one toggle and a RUN of downs is not a
 *    second one. The ANCHOR still protects it: a press that began on the header bar and
 *    slid onto the name cell fires neither control ([[declined-press-must-still-see-release]]).
 *
 * 4. A PRESS THAT BEGAN ELSEWHERE IS NEVER ADOPTED. This is the one hole the first
 *    draft of fx_feed() had: clearing the anchor on a miss let a finger that started on
 *    the glass and slid across the panel re-open the press at wherever it had got to.
 *    The module must answer MZ_FEED_NONE for the WHOLE of such a gesture, first report
 *    to last.
 *
 * 5. THE PICKER OWNS EVERY REPORT WHILE IT IS UP, wherever it lands, so a press that
 *    dismisses it cannot also press whatever is underneath. It is asked FIRST in
 *    pointsrc.c's ladder for exactly that reason.
 *
 * 6. THE PICKER FIRES ON RELEASE TOO, AND ONLY ON THE ROW THE PRESS ANCHORED TO, and a
 *    miss, a slide or a tap outside closes it and sends NOTHING -- an honest no-op, not
 *    a silent action. A release with no press behind it is swallowed but does NOT close
 *    the box: nothing was dismissed, so there is nothing to dismiss.
 *
 * 7. THE TIMEOUT, and the one clause in it that is not about time. It closes the box
 *    with nothing touching it -- and it does NOT while a finger is down, because closing
 *    there would clear the latch and hand rbp a release with no press behind it
 *    ([[declined-press-must-still-see-release]]). Deferring costs one release.
 *
 * 8. THE NAME TABLE. Fourteen rows, none empty, every character in the font's set:
 *    menu_font.h draws a GAP for one that is not, which would ship as a dead-looking row
 *    rather than as a failure. The table is MEASURED against rbp's own panel (fx_zone.c
 *    records the drill), so what is pinned here is its SHAPE and its legibility, not the
 *    order -- a reorder is a data edit and must not fail a test.
 *
 * 9. THE IMAGE. Every pixel of the view is written, so a second paint of the same state
 *    leaves the same bytes -- the plane's buffer is single-buffered and a paint that is
 *    not idempotent is a paint that tears. A pressed row is a different picture, and the
 *    difference is confined to that row's own rect.
 *
 * Build + run (static, so no rootfs is needed to load it):
 *     make test
 */
#define _GNU_SOURCE
#include "fx_paint.h"
#include "menu_zone.h"      /* MZ_LOGICAL_W/H: the space the box must fit in */
#include "menu_font.h"      /* menu_font_index(): the labels' character set */

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
 * The harness. fx_feed() and fxlist_feed() are the module's two doors, so every
 * gesture test is a sequence of reports fed exactly as pointsrc.c's funnel feeds them.
 *
 * The clock is a plain counter: nothing in fx_zone.c reads a clock of its own, so the
 * tests own time completely and the timeout is arithmetic rather than a sleep.
 * ------------------------------------------------------------------------- */

static unsigned long long now_ms;

/* A point comfortably inside a rect, so a boundary change does not turn a gesture test
 * into a geometry test. */
static int hx(void) { return (FX_HEADER_X0 + FX_HEADER_X1) / 2; }
static int hy(void) { return (FX_HEADER_Y0 + FX_HEADER_Y1) / 2; }
static int chx(void) { return (FX_CH_X0 + FX_CH_X1) / 2; }
static int chy(void) { return (FX_CH_Y0 + FX_CH_Y1) / 2; }
static int nmx(void) { return (FX_NAME_X0 + FX_NAME_X1) / 2; }
static int nmy(void) { return (FX_NAME_Y0 + FX_NAME_Y1) / 2; }

/* A row's own centre. */
static int rowx(void) { return (FXLIST_ROW_X0 + FXLIST_ROW_X1) / 2; }
static int rowy(int row) { return (FXLIST_ROW_Y0(row - 1) + FXLIST_ROW_Y1(row - 1)) / 2; }

/* Two report helpers, one per module: they differ by which feed they call and by what
 * "nothing" is, which is FX_ACT_NONE for the zone and -1 for the picker. */
static int zfeed(int down, int x, int y, int *act)
{
    return fx_feed(down, x, y, act);
}

static int pfeed(int down, int x, int y, int *act)
{
    return fxlist_feed(down, x, y, act);
}

/* A press and a release at the same point, with no movement: the tap the zone is for.
 * Returns the RELEASE's act, which is where the header bar and the CH box answer. */
static int ztap(int x, int y)
{
    int act = FX_ACT_NONE;

    zfeed(1, x, y, &act);
    act = FX_ACT_NONE;
    zfeed(0, x, y, &act);
    return act;
}

/* The act a PRESS answers, for a control that fires on the up->down edge (the
 * effect-name cell). Resets first, so the press is a fresh edge and not a run of one
 * already in flight. */
static int zpress(int x, int y)
{
    int act = FX_ACT_NONE;

    fx_reset();
    zfeed(1, x, y, &act);
    return act;
}

/* The same for the picker. Returns the row's switch position, or -1. */
static int ptap(int x, int y)
{
    int act = -1;

    pfeed(1, x, y, &act);
    act = -1;
    pfeed(0, x, y, &act);
    return act;
}

/* ---------------------------------------------------------------------------
 * 1. The geometry
 * ------------------------------------------------------------------------- */

static void test_geometry(void)
{
    int i;

    /* THE HEIGHT IS THE SUM OF ITS PARTS, because fx_zone.h derives it that way and says
     * the sum is exact. Written out from the literals rather than from the header, so a
     * change to one of them that is not matched by the others fails HERE and not as a row
     * hanging off the bottom of the box. There is no title and no rule to add in any
     * more: the operator struck the caption, so the box is a margin, fourteen rows and
     * nothing else. */
    {
        int h = 2 * FX_EDGE_Y
                + FXLIST_ROWS * FX_ROW_H + (FXLIST_ROWS - 1) * FX_ROW_GAP;

        CHECK(h == FX_H, "the parts sum to %d, not FX_H (%d)", h, FX_H);
    }

    /* THE BOX IS RBP'S OWN BEAT FX PLATE'S RECTANGLE, DRAWN OVER IT, which is the
     * operator's ask of 2026-10-06 -- "have the beatfx menu display on the right side
     * with the same dimensions of the beatfx box ... you don't need a title menu". Every
     * line below is a RELATION between the box and the plate rather than a number someone
     * typed, so rbp moving his panel has to move this box with it; if any of them is ever
     * re-spelled as a literal, this is what fails. The four edges are EQUAL, not reflected
     * -- the box covers the panel, and the equality is the whole of "same dimensions". */
    CHECK(FX_W == FX_PANEL_X1 - FX_PANEL_X0 + 1, "the box is not the plate's width");
    CHECK(FX_H == FX_PANEL_Y1 - FX_PANEL_Y0 + 1, "the box is not the plate's height");
    CHECK(FX_X0 == FX_PANEL_X0, "the box does not start at the plate's left edge");
    CHECK(FX_X1 == FX_PANEL_X1, "the box does not end at the plate's right edge");
    CHECK(FX_Y0 == FX_PANEL_Y0, "the box does not start at the plate's own height");
    CHECK(FX_Y1 == FX_PANEL_Y1, "the box does not end at the plate's own height");

    /* On the screen. */
    CHECK(FX_X0 >= 0 && FX_X1 < MZ_LOGICAL_W, "the box is off the screen horizontally");
    CHECK(FX_Y0 >= 0 && FX_Y1 < MZ_LOGICAL_H, "the box is off the screen vertically");

    /* ALL THREE HIT RECTS ARE INSIDE rbp'S OWN PLATE, which is what makes a finger land
     * on the drawn control and not in the margin beside it. */
    CHECK(FX_HEADER_X0 >= FX_PANEL_X0 && FX_HEADER_X1 <= FX_PANEL_X1,
          "the BEAT FX header bar is outside the panel plate");
    CHECK(FX_CH_X0   >= FX_PANEL_X0 && FX_CH_X1   <= FX_PANEL_X1,
          "the CH SELECT box is outside the panel plate");
    CHECK(FX_NAME_X0 >= FX_PANEL_X0 && FX_NAME_X1 <= FX_PANEL_X1,
          "the effect-name cell is outside the panel plate");
    CHECK(FX_HEADER_Y0 >= FX_PANEL_Y0 && FX_HEADER_Y1 <= FX_PANEL_Y1,
          "the BEAT FX header bar is outside the panel plate");
    CHECK(FX_CH_Y0   >= FX_PANEL_Y0 && FX_CH_Y1   <= FX_PANEL_Y1,
          "the CH SELECT box is outside the panel plate");
    CHECK(FX_NAME_Y0 >= FX_PANEL_Y0 && FX_NAME_Y1 <= FX_PANEL_Y1,
          "the effect-name cell is outside the panel plate");

    /* THE HEADER BAR IS THE FULL PLATE WIDTH, which is rbp's own drawing -- his grey bar
     * spans the plate, so its rect is the plate's x bounds and not an inset box's. It is
     * the one hit rect that is NOT inset ten px, and this is what keeps it so. */
    CHECK(FX_HEADER_X0 == FX_PANEL_X0 && FX_HEADER_X1 == FX_PANEL_X1,
          "the header bar is not the full plate width");

    /* AND NO TWO TOUCH. The header sits above the name cell and the two inset boxes are
     * 30 rows apart -- every gap is the whole reason a thumb cannot straddle two of them,
     * and a change that grew one towards another fails here. */
    CHECK(FX_HEADER_Y1 < FX_NAME_Y0, "the header bar overlaps or touches the name cell");
    CHECK(FX_NAME_Y1 < FX_CH_Y0, "the two inset boxes overlap or touch");
    CHECK(FX_NAME_X0 == FX_CH_X0 && FX_NAME_X1 == FX_CH_X1,
          "the two inset boxes no longer share their columns");

    /* Every row inside the box, the right size, clear of the margin, and not touching its
     * neighbour. */
    for (i = 0; i < FXLIST_ROWS; i++) {
        CHECK(FXLIST_ROW_X0 >= FX_X0 + FX_EDGE_X, "row %d starts outside the margin", i);
        CHECK(FXLIST_ROW_X1 <= FX_X1 - FX_EDGE_X, "row %d ends outside the margin", i);
        CHECK(FXLIST_ROW_Y0(i) >= FX_Y0 + FX_EDGE_Y, "row %d starts above the margin", i);
        CHECK(FXLIST_ROW_Y1(i) <= FX_Y1 - FX_EDGE_Y, "row %d runs off the bottom", i);
        CHECK(FXLIST_ROW_Y1(i) - FXLIST_ROW_Y0(i) + 1 == FX_ROW_H,
              "row %d is not FX_ROW_H tall", i);
        if (i)
            CHECK(FXLIST_ROW_Y0(i) - FXLIST_ROW_Y1(i - 1) - 1 == FX_ROW_GAP,
                  "the gap above row %d is not FX_ROW_GAP", i);
    }
    /* The last row's foot, plus the margin, is the box's own bottom edge. */
    CHECK(FXLIST_ROW_Y1(FXLIST_ROWS - 1) + FX_EDGE_Y == FX_Y1,
          "the rows do not fill the box");

    /* A ROW IS EXACTLY THE WIDTH OF RBP'S OWN EFFECT-NAME CELL, and that is what "same
     * dimensions ... visually similar" comes to at the row: his cell is inset ten pixels
     * from his plate and so is this one, and the box IS the plate. It is the single
     * geometric promise the two boxes make each other, it is a promise about RBP'S
     * layout rather than this module's, and FX_EDGE_X is therefore a literal that this
     * check is here to hold against his measurement. */
    CHECK(FXLIST_ROW_X1 - FXLIST_ROW_X0 + 1 == FX_NAME_X1 - FX_NAME_X0 + 1,
          "a row is not the width of rbp's own effect-name cell");

    /* THE BOX COVERS rbp'S OWN PLATE, AND ITS FIRST NINE ROWS SIT UNDER THE SWIPE-DOWN
     * BAND'S STRIP -- x 1090..1269, y 47 up against the band's 0..55 -- and the RIGHT-hand
     * drawer is the same 180 px column at the same edge. All three are inherited and not
     * an oversight: the box IS rbp's plate, and rbp's plate already starts at y 47 under
     * the band. None of it can be asserted away here because it is not this module's to
     * decide -- what keeps the box and either of those surfaces off the glass together is
     * pointsrc.c's FX rung, which refuses while any drawer or the band is open and is
     * asked before either of them can open on a report the picker is holding. What IS
     * this file's is that the box is on the screen and clear of the deck QUANTIZE boxes
     * at the foot, so that is what is checked. */
    CHECK(FX_Y1 < 732, "the box reaches the deck QUANTIZE boxes");
    CHECK(FX_X0 > 0 && FX_X1 < MZ_LOGICAL_W - 1, "the box reaches a screen edge");
}

/* ---------------------------------------------------------------------------
 * 8. The names
 * ------------------------------------------------------------------------- */

static void test_names(void)
{
    int row, c, widths[FXLIST_ROWS];

    CHECK(FXLIST_ROWS == 14, "the picker has %d rows, not 14", FXLIST_ROWS);

    for (row = 1; row <= FXLIST_ROWS; row++) {
        const char *s = fxlist_row_label(row);

        CHECK(s != 0, "row %d has no label", row);
        if (!s)
            continue;
        CHECK(s[0] != 0, "row %d's label is empty", row);
        for (c = 0; s[c]; c++)
            CHECK(menu_font_index((unsigned char)s[c]) >= 0,
                  "'%c' in row %d's label is not in the font", s[c], row);
        /* Uppercase and nothing else: the atlas is a single case and a lowercase letter
         * would ship as a gap, the same failure as a missing glyph. */
        for (c = 0; s[c]; c++)
            CHECK((s[c] >= 'A' && s[c] <= 'Z') || s[c] == ' ',
                  "row %d's label '%s' is not upper case", row, s);
        widths[row - 1] = menu_text_width(s);
    }

    CHECK(fxlist_row_label(0) == 0, "row 0 has a label");
    CHECK(fxlist_row_label(FXLIST_ROWS + 1) == 0, "row 15 has a label");

    /* There is no title to walk: the operator struck the "BEAT FX" caption, and the
     * header's picture is now the box with nothing above its first row. */

    /* The widest label is the one the width refusal below is about, and the row is
     * comfortably wider than it at 1:1 -- stated here so a new name that does not fit is
     * caught as a fact about the table rather than as a plane that will not come up. */
    {
        int max = 0;

        for (row = 0; row < FXLIST_ROWS; row++)
            if (widths[row] > max)
                max = widths[row];
        CHECK(max + 2 <= FXLIST_ROW_X1 - FXLIST_ROW_X0 + 1,
              "the widest label (%d px) does not fit a row (%d px)", max,
              FXLIST_ROW_X1 - FXLIST_ROW_X0 + 1);
    }
}

/* ---------------------------------------------------------------------------
 * The hit test
 * ------------------------------------------------------------------------- */

static void test_hit(void)
{
    /* All three rects are INCLUSIVE at both ends, and one pixel outside is not. */
    CHECK(fx_hit(FX_HEADER_X0, FX_HEADER_Y0) == FX_HIT_HEADER, "the header's top-left missed");
    CHECK(fx_hit(FX_HEADER_X1, FX_HEADER_Y1) == FX_HIT_HEADER, "the header's bottom-right missed");
    CHECK(fx_hit(hx(), FX_HEADER_Y0 - 1) == FX_HIT_NONE, "one px above the header hit");
    CHECK(fx_hit(hx(), FX_HEADER_Y1 + 1) == FX_HIT_NONE, "one px below the header hit");
    CHECK(fx_hit(FX_HEADER_X0 - 1, hy()) == FX_HIT_NONE, "one px left of the header hit");
    CHECK(fx_hit(FX_HEADER_X1 + 1, hy()) == FX_HIT_NONE, "one px right of the header hit");

    CHECK(fx_hit(FX_CH_X0, FX_CH_Y0) == FX_HIT_CH, "the CH box's left edge missed");
    CHECK(fx_hit(FX_CH_X1, FX_CH_Y1) == FX_HIT_CH, "the CH box's right edge missed");
    CHECK(fx_hit(FX_CH_X0 - 1, chy()) == FX_HIT_NONE, "one px left of the CH box hit");
    CHECK(fx_hit(FX_CH_X1 + 1, chy()) == FX_HIT_NONE, "one px right of the CH box hit");
    CHECK(fx_hit(chx(), FX_CH_Y0 - 1) == FX_HIT_NONE, "one px above the CH box hit");
    CHECK(fx_hit(chx(), FX_CH_Y1 + 1) == FX_HIT_NONE, "one px below the CH box hit");

    CHECK(fx_hit(FX_NAME_X0, FX_NAME_Y0) == FX_HIT_NAME, "the name cell's left edge missed");
    CHECK(fx_hit(FX_NAME_X1, FX_NAME_Y1) == FX_HIT_NAME, "the name cell's right edge missed");
    CHECK(fx_hit(nmx(), FX_NAME_Y0 - 1) == FX_HIT_NONE, "one px above the name cell hit");
    CHECK(fx_hit(nmx(), FX_NAME_Y1 + 1) == FX_HIT_NONE, "one px below the name cell hit");

    /* THE HEADER BAR REACHES THE PLATE'S OWN EDGES -- it is the full plate width, so the
     * plate's left and right margins are the header, unlike the two inset boxes. */
    CHECK(fx_hit(FX_PANEL_X0, hy()) == FX_HIT_HEADER,
          "the header does not reach the plate's left edge");
    CHECK(fx_hit(FX_PANEL_X1, hy()) == FX_HIT_HEADER,
          "the header does not reach the plate's right edge");

    /* THE GAP BETWEEN THEM IS NOBODY'S, and so is the plate's margin beside the two inset
     * boxes -- the rect is the DRAWN box, not the plate. These are the taps that would
     * have fired the wrong control under the plan's first reading of the geometry. */
    CHECK(fx_hit(hx(), (FX_HEADER_Y1 + FX_NAME_Y0) / 2) == FX_HIT_NONE,
          "the plate between the header and the name cell hit something");
    CHECK(fx_hit(chx(), (FX_NAME_Y1 + FX_CH_Y0) / 2) == FX_HIT_NONE,
          "the gap between the two inset boxes hit something");
    CHECK(fx_hit(FX_PANEL_X0, chy()) == FX_HIT_NONE,
          "the plate's left margin hit the CH box");
    CHECK(fx_hit(FX_PANEL_X1, chy()) == FX_HIT_NONE,
          "the plate's right margin hit the CH box");
    CHECK(fx_hit(FX_PANEL_X0, nmy()) == FX_HIT_NONE,
          "the plate's left margin hit the name cell");

    /* And anywhere else on the screen is not this module's business. */
    CHECK(fx_hit(0, 0) == FX_HIT_NONE, "the top-left of the screen hit the panel");
    CHECK(fx_hit(MZ_LOGICAL_W - 1, MZ_LOGICAL_H - 1) == FX_HIT_NONE,
          "the bottom-right of the screen hit the panel");
    CHECK(fx_hit(-1, -1) == FX_HIT_NONE, "a negative point hit the panel");
    CHECK(fx_hit(FX_PANEL_X1 + 1, chy()) == FX_HIT_NONE, "just off the plate hit it");
}

/* ---------------------------------------------------------------------------
 * 2 + 3 + 3b + 4. The zone's gesture
 * ------------------------------------------------------------------------- */

static void test_zone(void)
{
    int act, code;

    /* A CLEAN TAP ON EACH BOX ANSWERS ITS OWN ACT, and sends it once. The header bar and
     * the CH box fire on the RELEASE, so ztap() answers for them. */
    fx_reset();
    CHECK(ztap(hx(), hy()) == FX_ACT_PICK, "a tap on the header bar did not raise the picker");
    fx_reset();
    CHECK(ztap(chx(), chy()) == FX_ACT_CH, "a tap on the CH box did not cycle the channel");

    /* THE EFFECT-NAME CELL FIRES ON THE PRESS, NOT THE RELEASE -- the operator's own
     * answer of 2026-10-07. Its press answers FX_ACT_POWER and its release answers
     * NOTHING, so a tap is exactly one toggle. */
    fx_reset();
    CHECK(zpress(nmx(), nmy()) == FX_ACT_POWER,
          "the name cell's PRESS did not toggle the Beat FX");
    fx_reset();
    act = FX_ACT_NONE;
    CHECK(zfeed(1, nmx(), nmy(), &act) == MZ_FEED_TAKEN, "a press on the name cell was not taken");
    CHECK(act == FX_ACT_POWER, "the name cell's press did not answer FX_ACT_POWER");
    act = FX_ACT_NONE;
    CHECK(zfeed(0, nmx(), nmy(), &act) == MZ_FEED_TAKEN, "the name cell's release was not taken");
    CHECK(act == FX_ACT_NONE, "the name cell's RELEASE fired a second act -- a tap must fire once");

    /* THE OTHER TWO PRESSES ANSWER NOTHING: for them the release decides. */
    fx_reset();
    act = FX_ACT_CH;
    code = zfeed(1, chx(), chy(), &act);
    CHECK(code == MZ_FEED_TAKEN, "a press on the CH box was not taken");
    CHECK(act == FX_ACT_NONE, "the CH box's press edge answered an act");
    fx_reset();
    act = FX_ACT_CH;
    code = zfeed(1, hx(), hy(), &act);
    CHECK(code == MZ_FEED_TAKEN, "a press on the header bar was not taken");
    CHECK(act == FX_ACT_NONE, "the header bar's press edge answered an act");

    /* AND THE RELEASE IS OURS WHATEVER IT DOES, because the press was. */
    act = FX_ACT_NONE;
    code = zfeed(0, hx(), hy(), &act);
    CHECK(code == MZ_FEED_TAKEN, "the release of an owned press was not taken");
    CHECK(act == FX_ACT_PICK, "the header bar's release did not raise the picker");

    /* A PRESS THAT BEGAN ELSEWHERE IS NEVER ADOPTED, first report to last -- including
     * one that slides right across all three rects. This is the hole the first draft had,
     * and on the name cell it is now the difference between a slide and a toggle. */
    fx_reset();
    CHECK(zfeed(1, 100, 400, &act) == MZ_FEED_NONE, "a press at x 100 was taken");
    act = FX_ACT_NONE;
    CHECK(zfeed(1, chx(), chy(), &act) == MZ_FEED_NONE,
          "a press that slid onto the CH box was adopted");
    act = FX_ACT_NONE;
    CHECK(zfeed(1, nmx(), nmy(), &act) == MZ_FEED_NONE,
          "a press that slid onto the name cell was adopted");
    CHECK(act == FX_ACT_NONE, "a slide onto the name cell TOGGLED the Beat FX");
    CHECK(zfeed(0, chx(), chy(), &act) == MZ_FEED_NONE,
          "the release of a foreign press was adopted");

    /* A release with no press behind it is not ours either, and answers nothing. */
    fx_reset();
    act = FX_ACT_CH;
    CHECK(zfeed(0, chx(), chy(), &act) == MZ_FEED_NONE, "a bare release was taken");
    CHECK(act == FX_ACT_NONE, "a bare release answered an act");

    /* ANCHORED: a press on the CH box that lifts on the name cell fires NEITHER. This is
     * the rule that stops a drag across the panel cycling the operator's channel. */
    fx_reset();
    act = FX_ACT_NONE;
    CHECK(zfeed(1, chx(), chy(), &act) == MZ_FEED_TAKEN, "the press was not taken");
    act = FX_ACT_NONE;
    CHECK(zfeed(1, nmx(), nmy(), &act) == MZ_FEED_TAKEN,
          "a move off the anchored rect was dropped -- the release would go to rbp deaf");
    CHECK(act == FX_ACT_NONE, "a move answered an act");
    act = FX_ACT_NONE;
    CHECK(zfeed(0, nmx(), nmy(), &act) == MZ_FEED_TAKEN, "the release was not taken");
    CHECK(act == FX_ACT_NONE, "a press on one rect that lifted on the other fired");

    /* THE ANCHOR PROTECTS THE TOGGLE TOO: a press on the HEADER bar that slides onto the
     * name cell fires NOTHING -- neither the picker (the release is not on the header) nor
     * the toggle (the press did not begin on the name cell). */
    fx_reset();
    act = FX_ACT_NONE;
    CHECK(zfeed(1, hx(), hy(), &act) == MZ_FEED_TAKEN, "the header press was not taken");
    CHECK(act == FX_ACT_NONE, "the header press answered an act");
    act = FX_ACT_NONE;
    zfeed(1, nmx(), nmy(), &act);
    CHECK(act == FX_ACT_NONE, "a slide from the header onto the name cell TOGGLED");
    act = FX_ACT_NONE;
    CHECK(zfeed(0, nmx(), nmy(), &act) == MZ_FEED_TAKEN, "the release was not taken");
    CHECK(act == FX_ACT_NONE, "a header press that lifted on the name cell fired");

    /* And the same the other way round: a press on the name cell toggles ON ITS PRESS,
     * and lifting on the CH box answers nothing more. */
    fx_reset();
    act = FX_ACT_NONE;
    zfeed(1, nmx(), nmy(), &act);
    CHECK(act == FX_ACT_POWER, "a press on the name cell that would lift on CH did not toggle");
    act = FX_ACT_NONE;
    zfeed(0, chx(), chy(), &act);
    CHECK(act == FX_ACT_NONE, "a press on the name cell that lifted on CH fired again");

    /* A RUN OF DOWNS IS ONE TOGGLE, not one per sample: the effect-name cell fires on the
     * up->down edge only, and a second `down` is the same finger. */
    fx_reset();
    act = FX_ACT_NONE;
    zfeed(1, nmx(), nmy(), &act);
    CHECK(act == FX_ACT_POWER, "the name cell's first down did not toggle");
    act = FX_ACT_NONE;
    CHECK(zfeed(1, nmx(), nmy(), &act) == MZ_FEED_TAKEN, "a run down was not taken");
    CHECK(act == FX_ACT_NONE, "a run of downs toggled a SECOND time");
    act = FX_ACT_NONE;
    zfeed(1, nmx(), nmy(), &act);
    CHECK(act == FX_ACT_NONE, "a third down toggled again");

    /* A PRESS THAT STARTED INSIDE AND SLID FAR OFF still owns its release, and -- on the
     * name cell -- fires exactly once, on the press. */
    fx_reset();
    act = FX_ACT_NONE;
    zfeed(1, nmx(), nmy(), &act);
    CHECK(act == FX_ACT_POWER, "the name cell's press did not toggle");
    CHECK(zfeed(1, 10, 10, &act) == MZ_FEED_TAKEN, "a press that slid far off was dropped");
    act = FX_ACT_NONE;
    CHECK(zfeed(0, 10, 10, &act) == MZ_FEED_TAKEN, "its release was not taken");
    CHECK(act == FX_ACT_NONE, "a name-cell press that slid off fired again");

    /* A FAST FLICK: press inside, release at a moved position with NO move report in
     * between, which is the case the release's own coordinates exist for. */
    fx_reset();
    zfeed(1, chx(), chy(), &act);
    act = FX_ACT_NONE;
    zfeed(0, nmx(), nmy(), &act);
    CHECK(act == FX_ACT_NONE, "a flick from CH to the name cell fired on the CH rect");

    /* RESET LEAVES NOTHING LATCHED: after it, a bare release is foreign again. */
    fx_reset();
    zfeed(1, chx(), chy(), &act);
    fx_reset();
    act = FX_ACT_CH;
    CHECK(zfeed(0, chx(), chy(), &act) == MZ_FEED_NONE, "fx_reset left a press latched");
    CHECK(act == FX_ACT_NONE, "fx_reset left a press latched and it fired");
}

/* ---------------------------------------------------------------------------
 * 5 + 6. The picker
 * ------------------------------------------------------------------------- */

static void test_picker_closed(void)
{
    int act, i;

    fxlist_reset();
    CHECK(!fxlist_is_open(), "the picker is open after a reset");

    /* WHILE IT IS SHUT IT ANSWERS FOR NOTHING AT ALL -- every point, both edges. */
    for (i = 0; i < 5; i++) {
        int x = i * 300, y = i * 150;

        CHECK(pfeed(1, x, y, &act) == MZ_FEED_NONE, "a shut picker took a press at %d,%d",
              x, y);
        CHECK(pfeed(0, x, y, &act) == MZ_FEED_NONE, "a shut picker took a release at %d,%d",
              x, y);
    }
}

static void test_picker_open(void)
{
    int act, row, i;

    fxlist_reset();
    now_ms = 1000;
    fxlist_open(now_ms);
    CHECK(fxlist_is_open(), "fxlist_open did not open it");
    CHECK(!fxlist_pressed(), "the picker opened with a row already pressed");
    CHECK(fxlist_deadline() == now_ms + FX_TIMEOUT_MS, "the deadline is not the timeout");

    /* A TAP ON A ROW ANSWERS THAT ROW'S SWITCH POSITION, 0-based, and closes the box. */
    for (row = 1; row <= FXLIST_ROWS; row++) {
        fxlist_reset();
        fxlist_open(now_ms);
        CHECK(ptap(rowx(), rowy(row)) == row - 1,
              "a tap on row %d answered %d, not %d", row, ptap(rowx(), rowy(row)), row - 1);
        CHECK(!fxlist_is_open(), "a tap on row %d left the picker up", row);
    }

    /* THE PRESS ITSELF ANSWERS NOTHING. */
    fxlist_reset();
    fxlist_open(now_ms);
    act = 99;
    CHECK(pfeed(1, rowx(), rowy(3), &act) == MZ_FEED_TAKEN, "a press on a row was not taken");
    CHECK(act == -1, "the press edge answered a row");
    CHECK(fxlist_pressed() == 3, "the finger on row 3 did not highlight row 3");

    /* The highlight FOLLOWS the finger, and what fires is still the anchor. */
    pfeed(1, rowx(), rowy(9), &act);
    CHECK(fxlist_pressed() == 9, "the highlight did not follow the finger");
    act = 99;
    pfeed(0, rowx(), rowy(9), &act);
    CHECK(act == -1, "a press anchored on row 3 fired row 9");
    CHECK(!fxlist_is_open(), "that release left the picker up");

    /* A PRESS ANCHORED ON A ROW THAT LIFTS ON THE SAME ROW FIRES, even after the finger
     * left it and came back. */
    fxlist_reset();
    fxlist_open(now_ms);
    pfeed(1, rowx(), rowy(5), &act);
    pfeed(1, rowx(), rowy(7), &act);
    pfeed(1, rowx(), rowy(5), &act);
    act = 99;
    pfeed(0, rowx(), rowy(5), &act);
    CHECK(act == 4, "a press that returned to its own row answered %d, not 4", act);

    /* A TAP ABOVE THE FIRST ROW, ON A GAP, ON THE MARGIN OR OFF THE BOX CLOSES IT AND
     * SENDS NOTHING. With the caption struck there is no title band to test any more,
     * and the plate above row 1 -- FX_EDGE_Y rows of it -- is what took its place. */
    {
        const struct { int x, y; const char *what; } misses[] = {
            { rowx(), FX_Y0 + 1, "the plate above the first row" },
            { rowx(), FXLIST_ROW_Y1(0) + FX_ROW_GAP / 2, "the gap between rows" },
            { FX_X0 + 1, (FX_Y0 + FX_Y1) / 2, "the box's own margin" },
            { 5, 5, "far outside the box" },
            { MZ_LOGICAL_W - 1, MZ_LOGICAL_H - 1, "the opposite corner" }
        };

        for (i = 0; i < (int)(sizeof misses / sizeof misses[0]); i++) {
            fxlist_reset();
            fxlist_open(now_ms);
            CHECK(ptap(misses[i].x, misses[i].y) == -1,
                  "a tap on %s answered a row", misses[i].what);
            CHECK(!fxlist_is_open(), "a tap on %s left the picker up", misses[i].what);
        }
    }

    /* IT OWNS EVERY REPORT WHILE IT IS UP, WHEREVER IT LANDS -- so a press that dismisses
     * it cannot also reach rbp. The far corner is not in the box at all, which is the
     * point: the answer is TAKEN by construction and not by a rectangle test. */
    for (i = 0; i < 4; i++) {
        int x = i * 400, y = i * 250;

        /* Re-opened each round: a release with a press behind it closes the box, which is
         * the rule the next test pins. What is asked HERE is only that while it is up it
         * takes everything. */
        fxlist_reset();
        fxlist_open(now_ms);
        CHECK(pfeed(1, x, y, &act) == MZ_FEED_TAKEN, "an open picker dropped a press");
        CHECK(pfeed(1, x + 1, y, &act) == MZ_FEED_TAKEN, "an open picker dropped a move");
        CHECK(pfeed(0, x, y, &act) == MZ_FEED_TAKEN, "an open picker dropped a release");
        CHECK(!fxlist_is_open(), "a release with a press behind it did not close the box");
    }

    /* AN ANSWERING TAP CLOSES THE BOX, so the report after it is NOT the box's -- which
     * is what makes the next tap on the glass land on whatever is underneath, as it
     * should. */
    fxlist_reset();
    fxlist_open(now_ms);
    act = 99;
    pfeed(1, rowx(), rowy(2), &act);
    pfeed(0, rowx(), rowy(2), &act);
    CHECK(act == 1, "a tap on row 2 answered %d, not 1", act);
    CHECK(!fxlist_is_open(), "the box survived an answering tap");
    CHECK(pfeed(1, rowx(), rowy(2), &act) == MZ_FEED_NONE,
          "a press after the box closed was still swallowed");
}

static void test_picker_bare_release(void)
{
    int act;

    /* A RELEASE WITH NO PRESS BEHIND IT IS SWALLOWED AND DOES NOT CLOSE THE BOX: nothing
     * was dismissed, so there is nothing to dismiss. Without this clause the box could be
     * closed by a release that never had a press, which is the state a device that went
     * away mid-gesture leaves behind. */
    fxlist_reset();
    fxlist_open(1000);
    act = 99;
    CHECK(pfeed(0, 5, 5, &act) == MZ_FEED_TAKEN, "a bare release was not swallowed");
    CHECK(act == -1, "a bare release answered a row");
    CHECK(fxlist_is_open(), "a bare release closed the picker");

    /* And a move with no press behind it neither. */
    CHECK(pfeed(1, 5, 5, &act) == MZ_FEED_TAKEN, "a bare move was not swallowed");
    CHECK(fxlist_is_open(), "a bare move closed the picker");
    fxlist_close();
}

/* ---------------------------------------------------------------------------
 * 7. The timeout
 * ------------------------------------------------------------------------- */

static void test_picker_timeout(void)
{
    int act;

    fxlist_reset();
    fxlist_open(1000);

    CHECK(fxlist_expire(1000) == 0, "the picker expired the instant it opened");
    CHECK(fxlist_expire(1000 + FX_TIMEOUT_MS - 1) == 0, "it expired one ms early");
    CHECK(fxlist_is_open(), "it closed one ms early");
    CHECK(fxlist_expire(1000 + FX_TIMEOUT_MS) == 1, "it did not expire on the deadline");
    CHECK(!fxlist_is_open(), "it is still open past the deadline");
    CHECK(fxlist_expire(1000 + FX_TIMEOUT_MS + 5000) == 0,
          "it expired twice -- the caller would log it twice");

    /* NOT WHILE A FINGER IS DOWN. Closing there would clear the latch and hand rbp a
     * release with no press behind it, which is the whole reason this clause exists. */
    fxlist_reset();
    fxlist_open(1000);
    pfeed(1, rowx(), rowy(4), &act);
    CHECK(fxlist_expire(1000 + FX_TIMEOUT_MS + 60000) == 0,
          "the picker expired while a finger was down");
    CHECK(fxlist_is_open(), "the picker closed under a finger");

    /* And the release closes it -- on its own path, a moment later, as it should. */
    act = 99;
    pfeed(0, rowx(), rowy(4), &act);
    CHECK(act == 3, "the release after a held press answered %d, not 3", act);
    CHECK(!fxlist_is_open(), "the release did not close the box");
    CHECK(fxlist_expire(1000 + FX_TIMEOUT_MS + 61000) == 0,
          "a closed picker expired again");

    /* A NEGATIVE DIFFERENCE IS NOT AN EXPIRY, which is the `long` -is-4-bytes trap
     * pointsrc.c's own clock documents: the ring is 2^64 and the comparison must not
     * care where the two values sit on it. A deadline far "behind" the clock is the
     * wrap seen from the future, and the one thing it must not do is close the box a
     * second time. */
    fxlist_reset();
    fxlist_open((unsigned long long)-1 - 100);   /* deadline wraps past 0 */
    CHECK(fxlist_is_open(), "the picker did not open near the wrap");
}

/* ---------------------------------------------------------------------------
 * 9. The image
 * ------------------------------------------------------------------------- */

#define IW   FX_W
#define IH   FX_H
#define BOX  (IW * IH * 2)

static unsigned char buf_a[BOX], buf_b[BOX], buf_c[BOX], buf_d[BOX];
static unsigned char buf_spare[BOX];

static void view_for(struct menu_view *v, void *p, int dw, int dh, int bpp)
{
    memset(v, 0, sizeof *v);
    v->pix = p;
    v->pitch = dw;
    v->fb_w = dw;
    v->fb_h = dh;
    v->bpp = bpp;
    v->dw = dw;
    v->dh = dh;
    v->bx = 0;
    v->by = 0;
}

static void test_paint_size(void)
{
    struct menu_view v;

    CHECK(fxlist_paint_w(MZ_LOGICAL_W) == FX_W, "the picker is not FX_W wide at 1:1");
    CHECK(fxlist_paint_h(MZ_LOGICAL_H) == FX_H, "the picker is not FX_H tall at 1:1");
    CHECK(fxlist_paint_w(0) == 1 && fxlist_paint_h(0) == 1,
          "a zero-size page gave a zero-size picker");
    CHECK(fxlist_paint_w(MZ_LOGICAL_W * 2) == FX_W * 2, "the width does not scale");
    CHECK(fxlist_paint_h(MZ_LOGICAL_H * 2) == FX_H * 2, "the height does not scale");

    view_for(&v, buf_a, IW, IH, 16);
    CHECK(fxlist_paint_ok(&v), "a picker-sized 16 bpp view was refused");

    CHECK(!fxlist_paint_ok(0), "a null view was accepted");

    view_for(&v, 0, IW, IH, 16);
    CHECK(!fxlist_paint_ok(&v), "a null buffer was accepted");

    view_for(&v, buf_a, IW, IH, 24);
    CHECK(!fxlist_paint_ok(&v), "a 24 bpp view was accepted");

    view_for(&v, buf_a, IW, IH, 16);
    v.pitch = IW - 1;
    CHECK(!fxlist_paint_ok(&v), "a pitch short of the width was accepted");

    /* A line too short for the font: fourteen rows is a lot of vertical arithmetic and a
     * page that scales the box down reaches a row that cannot host the line box. The
     * threshold is the font's, not a magic number. */
    view_for(&v, buf_a, IW, 100, 16);
    CHECK(!fxlist_paint_ok(&v),
          "a view too small to host a %d px line box in a row was accepted", MENU_FONT_LINE);

    /* AND THE WIDTH IS ITS OWN REFUSAL, which is a real route here: the box's 180 logical
     * px at a scale of 40/180 leave a row 35 px wide, which no label fits -- and the
     * height check above passes at that same scale, so the pair isolates the width. The
     * box is narrower than it was (fx_zone.h: it is rbp's plate mirrored now), so this is
     * the refusal a small page meets FIRST. */
    view_for(&v, buf_a, IW, IH, 16);
    CHECK(fxlist_paint_ok(&v), "a view whose rows fit was refused");
    view_for(&v, buf_a, 40, IH, 16);
    CHECK(fxlist_paint_ok(&v) == 0,
          "a view too narrow to hold a label whole was accepted");
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

static void test_paint_image(void)
{
    struct menu_view v;
    int i, j;

    /* Every PIXEL is written: fill the buffer with a value the paint cannot produce and
     * check that not one survives. The box's bed reaches the corners, so a fresh
     * uninitialised plane shown before the first build cannot leak through. The unit is
     * the pixel and not the byte -- a single byte of a legitimate 16 bpp value can equal
     * the sentinel, which is a false positive that cost one build. */
    memset(buf_a, 0xa5, BOX);
    view_for(&v, buf_a, IW, IH, 16);
    fxlist_paint(&v, 0);
    for (i = 0; i < BOX; i += 2)
        if (buf_a[i] == 0xa5 && buf_a[i + 1] == 0xa5) {
            CHECK(0, "the paint left pixel %d untouched", i / 2);
            break;
        }

    memset(buf_b, 0xa5, BOX);
    view_for(&v, buf_b, IW, IH, 16);
    fxlist_paint(&v, 0);
    CHECK(!memcmp(buf_a, buf_b, BOX), "painting the same state twice gave two pictures");

    /* A PRESSED ROW IS A DIFFERENT PICTURE. */
    memset(buf_c, 0, BOX);
    view_for(&v, buf_c, IW, IH, 16);
    fxlist_paint(&v, 6);
    CHECK(memcmp(buf_b, buf_c, BOX), "a pressed row drew the same as no row at all");

    /* AND THE DIFFERENCE IS CONFINED TO THAT ROW'S OWN RECT. A row that bleeds into its
     * neighbour, the rule or the margin shows up here and nowhere else. */
    {
        int px0, py0, px1, py1, outside = 0;

        fxlist_row_rect(6, &px0, &py0, &px1, &py1);
        /* THE BUFFER IS THE BOX, so a framebuffer column/row is the LOGICAL one less the
         * box's own origin -- fx_paint.c's fp_fx()/fp_fy() and nothing else. Comparing a
         * buffer row against a logical one is off by FX_Y0, which is the shape this check
         * wore on the first run and read as "the whole row is outside". */
        for (j = 0; j < IH; j++)
            for (i = 0; i < IW; i++) {
                size_t off = ((size_t)j * IW + i) * 2;

                if (!memcmp(&buf_b[off], &buf_c[off], 2))
                    continue;
                if (i + FX_X0 < px0 || i + FX_X0 > px1 ||
                    j + FX_Y0 < py0 || j + FX_Y0 > py1)
                    outside++;
            }
        CHECK(outside == 0, "%d px changed outside row 6's rect when it was pressed",
              outside);
    }

    /* EVERY ROW'S LABEL IS LAID DOWN BY THE PENS THE ATLAS MEASURES. The width the
     * picker CENTRES with (menu_text_width) and the pens fp_text() actually steps have
     * to be the same numbers, and nothing else here can see a disagreement: this file
     * has no classifier to fall out with, so a walk that stepped something else would
     * just draw a slightly different word, off centre by however much it got wrong.
     *
     * The INK WIDTH is the tooth, and it needs no copy of fp_rect()'s scaling: the
     * extent from the first inked column to the last is a pure function of the string
     * and the atlas -- the pen's origin cancels out. On a bake whose pair table is live
     * it also says WHICH walk was used, because the two answer differently at every
     * kerned pair; on the shipped Decker bake the table is all zero (MENU_FONT_KERNED
     * 0), so what it pins there is the pen arithmetic itself -- that a glyph is followed
     * by its own advance, and that the last glyph's ink is not counted as an advance.
     * The buffer is the box at 1:1, so a buffer column is the logical one less FX_X0. */
    {
        int row;

        for (row = 1; row <= FXLIST_ROWS; row++) {
            const char *s = fxlist_row_label(row);
            int ix0, ix1, n = 0, want, minx = IW, maxx = -1, ly0, ly1, lx0, lx1, y, x;

            while (s && s[n])
                n++;
            CHECK(n > 0, "row %d has no label to lay down", row);
            if (n <= 0)
                continue;

            ink_extent(s, 0, &ix0, &ix1);
            want = ix1 - ix0 + 1;

            ly0 = FXLIST_ROW_Y0(row - 1) - FX_Y0;
            ly1 = FXLIST_ROW_Y1(row - 1) - FX_Y0;
            lx0 = FXLIST_ROW_X0 - FX_X0;
            lx1 = FXLIST_ROW_X1 - FX_X0;
            if (lx0 < 0) lx0 = 0;
            if (lx1 > IW - 1) lx1 = IW - 1;
            for (y = ly0; y <= ly1 && y < IH; y++)
                for (x = lx0; x <= lx1; x++) {
                    size_t off = ((size_t)y * IW + x) * 2;
                    unsigned int px = (unsigned int)buf_b[off] |
                                      ((unsigned int)buf_b[off + 1] << 8);

                    /* ANY coverage, not the exact label value: a glyph's edge column
                     * is a blend, so asking for the palette value would measure the
                     * solid core and report a label 1-2 px narrower than it is. The
                     * row's face is MENU_FILL, which is black at both depths, so
                     * "not the face" is the ink. The scan stops at the ROW's columns:
                     * the box's own frame runs down both of the box's edges through
                     * every row band, and a scan of the whole buffer width would
                     * measure that instead and report 180 px for every label. */
                    if (px == menu_pixel(16, MENU_FILL))
                        continue;
                    if (x < minx) minx = x;
                    if (x > maxx) maxx = x;
                }
            CHECK(maxx >= 0, "row %d's label is blank", row);
            if (maxx < 0)
                continue;
            CHECK(maxx - minx + 1 == want,
                  "row %d: the label ink is %d px wide where the atlas's own advances"
                  " make it %d -- a walk that is not stepping menu_font_adv()",
                  row, maxx - minx + 1, want);
        }
    }

    /* An out-of-range row is CLAMPED to none rather than drawn as row 14. */
    memset(buf_d, 0, BOX);
    view_for(&v, buf_d, IW, IH, 16);
    fxlist_paint(&v, FXLIST_ROWS + 5);
    CHECK(!memcmp(buf_b, buf_d, BOX), "an out-of-range row was not clamped");
    memset(buf_d, 0, BOX);
    view_for(&v, buf_d, IW, IH, 16);
    fxlist_paint(&v, -3);
    CHECK(!memcmp(buf_b, buf_d, BOX), "a negative row was not clamped");

    /* The 32 bpp paint is idempotent too -- the unit may hand either.
     *
     * ONE `unsigned int` PER PIXEL, which is what 32 bpp means: these two buffers were
     * declared `[IW * IH / 4]` -- a quarter of a pixel each -- so the paint wrote three
     * quarters of its image past the end of the first one, over the second one and over
     * whatever the linker had put after them, and the memcmp below then compared a
     * quarter of a picture and passed. Nothing failed, which is exactly why it survived;
     * a host test with an out-of-bounds write in it is a test that can pass for the wrong
     * reason, and this one is asked to prove an image is deterministic. */
    {
        static unsigned int buf32[IW * IH], buf32b[IW * IH];

        memset(buf32, 0x5a, sizeof buf32);
        view_for(&v, buf32, IW, IH, 32);
        fxlist_paint(&v, 2);
        memcpy(buf32b, buf32, sizeof buf32);
        view_for(&v, buf32, IW, IH, 32);
        fxlist_paint(&v, 2);
        CHECK(!memcmp(buf32, buf32b, sizeof buf32), "the 32 bpp paint is not idempotent");
    }

    /* A refused view is left alone rather than half-drawn. */
    {
        static unsigned char tiny[8 * 8 * 2];

        memset(tiny, 0x5a, sizeof tiny);
        view_for(&v, tiny, 8, 8, 16);
        CHECK(!fxlist_paint_ok(&v), "an 8x8 view was accepted");
        fxlist_paint(&v, 0);
        for (i = 0; i < (int)sizeof tiny; i++)
            if (tiny[i] != 0x5a) {
                CHECK(0, "a refused view was written to at byte %d", i);
                break;
            }
    }

    /* A SCALED view -- the box on a smaller page -- draws without reading past its own
     * buffer, and the spare buffer is a canary for it: the paint writes inside dw x dh
     * and nowhere else, so a scale that got the clip wrong would walk into the bytes the
     * view never claimed. 300x480 is a page whose rows still host the line box and whose
     * columns still hold the widest label, which is what fxlist_paint_ok() is asked
     * below. (It is not the SMALLEST such page -- the refusal thresholds are
     * MENU_FONT_LINE and the widest label and they move with the box, so a claim of
     * minimality here would be a claim about fx_zone.h's arithmetic made in a file that
     * does not own it.) */
    {
        int dw = 300, dh = 480;

        memset(buf_spare, 0x5a, BOX);
        view_for(&v, buf_spare, dw, dh, 16);
        CHECK(fxlist_paint_ok(&v), "the smallest page whose rows fit was refused");
        fxlist_paint(&v, 0);
        /* The view's own pixels are dw x dh, so the bytes it may write are exactly the
         * first dw*dh*2 of the buffer -- the pitch is the width here, so there is no
         * padding to allow for. Anything past that is the paint having walked off its
         * rectangle, which at a scale is the one way a clipping mistake shows. */
        for (i = dw * dh * 2; i < BOX; i++)
            if (buf_spare[i] != 0x5a) {
                CHECK(0, "a scaled paint wrote past its view at byte %d", i);
                break;
            }
    }
}

/* ---------------------------------------------------------------------------
 * The row geometry, asked of the module rather than of the macros
 * ------------------------------------------------------------------------- */

static void test_rows(void)
{
    int row, x0, y0, x1, y1;

    for (row = 1; row <= FXLIST_ROWS; row++) {
        fxlist_row_rect(row, &x0, &y0, &x1, &y1);
        CHECK(x0 == FXLIST_ROW_X0 && x1 == FXLIST_ROW_X1,
              "row %d's rect has the wrong columns", row);
        CHECK(y0 == FXLIST_ROW_Y0(row - 1), "row %d's rect starts in the wrong place", row);
        CHECK(fxlist_row_at((x0 + x1) / 2, y0) == row, "row %d's top edge missed it", row);
        CHECK(fxlist_row_at((x0 + x1) / 2, y1) == row, "row %d's bottom edge missed it", row);
        CHECK(fxlist_row_at(x0, (y0 + y1) / 2) == row, "row %d's left edge missed it", row);
        CHECK(fxlist_row_at(x1, (y0 + y1) / 2) == row, "row %d's right edge missed it", row);
        CHECK(fxlist_row_at(x0 - 1, (y0 + y1) / 2) == 0,
              "one px left of row %d hit it", row);
        CHECK(fxlist_row_at(x1 + 1, (y0 + y1) / 2) == 0,
              "one px right of row %d hit it", row);
        if (row > 1)
            CHECK(fxlist_row_at((x0 + x1) / 2, y0 - 2) == 0 ||
                  fxlist_row_at((x0 + x1) / 2, y0 - 2) == row - 1,
                  "the gap above row %d answered something else", row);
    }

    /* Out of range answers an empty rect rather than row 1's. */
    fxlist_row_rect(0, &x0, &y0, &x1, &y1);
    CHECK(x1 < x0 && y1 < y0, "row 0 gave a real rect");
    fxlist_row_rect(FXLIST_ROWS + 1, &x0, &y0, &x1, &y1);
    CHECK(x1 < x0 && y1 < y0, "row 15 gave a real rect");

    /* The plate above the first row, the gaps, and the margin are nobody's row. */
    CHECK(fxlist_row_at(rowx(), FX_Y0) == 0, "the box's top edge answered a row");
    CHECK(fxlist_row_at(rowx(), FXLIST_ROW_Y1(0) + 1) == 0,
          "the gap under row 1 answered a row");
    CHECK(fxlist_row_at(FX_X0, FXLIST_ROW_Y0(0) + 5) == 0, "the margin answered a row");
    CHECK(fxlist_row_at(FX_X1, FXLIST_ROW_Y0(0) + 5) == 0, "the margin answered a row");

    /* The box's own rect is the box. */
    fxlist_box_rect(&x0, &y0, &x1, &y1);
    CHECK(x0 == FX_X0 && y0 == FX_Y0 && x1 == FX_X1 && y1 == FX_Y1,
          "the box's rect is not FX_X0..FX_X1 by FX_Y0..FX_Y1");
}

/* ---------------------------------------------------------------------------
 * The whole thing
 * ------------------------------------------------------------------------- */

int main(void)
{
    test_geometry();
    test_names();
    test_hit();
    test_zone();
    test_picker_closed();
    test_picker_open();
    test_picker_bare_release();
    test_picker_timeout();
    test_rows();
    test_paint_size();
    test_paint_image();

    printf("test_fx: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
