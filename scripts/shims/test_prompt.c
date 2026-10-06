/*
 * test_prompt.c -- the USB STOP chooser, with no Pi, no panel, no plane and no rbp
 * anywhere in this file.
 *
 * It links the PRODUCTION prompt_zone.c and prompt_paint.c, for the reason test_menu.c,
 * test_side.c, test_util.c and test_wave.c link theirs: a cell boundary, a dead-cell
 * rule, the arming and a timeout guard are exactly the kind of thing that gets quietly
 * rewritten in a copy.
 *
 * prompt_zone.c cannot see rbp. What rbp would tell it -- whether each of the two
 * devices has media in it -- arrives in a struct prompt_state the tests hand it, and
 * what it wants sent comes back as a PR_ACT_* code. That is what makes the things with
 * real consequences testable without a unit:
 *
 * 1. THE GEOMETRY, as arithmetic and as pinned numbers. PR_H must be the sum of the
 *    parts it claims to be made of, or the second line runs off the bottom of the box;
 *    every button rect must lie inside the box, be one PR_ROW_H tall and PR_CELL_W wide,
 *    be clear of the title and the rule, and not touch its neighbour. The box itself
 *    must be on the screen, and the two columns must tile the block exactly -- no
 *    leftover pixel handed to one of them.
 *
 * 2. THE LATCH, which is the whole safety property. A press the box swallowed has been
 *    withheld from rbp for its whole life; rbp is going to be given its release. So the
 *    box must never close mid-press, and it must answer on the RELEASE and on the
 *    release only -- never at the down edge, where a finger that is still moving has
 *    not decided anything yet.
 *
 * 3. FIRE-ON-RELEASE, ANCHORED. A press that begins on USB 1 and slides to CANCEL
 *    answers NOTHING: the cell that fires is the cell the press STARTED on, and only if
 *    the finger is still on it when it lifts. That is the same rule menu_zone.c keeps
 *    for the band, and it is what makes a mis-tap recoverable.
 *
 * 4. THE ARMING, which is the newest rule here and the reason the box has four buttons
 *    instead of three. A tap on a device button ARMS it and the box STAYS UP; only a
 *    second tap, on OK, sends anything. OK is dead until a LIVE device is armed. That
 *    is two deliberate taps between the operator's finger and their production media.
 *
 * 5. THE DEAD CELLS. A cell with nothing to do is drawn in the palette's OFF pair and
 *    answers PR_ACT_NONE -- but it still CLOSES the box, and it still highlights while a
 *    finger is on it. A tap there must not send the eject anyway and let rbp's own press
 *    branch drop it at [this+0x88] != 2: from the outside that is a working button that
 *    does nothing, which is the failure this whole box exists to avoid.
 *
 * 6. THE TIMEOUT, and the one clause in it that is not about time. It closes the box
 *    with nothing touching it -- and it does NOT while a finger is down, because closing
 *    there would clear the latch and hand rbp a release with no press behind it
 *    ([[declined-press-must-still-see-release]]). Deferring costs one release.
 *
 * 7. THE IMAGE. Every pixel of the view is written, so a second paint of the same state
 *    leaves the same bytes -- the plane's buffer is single-buffered and a paint that is
 *    not idempotent is a paint that tears. A dead cell, a live cell, an armed cell and a
 *    pressed cell are four different pictures. And every character of the four labels is
 *    in the font's set: menu_font.h draws a GAP for one that is not, which would ship as
 *    a dead-looking label rather than as a failure.
 *
 * Build + run (static, so no rootfs is needed to load it):
 *     make test
 */
#define _GNU_SOURCE
#include "prompt_paint.h"
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
 * The harness. prompt_feed() is the module's one door, so every gesture test is a
 * sequence of reports fed exactly as pointsrc.c's funnel feeds them.
 *
 * The clock is a plain counter: nothing in prompt_zone.c reads a clock of its own, so
 * the tests own time completely and the timeout is arithmetic rather than a sleep.
 * ------------------------------------------------------------------------- */

static unsigned long long now_ms;

static struct prompt_state S;

/* Both devices present. The tests that care about a dead one say so. */
static void begin(unsigned long long t)
{
    prompt_reset();
    now_ms = t;
    S.live[0] = 1;
    S.live[1] = 1;
    prompt_open(now_ms);
}

/* A point comfortably inside a cell, so a boundary change does not turn a gesture test
 * into a geometry test. */
static int cx(int cell) { return PR_CELL_X0(cell) + 10; }
static int cy(int cell) { return PR_CELL_Y0(cell) + 10; }

/* One report. Returns the MZ_FEED_* code; `*act` the answer. */
static int feed(int down, int x, int y, int *act)
{
    return prompt_feed(&S, down, x, y, act);
}

/* A press and a release at the same point, with no movement: the tap the box is for.
 * Returns the answer -- which for a DEVICE cell is always PR_ACT_NONE, because that
 * tap arms rather than answers. */
static int tap(int x, int y)
{
    int act = PR_ACT_NONE;

    feed(1, x, y, &act);
    act = PR_ACT_NONE;
    feed(0, x, y, &act);
    return act;
}

static int tap_cell(int cell) { return tap(cx(cell), cy(cell)); }

/* ---------------------------------------------------------------------------
 * 1. The geometry
 * ------------------------------------------------------------------------- */

