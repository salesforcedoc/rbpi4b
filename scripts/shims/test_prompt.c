/*
 * test_prompt.c -- the USB STOP chooser, with no Pi, no panel, no plane and no rbp
 * anywhere in this file.
 *
 * It links the PRODUCTION prompt_zone.c and prompt_paint.c, for the reason test_menu.c,
 * test_side.c, test_util.c and test_wave.c link theirs: a cell boundary, a dim-cell
 * rule, the hold and a timeout guard are exactly the kind of thing that gets quietly
 * rewritten in a copy.
 *
 * prompt_zone.c cannot see rbp. What rbp would tell it -- whether each of the two
 * devices has media in it -- arrives in a struct prompt_state the tests hand it, and
 * what it wants sent comes back as a PR_ACT_* code. That is what makes the things with
 * real consequences testable without a unit:
 *
 * 1. THE GEOMETRY, as arithmetic and as pinned numbers. PR_H must be the sum of the
 *    parts it claims to be made of, or the row runs off the bottom of the box; every
 *    button rect must lie inside the box, be one PR_ROW_H tall and PR_CELL_W wide, be
 *    clear of the title and the rule, and not touch its neighbour. The box itself must
 *    be on the screen, and the two columns must tile the block exactly -- no leftover
 *    pixel handed to one of them.
 *
 * 2. THE LATCH, which is the whole safety property. A press the box swallowed has been
 *    withheld from rbp for its whole life; rbp is going to be given its release. So the
 *    box must never close under a finger, and it must never answer on the DOWN edge --
 *    where a finger that is still moving has not decided anything yet. The one place the
 *    two rules meet is the eject: it fires under a finger and the box stays up for the
 *    release anyway, so that the release is still the box's to take.
 *
 * 3. THE HOLD, which is the whole of the gesture since 2026-10-07. A press on a LIVE
 *    button starts a three-second hold; nothing is sent before PR_HOLD_MS and the eject
 *    goes out at it; a release before it sends nothing at all and leaves the box up. That
 *    is the operator's own ask, and the three seconds are a property of the CLOCK and not
 *    of the caller's slicing -- so they are pinned both through prompt_tick() and through
 *    a release that arrives with no tick in between.
 *
 * 4. THE ANCHOR. A hold is a finger held STILL on one button: a finger that slides off
 *    cancels it, and it does not resume on a return, because a hold that could be left and
 *    rejoined is one step from an eject the operator did not watch themselves make. A
 *    press that begins off every button -- the title, the rule, the gap, the glass -- is
 *    the only kind that dismisses, and that is the way out that took CANCEL's place.
 *
 * 5. THE DIM BUTTONS. A button whose device rbp reports absent is drawn in the palette's
 *    OFF pair, CANNOT START A HOLD -- prompt_tick() re-asks rbp at the fire and refuses a
 *    device that has gone -- and does not dismiss the box either. It must never send the
 *    eject and let rbp's own press branch drop it at [this+0x88] != 2: from the outside
 *    that is a working button that does nothing, which is the failure this whole box
 *    exists to avoid.
 *
 * 6. THE TIMEOUT, and the one clause in it that is not about time. It closes the box with
 *    nothing touching it -- and it does NOT while a finger is down, because closing there
 *    would clear the latch and hand rbp a release with no press behind it
 *    ([[declined-press-must-still-see-release]]). Deferring costs one release. A hold that
 *    completes beats the timeout: three seconds of a finger is a decision, and starting it
 *    late in the box's ten does not unmake it.
 *
 * 7. THE IMAGE. Every pixel of the view is written, so a second paint of the same state
 *    leaves the same bytes -- the plane's buffer is single-buffered and a paint that is
 *    not idempotent is a paint that tears. A dim button, a live one, a pressed one and the
 *    flash's lit one are four different pictures. And every character of the two labels is
 *    in the font's set: menu_font.h draws a GAP for one that is not, which would ship as
 *    a dead-looking label rather than as a failure.
 *
 * 8. THE NAMES. A button reads "HOLD <the stick's own volume label>" when the host gave
 *    us one (prompt_zone.h), and a name is the operator's to choose -- so it is untrusted
 *    input in a rectangle this module owns. The composition is pinned here, because
 *    everything downstream is: an empty or absent name leaves the cell's shipped label
 *    alone (the fallback is what a unit with no label files draws, byte for byte); one
 *    device's name never reaches the other's button; a character the atlas cannot draw
 *    becomes '?' rather than a five-pixel hole (menu_font_index() at every character of
 *    the result is the tooth); the whole thing is capped at PR_LABEL_MAX; and
 *    prompt_text_clip() -- the second, per-pixel cut, which is what actually keeps a long
 *    name off the frame -- returns a maximal prefix that never exceeds its budget. The
 *    pixel half is the one that matters most: the frame is painted BEFORE the label, so a
 *    budget one pixel too generous would ship as a name painted over its own button's
 *    border, and nothing here would call it a failure.
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
 * The harness. prompt_feed() is the module's one door and prompt_tick() the one
 * clock, so every gesture test is a sequence of reports and ticks fed exactly as
 * pointsrc.c's funnel and read loop feed them.
 *
 * The clock is a plain counter: nothing in prompt_zone.c reads a clock of its own, so
 * the tests own time completely and the hold and the timeout are arithmetic rather
 * than sleeps.
 * ------------------------------------------------------------------------- */

static unsigned long long now_ms;

static struct prompt_state S;

/* Both devices present. The tests that care about a dim one say so. */
static void begin(unsigned long long t)
{
    prompt_reset();
    now_ms = t;
    S.live[0] = 1;
    S.live[1] = 1;
    /* No device has a name of its own, so every cell falls back to its shipped label --
     * which is the state a unit with no /tmp/udev_usbN.label is in. A test that wants a
     * name sets one with prompt_state_name(). */
    prompt_state_name(&S, 0, 0);
    prompt_state_name(&S, 1, 0);
    prompt_open(now_ms);
}

/* A point comfortably inside a cell, so a boundary change does not turn a gesture test
 * into a geometry test. */
static int cx(int cell) { return PR_CELL_X0(cell) + 10; }
static int cy(int cell) { return PR_CELL_Y0(cell) + 10; }

/* One report at the current time. Returns the MZ_FEED_* code; `*act` the answer. */
static int feed(int down, int x, int y, int *act)
{
    return prompt_feed(&S, down, x, y, now_ms, act);
}

/* One report at a stated time. The caller moves the clock and then reports, which is
 * what pointsrc.c does: every report carries the millisecond it arrived at. */
static int feed_at(int down, int x, int y, unsigned long long t, int *act)
{
    now_ms = t;
    return prompt_feed(&S, down, x, y, t, act);
}

/* One slice of the read loop's wait. pointsrc.c takes one of these every
 * POINT_MENU_HOLD_TICK_MS while the box is up, and this is the only thing that advances
 * the flash or fires a hold with nobody touching anything. */
