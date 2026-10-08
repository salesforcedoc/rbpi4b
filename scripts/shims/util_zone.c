/*
 * util_zone.c -- touch inside rbp's UTILITY screen: drag to scroll, tap to Enter,
 * and -- while rbp has an item in edit mode -- drag to change that item's value.
 *
 * See util_zone.h for the feature, the measurements every constant comes from and
 * the rules in full. Nothing here reaches rbp, the framebuffer, the environment or
 * the clock; it is side_zone.c's shape with a value-free gesture -- one press, one
 * list, and a rotation count as the answer. The rotation count means "scroll the
 * list" or "edit the item", and rbp makes that distinction, not this file; from here
 * they are the same number on the same wire.
 */
#include "util_zone.h"

#include "menu_zone.h"       /* MZ_FEED_NONE, MZ_FEED_TAKEN */

#include <string.h>          /* memset, in util_reset() */

/* ---------------------------------------------------------------------------
 * State. Same fields as side_zone.c:34-42 would be if the drawers had one edge and
 * no controls, plus the two the scroll needs.
 *
 * `was_down` is set at EVERY down edge and cleared at every release, whether or not
 * the press was ours -- the release has to be able to tell "a press I ignored" from
 * "a release with no press behind it", and only the second is somebody else's.
 * `latched` is the narrower "this one is ours", fixed at the down edge so a press
 * that starts off the list never becomes ours when it wanders on.
 *
 * `anchor_y` is where the finger landed IN SCREEN SPACE and `sent` is how many rows
 * this press has already asked rbp to rotate. The two together are the whole of the
 * scroll: the rows travelled is `(y - anchor_y) / UTIL_ROW_H` measured from the
 * anchor rather than from the last report, and what goes out is the difference from
 * `sent` -- so a resting finger sends nothing, a wobble sends nothing, and a
 * direction change reverses by exactly the amount it overran.
 * ------------------------------------------------------------------------- */
struct util_st {
    int was_down;
    int latched;
    int anchor_x;
    int anchor_y;
    int row_at_press;   /* what the tap will aim at: the row the finger landed on */
    int sent;           /* rotations THIS press has already asked for */
    int wandered;       /* travel past UTIL_TAP_SLOP: the release is a DRAG, not a tap */
};

static struct util_st st;

static int iabs(int v)
{
    return v < 0 ? -v : v;
}

/* Is rbp on the screen this module is for, and not on a sub-screen of it? */
static int util_ready(const struct util_state *s)
{
    return s != NULL && s->mode == UTIL_MODE_UTILITY && !s->calibrating;
}

int util_row(int y)
{
    int r;

    if (y < UTIL_ROW0_Y || y > UTIL_LIST_Y1)
        return -1;
    r = (y - UTIL_ROW0_Y) / UTIL_ROW_H;
    /* The clamp is not defensive padding: UTIL_LIST_Y1 is the last row's LAST pixel,
     * so the division above cannot leave 0..UTIL_ROWS-1 for a y that passed the band
     * test. It is here to keep that true if the band is ever widened alone. */
    if (r < 0)
        r = 0;
    if (r > UTIL_ROWS - 1)
        r = UTIL_ROWS - 1;
    return r;
}

int util_in_list(int x, int y)
{
    if (x < UTIL_LIST_X0 || x > UTIL_LIST_X1)
        return 0;
    return util_row(y) >= 0;
}