static void test_geometry(void)
{
    int x0, y0, x1, y1, c, r;

    /* PR_H is claimed in prompt_zone.h to be the sum of the parts. Check it against the
     * parts rather than against 199, so a change to a button height moves the line under
     * it instead of running the last line off the bottom of the box. */
    CHECK(PR_H == 2 * PR_BORDER + PR_TITLE_H + 1 + PR_PAD
                 + PROMPT_ROWS * PR_ROW_H + (PROMPT_ROWS - 1) * PR_ROW_GAP + PR_PAD,
          "PR_H is not the sum of the parts it is made of");

    /* The box is on the screen, and centred. */
    prompt_box_rect(&x0, &y0, &x1, &y1);
    CHECK(x0 == PR_X0 && y0 == PR_Y0 && x1 == PR_X1 && y1 == PR_Y1,
          "prompt_box_rect() disagrees with the PR_* literals");
    CHECK(x0 >= 0 && y0 >= 0 && x1 < MZ_LOGICAL_W && y1 < MZ_LOGICAL_H,
          "the box is off the %dx%d screen: %d,%d..%d,%d",
          MZ_LOGICAL_W, MZ_LOGICAL_H, x0, y0, x1, y1);
    {
        int dx = (x0 + x1) - (MZ_LOGICAL_W - 1), dy = (y0 + y1) - (MZ_LOGICAL_H - 1);

        CHECK(dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1,
              "the box is not centred: %d,%d..%d,%d", x0, y0, x1, y1);
    }

    /* TWO COLUMNS TILE THE BLOCK EXACTLY. The block is PR_ROW_X0..PR_ROW_X1 wide and
     * the two buttons and the gap between them have to fill it with nothing left over:
     * a leftover pixel handed to one column is a pair of buttons of different widths and
     * a label centred off its own frame. */
    {
        int span = PR_ROW_X1 - PR_ROW_X0 + 1;

        CHECK((span - PR_COL_GAP) % PROMPT_COLS == 0,
              "the %d px block leaves %d px over when split in two",
              span, (span - PR_COL_GAP) % PROMPT_COLS);
        CHECK(PR_CELL_X0(1) == PR_ROW_X0, "the left column does not start at the block");
        CHECK(PR_CELL_X1(PROMPT_COLS) == PR_ROW_X1,
              "the right column does not end at the block");
    }

    /* The last line's bottom is PR_PAD + PR_BORDER above the box's own bottom edge, which
     * is the whole point of the arithmetic above: nothing is clipped. */
    prompt_cell_rect(PR_CELL_CANCEL, &x0, &y0, &x1, &y1);
    CHECK(y1 == PR_Y1 - PR_BORDER - PR_PAD,
          "the last button ends %d px above the box, not %d",
          PR_Y1 - y1, PR_BORDER + PR_PAD);

    for (c = 1; c <= PROMPT_CELLS; c++) {
        int rx0, ry0, rx1, ry1, col = PR_CELL_COL(c);

        prompt_cell_rect(c, &rx0, &ry0, &rx1, &ry1);
        CHECK(rx1 - rx0 == PR_CELL_W - 1,
              "cell %d is %d px wide, not %d", c, rx1 - rx0 + 1, PR_CELL_W);
        CHECK(ry1 - ry0 == PR_ROW_H - 1,
              "cell %d is %d px tall, not %d", c, ry1 - ry0 + 1, PR_ROW_H);
        CHECK(ry0 > PR_RULE_Y, "cell %d starts at or above the rule", c);
        CHECK(rx0 > PR_X0 && rx1 < PR_X1, "cell %d is not inside the box", c);

        /* The cell to the left of this one, in the same line, is PR_COL_GAP away -- and
         * the gap itself belongs to neither. */
        if (col > 1) {
            int lx0, ly0, lx1, ly1;

            prompt_cell_rect(c - 1, &lx0, &ly0, &lx1, &ly1);
            CHECK(ry0 == ly0 && ry1 == ly1, "cell %d is on a different line to cell %d",
                  c, c - 1);
            CHECK(rx0 == lx1 + 1 + PR_COL_GAP,
                  "cell %d does not follow cell %d by PR_COL_GAP", c, c - 1);
        }
    }

    /* Same for the lines: every cell on line r follows the one above by PR_ROW_GAP. */
    for (r = 2; r <= PROMPT_ROWS; r++)
        for (c = 1; c <= PROMPT_COLS; c++) {
            int a = (r - 1) * PROMPT_COLS + c, b = (r - 2) * PROMPT_COLS + c;
            int ay0, ay1, by1, dummy;

            prompt_cell_rect(a, &dummy, &ay0, &dummy, &ay1);
            prompt_cell_rect(b, &dummy, &dummy, &dummy, &by1);
            CHECK(ay0 == by1 + 1 + PR_ROW_GAP,
                  "cell %d does not follow cell %d by PR_ROW_GAP", a, b);
        }

    /* prompt_cell_at() and prompt_cell_rect() are the SAME rect -- the ink and the hit
     * target are the same pixels -- so every cell's interior, and its four corners, must
     * hit, and one pixel outside it must miss. */
    for (c = 1; c <= PROMPT_CELLS; c++) {
        int rx0, ry0, rx1, ry1, mx, my;

        prompt_cell_rect(c, &rx0, &ry0, &rx1, &ry1);
        for (mx = rx0; mx <= rx1; mx += rx1 - rx0)
            for (my = ry0; my <= ry1; my += ry1 - ry0)
                CHECK(prompt_cell_at(mx, my) == c,
                      "cell %d's corner (%d,%d) does not hit it", c, mx, my);
        CHECK(prompt_cell_at(rx0 + (rx1 - rx0) / 2, ry0) == c,
              "the top middle of cell %d does not hit it", c);
        CHECK(prompt_cell_at(rx0, ry1 + 1) == 0,
              "one px below cell %d still hits something", c);
        CHECK(prompt_cell_at(rx0 - 1, (ry0 + ry1) / 2) == 0,
              "one px left of cell %d still hits something", c);
    }

    /* THE GAP BETWEEN THE COLUMNS IS NOT A BUTTON. A press there is a press the box
     * swallowed and answered nothing for -- the same rule the title and the margin keep,
     * and the reason "tap anywhere else" dismisses with no second rule. */
    CHECK(prompt_cell_at(PR_CELL_X1(1) + PR_COL_GAP / 2, cy(PR_CELL_USB1)) == 0,
          "the gap between the two devices is a button");
    CHECK(prompt_cell_at(PR_CELL_X1(PR_CELL_OK) + PR_COL_GAP / 2, cy(PR_CELL_OK)) == 0,
          "the gap between OK and CANCEL is a button");

    /* The title, the rule and the margin are NOT cells either. */
    CHECK(prompt_cell_at(PR_X0 + 1, PR_TITLE_Y0 + 1) == 0, "the title is a button");
    CHECK(prompt_cell_at(PR_X0 + 1, PR_RULE_Y) == 0, "the rule is a button");
    CHECK(prompt_cell_at(PR_X0 + 1, PR_Y1 - 1) == 0, "the margin is a button");
    CHECK(prompt_cell_at(0, 0) == 0, "the top-left of the screen is a button");
    CHECK(prompt_cell_at(MZ_LOGICAL_W - 1, MZ_LOGICAL_H - 1) == 0,
          "the bottom-right of the screen is a button");
    CHECK(prompt_cell_at(-1, 400) == 0, "a negative x is a button");
    CHECK(prompt_cell_at(PR_ROW_X0 - 1, cy(PR_CELL_USB1)) == 0,
          "a press left of the block, inside the pad, is a button");
    CHECK(prompt_cell_at(PR_ROW_X1 + 1, cy(PR_CELL_USB2)) == 0,
          "a press right of the block, inside the pad, is a button");

    /* Out-of-range cells answer 0 rather than reading past the labels. */
    CHECK(prompt_cell_label(0) == 0 && prompt_cell_label(PROMPT_CELLS + 1) == 0,
          "an out-of-range cell gave a label");
    CHECK(prompt_cell_live(&S, 0) == 0 && prompt_cell_live(&S, PROMPT_CELLS + 1) == 0,
          "an out-of-range cell answered live");
    prompt_cell_rect(0, &x0, &y0, &x1, &y1);
    CHECK(x0 == 0 && y0 == 0 && x1 == -1 && y1 == -1,
          "an out-of-range cell gave a rect");

    /* And the strings are the ones the operator asked for, in the seats they asked for
     * them in: the devices on one line, the answers on the next. */
    CHECK(!strcmp(prompt_title(), "USB STOP"), "the title is '%s'", prompt_title());
    CHECK(!strcmp(prompt_cell_label(PR_CELL_USB1), "USB 1"), "cell 1 is '%s'",
          prompt_cell_label(PR_CELL_USB1));
    CHECK(!strcmp(prompt_cell_label(PR_CELL_USB2), "USB 2"), "cell 2 is '%s'",
          prompt_cell_label(PR_CELL_USB2));
    CHECK(!strcmp(prompt_cell_label(PR_CELL_OK), "OK"), "cell 3 is '%s'",
          prompt_cell_label(PR_CELL_OK));
    CHECK(!strcmp(prompt_cell_label(PR_CELL_CANCEL), "CANCEL"), "cell 4 is '%s'",
          prompt_cell_label(PR_CELL_CANCEL));
    CHECK(PR_CELL_ROW(PR_CELL_USB1) == PR_CELL_ROW(PR_CELL_USB2),
          "the two devices are not on one line");
    CHECK(PR_CELL_ROW(PR_CELL_OK) == PR_CELL_ROW(PR_CELL_CANCEL),
          "OK and CANCEL are not on one line");
    CHECK(PR_CELL_ROW(PR_CELL_OK) == PR_CELL_ROW(PR_CELL_USB1) + 1,
          "the answers are not on the line under the devices");

    /* EVERY CHARACTER OF EVERY LABEL IS IN THE FONT. menu_font.h draws a GAP for one that
     * is not, so an unsupported character ships as a dead-looking label rather than as a
     * failure -- which is exactly the kind of thing a test has to catch instead. */
    for (c = 1; c <= PROMPT_CELLS; c++) {
        const char *s = prompt_cell_label(c);

        for (; *s; s++)
            CHECK(menu_font_index((unsigned char)*s) >= 0,
                  "'%c' in label %d is not in the font", *s, c);
    }
    for (c = 0; prompt_title()[c]; c++)
        CHECK(menu_font_index((unsigned char)prompt_title()[c]) >= 0,
              "'%c' in the title is not in the font", prompt_title()[c]);
}