static int tick(unsigned long long t, int *act)
{
    now_ms = t;
    return prompt_tick(&S, t, act);
}

/* A press and a release at the same point, with no time passing and no tick between:
 * the tap, which since 2026-10-07 can never fire anything -- it is always shorter than
 * the hold. */
static int tap(int x, int y)
{
    int act = PR_ACT_NONE;

    feed(1, x, y, &act);
    act = PR_ACT_NONE;
    feed(0, x, y, &act);
    return act;
}

static int tap_cell(int cell) { return tap(cx(cell), cy(cell)); }

/* A hold taken at `t` and run to `t + ms` with no report in between, then one tick.
 * That is what a finger held still looks like from this module's side: the only thing
 * that happens for three seconds is the caller's clock. */
static int hold_run(int cell, unsigned long long t, unsigned long long ms, int *act)
{
    *act = PR_ACT_NONE;
    feed_at(1, cx(cell), cy(cell), t, act);
    *act = PR_ACT_NONE;
    return tick(t + ms, act);
}

/* ---------------------------------------------------------------------------
 * 1. The geometry
 * ------------------------------------------------------------------------- */

static void test_geometry(void)
{
    int x0, y0, x1, y1, c, r;

    /* PR_H is claimed in prompt_zone.h to be the sum of the parts. Check it against the
     * parts rather than against 127, so a change to a button height moves what is under
     * it instead of running the row off the bottom of the box. */
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

    /* The row's bottom is PR_PAD + PR_BORDER above the box's own bottom edge, which is
     * the whole point of the arithmetic above: nothing is clipped. */
    prompt_cell_rect(PR_CELL_USB2, &x0, &y0, &x1, &y1);
    CHECK(y1 == PR_Y1 - PR_BORDER - PR_PAD,
          "the row ends %d px above the box, not %d",
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

    /* THE ROW STRIDE, which one row cannot exercise and a third device would. It is
     * written against PROMPT_ROWS so that the day a second line exists this becomes a
     * real check rather than a thing nobody remembered to write. */
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

    /* THE GAP BETWEEN THE TWO BUTTONS IS NOT A BUTTON. A press there is a press the box
     * swallowed and answered nothing for -- the same rule the title and the margin keep,
     * and the reason "tap anywhere else" dismisses with no second rule. */
    CHECK(prompt_cell_at(PR_CELL_X1(PR_CELL_USB1) + PR_COL_GAP / 2, cy(PR_CELL_USB1)) == 0,
          "the gap between the two devices is a button");

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
    /* ...and so does a cell past the DEVICES even when it is a legal button number, which
     * is the second guard prompt_cell_live() carries: the two arrays are sized by two
     * different constants and only today's box makes them equal. */
    CHECK(prompt_cell_live(&S, PROMPT_DEVICES + 1) == 0,
          "a button past the devices answered live");
    prompt_cell_rect(0, &x0, &y0, &x1, &y1);
    CHECK(x0 == 0 && y0 == 0 && x1 == -1 && y1 == -1,
          "an out-of-range cell gave a rect");

    /* And the strings are the ones the operator asked for: a title, and a button per
     * device that says on it what has to be done to it. */
    CHECK(!strcmp(prompt_title(), "USB STOP"), "the title is '%s'", prompt_title());
    CHECK(!strcmp(prompt_cell_label(PR_CELL_USB1), "HOLD USB 1"), "cell 1 is '%s'",
          prompt_cell_label(PR_CELL_USB1));
    CHECK(!strcmp(prompt_cell_label(PR_CELL_USB2), "HOLD USB 2"), "cell 2 is '%s'",
          prompt_cell_label(PR_CELL_USB2));
    CHECK(PR_CELL_ROW(PR_CELL_USB1) == PR_CELL_ROW(PR_CELL_USB2),
          "the two devices are not on one line");
    CHECK(PROMPT_ROWS == 1 && PR_CELL_ROW(PR_CELL_USB1) == 1,
          "the box has grown a second line, which the hold gesture does not have");

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
 * 2. Shut, and swallowing
 * ------------------------------------------------------------------------- */

static void test_closed(void)
{
    int act = PR_ACT_NONE, i;

    prompt_reset();
    CHECK(!prompt_is_open(), "a reset box is still open");
    CHECK(prompt_pressed() == 0, "a reset box has a pressed cell");
    CHECK(prompt_hold_cell() == 0, "a reset box has a hold");
    CHECK(prompt_hold_flash() == 0, "a reset box has a flash");

    /* Shut, the module answers for NOTHING -- not for a press, not for a release, not
     * for a release with no press behind it. rbp's stream is untouched by construction
     * and not by a rectangle test. */
    for (i = 0; i < 2; i++) {
        CHECK(feed(i, cx(PR_CELL_USB1), cy(PR_CELL_USB1), &act) == MZ_FEED_NONE,
              "a shut box took a report");
        CHECK(act == PR_ACT_NONE, "a shut box answered something");
    }
    CHECK(feed(0, 0, 0, &act) == MZ_FEED_NONE, "a shut box took a stray release");

    /* And its clock does nothing either, which is what lets pointsrc.c ask it on every
     * slice whether the box is up or not. */
    act = PR_ACT_USB1;
    CHECK(tick(100000, &act) == PR_TICK_NONE,
          "a shut box reported a tick");
    CHECK(act == PR_ACT_NONE, "a shut box's tick left an answer standing");

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
    CHECK(prompt_hold_cell() == 0, "a press off the buttons started a hold");

    CHECK(feed(1, PR_CELL_X0(PR_CELL_USB1), PR_CELL_Y0(PR_CELL_USB1), &act)
              == MZ_FEED_TAKEN,
          "a press on a button was not taken");

    /* And every report of the same press is taken too. */
    CHECK(feed(1, cx(PR_CELL_USB2), cy(PR_CELL_USB2), &act) == MZ_FEED_TAKEN,
          "a move was not taken");
    CHECK(feed(0, cx(PR_CELL_USB2), cy(PR_CELL_USB2), &act) == MZ_FEED_TAKEN,
          "the release was not taken");

    /* A stray release -- one rbp never saw the press for -- is still ours while the box
     * is up, because rbp must not hear either edge of anything. It does NOT close the
     * box, though: nothing was pressed into it, so nothing has been dismissed. */
    begin(1000);
    CHECK(feed(0, 500, 400, &act) == MZ_FEED_TAKEN, "a stray release was not taken");
    CHECK(prompt_is_open(), "a stray release closed the box");
}

/* ---------------------------------------------------------------------------
 * 3. The hold
 * ------------------------------------------------------------------------- */

static void test_hold(void)
{
    int act;

    /* A PRESS ON A LIVE BUTTON STARTS A HOLD, and nothing else at all happens at the
     * down edge: no answer, no close. */
    begin(1000);
    act = PR_ACT_USB1;
    CHECK(feed_at(1, cx(PR_CELL_USB1), cy(PR_CELL_USB1), 1000, &act) == MZ_FEED_TAKEN,
          "a press on a live button was not taken");
    CHECK(act == PR_ACT_NONE, "the down edge answered %d", act);
    CHECK(prompt_hold_cell() == PR_CELL_USB1, "USB 1 did not start a hold");
    CHECK(prompt_hold_ms(1000) == 0, "a fresh hold was already old");
    CHECK(prompt_is_open(), "a press on a live button closed the box");

    /* NOTHING BEFORE THE THIRD SECOND -- the operator's "if they release it before three
     * seconds don't eject it", asked here as a tick one millisecond short. */
    act = PR_ACT_USB1;
    CHECK(tick(1000 + PR_HOLD_MS - 1, &act) == PR_TICK_FLASH,
          "the box was not still blinking a millisecond before the threshold");
    CHECK(act == PR_ACT_NONE, "a hold fired a millisecond early");
    CHECK(prompt_is_open(), "a hold that had not fired closed the box");
    CHECK(prompt_hold_cell() == PR_CELL_USB1, "the hold did not survive to the threshold");
    CHECK(prompt_hold_ms(1000 + PR_HOLD_MS - 1) == PR_HOLD_MS - 1,
          "prompt_hold_ms() disagrees with the clock it was given");

    /* AND AT IT, the eject -- on the device's own channel, which is what gets the key
     * past rbp's per-manager filter at 0x325a2c. CH_GLOBAL is 1, which is why the old
     * single-column binding could only ever have stopped USB 1. */
    act = PR_ACT_NONE;
    CHECK(tick(1000 + PR_HOLD_MS, &act) == PR_TICK_EJECT, "the hold did not fire at 3 s");
    CHECK(act == PR_ACT_USB1, "the fire answered %d", act);
    CHECK(prompt_act_channel(act) == 1, "USB 1's channel is not 1");

    /* THE BOX IS STILL UP, and this is not an oversight: the finger that made the hold is
     * still down, so its release is still owed to the box -- see the release test below,
     * where that release is what puts the box away. */
    CHECK(prompt_is_open(), "the box closed under the finger that was still holding");

    /* It cannot fire twice. Three seconds have passed and the hold is over; a tick a
     * second later is nothing but a blink. */
    act = PR_ACT_USB1;
    CHECK(tick(1000 + PR_HOLD_MS + 1000, &act) == PR_TICK_FLASH,
          "a fired hold fired again");
    CHECK(act == PR_ACT_NONE, "a fired hold answered twice");

    /* AND THE RELEASE PUTS IT AWAY, sending nothing -- the eject has already gone, and
     * this report must not be handed to rbp as an up for a down it never saw. */
    act = PR_ACT_USB1;
    CHECK(feed_at(0, cx(PR_CELL_USB1), cy(PR_CELL_USB1), 1000 + PR_HOLD_MS + 1000, &act)
              == MZ_FEED_TAKEN,
          "the release after a fire was not taken");
    CHECK(act == PR_ACT_NONE, "the release after a fire answered %d", act);
    CHECK(!prompt_is_open(), "the box outlived the release that followed its own eject");

    /* The other device: the same three seconds, the other channel. */
    begin(1000);
    act = PR_ACT_NONE;
    CHECK(hold_run(PR_CELL_USB2, 1000, PR_HOLD_MS, &act) == PR_TICK_EJECT,
          "a hold on USB 2 did not fire");
    CHECK(act == PR_ACT_USB2, "USB 2's hold answered %d", act);
    CHECK(prompt_act_channel(act) == 2, "USB 2's channel is not 2");

    /* A HOLD THAT COMPLETES ON THE RELEASE ITSELF. pointsrc.c slices its wait while the
     * box is up, so this is nearly unreachable -- which is exactly why it is pinned: the
     * three seconds are a property of the clock and not of the caller's slicing, and a
     * release arriving at 3001 ms must not be refused because nobody looked at 3000. */
    begin(1000);
    act = PR_ACT_NONE;
    feed_at(1, cx(PR_CELL_USB1), cy(PR_CELL_USB1), 1000, &act);
    act = PR_ACT_NONE;
    feed_at(0, cx(PR_CELL_USB1), cy(PR_CELL_USB1), 1000 + PR_HOLD_MS, &act);
    CHECK(act == PR_ACT_USB1, "a release at the threshold answered %d", act);
    CHECK(!prompt_is_open(), "the box stayed up after firing on its own release");

    /* A LONG HOLD IS STILL ONE EJECT. Ten seconds of finger is three seconds of finger. */
    begin(1000);
    act = PR_ACT_NONE;
    CHECK(hold_run(PR_CELL_USB1, 1000, PR_HOLD_MS * 3 + 500, &act) == PR_TICK_EJECT,
          "a long hold did not fire");
    CHECK(act == PR_ACT_USB1, "a long hold answered %d", act);

    /* NO HOLD IS INHERITED BY A RE-RAISED BOX: a box that comes back must not still be
     * holding a button the operator pressed ten minutes ago. */
    begin(1000);
    feed_at(1, cx(PR_CELL_USB1), cy(PR_CELL_USB1), 1000, &act);
    CHECK(prompt_hold_cell() == PR_CELL_USB1, "the press did not start a hold");
    prompt_close();
    prompt_open(2000);
    CHECK(prompt_hold_cell() == 0, "a re-opened box inherited a hold");
    CHECK(prompt_hold_flash() == 0, "a re-opened box inherited a flash");
}

/* THE FLASH, which is the whole of what tells the operator their hold is running: three
 * seconds is a long time to hold a button with nothing happening. */
static void test_flash(void)
{
    int act = PR_ACT_NONE;

    begin(1000);
    feed_at(1, cx(PR_CELL_USB1), cy(PR_CELL_USB1), 1000, &act);

    /* Lit the instant the finger lands, before any tick at all -- a tap of any length
     * shorter than one blink still has to show that it landed on something live. */
    CHECK(prompt_hold_flash() == 1, "a fresh hold did not light its button");

    /* And the finger's own cell is NOT the highlighted one at the same time: the two draw
     * the same pair of pixels, so a cell that were both could not blink at all. */
    CHECK(prompt_pressed() == 0,
          "the cell whose hold is running was also the finger's own highlight");

    /* Off a half period later, on again after two -- and the cell underneath never moves. */
    CHECK(tick(1000 + PR_HOLD_FLASH_MS, &act) == PR_TICK_FLASH, "the flash tick answered");
    CHECK(prompt_hold_flash() == 0, "the flash did not go off a half period in");
    CHECK(prompt_hold_cell() == PR_CELL_USB1, "the blink moved the hold off its cell");
    CHECK(prompt_is_open(), "a blink closed the box");

    tick(1000 + 2 * PR_HOLD_FLASH_MS, &act);
    CHECK(prompt_hold_flash() == 1, "the flash did not come back");

    /* It blinks for the WHOLE hold and not just the start -- at two and a half seconds it
     * is still alternating, or the operator would be holding a button that had stopped
     * telling them anything a third of the way in. */
    tick(1000 + (PR_HOLD_MS - PR_HOLD_FLASH_MS) * 2 / 3, &act);
    CHECK(prompt_hold_cell() == PR_CELL_USB1, "the hold gave out before its threshold");

    /* NO HOLD, NO FLASH. A finger on a dim button is highlighted as the finger (it landed
     * somewhere, and a panel that says nothing reads as broken) but nothing blinks, because
     * nothing is counting. */
    begin(1000);
    S.live[0] = 0;
    feed_at(1, cx(PR_CELL_USB1), cy(PR_CELL_USB1), 1000, &act);
    CHECK(prompt_hold_cell() == 0, "a dim button started a hold");
    CHECK(prompt_hold_flash() == 0, "a dim button flashed");
    CHECK(prompt_pressed() == PR_CELL_USB1, "a dim button did not highlight the finger");
}

/* ---------------------------------------------------------------------------
 * 4. The anchor, and the way out
 * ------------------------------------------------------------------------- */

static void test_anchor(void)
{
    int act;

    /* A PRESS THAT BEGINS OFF EVERY BUTTON DISMISSES. There is no second rule for "tap
     * outside to dismiss", and this is the way out that took CANCEL's place. */
    begin(1000);
    act = tap(PR_X0 + 4, PR_TITLE_Y0 + 4);
    CHECK(act == PR_ACT_NONE, "a tap on the title answered %d", act);
    CHECK(!prompt_is_open(), "a tap on the title left the box up");

    begin(1000);
    act = tap(10, 10);
    CHECK(act == PR_ACT_NONE, "a tap outside the box answered %d", act);
    CHECK(!prompt_is_open(), "a tap outside the box left the box up");

    /* A press in the gap between the two buttons is a press that missed, so it dismisses
     * -- which is the reason the gap is worth having. */
    begin(1000);
    act = tap(PR_CELL_X1(PR_CELL_USB1) + PR_COL_GAP / 2, cy(PR_CELL_USB1));
    CHECK(act == PR_ACT_NONE, "a tap in the gap answered %d", act);
    CHECK(!prompt_is_open(), "a tap in the gap left the box up");

    /* AND A PRESS THAT BEGAN ON A BUTTON NEVER DISMISSES, however it ends: the box is not
     * dismissed by a press that landed on it. An early release leaves it standing so the
     * operator can take a fresh hold, which is the operator's own "don't eject it". */
    begin(1000);
    act = PR_ACT_NONE;
    feed_at(1, cx(PR_CELL_USB1), cy(PR_CELL_USB1), 1000, &act);
    act = PR_ACT_USB1;
    feed_at(0, cx(PR_CELL_USB1), cy(PR_CELL_USB1), 1000 + PR_HOLD_MS - 1, &act);
    CHECK(act == PR_ACT_NONE, "an early release answered %d", act);
    CHECK(prompt_is_open(), "an early release closed the box");
    CHECK(prompt_hold_cell() == 0, "the hold survived the release that ended it");

    /* And the box is good for another hold straight away. */
    act = PR_ACT_NONE;
    CHECK(hold_run(PR_CELL_USB1, 1000 + PR_HOLD_MS, PR_HOLD_MS, &act) == PR_TICK_EJECT,
          "the box would not take a second hold after an abandoned first");
    CHECK(act == PR_ACT_USB1, "the second hold answered %d", act);

    /* SLIDE OFF CANCELS, AND IT DOES NOT RESUME. The cell that fires is the cell the press
     * STARTED on, and only while the finger is still on it. */
    begin(1000);
    feed_at(1, cx(PR_CELL_USB1), cy(PR_CELL_USB1), 1000, &act);
    CHECK(prompt_hold_cell() == PR_CELL_USB1, "the press did not start a hold");
    feed_at(1, cx(PR_CELL_USB2), cy(PR_CELL_USB2), 1000 + 500, &act);
    CHECK(prompt_pressed() == PR_CELL_USB2, "the finger did not follow to USB 2");
    CHECK(prompt_hold_cell() == 0, "sliding on to USB 2 kept USB 1's hold");
    act = PR_ACT_USB1;
    CHECK(tick(1000 + PR_HOLD_MS * 2, &act) == PR_TICK_FLASH,
          "a hold that had slid off still fired");
    CHECK(act == PR_ACT_NONE, "a hold that had slid off answered %d", act);

    /* A finger that wanders OFF the button and back on again is a different gesture: the
     * hold was cancelled when it left, and it does not come back on the return. This is the
     * one rule here that a returned finger could get wrong, and it is deliberately strict --
     * a hold that could be left and rejoined is one step from an eject the operator did not
     * watch themselves make. */
    begin(1000);
    feed_at(1, cx(PR_CELL_USB2), cy(PR_CELL_USB2), 1000, &act);
    CHECK(prompt_hold_cell() == PR_CELL_USB2, "the press did not start a hold");
    feed_at(1, 10, 10, 1000 + 200, &act);
    CHECK(prompt_pressed() == 0, "a finger off the box still highlighted a cell");
    CHECK(prompt_hold_cell() == 0, "a finger off the box kept its hold");
    feed_at(1, cx(PR_CELL_USB2), cy(PR_CELL_USB2), 1000 + 400, &act);
    act = PR_ACT_USB2;
    CHECK(tick(1000 + PR_HOLD_MS * 2, &act) == PR_TICK_FLASH,
          "a hold that had left the button and come back still fired");
    CHECK(act == PR_ACT_NONE, "a hold that had left the button answered %d", act);

    /* A press that began on USB 1 and lifts on USB 2 holds NOTHING: the anchor is where the
     * press started and the release is judged there, so a wandering finger cannot hold the
     * button it arrives on. */
    begin(1000);
    feed_at(1, cx(PR_CELL_USB1), cy(PR_CELL_USB1), 1000, &act);
    act = PR_ACT_USB1;
    feed_at(0, cx(PR_CELL_USB2), cy(PR_CELL_USB2), 1000 + PR_HOLD_MS * 2, &act);
    CHECK(act == PR_ACT_NONE, "a slide from USB 1 to USB 2 answered %d", act);
    CHECK(prompt_is_open(), "a slide between the buttons closed the box");

    /* THE MIRROR, which is the dangerous one: press on USB 2, slide ONTO USB 1, lift. The
     * anchor is still USB 2 and the release is on USB 1, so nothing is stopped -- a finger
     * that wanders onto a button cannot hold it by arriving. */
    begin(1000);
    feed_at(1, cx(PR_CELL_USB2), cy(PR_CELL_USB2), 1000, &act);
    feed_at(1, cx(PR_CELL_USB1), cy(PR_CELL_USB1), 1000 + PR_HOLD_MS - 10, &act);
    act = PR_ACT_USB2;
    feed_at(0, cx(PR_CELL_USB1), cy(PR_CELL_USB1), 1000 + PR_HOLD_MS * 2, &act);
    CHECK(act == PR_ACT_NONE, "sliding on to USB 1 held it (%d)", act);
    CHECK(prompt_is_open(), "a slide on to USB 1 closed the box");
}

static void test_dim_buttons(void)
{
    int act;

    /* rbp says USB 1 is not there. The button is drawn dim and CANNOT START A HOLD -- and
     * it must never send the eject and let rbp's own press branch drop it at
     * [this+0x88] != 2: from the outside that is a working button that does nothing. */
    begin(1000);
    S.live[0] = 0;
    CHECK(!prompt_cell_live(&S, PR_CELL_USB1), "an absent device reported live");
    CHECK(prompt_cell_live(&S, PR_CELL_USB2), "USB 2 was dragged down with USB 1");

    act = tap_cell(PR_CELL_USB1);
    CHECK(act == PR_ACT_NONE, "a dim button answered %d", act);
    CHECK(prompt_act_channel(act) == 0, "a dim button named a channel");
    /* AND IT DOES NOT DISMISS EITHER: the operator who pressed the dim one probably wanted
     * the other, and the box staying up is what lets them have it. */
    CHECK(prompt_is_open(), "a dim button dismissed the box");

    /* Ten seconds of finger on a dim button is still nothing: no hold ever started, so
     * there is nothing for the clock to complete. */
    begin(1000);
    S.live[0] = 0;
    act = PR_ACT_USB1;
    CHECK(hold_run(PR_CELL_USB1, 1000, PR_HOLD_MS * 10, &act) == PR_TICK_FLASH,
          "a hold on a dim button fired");
    CHECK(act == PR_ACT_NONE, "a hold on a dim button answered %d", act);
    CHECK(prompt_hold_cell() == 0, "a dim button held");

    /* THE OTHER DEVICE IS UNAFFECTED. */
    begin(1000);
    S.live[0] = 0;
    act = PR_ACT_NONE;
    CHECK(hold_run(PR_CELL_USB2, 1000, PR_HOLD_MS, &act) == PR_TICK_EJECT,
          "the live device was refused with the dim one");
    CHECK(act == PR_ACT_USB2, "the live device answered %d", act);

    /* A DEVICE THAT GOES AWAY UNDER A RUNNING HOLD TAKES THE HOLD WITH IT. rbp is asked at
     * the fire and not only at the press (pointsrc.c re-reads on every report), so the
     * three seconds cannot complete for a stick that has gone. */
    begin(1000);
    feed_at(1, cx(PR_CELL_USB1), cy(PR_CELL_USB1), 1000, &act);
    CHECK(prompt_hold_cell() == PR_CELL_USB1, "the press did not start a hold");
    S.live[0] = 0;
    act = PR_ACT_USB1;
    CHECK(tick(1000 + PR_HOLD_MS, &act) == PR_TICK_FLASH,
          "a hold fired for a device that had gone");
    CHECK(act == PR_ACT_NONE, "a hold on a vanished device answered %d", act);
    CHECK(prompt_hold_cell() == 0, "the hold survived the device going away");
    CHECK(prompt_is_open(), "a vanished device closed the box");

    /* BOTH DEVICES DIM is a real state, not just a hypothetical: it is what the operator
     * sees with no stick in either slot. No hold can start on either button, and the way
     * out is a tap outside or the timeout -- there is no CANCEL any more. */
    begin(1000);
    S.live[0] = 0;
    S.live[1] = 0;
    CHECK(!prompt_cell_live(&S, PR_CELL_USB1), "USB 1 reported live with no media");
    CHECK(!prompt_cell_live(&S, PR_CELL_USB2), "USB 2 reported live with no media");
    act = PR_ACT_NONE;
    feed_at(1, cx(PR_CELL_USB1), cy(PR_CELL_USB1), 1000, &act);
    act = PR_ACT_USB1;
    CHECK(tick(1000 + PR_HOLD_MS * 2, &act) == PR_TICK_FLASH,
          "a hold fired with no media anywhere");
    CHECK(act == PR_ACT_NONE, "a hold answered with no media anywhere");
    /* The release, so that the tap below is a tap and not the tail of a press the box
     * anchored on the dim button -- prompt_feed() anchors a press wherever it lands, and
     * a release with no fresh press behind it would take the "began on a button" path and
     * leave the box standing. */
    feed_at(0, cx(PR_CELL_USB1), cy(PR_CELL_USB1), 1000 + PR_HOLD_MS * 2, &act);
    act = tap(PR_X0 + 4, PR_TITLE_Y0 + 4);
    CHECK(!prompt_is_open(), "a box with nothing to offer would not go away");

    /* A caller that cannot read rbp leaves both 0. That is a refusal and not an
     * assumption: both buttons dim, and neither can hold. */
    begin(1000);
    S.live[0] = 0;
    S.live[1] = 0;
    CHECK(tap_cell(PR_CELL_USB1) == PR_ACT_NONE, "an unreadable device answered USB 1");
    act = PR_ACT_NONE;
    CHECK(hold_run(PR_CELL_USB1, 1000, PR_HOLD_MS, &act) == PR_TICK_FLASH,
          "an unreadable device held");
}

/* ---------------------------------------------------------------------------
 * 6. The timeout
 * ------------------------------------------------------------------------- */

static void test_timeout(void)
{
    int act = PR_ACT_NONE;
    unsigned long long opened;

    begin(1000);
    opened = now_ms;

    /* Not yet: the clock is a deadline and not a countdown from the last report. */
    CHECK(tick(opened + PR_TIMEOUT_MS - 1, &act) == PR_TICK_FLASH, "the box expired early");
    CHECK(prompt_is_open(), "the box closed a tick early");
    CHECK(prompt_deadline() == opened + PR_TIMEOUT_MS,
          "the deadline is not the open time plus the timeout");

    /* Exactly on the deadline it goes. */
    act = PR_ACT_USB1;
    CHECK(tick(opened + PR_TIMEOUT_MS, &act) == PR_TICK_TIMEOUT,
          "the box outlived its deadline");
    CHECK(act == PR_ACT_NONE, "an expiring box left an answer standing");
    CHECK(!prompt_is_open(), "the box is still open after expiring");

    /* And it reports the transition ONCE, so a caller can log it once. */
    CHECK(tick(opened + PR_TIMEOUT_MS + 5000, &act) == PR_TICK_NONE,
          "an expired box reported a second expiry");

    /* NOT WHILE A FINGER IS DOWN. A press the box swallowed has been withheld from rbp
     * for its whole life and rbp is going to be given its release; closing here would
     * clear the latch and hand that release over with no press behind it. A DIM button is
     * the press to use for this: it starts no hold, so the tick has nothing else to say. */
    begin(1000);
    S.live[0] = 0;
    feed_at(1, cx(PR_CELL_USB1), cy(PR_CELL_USB1), 1000, &act);
    CHECK(tick(1000 + PR_TIMEOUT_MS * 10, &act) == PR_TICK_FLASH,
          "the box expired under a finger");
    CHECK(prompt_is_open(), "the box closed under a finger");

    /* And the release then takes its OWN path, a moment later, and does not dismiss: the
     * press began on a button. The deferral costs one release and nothing else -- and the
     * box then goes on the very next tick, which is what makes it a deferral and not a
     * reprieve. */
    feed_at(0, cx(PR_CELL_USB1), cy(PR_CELL_USB1), 1000 + PR_TIMEOUT_MS * 10, &act);
    CHECK(act == PR_ACT_NONE, "the deferred release answered %d", act);
    CHECK(prompt_is_open(), "the deferred release closed the box");
    CHECK(tick(1000 + PR_TIMEOUT_MS * 10 + 1, &act) == PR_TICK_TIMEOUT,
          "the box did not expire once the finger was gone");

    /* A HOLD THAT COMPLETES BEATS THE TIMEOUT. Three seconds of a finger on the button is
     * the operator's decision, and starting it late in the box's ten does not unmake it --
     * which is why the hold is asked FIRST. */
    begin(1000);
    act = PR_ACT_NONE;
    CHECK(hold_run(PR_CELL_USB1, 1000 + PR_TIMEOUT_MS - 100, PR_HOLD_MS, &act)
              == PR_TICK_EJECT,
          "a hold that outlived the deadline was turned into a timeout");
    CHECK(act == PR_ACT_USB1, "a late hold answered %d", act);

    /* Re-raising restarts the clock, which is what makes the column idempotent: tapping
     * it twice is a longer look, not a broken box. */
    begin(1000);
    prompt_open(6000);
    CHECK(prompt_deadline() == 6000 + PR_TIMEOUT_MS,
          "re-raising did not restart the clock");
    /* And it clears any press in flight: the tap that raised it is already over, and a
     * box that inherited a finger it never saw go down would answer on that finger's
     * next release. */
    CHECK(prompt_pressed() == 0, "a re-raised box inherited a press");
    CHECK(prompt_hold_cell() == 0, "a re-raised box inherited a hold");
    CHECK(feed(0, cx(PR_CELL_USB1), cy(PR_CELL_USB1), &act) == MZ_FEED_TAKEN,
          "a re-raised box did not take a stray release");
    CHECK(prompt_is_open(), "a stray release closed a re-raised box");
}

static void test_reset(void)
{
    int act = PR_ACT_NONE;

    /* prompt_reset() is what pointsrc.c calls when the pointer device goes away. A press
     * left anchored, a hold left running, or a box left up would survive a replug. */
    begin(1000);
    feed_at(1, cx(PR_CELL_USB1), cy(PR_CELL_USB1), 1000, &act);
    CHECK(prompt_hold_cell() == PR_CELL_USB1, "the press did not start a hold");
    prompt_reset();
    CHECK(!prompt_is_open(), "the box survived a reset");
    CHECK(prompt_pressed() == 0, "a press survived a reset");
    CHECK(prompt_hold_cell() == 0, "a hold survived a reset");
    CHECK(prompt_hold_flash() == 0, "a flash survived a reset");
    CHECK(prompt_deadline() == 0, "a deadline survived a reset");
    CHECK(feed(0, cx(PR_CELL_USB1), cy(PR_CELL_USB1), &act) == MZ_FEED_NONE,
          "a reset box took a report");

    /* A box re-raised after a reset does not inherit the old press either. */
    prompt_open(2000);
    act = PR_ACT_USB1;
    feed_at(0, cx(PR_CELL_USB1), cy(PR_CELL_USB1), 2000, &act);
    CHECK(act == PR_ACT_NONE, "a press from before the reset answered after it");
}

/* ---------------------------------------------------------------------------
 * 7. The image
 *
 * Local arrays, at the box's own size: no framebuffer, no plane, no device.
 * ------------------------------------------------------------------------- */

#define IW PR_W
#define IH PR_H
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

    /* AND THE WIDTH IS ITS OWN REFUSAL. A button is only half the box wide, so a page that
     * scales the box down can reach a scale where a line still fits but a LABEL no longer
     * does -- and since 2026-10-07 the labels are half again as long as they were ("HOLD
     * USB 1" is 99 px where "USB 1" was 46), so that scale is reached sooner. The pair
     * below isolates it -- the same height, two widths, opposite answers -- so a change
     * that drops the width check fails here rather than on the glass. */
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

    /* Both devices live, nothing pressed, nothing lit. */
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

    /* A DIM CELL IS A DIFFERENT PICTURE. USB 1 dim. */
    S.live[0] = 0;
    view_for(&v, buf_c, IW, IH, 16);
    prompt_paint(&v, &S, 0, 0);
    CHECK(memcmp(buf_b, buf_c, BOX), "a dim button drew the same as a live one");

    /* And the dim cell is confined to the dim cell: the rest of the box is untouched by
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

    /* THE FLASH'S LIT HALF IS A DIFFERENT PICTURE, and it is the whole of the hold's
     * feedback: without it a three-second hold and a swallowed tap look the same on the
     * glass. It is also confined to its own cell, like everything else here. */
    {
        unsigned char buf_d[BOX];
        int rx0, ry0, rx1, ry1, k, outside = 0;

        S.live[0] = 1;
        view_for(&v, buf_d, IW, IH, 16);
        prompt_paint(&v, &S, 0, PR_CELL_USB1);
        CHECK(memcmp(buf_b, buf_d, BOX),
              "a lit button drew the same as an idle one");

        prompt_cell_rect(PR_CELL_USB1, &rx0, &ry0, &rx1, &ry1);
        rx0 -= PR_X0; rx1 -= PR_X0; ry0 -= PR_Y0; ry1 -= PR_Y0;
        for (j = 0; j < IH; j++)
            for (k = 0; k < IW; k++) {
                size_t off = (size_t)(j * IW + k) * 2;

                if (!memcmp(&buf_b[off], &buf_d[off], 2))
                    continue;
                if (k < rx0 || k > rx1 || j < ry0 || j > ry1)
                    outside++;
            }
        CHECK(outside == 0, "%d px changed outside the lit button", outside);
    }

    /* A PRESSED CELL IS A DIFFERENT PICTURE TOO, and it is a different picture from the
     * dim one -- the pressed pair wins over the off pair, deliberately. */
    {
        unsigned char buf_e[BOX];

        S.live[0] = 0;
        view_for(&v, buf_e, IW, IH, 16);
        prompt_paint(&v, &S, PR_CELL_USB1, 0);
        CHECK(memcmp(buf_c, buf_e, BOX),
              "a pressed dim button drew the same as an unpressed one");
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

    /* An out-of-range pressed or lit cell is clamped rather than read off the end of
     * the label table. */
    view_for(&v, buf_a, IW, IH, 16);
    prompt_paint(&v, &S, 99, 99);
    prompt_paint(&v, &S, -3, -3);
    prompt_paint(&v, &S, 0, 0);
    view_for(&v, buf_b, IW, IH, 16);
    prompt_paint(&v, &S, 0, 0);
    CHECK(!memcmp(buf_a, buf_b, BOX), "an out-of-range cell was not clamped");
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

/* THE CHOOSER'S LABELS ARE LAID DOWN BY THE PENS THE ATLAS MEASURES.
 *
 * The width pp_text_center() CENTRES with (menu_text_width) and the pens pp_text() steps
 * have to be the same numbers, and nothing else in this file can see a disagreement: it
 * has no classifier to notice one, so a walk that stepped something else would draw a
 * slightly different word a pixel or two off its centre.
 *
 * The INK WIDTH is the tooth, and it needs no copy of pp_fx()'s scaling: the extent from
 * the first inked column to the last is a pure function of the string and the atlas --
 * the pen's origin cancels out. On a bake whose pair table is live it also says WHICH
 * walk was used, because the two answer differently at every kerned pair; on the shipped
 * Decker bake the table is all zero (MENU_FONT_KERNED 0), so what it pins there is the
 * pen arithmetic itself -- that a glyph is followed by its own advance, and that the last
 * glyph's ink is not counted as an advance. The scan stays inside the frame (pp_frame is
 * 2 px) so it measures the label and not the box. Both labels have a space in them, which
 * is the one glyph the atlas gives no ink. */
static void test_paint_label_pens(void)
{
    static const int cells[] = { PR_CELL_USB1, PR_CELL_USB2 };
    struct menu_view v;
    int i, x, y;

    S.live[0] = 1;
    S.live[1] = 1;
    view_for(&v, buf_b, IW, IH, 16);
    prompt_paint(&v, &S, 0, 0);

    for (i = 0; i < 2; i++) {
        const char *s = prompt_cell_label(cells[i]);
        unsigned int face = menu_pixel(16, MENU_BTN);
        int rx0, ry0, rx1, ry1, minx = IW, maxx = -1, ix0, ix1, want;

        prompt_cell_rect(cells[i], &rx0, &ry0, &rx1, &ry1);
        rx0 += 2 - PR_X0;               /* inside the frame, into buffer columns */
        rx1 -= 2 + PR_X0;
        ry0 += 2 - PR_Y0;
        ry1 -= 2 + PR_Y0;

        ink_extent(s, 0, &ix0, &ix1);
        want = ix1 - ix0 + 1;

        /* AND IT HAS TO FIT. pp_labels_fit() refuses a view that cannot hold the DEFAULT
         * label whole, so a label that grew past the cell would refuse every view -- and
         * refuse it as a blank box, since a failed prompt_paint_ok() paints nothing. A
         * device's own name is longer than this and is CLIPPED instead (pp_cell(),
         * test_names()), so what is measured here is the floor the refusal is written
         * against, not the widest string that can reach the glass. The margin is
         * PR_LABEL_MARGIN and not the frame's one because the bake's ink overhangs
         * (prompt_zone.h). */
        CHECK(want < PR_CELL_W - 2 * PR_LABEL_MARGIN,
              "label %d ('%s') is %d px of ink in a %d px cell",
              cells[i], s, want, PR_CELL_W - 2 * PR_LABEL_MARGIN);

        for (y = ry0; y <= ry1; y++)
            for (x = rx0; x <= rx1; x++) {
                size_t off = ((size_t)y * IW + x) * 2;
                unsigned int px = (unsigned int)buf_b[off] |
                                  ((unsigned int)buf_b[off + 1] << 8);

                if (px == face)
                    continue;
                if (x < minx) minx = x;
                if (x > maxx) maxx = x;
            }
        CHECK(maxx >= 0, "cell %d ('%s') drew no ink at all", cells[i], s);
        if (maxx < 0)
            continue;
        CHECK(maxx - minx + 1 == want,
              "cell %d ('%s'): the ink is %d px wide where the atlas's own advances"
              " make it %d -- a walk that is not stepping menu_font_adv()",
              cells[i], s, maxx - minx + 1, want);
    }
}

/* THE BUTTONS NAME THE STICKS. See item 8 at the top of this file for why this is a
 * property worth pinning rather than a detail of the painter. */
static void test_names(void)
{
    char out[PR_LABEL_MAX + 2];
    int c, i;

    /* NO NAME IS THE SHIPPED PICTURE. A unit whose usb-watch.sh never wrote a label
     * file -- or whose stick has an empty volume label -- has to draw exactly what it
     * drew before this existed, so the fallback is checked for every cell, and for a
     * state that is not there at all. */
    for (c = 1; c <= PROMPT_CELLS; c++) {
        CHECK(!strcmp(prompt_cell_text(&S, c), prompt_cell_label(c)),
              "cell %d with no name is '%s', not its own label '%s'",
              c, prompt_cell_text(&S, c), prompt_cell_label(c));
        CHECK(!strcmp(prompt_cell_text(0, c), prompt_cell_label(c)),
              "a NULL state did not fall back to the label for cell %d", c);
    }
    CHECK(prompt_cell_text(&S, 0) == 0, "cell 0 answered with a label");
    CHECK(prompt_cell_text(&S, PROMPT_CELLS + 1) == 0, "cell past the end answered");

    /* A NAME IS COMPOSED, AND CONFINED TO ITS OWN BUTTON. The second half is the half
     * that matters: the two labels live in one struct and are indexed by cell-1, so an
     * off-by-one here would put the library's name on the empty port's button. */
    prompt_state_name(&S, 0, "RBOX USB");
    CHECK(!strcmp(prompt_cell_text(&S, PR_CELL_USB1), "HOLD RBOX USB"),
          "the named cell reads '%s'", prompt_cell_text(&S, PR_CELL_USB1));
    CHECK(!strcmp(prompt_cell_text(&S, PR_CELL_USB2), prompt_cell_label(PR_CELL_USB2)),
          "naming USB 1 changed USB 2's button to '%s'",
          prompt_cell_text(&S, PR_CELL_USB2));

    prompt_state_name(&S, 1, "UNTITLED");
    CHECK(!strcmp(prompt_cell_text(&S, PR_CELL_USB2), "HOLD UNTITLED"),
          "the second device's own name did not reach its button");
    CHECK(!strcmp(prompt_cell_text(&S, PR_CELL_USB1), "HOLD RBOX USB"),
          "naming USB 2 disturbed USB 1's button");

    /* AND IT CLEARS. usb-watch.sh removes the label file at detach, so "" and NULL both
     * have to mean "this device has no name of its own" and put the number back. */
    prompt_state_name(&S, 0, "");
    CHECK(!strcmp(prompt_cell_text(&S, PR_CELL_USB1), prompt_cell_label(PR_CELL_USB1)),
          "an empty name did not restore the label");
    prompt_state_name(&S, 1, 0);
    CHECK(!strcmp(prompt_cell_text(&S, PR_CELL_USB2), prompt_cell_label(PR_CELL_USB2)),
          "a NULL name did not restore the label");

    /* An out-of-range device is a no-op, not a write off the end of the table. */
    prompt_state_name(&S, 0, "RBOX USB");
    prompt_state_name(&S, -1, "XXXX");
    prompt_state_name(&S, PROMPT_DEVICES, "XXXX");
    CHECK(!strcmp(prompt_cell_text(&S, PR_CELL_USB1), "HOLD RBOX USB"),
          "an out-of-range device index clobbered USB 1");

    /* THE CHARACTER SET IS THE ATLAS'S. menu_font_index() answers -1 for a byte the bake
     * cannot draw and the painter draws a five-pixel GAP for it -- a hole in the middle of
     * a button that reads as a dead panel rather than as a failure. A volume label is
     * whatever the operator typed into a formatter, so the composition has to remap it.
     * The tooth is that EVERY character of the result is drawable, not that this one
     * string came out as expected. */
    prompt_state_name(&S, 0, "AB(CD)");
    CHECK(!strcmp(prompt_cell_text(&S, PR_CELL_USB1), "HOLD AB?CD?"),
          "an undrawable character became '%s'", prompt_cell_text(&S, PR_CELL_USB1));
    for (i = 0; i < PROMPT_CELLS; i++) {
        const char *s = prompt_cell_text(&S, i + 1);

        for (c = 0; s[c]; c++)
            CHECK(menu_font_index((unsigned char)s[c]) >= 0,
                  "the label carries 0x%02x, which the atlas cannot draw",
                  (unsigned char)s[c]);
    }

    /* AND IT IS CAPPED at PR_LABEL_MAX characters -- "HOLD " plus usb_label.h's own
     * USB_LABEL_MAX -- so a name longer than the file's limit cannot overflow the cell. */
    {
        char long_name[128];

        memset(long_name, 'A', sizeof long_name - 1);
        long_name[sizeof long_name - 1] = '\0';
        prompt_state_name(&S, 0, long_name);
        CHECK((int)strlen(prompt_cell_text(&S, PR_CELL_USB1)) == PR_LABEL_MAX,
              "a %zu-character name composed to %zu characters, not %d",
              strlen(long_name), strlen(prompt_cell_text(&S, PR_CELL_USB1)), PR_LABEL_MAX);
        CHECK(!strncmp(prompt_cell_text(&S, PR_CELL_USB1), "HOLD ", 5),
              "a long name lost its prefix");
    }

    /* prompt_text_clip() -- the per-pixel cut, and the only thing standing between a long
     * name and the button's own frame. A budget that fits returns the string whole; a
     * budget that does not returns the LONGEST prefix that fits, which is the property a
     * "shorten it until it fits" loop can lose by a pixel. */
    {
        const char *s = "HOLD RBOX USB";
        int full = menu_text_width(s);
        int n;

        n = prompt_text_clip(s, full + 40, out, (int)sizeof out);
        CHECK(n == (int)strlen(s) && !strcmp(out, s),
              "a generous budget clipped '%s' to '%s'", s, out);

        for (i = 1; i < full; i++) {
            n = prompt_text_clip(s, i, out, (int)sizeof out);

            CHECK(n == (int)strlen(out), "clip returned %d for a %zu-character string",
                  n, strlen(out));
            CHECK(!strncmp(out, s, (size_t)n), "clip at %d did not return a prefix", i);
            CHECK(menu_text_width(out) <= i,
                  "clip at %d returned '%s', which is %d px wide",
                  i, out, menu_text_width(out));
            if (s[n])
                CHECK(menu_text_width(out) +
                      menu_font_adv((unsigned char)s[n], (unsigned char)s[n + 1]) > i,
                      "clip at %d returned '%s' with room for another character", i, out);
        }

        /* The guards: nothing is written, and nothing is read, for a budget or a buffer
         * that cannot hold anything. A zero-length buffer is the one case with nothing to
         * assert about its contents -- writing the terminator is exactly the bug -- so
         * that one is checked for the return alone. */
        out[0] = 'x';
        CHECK(prompt_text_clip(s, 100, out, 0) == 0 && out[0] == 'x',
              "a zero-length buffer was written to");
        CHECK(prompt_text_clip(s, 0, out, (int)sizeof out) == 0 && out[0] == '\0',
              "a zero-pixel budget returned something");
        CHECK(prompt_text_clip(s, -5, out, (int)sizeof out) == 0 && out[0] == '\0',
              "a negative budget returned something");
        CHECK(prompt_text_clip(0, 40, out, (int)sizeof out) == 0 && out[0] == '\0',
              "a NULL string returned something");
        CHECK(prompt_text_clip(s, 40, 0, (int)sizeof out) == 0,
              "a NULL buffer was written into");
    }

    /* AND THE CUT KEEPS THE INK OFF THE FRAME. The frame is painted BEFORE the label, so
     * an over-wide name does not fail to draw -- it draws over the button's border, and
     * that is what this reads. A maximum-length name is the case with the least room; its
     * ink has to stay inside the two-pixel frame, leaving those columns exactly as the
     * default label left them. */
    {
        static const int cells[] = { PR_CELL_USB1, PR_CELL_USB2 };
        unsigned char ref[BOX], got[BOX];
        struct menu_view v;
        char name[64];

        memset(name, 'W', sizeof name - 1);
        name[sizeof name - 1] = '\0';

        view_for(&v, ref, IW, IH, 16);
        prompt_paint(&v, &S, 0, 0);          /* still "HOLD RBOX USB" on USB 1 */
        prompt_state_name(&S, 0, name);
        view_for(&v, got, IW, IH, 16);
        prompt_paint(&v, &S, 0, 0);

        CHECK(memcmp(ref, got, BOX), "the longest possible name drew nothing new");

        for (c = 0; c < 2; c++) {
            int rx0, ry0, rx1, ry1, x, y, on_frame = 0;

            prompt_cell_rect(cells[c], &rx0, &ry0, &rx1, &ry1);
            rx0 -= PR_X0; rx1 -= PR_X0; ry0 -= PR_Y0; ry1 -= PR_Y0;
            for (y = ry0; y <= ry1; y++)
                for (x = rx0; x <= rx1; x++) {
                    /* The frame's own two columns on each side, top to bottom. */
                    if (x != rx0 && x != rx0 + 1 && x != rx1 - 1 && x != rx1)
                        continue;
                    if (memcmp(&ref[((size_t)y * IW + x) * 2],
                               &got[((size_t)y * IW + x) * 2], 2))
                        on_frame++;
                }
            CHECK(on_frame == 0, "%d px of a clipped name landed on cell %d's frame",
                  on_frame, cells[c]);
        }
    }
}

int main(void)
{
    test_geometry();
    test_closed();
    test_swallow();
    test_hold();
    test_flash();
    test_anchor();
    test_dim_buttons();
    test_timeout();
    test_reset();
    test_paint_size();
    test_paint_image();
    test_paint_label_pens();
    test_names();

    printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "ok", checks, failures);
    return failures ? 1 : 0;
}