int util_feed(const struct util_state *s, int down, int x, int y,
              int *act, int *value)
{
    if (act)
        *act = UTIL_ACT_NONE;
    if (value)
        *value = 0;

    if (!down) {
        int a = UTIL_ACT_NONE;
        int v = 0;

        if (!st.was_down)
            return MZ_FEED_NONE;      /* a release with no press behind it */
        st.was_down = 0;
        if (!st.latched)
            return MZ_FEED_NONE;      /* a press of somebody else's */

        /* THE TAP. Only for a press that never wandered -- a drag is not a tap, even
         * if it ends back where it started, because the list has already scrolled
         * under it and "Enter the row you touched" would then mean a row that is no
         * longer the one the operator was looking at.
         *
         * THE TRAVEL IS `row - cursor`, and the window cancels out of it: the operator
         * touched the row drawn at that y, which holds the item `initialNo + row`, and
         * the cursor is already on `initialNo + cursor` -- so the count is the same
         * whether the list is scrolled or not, and initialNo is not needed. A count of
         * zero is a real answer ("the row you touched is the one already on"), and it
         * still sends its Enter.
         *
         * WHILE EDITING THE COUNT IS ZERO ON PURPOSE. rbp's rotate edits the
         * highlighted item's value in that state, not the list, so rotating first
         * would rewrite a setting on the way to entering another row. The Enter alone
         * is what leaves edit mode -- the tap is therefore always safe, and always the
         * way back out. */
        /* THE SCREEN CAN GO AWAY UNDER A PRESS, and the release may be the FIRST
         * report to arrive after it does (the FLX4's BACK, the operator's hand --
         * no move report in between). The move path unlatches for that, and this is
         * the same refusal on the other end: without it a drag left latched across
         * a screen change would land its Enter on whatever screen is now up. rbp
         * never saw the press, so there is no release owed to it either. */
        if (!util_ready(s)) {
            util_reset();
            return MZ_FEED_NONE;
        }

        if (!st.wandered) {
            if (!s->editing) {
                v = st.row_at_press - s->cursor;
                if (v > UTIL_ROWS - 1)
                    v = UTIL_ROWS - 1;
                if (v < -(UTIL_ROWS - 1))
                    v = -(UTIL_ROWS - 1);
            }
            a = UTIL_ACT_TAP;
        }
        util_reset();
        if (act)
            *act = a;
        if (value)
            *value = v;
        return MZ_FEED_TAKEN;
    }

    if (!st.was_down) {
        /* THE DOWN EDGE. Nothing is sent here, so a tap costs exactly one report
         * however long the finger stays on the glass -- there is no per-report step
         * to starve the loop the shared reader runs on. */
        st.was_down = 1;
        st.latched = 0;
        st.wandered = 0;
        st.sent = 0;

        if (!util_ready(s) || !util_in_list(x, y))
            return MZ_FEED_NONE;
        st.latched = 1;
        st.anchor_x = x;
        st.anchor_y = y;
        st.row_at_press = util_row(y);
        return MZ_FEED_TAKEN;
    }

    /* -----------------------------------------------------------------------
     * A MOVE (or the down edge repeated, which carries no new information).
     * ----------------------------------------------------------------------- */
    if (!st.latched)
        return MZ_FEED_NONE;

    /* THE SCREEN CAN GO AWAY UNDER A PRESS. A tap's own Enter is what opens a
     * sub-screen, and a tap that opened one has already been answered -- but the
     * screen can also change under a *drag* (the FLX4's BACK, the operator's hand),
     * and a press left latched across that would swallow touches on a screen this
     * module knows nothing about. Unlatching hands the rest of the press back to rbp
     * and makes the release a NONE as well. */
    if (!util_ready(s)) {
        util_reset();
        return MZ_FEED_NONE;
    }

    /* `wandered` is a statement about the RELEASE, not about this report: it says
     * "whatever else this turns out to be, it is not a tap". The scroll below is
     * deliberately NOT gated on it -- every real drag crosses the slop long before
     * it crosses a row (24 px against 52), so gating the scroll on it would make the
     * gesture unable to scroll at all and leave a drag in the 25..51 px band doing
     * nothing whatever, which is the correct and intended outcome. */
    if (iabs(y - st.anchor_y) > UTIL_TAP_SLOP ||
        iabs(x - st.anchor_x) > UTIL_TAP_SLOP)
        st.wandered = 1;

    /* WHILE rbp IS EDITING, THE ROTATION EDITS -- and this was the other way round in
     * the first release, which is worth keeping on the record because the reversal was
     * the operator's, not a change of mind here.
     *
     * The first version refused every rotation while editing, and it was right about
     * the hazard: a rotation in that state changes the highlighted item's VALUE
     * (measured, one rotate turned LOAD LOCK from UNLOCK to LOCK), so a scroll that runs
     * while an item is in edit mode does not scroll -- it rewrites a setting, silently,
     * with no visible cause. The refusal was wrong about the remedy. It left the operator
     * able to ENTER a value row and unable to change it, which is exactly the report that
     * came back:
     *
     *     *"selecting items in utility menu also works, only issue is that i have no
     *      way of changing the values once selected"*
     *
     * And there is no gesture that avoids the trade, because it is not ours to make:
     * rbp decides what a rotation means from its own cursor mode, not from who sent it.
     * On the RX3 the same encoder both moves the highlight and edits the value, and
     * which one it is depends on whether the operator has entered edit mode. So a list
     * cannot scroll while an item is being edited, here or on the real machine. The rule
     * is the operator's own:
     *
     *     a tap enters edit mode; a drag changes the value; a tap leaves.
     *
     * The one thing kept from the refusal is on the release path: the tap's travel count
     * stays ZERO in this state (see above), so the Enter that leaves edit mode cannot
     * also drag the highlight onto whatever row the finger happened to be over. */
    {
        /* TRUNCATION TOWARD ZERO IS THE MAPPING, for both directions: -60/52 is -1
         * and -104/52 is -2, so an upward drag counts rows exactly as a downward one
         * does and no sign correction is needed. */
        int rows = (y - st.anchor_y) / UTIL_ROW_H;
        int delta = rows - st.sent;

        if (delta > UTIL_ROT_MAX)
            delta = UTIL_ROT_MAX;
        if (delta < -UTIL_ROT_MAX)
            delta = -UTIL_ROT_MAX;
        if (delta == 0)
            return MZ_FEED_TAKEN;     /* nothing new: a resting or wobbling finger */
        st.sent += delta;
        if (act)
            *act = UTIL_ACT_SCROLL;
        if (value)
            *value = delta;
        return MZ_FEED_TAKEN;
    }
}

void util_reset(void)
{
    memset(&st, 0, sizeof(st));
}
