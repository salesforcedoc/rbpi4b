/*
 * prompt_zone.c -- the USB STOP chooser's geometry and gesture.
 *
 * See prompt_zone.h for what it is for, where the two devices come from and why
 * this is pure. Nothing here reaches rbp, the framebuffer or a clock; the whole of
 * the module is the box, the hold and the arithmetic that turns a press into an
 * answer.
 *
 * THE GRID IS TWO CELLS AND THE GESTURE IS ONE THREE-SECOND HOLD. `USB 1` and
 * `USB 2` are the buttons, a press on a live one starts the hold and blinks it, and
 * the only way to reach an eject is to still be holding three seconds later. The hold
 * lives here and not in the caller because it is the same latch the painter reads
 * (prompt_hold_cell(), prompt_hold_flash()) -- a caller holding it would have to be
 * asked on every repaint anyway, and this module already owns the one press in flight.
 */
#include "prompt_zone.h"
#include "menu_font.h"    /* menu_font_adv(): how wide a name draws, which is what
                           * decides how much of it a button can hold. A compile-time
                           * table and nothing else -- see prompt_zone.h's PURE note. */

/* The cells in cell-number order, which is PROMPT_ROWS x PROMPT_COLS row-major --
 * see prompt_zone.h's PR_CELL_* enum. These are the DEFAULTS: what a cell reads when
 * the device in it has no name of its own (prompt_cell_text()), and the yardstick the
 * painter measures the button against (prompt_cell_label()). A character the font
 * cannot draw would ship as a gap rather than as a failure, which is why test_prompt.c
 * walks every character of every one of these against menu_font.h. */
static const char *const cell_defaults[PROMPT_CELLS] = {
    "HOLD USB 1", "HOLD USB 2"
};

/* The word that says what the gesture is, in front of whichever name follows. It is
 * part of the label and never dropped by the clip: at 53 px it is a third of the
 * narrowest budget prompt_text_clip() can be handed, since a view only reaches this box
 * if it hosts the default label whole (PR_LABEL_MARGIN). */
#define PR_LABEL_PREFIX "HOLD "

/* ---------------------------------------------------------------------------
 * State. All of it is cleared by prompt_reset(); the touch thread is the only
 * writer, exactly as it is for menu_zone.c's panel, and menu_draw.c only reads
 * prompt_is_open(), prompt_pressed(), prompt_hold_cell() and prompt_hold_flash().
 *
 * `was_down` is the edge detector every feeder in this shim carries
 * (touch_zone.c:60-70's shape) and here it does a second job as well -- see the
 * expiry guard in prompt_tick(). `press_cell` is the anchor and `cur_cell` is what
 * the painter highlights, and the two are separate on purpose: a finger that slides
 * from USB 1 to USB 2 must highlight USB 2 and hold nothing, which is also
 * menu_zone.c's rule.
 *
 * `hold_cell` IS THE ONE PIECE OF STATE THAT OUTLIVES A PRESS REPORT, and it is the
 * whole of what makes the eject reachable. It is set only by a press on a cell rbp
 * says is live, and it is cleared by the release, by the finger leaving the cell, by
 * a tick that finds the device gone, and by the close -- so "there is a hold" and
 * "there is a live device under the finger that has not moved" cannot disagree.
 *
 * `flash_on` is the blink, and it is STATE rather than a function of a clock for the
 * reason prompt_zone.h gives: this module has no clock, so the phase is advanced in
 * prompt_tick() and read back by the painter.
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
static int hold_cell;             /* PR_CELL_USB1, PR_CELL_USB2, or 0 */
static unsigned long long hold_since_ms;
static int flash_on;              /* 1 while the held button draws PRESSED */
static unsigned long long deadline_ms;

/* The clock is the caller's and it is 64 bits wide on purpose: `long` is four bytes
 * on this target and a monotonic millisecond count in one wraps every 4.29 s, which
 * is the trap pointsrc.c's own shim_now_ms() documents. Every comparison below is
 * written so it does not care where the two values sit on the ring -- each one is a
 * signed difference against a stored instant, never a comparison of two instants. */