/* ---------------------------------------------------------------------------
 * 2 + 3 + 4 + 5. The gesture
 * ------------------------------------------------------------------------- */

static void test_closed(void)
{
    int act = PR_ACT_NONE, i;

    prompt_reset();
    CHECK(!prompt_is_open(), "a reset box is still open");
    CHECK(prompt_pressed() == 0, "a reset box has a pressed cell");
    CHECK(prompt_selected() == 0, "a reset box has something armed");

    /* Shut, the module answers for NOTHING -- not for a press, not for a release, not
     * for a release with no press behind it. rbp's stream is untouched by construction
     * and not by a rectangle test. */
    for (i = 0; i < 2; i++) {
        CHECK(feed(i, cx(PR_CELL_USB1), cy(PR_CELL_USB1), &act) == MZ_FEED_NONE,
              "a shut box took a report");
        CHECK(act == PR_ACT_NONE, "a shut box answered something");
    }
    CHECK(feed(0, 0, 0, &act) == MZ_FEED_NONE, "a shut box took a stray release");

    /* And close() on a shut box is harmless, which the tick relies on. */
    prompt_close();
    CHECK(!prompt_is_open(), "close() on a shut box opened it");
}

static void test_swallow(void)
{
    int act = PR_ACT_NONE;

    begin(1000);

    /* WHILE THE BOX IS UP IT OWNS EVERY REPORT, wherever it lands. A press that
     * dismisses must not also press whatever is underneath it -- the performance screen
     * is under here. */
    CHECK(feed(1, 0, 0, &act) == MZ_FEED_TAKEN, "a press off the box was not taken");
    CHECK(feed(1, MZ_LOGICAL_W - 1, MZ_LOGICAL_H - 1, &act) == MZ_FEED_TAKEN,
          "a press in the far corner was not taken");
    CHECK(act == PR_ACT_NONE, "the down edge answered something");

    /* The down edge never answers, and a press that is not on a cell highlights nothing:
     * a finger that is still moving has not decided. */
    CHECK(prompt_pressed() == 0, "a press off the buttons highlighted one");

    CHECK(feed(1, PR_CELL_X0(PR_CELL_USB1), PR_CELL_Y0(PR_CELL_USB1), &act)
              == MZ_FEED_TAKEN,
          "a press on a button was not taken");

    /* And every report of the same press is taken too. */
    CHECK(feed(1, cx(PR_CELL_CANCEL), cy(PR_CELL_CANCEL), &act) == MZ_FEED_TAKEN,
          "a move was not taken");
    CHECK(feed(0, cx(PR_CELL_CANCEL), cy(PR_CELL_CANCEL), &act) == MZ_FEED_TAKEN,
          "the release was not taken");

    /* A stray release -- one rbp never saw the press for -- is still ours while the box
     * is up, because rbp must not hear either edge of anything. It does NOT close the
     * box, though: nothing was pressed into it, so nothing has been dismissed. */
    begin(1000);
    CHECK(feed(0, 500, 400, &act) == MZ_FEED_TAKEN, "a stray release was not taken");
    CHECK(prompt_is_open(), "a stray release closed the box");
}

