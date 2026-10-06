/*
 * menu_zone.c -- the swipe-down top menu's geometry and gesture.
 *
 * See menu_zone.h for the gesture rules in full, the geometry's measurements and
 * the reason this is a pure module. Nothing here reaches rbp, the framebuffer or
 * the environment; it is the same shape as touch_zone.c, with one more piece of
 * state (an open/closed latch rather than a one-shot edge) because this gesture
 * has two halves.
 */
#include "menu_zone.h"

/* In column order, one per menu_key[] slot in pointsrc.c -- the two tables are read
 * together and are the same length by construction. "USB STOP" is the seventh and
 * the only one that is not a browse key; it is uppercase like the rest because the
 * font has no lowercase (menu_font.h's set). */
static const char *const labels[MZ_COLS] = {
    "SOURCE", "BROWSE", "TAG LIST", "PLAYLIST", "SEARCH", "MENU", "USB STOP"
};

/* ---------------------------------------------------------------------------
 * State. All of it is cleared by menu_reset(); the whole module is fed from
 * pointsrc.c's single ABS/SYN_REPORT site, so there is one writer and no lock
 * here -- exactly as touch_zone.c has none.
 *
 * `swallow` is the latch, and it is set once per press (at the down edge) and
 * cleared once (at the release). `armed` is narrower: it means "this press began
 * in the strip and the panel is closed", i.e. the only press that can open it.
 * `press_btn` is the button the press started on -- the anchor for
 * fire-on-release; `cur_btn` is the one under the finger now, which is what the
 * painter highlights. The two are separate on purpose: a press that slides from
 * SOURCE to BROWSE must highlight BROWSE and fire nothing, and a swipe that opens
 * the panel must start on no button at all even though it ends up over one.
 * ------------------------------------------------------------------------- */
static int was_down;      /* edge detector, touch_zone.c:60-70's shape */
static int open_state;
static int swallow;       /* rbp must not see this press, for its whole life */
static int armed;         /* this press may open the panel */
static int started_open;  /* the press began with the panel already open */
static int start_x;
static int start_y;
static int press_btn;
static int cur_btn;

static int iabs(int v)
{
    return v < 0 ? -v : v;
}

/* The seven labelled columns tile the whole logical width -- MZ_BTN_W is
 * MZ_LOGICAL_W since the web cell went (menu_zone.h's block). 1280 does not divide by
 * 7, so the widths are 182/184 and NOT equal; what is exact is the tiling, each column
 * starting one past the last one's end with no gap and no overlap. That is the
 * property menu_button_at() and menu_paint_cols() both rest on, and test_menu.c
 * asserts it at every column rather than asserting a width. */
int menu_button_x0(int i)
{
    if (i < 0)
        i = 0;
    if (i > MZ_COLS - 1)
        i = MZ_COLS - 1;
    return (i * MZ_BTN_W) / MZ_COLS;
}

int menu_button_x1(int i)
{
    if (i < 0)
        i = 0;
    if (i > MZ_COLS - 1)
        i = MZ_COLS - 1;
    return ((i + 1) * MZ_BTN_W) / MZ_COLS - 1;
}

const char *menu_label(int button)
{
    if (button < 1 || button > MZ_COLS)
        return 0;
    return labels[button - 1];
}

int menu_button_at(int x, int y)
{
    int i;

    if (y < MZ_BTN_Y0 || y > MZ_BTN_Y1)
        return 0;                     /* the strip, or the border below */
    for (i = 0; i < MZ_COLS; i++) {
        if (x >= menu_button_x0(i) && x <= menu_button_x1(i))
            return i + 1;
    }
    return 0;                         /* can only be x outside 0..1279 */
}

int menu_is_open(void)
{
    return open_state;
}

int menu_pressed(void)
{
    return open_state ? cur_btn : 0;
}

int menu_hold_fires(int held_ms, int threshold_ms)
{
    if (threshold_ms <= 0)
        return 0;              /* the hold is off: every press is a plain tap */
    if (held_ms < 0)
        return 0;              /* no measurement for this press: not a hold */
    return held_ms >= threshold_ms;
}

int menu_hold_pending(int held_ms, int threshold_ms)
{
    int btn;

    /* Only a press this module owns, on the panel it owns, still down, and not
     * already answered. `press_btn` is non-zero exactly for a press that began with
     * the panel open AND landed on a button -- so the swipe that opened the panel
     * and a press on the panel's own background are both excluded, which is what
     * keeps "a finger is on the glass" from being the same question as "a finger is
     * holding a button". `cur_btn == press_btn` is the slide-off rule the release
     * path has, applied one press earlier: a finger that drifted onto the next
     * column is not holding the button it started on. */
    if (!open_state || !swallow || !press_btn || cur_btn != press_btn)
        return 0;
    if (!menu_hold_fires(held_ms, threshold_ms))
        return 0;

    btn = press_btn;

    /* Close the panel HERE rather than leaving it for the caller: the whole point
     * is that the panel is gone at this moment and not at the release. It is also
     * what makes a second call a no-op -- with open_state clear the guard above
     * cannot pass again -- and what makes the release still to come the silent
     * swallow it already was: `started_open` finds a closed panel and no button, so
     * menu_feed()'s release arm fires nothing and returns MZ_FEED_TAKEN. The press
     * stays swallowed to its end, so rbp hears neither edge. */
    open_state = 0;
    press_btn = 0;
    cur_btn = 0;
    return btn;
}