static int expired(unsigned long long now_ms)
{
    return (long long)(now_ms - deadline_ms) >= 0;
}

/* Has the running hold reached the operator's three seconds yet? */
static int hold_reached(unsigned long long now_ms)
{
    return (long long)(now_ms - hold_since_ms - PR_HOLD_MS) >= 0;
}

/* Which half of the blink the hold is in. PRESSED first and for a whole half period,
 * so a tap -- however short -- still shows the operator that their finger landed on
 * something live. */
static int flash_phase(unsigned long long now_ms)
{
    return ((now_ms - hold_since_ms) / PR_HOLD_FLASH_MS) & 1ull ? 0 : 1;
}

int prompt_cell_live(const struct prompt_state *S, int cell)
{
    /* TWO guards and not one, because the two arrays are sized by two different
     * constants and only today's box happens to make them equal: PROMPT_CELLS is what
     * the painter and the hit test loop over, PROMPT_DEVICES is how far pointsrc.c's
     * walk of rbp's managers goes. A cell past the devices is not a live one, whatever
     * it is. */
    if (cell < 1 || cell > PROMPT_CELLS || cell > PROMPT_DEVICES)
        return 0;
    return S && S->live[cell - 1] ? 1 : 0;
}

/* The answer a cell gives when its hold completes. A dim cell answers NOTHING, and
 * that is not a refusal to answer -- it is the whole point of the dim button. What it
 * must never do is send the eject anyway and let rbp's own press branch drop it at
 * `[this+0x88] != 2`, which would look from the outside exactly like a working button
 * that does nothing. */
static int cell_act(const struct prompt_state *S, int cell)
{
    if (!prompt_cell_live(S, cell))
        return PR_ACT_NONE;
    switch (cell) {
    case PR_CELL_USB1:    return PR_ACT_USB1;
    case PR_CELL_USB2:    return PR_ACT_USB2;
    default:              return PR_ACT_NONE;
    }
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
    return cell_defaults[cell - 1];
}

void prompt_state_name(struct prompt_state *S, int dev, const char *name)
{
    int k = 0, i;

    if (!S || dev < 0 || dev >= PROMPT_DEVICES)
        return;

    /* THE EMPTY ANSWER COMES FIRST, and it is the whole of "this cell has no name of
     * its own": prompt_cell_text() reads it straight back as the cell's default. A
     * stick that has been pulled leaves its cell here, not holding the name it had. */
    S->label[dev][0] = '\0';
    if (!name || !name[0])
        return;

    for (i = 0; PR_LABEL_PREFIX[i] && k < PR_LABEL_MAX; i++)
        S->label[dev][k++] = PR_LABEL_PREFIX[i];

    /* THE ATLAS'S OWN QUESTION, ASKED OF EVERY CHARACTER. usb_label.h's sanitizer has
     * already dealt with everything outside printable ASCII -- udev's LABEL is UTF-8
     * and a decoded name would draw as boxes -- and what is left is the printable
     * characters the 19 px face simply does not carry: ( ) ! , % + ' ~ $ " ; < > [ ]
     * ^ { | }. menu_font.c draws each of those as a 5 px GAP, which is a name that is
     * silently not the one on the stick. A '?' is in the atlas and says so instead. */
    for (i = 0; name[i] && k < PR_LABEL_MAX; i++) {
        unsigned char c = (unsigned char)name[i];

        S->label[dev][k++] = menu_font_index(c) < 0 ? '?' : (char)c;
    }
    S->label[dev][k] = '\0';
}

const char *prompt_cell_text(const struct prompt_state *S, int cell)
{
    if (cell < 1 || cell > PROMPT_CELLS)
        return 0;
    if (S && S->label[cell - 1][0])
        return S->label[cell - 1];
    return cell_defaults[cell - 1];
}