/* THE ARMING, which is what the fourth button buys. A device button takes the box
 * nowhere; OK is the only thing that sends, and it cannot send until a live device has
 * been chosen. */
static void test_arming(void)
{
    int act;

    /* A DEVICE TAP ARMS AND STAYS UP. This is the one release in the module that does not
     * close the box, so it is worth pinning hard. */
    begin(1000);
    act = PR_ACT_NONE;
    feed(1, cx(PR_CELL_USB1), cy(PR_CELL_USB1), &act);
    act = PR_ACT_NONE;
    feed(0, cx(PR_CELL_USB1), cy(PR_CELL_USB1), &act);
    CHECK(act == PR_ACT_NONE, "tapping a device answered %d", act);
    CHECK(prompt_is_open(), "tapping a device closed the box");
    CHECK(prompt_selected() == PR_CELL_USB1, "USB 1 was not armed (armed %d)",
          prompt_selected());

    /* AND OK NOW SENDS, on the armed device's own channel. */
    act = tap_cell(PR_CELL_OK);
    CHECK(act == PR_ACT_USB1, "OK with USB 1 armed answered %d", act);
    CHECK(prompt_act_channel(act) == 1, "USB 1's channel is not 1");
    CHECK(!prompt_is_open(), "the box stayed up after OK");

    /* The other device: same two taps, the other channel. The channel is the DEVICE'S
     * OWN, which is what gets the key past rbp's per-manager filter at 0x325a2c.
     * CH_GLOBAL is 1, which is why the old single-column binding could only ever have
     * stopped USB 1. */
    begin(1000);
    tap_cell(PR_CELL_USB2);
    act = tap_cell(PR_CELL_OK);
    CHECK(act == PR_ACT_USB2, "OK with USB 2 armed answered %d", act);
    CHECK(prompt_act_channel(act) == 2, "USB 2's channel is not 2");

    /* ARMING REPLACES ARMING: the last device touched is the one OK stops. The operator
     * changes their mind by tapping the other device, and nothing has been sent yet. */
    begin(1000);
    tap_cell(PR_CELL_USB1);
    tap_cell(PR_CELL_USB2);
    CHECK(prompt_selected() == PR_CELL_USB2, "USB 2 did not replace USB 1");
    CHECK(tap_cell(PR_CELL_OK) == PR_ACT_USB2, "OK stopped the wrong device");

    /* OK IS DEAD UNTIL SOMETHING IS ARMED. A freshly opened box arms nothing, so the
     * only way to reach an eject is through a device -- and a tap on the dead OK closes
     * the box and sends nothing, exactly as a tap on a dead device does. */
    begin(1000);
    CHECK(!prompt_cell_live(&S, PR_CELL_OK), "OK was live with nothing armed");
    CHECK(prompt_selected() == 0, "a freshly opened box had something armed");
    CHECK(tap_cell(PR_CELL_OK) == PR_ACT_NONE, "OK sent with nothing armed");
    CHECK(!prompt_is_open(), "a dead OK left the box up");

    /* THE ARMING IS CLEARED WITH THE BOX. A box that comes back must not still be
     * holding the device the operator chose last time: the next OK would stop something
     * they had stopped thinking about. */
    begin(1000);
    tap_cell(PR_CELL_USB1);
    CHECK(prompt_selected() == PR_CELL_USB1, "USB 1 was not armed");
    prompt_close();
    prompt_open(now_ms + 1000);
    CHECK(prompt_selected() == 0, "a re-opened box inherited the arming");
    CHECK(!prompt_cell_live(&S, PR_CELL_OK), "a re-opened box had a live OK");

    /* CANCEL answers CANCEL, which pointsrc.c maps to channel 0 -- nothing is sent. It
     * is always live and it never needs an arming. */
    begin(1000);
    CHECK(prompt_cell_live(&S, PR_CELL_CANCEL), "CANCEL is not live");
    act = tap_cell(PR_CELL_CANCEL);
    CHECK(act == PR_ACT_CANCEL, "tapping CANCEL answered %d", act);
    CHECK(prompt_act_channel(act) == 0, "CANCEL has a channel");

    /* CANCEL THROWS THE ARMING AWAY with the box, which is the whole of what it is for:
     * a box re-raised a moment later must not still be holding a device. */
    begin(1000);
    tap_cell(PR_CELL_USB1);
    tap_cell(PR_CELL_CANCEL);
    prompt_open(now_ms + 1000);
    CHECK(prompt_selected() == 0, "CANCEL left a device armed for the next box");
}

