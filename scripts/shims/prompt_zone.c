/*
 * prompt_zone.c -- the USB STOP chooser's geometry and gesture.
 *
 * See prompt_zone.h for what it is for, where the two devices come from and why
 * this is pure. Nothing here reaches rbp, the framebuffer or a clock; the whole of
 * the module is the box, the latch and the arithmetic that turns a press into an
 * answer.
 *
 * THE GRID IS FOUR CELLS AND THE GESTURE IS TWO TAPS. `USB 1` and `USB 2` are a
 * choice, `OK` and `CANCEL` are the answer, and the only way to reach an eject is a
 * tap on a live device FOLLOWED BY a tap on OK. The arming lives here and not in the
 * caller because it is the same latch the painter reads (prompt_selected()) -- a
 * caller holding it would have to be asked on every repaint anyway, and this module
 * already owns the one press in flight.
 */
#include "prompt_zone.h"

/* The cells in cell-number order, which is PROMPT_ROWS x PROMPT_COLS row-major --
 * see prompt_zone.h's PR_CELL_* enum. A label the font cannot draw would ship as a
 * gap rather than as a failure, which is why test_prompt.c walks every character of
 * every one of these against menu_font.h. */
static const char *const cell_labels[PROMPT_CELLS] = {
    "USB 1", "USB 2",       /* row 1: the devices */
    "OK",    "CANCEL"       /* row 2: the answers */
};

/* ---------------------------------------------------------------------------
 * State. All of it is cleared by prompt_reset(); the touch thread is the only
 * writer, exactly as it is for menu_zone.c's panel, and menu_draw.c only reads
 * prompt_is_open() and prompt_selected().
 *
 * `was_down` is the edge detector every feeder in this shim carries
 * (touch_zone.c:60-70's shape) and here it does a second job as well -- see the
 * expiry guard below. `press_cell` is the anchor for fire-on-release and `cur_cell`
 * is what the painter highlights, and the two are separate on purpose: a finger that
 * slides from USB 1 to CANCEL must highlight CANCEL and answer nothing, which is
 * also menu_zone.c's rule.
 *
 * `armed` IS THE ONE PIECE OF STATE THAT OUTLIVES A PRESS, and it is the whole of
 * what makes OK mean anything. It never holds a dead device (row_act refuses one at
 * the source), so "OK is live" and "armed is live" cannot disagree.
 *
 * THERE IS NO SEPARATE SWALLOW LATCH, and that is not an omission. menu_zone.c needs
 * one because a press can begin outside its strip and must be returned untouched;
 * here the box is up, so every report is the box's -- there is no declined press for
 * this module to remember, and the state would be a constant 1.
 * ------------------------------------------------------------------------- */
static int was_down;
static int open_state;
static int press_cell;
static int cur_cell;
static int armed;                 /* PR_CELL_USB1, PR_CELL_USB2, or 0 */
static unsigned long long deadline_ms;

/* The clock is the caller's and it is 64 bits wide on purpose: `long` is four bytes
 * on this target and a monotonic millisecond count in one wraps every 49.7 days,
 * which is the trap pointsrc.c's own shim_now_ms() documents. Every comparison
 * below is written so it does not care where the two values sit on the ring. */
static int expired(unsigned long long now_ms)
{
    return (long long)(now_ms - deadline_ms) >= 0;
}

int prompt_cell_live(const struct prompt_state *S, int cell)
{
    if (cell < 1 || cell > PROMPT_CELLS)
        return 0;
    switch (cell) {
    case PR_CELL_CANCEL:
        return 1;                  /* the way out is always there */
    case PR_CELL_OK:
        /* OK has something to do exactly when a live device is armed. `armed` is
         * already known live (row_act refuses a dead one), so this is the whole test
         * -- but it is written through liveness anyway, so a caller that hands in a
         * different `S` cannot arm a device and then stop it under a box that says
         * the stick is gone. */
        return armed != 0 && prompt_cell_live(S, armed);
    default:
        return S && S->live[cell - 1] ? 1 : 0;
    }
}

/* The answer a cell gives. A dead cell answers NOTHING, and that is not a refusal to
 * answer -- the box still closes, which is the whole of what a tap on a dead cell
 * does. What it must never do is send the eject anyway and let rbp's own press
 * branch drop it at `[this+0x88] != 2`, which would look from the outside exactly
 * like a working button that does nothing. */
static int cell_act(const struct prompt_state *S, int cell)
{
    if (!prompt_cell_live(S, cell))
        return PR_ACT_NONE;
    switch (cell) {
    case PR_CELL_USB1:    return PR_ACT_USB1;
    case PR_CELL_USB2:    return PR_ACT_USB2;
    case PR_CELL_OK:      return (armed == PR_CELL_USB2) ? PR_ACT_USB2 : PR_ACT_USB1;
    case PR_CELL_CANCEL:  return PR_ACT_CANCEL;
    default:              return PR_ACT_NONE;
    }
}

/* Is this a device cell, i.e. one whose release ARMS rather than answers? */
static int cell_is_device(int cell)
{
    return cell == PR_CELL_USB1 || cell == PR_CELL_USB2;
}

int prompt_act_channel(int act)
{
    if (act == PR_ACT_USB1)
        return 1;
    if (act == PR_ACT_USB2)
        return 2;
    return 0;
}

int prompt_cell_at(int x, int y)
{
    int c;

    for (c = 1; c <= PROMPT_CELLS; c++) {
        if (x < PR_CELL_X0(c) || x > PR_CELL_X1(c))
            continue;
        if (y < PR_CELL_Y0(c) || y > PR_CELL_Y1(c))
            continue;
        return c;
    }
    return 0;                      /* the title, the rule, a gap, or off the box */
}