int prompt_text_clip(const char *text, int max_px, char *out, int max)
{
    int w = 0, k = 0;

    if (!out || max <= 0)
        return 0;
    out[0] = '\0';
    if (!text || max_px <= 0)
        return 0;

    /* One character at a time, against the same advances pp_text() will walk, so the
     * string the painter centres is exactly the string measured here. Each kept
     * character is charged its own advance AND the last one kept carries the whole of
     * the running total, so the answer is never wider than the budget -- including the
     * last glyph's own advance, which is what pp_text_center() centres by. A budget that
     * cannot hold even one character answers 0, which is an empty label and not an
     * overflow; the box cannot be handed one, because it is not painted at all unless it
     * hosts the default label whole (PR_LABEL_MARGIN). */
    while (text[k] && k < max - 1) {
        int adv = menu_font_adv((unsigned char)text[k], (unsigned char)text[k + 1]);

        if (w + adv > max_px)
            break;
        w += adv;
        out[k] = text[k];
        k++;
    }
    out[k] = '\0';
    return k;
}

void prompt_open(unsigned long long now_ms)
{
    open_state = 1;
    deadline_ms = now_ms + PR_TIMEOUT_MS;
    /* No press is inherited. The tap that raised this is already over -- it fired
     * on a release -- and a caller that raised it from anywhere else still must not
     * adopt a finger it never saw go down, which is menu_zone.c's `started_open`
     * rule in its simplest form: there is nothing to be the continuation of. Neither
     * is a hold, for the reason prompt_zone.h gives. */
    was_down = 0;
    press_cell = 0;
    cur_cell = 0;
    hold_cell = 0;
    flash_on = 0;
}

void prompt_close(void)
{
    open_state = 0;
    was_down = 0;
    press_cell = 0;
    cur_cell = 0;
    hold_cell = 0;
    flash_on = 0;
}

int prompt_is_open(void)
{
    return open_state;
}

int prompt_pressed(void)
{
    if (!open_state)
        return 0;
    return hold_cell ? 0 : cur_cell;
}

int prompt_hold_cell(void)
{
    return open_state ? hold_cell : 0;
}

int prompt_hold_flash(void)
{
    return open_state && hold_cell ? flash_on : 0;
}

unsigned long long prompt_hold_ms(unsigned long long now_ms)
{
    if (!open_state || !hold_cell)
        return 0;
    return now_ms - hold_since_ms;
}

unsigned long long prompt_deadline(void)
{
    return deadline_ms;
}

int prompt_tick(const struct prompt_state *S, unsigned long long now_ms, int *act)
{
    if (act)
        *act = PR_ACT_NONE;

    if (!open_state)
        return PR_TICK_NONE;

    /* THE HOLD FIRST, and before the timeout -- a hold that completes at the same
     * instant the box was due to expire is still a completed hold. Three seconds of a
     * finger on the button is the operator's decision, and it does not become nothing
     * because they started it late in the box's ten. */
    if (hold_cell && hold_reached(now_ms)) {
        int a = cell_act(S, hold_cell);

        if (a != PR_ACT_NONE) {
            if (act)
                *act = a;
            /* THE BOX DOES NOT CLOSE HERE, and the reason is the release still to come.
             * The finger that made this hold is still down -- that is what a hold IS --
             * so a release report is on its way, and the box has swallowed every report
             * of that press from the beginning. Closing now would clear `was_down`, and
             * prompt_feed() would then hand the release to rbp as an up for a down it
             * never saw ([[declined-press-must-still-see-release]]).
             *
             * So the press is DISOWNED instead: the anchor, the highlight and the hold all
             * go, and the release finds a press with nothing anchored to it and takes the
             * ordinary "began off every button" path, which closes the box and sends
             * nothing. The hold cannot re-fire either, since there is no hold left. */
            press_cell = 0;
            cur_cell = 0;
            hold_cell = 0;
            flash_on = 0;
            return PR_TICK_EJECT;
        }
        /* rbp says the device went away while the finger was on it. The hold is off,
         * nothing is sent, and the box stays up -- the operator is looking at a button
         * that has just gone dim and may want the other one. */
        hold_cell = 0;
        flash_on = 0;
    }

    /* NOT WHILE A FINGER IS DOWN, and this is the one rule here that has consequences
     * outside this file. A press the box swallowed has been withheld from rbp for its
     * whole life, and rbp is going to be given its release. Closing the box mid-press
     * would clear `was_down`, so the release would be handed back as a report with no
     * press behind it -- a release rbp never saw the press for
     * ([[declined-press-must-still-see-release]] is the same rule the fan-out feeders
     * keep). Deferring instead costs nothing: the release closes the box on its own
     * path a moment later, or leaves it up if it began on a button. */
    if (!was_down && expired(now_ms)) {
        prompt_close();
        return PR_TICK_TIMEOUT;
    }

    flash_on = hold_cell ? flash_phase(now_ms) : 0;
    return PR_TICK_FLASH;
}