/* 3. Fire-on-release, anchored -- on the OK button, which is the one that fires. */
static void test_fire(void)
{
    int act;

    /* A press that begins OFF a cell -- the title, the gap, the glass -- answers nothing
     * and still closes. There is no second rule for "tap outside to dismiss". */
    begin(1000);
    act = tap(PR_X0 + 4, PR_TITLE_Y0 + 4);
    CHECK(act == PR_ACT_NONE, "a tap on the title answered %d", act);
    CHECK(!prompt_is_open(), "a tap on the title left the box up");
    CHECK(prompt_selected() == 0, "a tap on the title armed something");

    begin(1000);
    act = tap(10, 10);
    CHECK(act == PR_ACT_NONE, "a tap outside the box answered %d", act);
    CHECK(!prompt_is_open(), "a tap outside the box left the box up");

    /* A press in the gap between two buttons is a press that missed, so it dismisses and
     * arms nothing -- which is the reason the gap is worth having. */
    begin(1000);
    act = tap(PR_CELL_X1(PR_CELL_USB1) + PR_COL_GAP / 2, cy(PR_CELL_USB1));
    CHECK(act == PR_ACT_NONE, "a tap in the gap answered %d", act);
    CHECK(!prompt_is_open(), "a tap in the gap left the box up");
    CHECK(prompt_selected() == 0, "a tap in the gap armed something");

    /* SLIDE OFF CANCELS. The cell that fires is the cell the press STARTED on, and only
     * if the finger is still on it when it lifts -- on OK, this time, because OK is the
     * one that sends. */
    begin(1000);
    tap_cell(PR_CELL_USB1);
    feed(1, cx(PR_CELL_OK), cy(PR_CELL_OK), &act);
    CHECK(prompt_pressed() == PR_CELL_OK, "the finger did not highlight OK");
    feed(1, cx(PR_CELL_CANCEL), cy(PR_CELL_CANCEL), &act);
    CHECK(prompt_pressed() == PR_CELL_CANCEL, "the finger did not follow to CANCEL");
    act = PR_ACT_NONE;
    feed(0, cx(PR_CELL_CANCEL), cy(PR_CELL_CANCEL), &act);
    CHECK(act == PR_ACT_NONE, "a slide from OK to CANCEL answered %d", act);
    CHECK(!prompt_is_open(), "a slide off left the box up");

    /* THE MIRROR IS THE DANGEROUS ONE NOW, and it is the mirror in the other direction
     * too: press on CANCEL, slide ONTO OK, lift. The anchor is still CANCEL, so nothing
     * is stopped -- a finger that wanders onto OK cannot fire it by arriving. */
    begin(1000);
    tap_cell(PR_CELL_USB1);
    feed(1, cx(PR_CELL_CANCEL), cy(PR_CELL_CANCEL), &act);
    feed(1, cx(PR_CELL_OK), cy(PR_CELL_OK), &act);
    act = PR_ACT_NONE;
    feed(0, cx(PR_CELL_OK), cy(PR_CELL_OK), &act);
    CHECK(act == PR_ACT_NONE, "sliding onto OK fired it (%d)", act);
    CHECK(!prompt_is_open(), "a slide onto OK left the box up");

    /* AND THE DEVICE BUTTONS KEEP THE SAME ANCHOR RULE: a press that begins on USB 1 and
     * slides onto USB 2 arms NOTHING, because the cell that arms is the cell the press
     * started on and the finger is not on it any more. */
    begin(1000);
    feed(1, cx(PR_CELL_USB1), cy(PR_CELL_USB1), &act);
    feed(1, cx(PR_CELL_USB2), cy(PR_CELL_USB2), &act);
    act = PR_ACT_NONE;
    feed(0, cx(PR_CELL_USB2), cy(PR_CELL_USB2), &act);
    CHECK(prompt_selected() == 0, "a slide from USB 1 to USB 2 armed something");
    CHECK(!prompt_is_open(), "a slide off a device left the box up");

    /* A press that leaves the box and comes back to the cell it started on has not moved
     * as far as this module is concerned: cur_cell is recomputed per report, so the
     * anchor and the live cell agree again. That is menu_zone.c's rule and it is stated
     * here so a change to it is a change to a pinned number. */
    begin(1000);
    feed(1, cx(PR_CELL_USB2), cy(PR_CELL_USB2), &act);
    feed(1, 10, 10, &act);
    CHECK(prompt_pressed() == 0, "a finger off the box still highlighted a cell");
    feed(1, cx(PR_CELL_USB2), cy(PR_CELL_USB2), &act);
    act = PR_ACT_NONE;
    feed(0, cx(PR_CELL_USB2), cy(PR_CELL_USB2), &act);
    CHECK(prompt_selected() == PR_CELL_USB2,
          "a press that wandered off and back armed nothing");
}