void prompt_cell_rect(int cell, int *x0, int *y0, int *x1, int *y1)
{
    if (cell < 1 || cell > PROMPT_CELLS) {
        if (x0) *x0 = 0;
        if (y0) *y0 = 0;
        if (x1) *x1 = -1;
        if (y1) *y1 = -1;
        return;
    }
    if (x0) *x0 = PR_CELL_X0(cell);
    if (y0) *y0 = PR_CELL_Y0(cell);
    if (x1) *x1 = PR_CELL_X1(cell);
    if (y1) *y1 = PR_CELL_Y1(cell);
}

void prompt_box_rect(int *x0, int *y0, int *x1, int *y1)
{
    if (x0) *x0 = PR_X0;
    if (y0) *y0 = PR_Y0;
    if (x1) *x1 = PR_X1;
    if (y1) *y1 = PR_Y1;
}

const char *prompt_title(void)
{
    return "USB STOP";
}

const char *prompt_cell_label(int cell)
{
    if (cell < 1 || cell > PROMPT_CELLS)
        return 0;
    return cell_labels[cell - 1];
}

void prompt_open(unsigned long long now_ms)
{
    open_state = 1;
    deadline_ms = now_ms + PR_TIMEOUT_MS;
    /* No press is inherited. The tap that raised this is already over -- it fired
     * on a release -- and a caller that raised it from anywhere else still must not
     * adopt a finger it never saw go down, which is menu_zone.c's `started_open`
     * rule in its simplest form: there is nothing to be the continuation of. Neither
     * is an arming, for the reason prompt_zone.h gives. */
    was_down = 0;
    press_cell = 0;
    cur_cell = 0;
    armed = 0;
}

void prompt_close(void)
{
    open_state = 0;
    was_down = 0;
    press_cell = 0;
    cur_cell = 0;
    armed = 0;
}

int prompt_is_open(void)
{
    return open_state;
}

int prompt_pressed(void)
{
    return open_state ? cur_cell : 0;
}

int prompt_selected(void)
{
    return open_state ? armed : 0;
}

unsigned long long prompt_deadline(void)
{
    return deadline_ms;
}

int prompt_expire(unsigned long long now_ms)
{
    /* NOT WHILE A FINGER IS DOWN, and this is the one rule here that has
     * consequences outside this file. A press the box swallowed has been withheld
     * from rbp for its whole life, and rbp is going to be given its release. Closing
     * the box mid-press would clear `was_down`, so the release would be handed back
     * as a report with no press behind it -- a release rbp never saw the press for
     * ([[declined-press-must-still-see-release]] is the same rule the fan-out feeders
     * keep). Deferring instead costs nothing: the release closes the box on its own
     * path a moment later. */
    if (!open_state || was_down || !expired(now_ms))
        return 0;
    prompt_close();
    return 1;
}

void prompt_reset(void)
{
    prompt_close();
    deadline_ms = 0;
}

int prompt_feed(const struct prompt_state *S, int down, int x, int y, int *act)
{
    if (act)
        *act = PR_ACT_NONE;

    if (!open_state)
        return MZ_FEED_NONE;      /* shut: this module answers for nothing at all */

    if (!down) {
        int cell;

        if (!was_down)
            return MZ_FEED_TAKEN; /* a release we never saw the press for: still
                                   * ours, because the box is up and rbp must not
                                   * hear either edge of anything. */
        was_down = 0;
        /* Fire on the release, and only for a press that began and ended on the
         * SAME cell. The cell is taken from the RELEASE's own coordinates and not
         * from `cur_cell`, and the difference is real: a fast flick can lift at a moved
         * position with no move report in between -- the panel reports the up wherever
         * the finger was, and a release that moved must not answer for the cell the
         * finger has already left. `press_cell` is 0 for a press that began off a cell --
         * the title, a gap, the glass outside the box -- so a miss can never answer,
         * and it still closes the box: that is the whole of "tap outside to dismiss",
         * with no second rule for it. */
        cell = prompt_cell_at(x, y);
        if (press_cell && cell == press_cell && prompt_cell_live(S, press_cell)) {
            if (cell_is_device(cell)) {
                /* THE MIDDLE OF THE GESTURE, not the end of it: arm and stay up.
                 * `was_down` is already 0, so a finger arriving now starts a fresh
                 * press with nothing to be the continuation of -- the box is not
                 * waiting for the same finger to come back. The press is cleared with
                 * it: the finger has LIFTED, and a cur_cell left standing would have
                 * the painter showing a highlight for a finger that is not there (the
                 * two draw the same pair, so only the state would be wrong -- which is
                 * exactly the kind of wrong that stops being harmless the next time
                 * something else reads it). */
                armed = cell;
                press_cell = 0;
                cur_cell = 0;
                return MZ_FEED_TAKEN;
            }
            if (act)
                *act = cell_act(S, cell);
        }
        prompt_close();
        return MZ_FEED_TAKEN;
    }

    if (!was_down) {
        was_down = 1;
        press_cell = prompt_cell_at(x, y);
        cur_cell = press_cell;
        return MZ_FEED_TAKEN;
    }

    /* Still the same press: track which cell the finger is over now. This is the
     * HIGHLIGHT and nothing else -- what fires is decided at the release, from the
     * release's own coordinates. */
    cur_cell = prompt_cell_at(x, y);
    return MZ_FEED_TAKEN;
}