void menu_tap_point(int *x, int *y)
{
    if (x)
        *x = start_x;
    if (y)
        *y = start_y;
}

void menu_reset(void)
{
    was_down = 0;
    open_state = 0;
    swallow = 0;
    armed = 0;
    started_open = 0;
    start_x = 0;
    start_y = 0;
    press_btn = 0;
    cur_btn = 0;
}

int menu_feed(int down, int x, int y, int *button)
{
    if (button)
        *button = 0;

    if (!down) {
        if (!was_down)
            return MZ_FEED_NONE;      /* a release we never saw the press for */
        was_down = 0;
        if (swallow) {
            int tap = 0;

            /* The panel fires on the release and only for a press that started
             * and ended on the same button. `press_btn` is 0 for any press that
             * began while the panel was closed, so the swipe that opened it can
             * never fire -- which is the whole reason it is captured at the down
             * edge rather than recomputed here.
             *
             * And a press that began while CLOSED does not close the panel: that
             * is the swipe that opened it, and the operator asked for a toggle --
             * the panel stays until a button is tapped or it is dismissed. Only a
             * press that began with the panel already out is a press *on* the
             * panel, and only that one dismisses it. */
            if (started_open) {
                if (open_state && press_btn && cur_btn == press_btn && button)
                    *button = press_btn;
                open_state = 0;
            } else if (!open_state) {
                /* Started in the strip and the panel is shut, so this press never
                 * travelled far enough to be a swipe: it is a tap, and the only
                 * report the menu takes and gives back. menu_zone.h states why
                 * `!open_state` is the whole of "and never opened the panel"; the
                 * short of it is that a press beginning in the strip cannot
                 * satisfy the dismissal rule, so open_state can only be 1 here if
                 * this very press opened it -- and then it is a swipe, not a tap,
                 * and rbp gets nothing. */
                tap = 1;
            }
            swallow = 0;
            armed = 0;
            started_open = 0;
            press_btn = 0;
            cur_btn = 0;
            return tap ? MZ_FEED_TAP : MZ_FEED_TAKEN;
        }
        armed = 0;
        started_open = 0;
        return MZ_FEED_NONE;
    }

    if (!was_down) {
        /* The down edge: decide once whether this press is ours. */
        was_down = 1;
        start_x = x;
        start_y = y;
        started_open = open_state;
        if (open_state) {
            /* Open: every press is ours, wherever it lands -- a press that
             * dismisses the panel must not also press what is under it. */
            swallow = 1;
            armed = 0;
            press_btn = menu_button_at(x, y);
            cur_btn = press_btn;
        } else if (y <= MZ_STRIP_Y1 && MZ_ENTRY_IN(x)) {
            /* The entry zone, and it is the ONLY arm the x bounds apply to.
             * Everything below (a press that started outside) and everything above
             * (a press that started with the panel open) is decided by y alone.
             * The open arm must stay full width: the panel's own leftmost and
             * rightmost columns are SOURCE and MENU, and a press on either of them
             * that fell through to rbp instead would be a dead button. */
            swallow = 1;
            armed = 1;
            press_btn = 0;
            cur_btn = 0;
        } else {
            /* Not ours, and never becomes ours: a press that starts outside is
             * returned untouched end to end (menu_zone.h's latch rule). */
            swallow = 0;
            armed = 0;
            press_btn = 0;
            cur_btn = 0;
        }
        return swallow ? MZ_FEED_TAKEN : MZ_FEED_NONE;
    }

    /* Still the same press. Track the highlight, and make or unmake the gesture.
     * Both the open and the close are required to be predominantly vertical --
     * a drag along the strip is not a swipe, and a sideways drag while open is
     * not a dismissal. */
    if (armed) {
        int dy = y - start_y;
        int dx = iabs(x - start_x);

        if (dy >= MZ_SWIPE_PX && dy > dx) {
            open_state = 1;
            armed = 0;
            /* press_btn stays 0: the finger is over the panel now, but the press
             * began above it, so lifting here opens and does nothing else. */
            cur_btn = menu_button_at(x, y);
        }
        return MZ_FEED_TAKEN;         /* armed implies swallowed */
    }

    if (swallow && open_state) {
        if (start_y - y >= MZ_CLOSE_PX && start_y - y > iabs(x - start_x)) {
            /* Dismissed mid-press. The press is still ours to the end -- the
             * release finds open_state clear and fires nothing. */
            open_state = 0;
            press_btn = 0;
            cur_btn = 0;
            return MZ_FEED_TAKEN;
        }
        cur_btn = menu_button_at(x, y);
        return MZ_FEED_TAKEN;
    }

    return swallow ? MZ_FEED_TAKEN : MZ_FEED_NONE;
}