static void test_dead_cells(void)
{
    int act;

    /* rbp says USB 1 is not there. The cell is drawn off, it answers NOTHING -- and the
     * box closes anyway, which is the whole of what a tap on a dead cell does. It must
     * never send the eject and let rbp's own press branch drop it at [this+0x88] != 2:
     * from the outside that is a working button that does nothing. */
    begin(1000);
    S.live[0] = 0;
    CHECK(!prompt_cell_live(&S, PR_CELL_USB1), "an absent device reported live");
    CHECK(prompt_cell_live(&S, PR_CELL_USB2), "USB 2 was dragged down with USB 1");
    CHECK(prompt_cell_live(&S, PR_CELL_CANCEL), "CANCEL is not always live");

    act = tap_cell(PR_CELL_USB1);
    CHECK(act == PR_ACT_NONE, "a dead cell answered %d", act);
    CHECK(prompt_act_channel(act) == 0, "a dead cell named a channel");
    CHECK(!prompt_is_open(), "a dead cell left the box up");
    CHECK(prompt_selected() == 0, "a DEAD device was armed");

    /* The other device is unaffected. */
    begin(1000);
    S.live[0] = 0;
    tap_cell(PR_CELL_USB2);
    CHECK(prompt_selected() == PR_CELL_USB2, "the live device was refused with the dead one");
    CHECK(tap_cell(PR_CELL_OK) == PR_ACT_USB2, "OK was refused with a live device armed");

    /* A DEVICE THAT GOES AWAY UNDER AN ARMED OK DEADENS THE OK. rbp is asked on every
     * report (pointsrc.c), so the arming is checked against the same answer the picture
     * is drawn from: OK cannot send for a stick that has gone. */
    begin(1000);
    tap_cell(PR_CELL_USB1);
    CHECK(prompt_cell_live(&S, PR_CELL_OK), "OK was dead with a live device armed");
    S.live[0] = 0;
    CHECK(!prompt_cell_live(&S, PR_CELL_OK), "OK was live for a device that had gone");
    CHECK(tap_cell(PR_CELL_OK) == PR_ACT_NONE, "OK stopped a device that had gone");

    /* THE PORT'S OWN CASE: usb-watch.sh feeds only usb1, so USB 2 is always dead here.
     * Both devices dead is the state the operator will actually see, and CANCEL is still
     * the way out -- a box with nothing to offer must still be dismissible. */
    begin(1000);
    S.live[0] = 0;
    S.live[1] = 0;
    CHECK(!prompt_cell_live(&S, PR_CELL_USB1), "USB 1 reported live with no media");
    CHECK(!prompt_cell_live(&S, PR_CELL_USB2), "USB 2 reported live with no media");
    CHECK(!prompt_cell_live(&S, PR_CELL_OK), "OK reported live with both devices dead");
    CHECK(prompt_cell_live(&S, PR_CELL_CANCEL), "CANCEL died with both devices");
    CHECK(tap_cell(PR_CELL_CANCEL) == PR_ACT_CANCEL,
          "CANCEL was refused with both devices dead");

    /* A caller that cannot read rbp leaves both 0. That is a refusal and not an
     * assumption: both device cells dim, both send nothing, and OK has nothing to do. */
    begin(1000);
    S.live[0] = 0;
    S.live[1] = 0;
    CHECK(tap_cell(PR_CELL_USB1) == PR_ACT_NONE, "an unreadable device answered USB 1");

    /* A DEAD CELL STILL HIGHLIGHTS. A finger that gets no feedback at all reads as a dead
     * panel rather than as a refused button, so prompt_pressed() and the live/dead
     * distinction are independent -- prompt_paint.h draws the pressed pair for either. */
    begin(1000);
    S.live[0] = 0;
    feed(1, cx(PR_CELL_USB1), cy(PR_CELL_USB1), &act);
    CHECK(prompt_pressed() == PR_CELL_USB1, "a dead cell did not highlight");
}

static void test_timeout(void)
{
    int act = PR_ACT_NONE;

    begin(1000);

    /* Not yet: the clock is a deadline and not a countdown from the last report. */
    CHECK(prompt_expire(now_ms + PR_TIMEOUT_MS - 1) == 0, "the box expired a tick early");
    CHECK(prompt_is_open(), "the box closed a tick early");
    CHECK(prompt_deadline() == now_ms + PR_TIMEOUT_MS,
          "the deadline is not the open time plus the timeout");

    /* Exactly on the deadline it goes. */
    CHECK(prompt_expire(now_ms + PR_TIMEOUT_MS) == 1, "the box outlived its deadline");
    CHECK(!prompt_is_open(), "the box is still open after expiring");

    /* And it reports the transition ONCE, so a caller can log it once. */
    CHECK(prompt_expire(now_ms + PR_TIMEOUT_MS + 5000) == 0,
          "an expired box reported a second expiry");

    /* NOT WHILE A FINGER IS DOWN. A press the box swallowed has been withheld from rbp
     * for its whole life and rbp is going to be given its release; closing here would
     * clear the latch and hand that release over with no press behind it. */
    begin(1000);
    feed(1, cx(PR_CELL_USB1), cy(PR_CELL_USB1), &act);
    CHECK(prompt_expire(now_ms + PR_TIMEOUT_MS * 10) == 0,
          "the box expired under a finger");
    CHECK(prompt_is_open(), "the box closed under a finger");

    /* And the release then takes its OWN path, a moment later -- the deferral costs one
     * release and nothing else. On a device cell that path is the arming, so the box is
     * still up and the device is armed: the deferral must not turn the tap into an
     * eject. */
    feed(0, cx(PR_CELL_USB1), cy(PR_CELL_USB1), &act);
    CHECK(act == PR_ACT_NONE, "the deferred release answered %d", act);
    CHECK(prompt_is_open(), "the deferred release closed the box");
    CHECK(prompt_selected() == PR_CELL_USB1, "the deferred release did not arm");

    /* A deferred press on OK, though, is still an OK: the deferral is about WHEN the
     * release is judged, not about what it means. */
    begin(1000);
    tap_cell(PR_CELL_USB1);
    feed(1, cx(PR_CELL_OK), cy(PR_CELL_OK), &act);
    prompt_expire(now_ms + PR_TIMEOUT_MS * 10);
    act = PR_ACT_NONE;
    feed(0, cx(PR_CELL_OK), cy(PR_CELL_OK), &act);
    CHECK(act == PR_ACT_USB1, "a deferred OK answered %d", act);

    /* A press that has been deferred does not answer anything if it slides off, either:
     * the deferral must not turn a wandering finger into an answer. */
    begin(1000);
    feed(1, cx(PR_CELL_USB2), cy(PR_CELL_USB2), &act);
    prompt_expire(now_ms + PR_TIMEOUT_MS * 10);
    act = PR_ACT_NONE;
    feed(0, 10, 10, &act);
    CHECK(act == PR_ACT_NONE, "a deferred press that slid off answered %d", act);

    /* Re-raising restarts the clock, which is what makes the column idempotent: tapping
     * it twice is a longer look, not a broken box. */
    begin(1000);
    prompt_open(now_ms + 5000);
    CHECK(prompt_deadline() == now_ms + 5000 + PR_TIMEOUT_MS,
          "re-raising did not restart the clock");
    /* And it clears any press in flight: the tap that raised it is already over, and a
     * box that inherited a finger it never saw go down would answer on that finger's
     * next release. */
    CHECK(prompt_pressed() == 0, "a re-raised box inherited a press");
    CHECK(feed(0, cx(PR_CELL_USB1), cy(PR_CELL_USB1), &act) == MZ_FEED_TAKEN,
          "a re-raised box did not take a stray release");
    CHECK(prompt_is_open(), "a stray release closed a re-raised box");
}