void prompt_reset(void)
{
    prompt_close();
    deadline_ms = 0;
}

int prompt_feed(const struct prompt_state *S, int down, int x, int y,
                unsigned long long now_ms, int *act)
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
         * the finger was, and a release that moved must not hold for the cell the
         * finger has already left. `press_cell` is 0 for a press that began off a cell --
         * the title, a gap, the glass outside the box -- so a miss can never hold, and it
         * is the only kind of release that dismisses. */
        cell = prompt_cell_at(x, y);

        /* A HOLD THAT COMPLETED WITH NO TICK IN BETWEEN. The caller slices its wait while
         * the box is up, so this is nearly unreachable -- which is exactly why it is
         * written: the three seconds are a property of the clock, and a release that
         * arrives at 3001 ms must not be refused because nobody looked at 3000. */
        if (press_cell && cell == press_cell && hold_cell == press_cell &&
            hold_reached(now_ms)) {
            int a = cell_act(S, press_cell);

            if (a != PR_ACT_NONE) {
                if (act)
                    *act = a;
                prompt_close();
                return MZ_FEED_TAKEN;
            }
            /* The hold completed on a device rbp has just reported gone: fall through
             * and treat it as the ordinary release it is, which sends nothing and
             * leaves the box up. */
        }

        if (!press_cell) {
            /* Began off every button: this is "tap outside to dismiss", and the only
             * release in the module that puts the box away without stopping anything. */
            prompt_close();
            return MZ_FEED_TAKEN;
        }

        /* BEGAN ON A BUTTON. Whether the hold completed, was let go early, or was on a
         * cell that is dim, the box STAYS UP: it is not dismissed by a press that landed
         * on it. The press is cleared with it -- the finger has LIFTED, and a cur_cell
         * left standing would have the painter showing a highlight for a finger that is
         * not there. */
        press_cell = 0;
        cur_cell = 0;
        hold_cell = 0;
        flash_on = 0;
        return MZ_FEED_TAKEN;
    }

    if (!was_down) {
        was_down = 1;
        press_cell = prompt_cell_at(x, y);
        cur_cell = press_cell;
        /* A HOLD STARTS ONLY ON A LIVE BUTTON. rbp's answer is asked here, at the press,
         * and then again at the fire -- so a device that goes away mid-hold is refused by
         * the tick and not by a stale latch (prompt_cell_live() is the same test
         * UsbStorageManager::onKey's own press branch makes). */
        if (press_cell && prompt_cell_live(S, press_cell)) {
            hold_cell = press_cell;
            hold_since_ms = now_ms;
            flash_on = 1;         /* show the press before the first tick arrives */
        } else {
            hold_cell = 0;
        }
        return MZ_FEED_TAKEN;
    }

    /* Still the same press: track which cell the finger is over now. This is the
     * HIGHLIGHT and nothing else -- what fires is decided by the hold, and the hold is
     * anchored to where the press STARTED.
     *
     * LEAVING THE BUTTON CANCELS THE HOLD, and it does not come back on a return. A hold
     * is a finger held still on one button; a finger that wanders off and back is a
     * different gesture, and letting it resume would make the anchor a thing the finger
     * could leave and return to at no cost -- which is one step from an eject the
     * operator did not watch themselves make. */
    cur_cell = prompt_cell_at(x, y);
    if (hold_cell && cur_cell != hold_cell) {
        hold_cell = 0;
        flash_on = 0;
    }
    return MZ_FEED_TAKEN;
}