static void test_reset(void)
{
    int act = PR_ACT_NONE;

    /* prompt_reset() is what pointsrc.c calls when the pointer device goes away. A press
     * left anchored, a device left armed, or a box left up would survive a replug. */
    begin(1000);
    tap_cell(PR_CELL_USB1);
    feed(1, cx(PR_CELL_OK), cy(PR_CELL_OK), &act);
    prompt_reset();
    CHECK(!prompt_is_open(), "the box survived a reset");
    CHECK(prompt_pressed() == 0, "a press survived a reset");
    CHECK(prompt_selected() == 0, "an arming survived a reset");
    CHECK(prompt_deadline() == 0, "a deadline survived a reset");
    CHECK(feed(0, cx(PR_CELL_OK), cy(PR_CELL_OK), &act) == MZ_FEED_NONE,
          "a reset box took a report");

    /* A box re-raised after a reset does not inherit the old press either. */
    prompt_open(2000);
    act = PR_ACT_NONE;
    feed(0, cx(PR_CELL_USB1), cy(PR_CELL_USB1), &act);
    CHECK(act == PR_ACT_NONE, "a press from before the reset answered after it");
}

/* ---------------------------------------------------------------------------
 * 7. The image
 *
 * Local arrays, at the box's own size: no framebuffer, no plane, no device.
 * ------------------------------------------------------------------------- */

#define IW 560
#define IH 199
#define BOX (IW * IH * 2)

static unsigned char buf_a[BOX], buf_b[BOX], buf_c[BOX];

static void view_for(struct menu_view *v, void *pix, int dw, int dh, int bpp)
{
    memset(v, 0, sizeof *v);
    v->pix = pix;
    v->pitch = dw;
    v->fb_w = dw;
    v->fb_h = dh;
    v->bpp = bpp;
    v->bx = 0;
    v->by = 0;
    v->dw = dw;
    v->dh = dh;
}

static void check_ok_refusals(void);

static void test_paint_size(void)
{
    /* The size helpers scale with the page, and never answer zero -- a plane of zero
     * pixels is a plane that cannot be set up. */
    CHECK(prompt_paint_w(MZ_LOGICAL_W) == PR_W, "the box is not PR_W wide at 1:1");
    CHECK(prompt_paint_h(MZ_LOGICAL_H) == PR_H, "the box is not PR_H tall at 1:1");
    CHECK(prompt_paint_w(0) == 1 && prompt_paint_h(0) == 1,
          "a zero-size page gave a zero-size box");
    CHECK(prompt_paint_w(MZ_LOGICAL_W * 2) == PR_W * 2, "the width does not scale");
    CHECK(prompt_paint_h(MZ_LOGICAL_H * 2) == PR_H * 2, "the height does not scale");

    check_ok_refusals();
}

static void check_ok_refusals(void)
{
    struct menu_view v;

    view_for(&v, buf_a, IW, IH, 16);
    CHECK(prompt_paint_ok(&v), "a box-sized 16 bpp view was refused");

    CHECK(!prompt_paint_ok(0), "a null view was accepted");

    view_for(&v, 0, IW, IH, 16);
    CHECK(!prompt_paint_ok(&v), "a null buffer was accepted");

    view_for(&v, buf_a, IW, IH, 24);
    CHECK(!prompt_paint_ok(&v), "a 24 bpp view was accepted");

    view_for(&v, buf_a, IW, IH, 16);
    v.pitch = IW - 1;
    CHECK(!prompt_paint_ok(&v), "a pitch short of the width was accepted");

    view_for(&v, buf_a, IW, IH, 16);
    v.pix = 0;
    CHECK(!prompt_paint_ok(&v), "a null buffer was accepted a second time");

    /* A line too short for the font: PR_ROW_H logical px at a scale where the line box
     * no longer fits. The threshold is the font's, not a magic number. */
    view_for(&v, buf_a, 64, 32, 16);
    CHECK(!prompt_paint_ok(&v),
          "a view too small to host the %d px line box was accepted", MENU_FONT_LINE);

    /* AND THE WIDTH IS ITS OWN REFUSAL, which is new with the two-column grid: a box
     * half the width it was has a button half the width it had, so a page that scales
     * the box down can reach a scale where a line still fits but a LABEL no longer does.
     * The pair below isolates it -- the same height, two widths, opposite answers -- so
     * a change that drops the width check fails here rather than on the glass. */
    view_for(&v, buf_a, IW, 81, 16);
    CHECK(prompt_paint_ok(&v), "a view whose lines fit but is full width was refused");
    view_for(&v, buf_a, 40, 81, 16);
    CHECK(!prompt_paint_ok(&v),
          "a view too narrow to hold a label whole was accepted");
}

static void test_paint_image(void)
{
    struct menu_view v;
    int i, j;

    /* Both devices live, nothing pressed, nothing armed. */
    S.live[0] = 1;
    S.live[1] = 1;

    view_for(&v, buf_a, IW, IH, 16);
    prompt_paint(&v, &S, 0, 0);
    /* PAINTING IT TWICE PAINTS IT ONCE. The plane's buffer is single-buffered, so a
     * paint that is not idempotent is a paint that tears. */
    prompt_paint(&v, &S, 0, 0);
    view_for(&v, buf_b, IW, IH, 16);
    prompt_paint(&v, &S, 0, 0);
    CHECK(!memcmp(buf_a, buf_b, BOX), "painting the same state twice gave two pictures");

    /* A DEAD CELL IS A DIFFERENT PICTURE. USB 1 dim. */
    S.live[0] = 0;
    view_for(&v, buf_c, IW, IH, 16);
    prompt_paint(&v, &S, 0, 0);
    CHECK(memcmp(buf_b, buf_c, BOX), "a dead button drew the same as a live one");

    /* And the dead cell is confined to the dead cell: the rest of the box is untouched by
     * one device going away, so the difference lies inside that one rect and nowhere
     * else. This is the property the two-column grid puts most at risk -- a column
     * arithmetic that bleeds into its neighbour shows up here and nowhere else. */
    {
        int rx0, ry0, rx1, ry1, k, outside = 0;

        /* The view's picture rect IS the box (prompt_paint.h), so a logical box point
         * lands at fb (lx - PR_X0, ly - PR_Y0) when bx/by are 0 and the scale is 1:1. */
        prompt_cell_rect(PR_CELL_USB1, &rx0, &ry0, &rx1, &ry1);
        rx0 -= PR_X0; rx1 -= PR_X0; ry0 -= PR_Y0; ry1 -= PR_Y0;
        for (j = 0; j < IH; j++)
            for (k = 0; k < IW; k++) {
                size_t off = (size_t)(j * IW + k) * 2;

                if (!memcmp(&buf_b[off], &buf_c[off], 2))
                    continue;
                if (k < rx0 || k > rx1 || j < ry0 || j > ry1)
                    outside++;
            }
        CHECK(outside == 0, "%d px changed outside USB 1 when it went away", outside);
    }

    /* AN ARMED CELL IS A DIFFERENT PICTURE, and it is the picture that carries the whole
     * of the new gesture's feedback: without it the operator cannot see which stick OK
     * would stop. It is also confined to its own cell. */
    {
        unsigned char buf_d[BOX];

        S.live[0] = 1;
        view_for(&v, buf_d, IW, IH, 16);
        prompt_paint(&v, &S, 0, PR_CELL_USB1);
        CHECK(memcmp(buf_b, buf_d, BOX),
              "an armed USB 1 drew the same as an unarmed one");
    }

    /* A PRESSED CELL IS A DIFFERENT PICTURE TOO, and it is a different picture from the
     * dead one -- the pressed pair wins over the off pair, deliberately. */
    {
        unsigned char buf_e[BOX];

        S.live[0] = 0;
        view_for(&v, buf_e, IW, IH, 16);
        prompt_paint(&v, &S, PR_CELL_USB1, 0);
        CHECK(memcmp(buf_c, buf_e, BOX),
              "a pressed dead button drew the same as an unpressed one");
    }

    /* Every pixel is written: the box's own bed reaches the corners, so a fresh
     * uninitialised plane shown before the first build cannot leak through. Fill the
     * buffer with a value that is not the bed and check that not one byte of it
     * survives. */
    {
        unsigned char buf_f[BOX];
        int survived = 0;

        memset(buf_f, 0xa5, sizeof buf_f);
        view_for(&v, buf_f, IW, IH, 16);
        prompt_paint(&v, &S, 0, 0);
        for (j = 0; j < BOX; j += 2)
            if (buf_f[j] == 0xa5 && buf_f[j + 1] == 0xa5)
                survived++;
        CHECK(survived == 0, "%d px of the previous contents survived a paint", survived);
    }

    /* The same box at 32 bpp: the depth is the view's, and a 32 bpp paint is a different
     * picture byte for byte from a 16 bpp one. */
    {
        static unsigned char buf32[IW * IH * 4];
        unsigned char buf32b[IW * IH * 4];

        view_for(&v, buf32, IW, IH, 32);
        prompt_paint(&v, &S, 0, 0);
        view_for(&v, buf32b, IW, IH, 32);
        prompt_paint(&v, &S, 0, 0);
        CHECK(!memcmp(buf32, buf32b, sizeof buf32),
              "the 32 bpp paint is not idempotent");
    }

    /* A refused view is left alone rather than half-drawn. */
    {
        static unsigned char tiny[8 * 8 * 2];
        int untouched = 1;

        memset(tiny, 0x5a, sizeof tiny);
        view_for(&v, tiny, 8, 8, 16);
        prompt_paint(&v, &S, 0, 0);
        for (i = 0; i < (int)sizeof tiny; i++)
            if (tiny[i] != 0x5a)
                untouched = 0;
        CHECK(untouched, "a view prompt_paint_ok() refuses was drawn into anyway");
    }

    /* An out-of-range pressed or armed cell is clamped rather than read off the end of
     * the label table. */
    view_for(&v, buf_a, IW, IH, 16);
    prompt_paint(&v, &S, 99, 99);
    prompt_paint(&v, &S, -3, -3);
    prompt_paint(&v, &S, 0, 0);
    view_for(&v, buf_b, IW, IH, 16);
    prompt_paint(&v, &S, 0, 0);
    CHECK(!memcmp(buf_a, buf_b, BOX), "an out-of-range cell was not clamped");
}

int main(void)
{
    test_geometry();
    test_closed();
    test_swallow();
    test_arming();
    test_fire();
    test_dead_cells();
    test_timeout();
    test_reset();
    test_paint_size();
    test_paint_image();

    printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "ok", checks, failures);
    return failures ? 1 : 0;
}
