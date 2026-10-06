/*
 * menu_window.c -- the window's state, hit test and request ring. See menu_window.h.
 *
 * One writer, no lock, exactly as menu_zone.c: the touch thread writes the state
 * and the paint thread reads it, and every field is a single int, so a paint that
 * races a tap sees the state before or after it and never a torn one. Nothing here
 * allocates, opens a file or looks at a clock.
 *
 * THE ONE EXCEPTION IS THE REQUEST RING, and it is stated rather than hidden: the
 * text of a request is several bytes crossing from the touch thread to the tick
 * thread, so it is a single-producer / single-consumer ring with the head published
 * after the slot is written and the tail published after it is read. The indices are
 * each written by one thread, which is all a ring's counter needs; the text is
 * never read until the head says it is there. See menu_window_take_req().
 */
#include "menu_window.h"
#include "menu_keyboard.h"

#include <string.h>             /* strcmp, for the repaint gate in set_url() */

static int mw_state = MW_CLOSED;

/* The finger, and where the press behind it began. Both are the touch thread's;
 * the paint thread only reads mw_pressed, and only for the highlight. */
static int mw_pressed = MW_HIT_NONE;
static int mw_down_hit = MW_HIT_NONE;
static int mw_down;

/* Which KEY the finger is on, or -1. The chrome's highlight is the cell above; the
 * keyboard's is this one, because the keyboard is fifty marks in one cell. */
static int mw_key_pressed = -1;

/* The KEY the press began on, or -1 -- the latch the fifty-caps-one-cell problem
 * needs. Every key is MW_HIT_KEYBOARD, so the cell comparison in the release path
 * cannot tell a finger that stayed on 'q' from one that slid onto 'w' before
 * lifting; without this, the cap that lights under the finger and the key that
 * fires are two different keys. menu_window.h states the rule. */
static int mw_down_key = -1;

/* Where the finger went down, in logical space: the page click a tap on the content
 * sends is the PRESS's coordinate, not the release's, because that is the point the
 * operator aimed at and menu_zone.c's roll-off rule has already rejected a release
 * that moved to another cell. */
static int mw_down_x, mw_down_y;

/* THE DRAG, which is a press that kept moving. `down` is true for every report of a
 * held finger, so a press in the page is not one call but a run of them, and the
 * difference between a scroll and a click is only how far that run went. mw_move_x/y
 * is where the last report was (the deltas are per-report), mw_travel is |dx| + |dy|
 * summed over the whole gesture, mw_acc_x/y is the travel not yet sent, and
 * mw_scrolled latches once MW_TAP_SLOP is passed so the release knows to drop the
 * click. menu_window.h states the rule. */
static int mw_move_x, mw_move_y;
static int mw_travel;
static int mw_acc_x, mw_acc_y;
static int mw_scrolled;

/* The edit buffer, what it shows when the operator is not editing, and the
 * selection flag. menu_window.h explains why the buffer is ours and a page input's
 * text is not. */
static char mw_buf[MW_URL_MAX];
static int  mw_buf_len;
static int  mw_sel;
static char mw_live[MW_URL_MAX];
static int  mw_target;

/* The page's focus generation, and the last one acted on. */
static int  mw_focus_seen;

/* Bumped by anything that changes what the field SHOWS -- the buffer, the selection,
 * the target and the live URL. The painter is a pure function of the window's state,
 * but the state is now a string as well as a few ints, and comparing two strings a
 * hundred times a second to decide whether to repaint is the kind of thing that ends
 * up costing more than the repaint. A counter is one int and cannot be wrong about
 * whether something changed. */
static unsigned mw_seq;

static void mw_touch(void)
{
    mw_seq++;
}

unsigned menu_window_field_seq(void)
{
    return mw_seq;
}

/* The request ring. */
static struct mw_req mw_ring[MW_REQ_RING];
static volatile unsigned mw_head, mw_tail;

static void mw_push(int kind, const char *text, int x, int y)
{
    unsigned h = mw_head;
    struct mw_req *r;

    if (h - mw_tail >= MW_REQ_RING)
        /* Full. Dropping is the honest answer -- blocking the touch thread on the
         * browser would stall the panel, and the tick drains this every frame, so
         * eight taps between two ticks is a browser that has already stopped. */
        return;
    r = &mw_ring[h % MW_REQ_RING];
    r->kind = kind;
    r->x = x;
    r->y = y;
    r->text[0] = '\0';
    if (text) {
        int i = 0;

        while (text[i] && i < MW_REQ_TEXT_MAX - 1) {
            r->text[i] = text[i];
            i++;
        }
        r->text[i] = '\0';
    }
    mw_head = h + 1;        /* published LAST: the text is readable when it lands */
}

int menu_window_take_req(struct mw_req *out)
{
    unsigned t = mw_tail;
    struct mw_req *r;

    if (t == mw_head)
        return 0;
    r = &mw_ring[t % MW_REQ_RING];
    if (out)
        *out = *r;
    mw_tail = t + 1;        /* published LAST: the slot is free when it lands */
    return 1;
}

/* --- the edit buffer ------------------------------------------------------- */

static void mw_buf_set(const char *s)
{
    int i = 0;

    if (s) {
        while (s[i] && i < MW_URL_MAX - 1) {
            mw_buf[i] = s[i];
            i++;
        }
    }
    mw_buf[i] = '\0';
    mw_buf_len = i;
    mw_touch();
}

static void mw_buf_ins(char c)
{
    /* A selected field is REPLACED by the first character, which is what makes
     * typing a new address one tap rather than one tap and thirty backspaces. */
    if (mw_sel) {
        mw_buf_len = 0;
        mw_sel = 0;
    }
    if (mw_buf_len >= MW_URL_MAX - 1)
        return;
    mw_buf[mw_buf_len++] = c;
    mw_buf[mw_buf_len] = '\0';
    mw_touch();
}

static void mw_buf_back(void)
{
    if (mw_sel) {
        mw_buf_len = 0;
        mw_sel = 0;
    } else if (mw_buf_len > 0) {
        mw_buf_len--;
    }
    mw_buf[mw_buf_len] = '\0';
    mw_touch();
}

/* The target and the selection, together, because the field draws both and every
 * change to either has to repaint. */
static void mw_target_set(int target, int sel)
{
    if (mw_target != target || mw_sel != sel)
        mw_touch();
    mw_target = target;
    mw_sel = sel;
}

/* --- state ----------------------------------------------------------------- */

int menu_window_state(void)
{
    return mw_state;
}

int menu_window_is_open(void)
{
    return mw_state != MW_CLOSED;
}

void menu_window_open(void)
{
    mw_state = MW_NORMAL;
}

void menu_window_close(void)
{
    /* Closing is the end of the interaction, keyboard included: a keyboard left up
     * over a page nobody can see is a state with no way back to it. */
    mw_state = MW_CLOSED;
    mw_target_set(MW_EDIT_NONE, 0);
    menu_keyboard_hide();
}

void menu_window_minimize(void)
{
    /* Only from NORMAL. Minimizing a closed window must not conjure a title bar
     * onto the glass, which is the one way this state machine could put something
     * up that the operator never asked for. */
    if (mw_state == MW_NORMAL) {
        mw_state = MW_MIN;
        /* The keyboard goes with it. The minimized window is a title bar, and a
         * keyboard floating under a title bar with no page above it is furniture
         * the operator would have to work out how to dismiss. */
        mw_target_set(MW_EDIT_NONE, 0);
        menu_keyboard_hide();
    }
}

void menu_window_restore(void)
{
    if (mw_state == MW_MIN)
        mw_state = MW_NORMAL;
}

void menu_window_reset(void)
{
    mw_state = MW_CLOSED;
    mw_pressed = MW_HIT_NONE;
    mw_down_hit = MW_HIT_NONE;
    mw_down = 0;
    mw_down_x = mw_down_y = 0;
    mw_key_pressed = -1;
    mw_down_key = -1;
    mw_move_x = mw_move_y = 0;
    mw_travel = mw_acc_x = mw_acc_y = 0;
    mw_scrolled = 0;
    mw_target_set(MW_EDIT_NONE, 0);
    mw_buf_set("");
    mw_live[0] = '\0';
    mw_focus_seen = 0;
    /* The ring is emptied by the consumer, so it is the tail that is moved to the
     * head here rather than the head back to the tail: a request recorded before a
     * reset is still a request the operator made. */
    mw_tail = mw_head;
    menu_keyboard_hide();
}

int menu_window_target(void)
{
    return mw_target;
}

const char *menu_window_field(void)
{
    return (mw_target == MW_EDIT_URL) ? mw_buf : mw_live;
}

int menu_window_field_sel(void)
{
    return mw_target == MW_EDIT_URL && mw_sel;
}

void menu_window_set_url(const char *s)
{
    char next[MW_URL_MAX];
    int i = 0;

    if (!s)
        s = "";
    while (s[i] && i < MW_URL_MAX - 1) {
        next[i] = s[i];
        i++;
    }
    next[i] = '\0';
    /* THE SEQUENCE IS THE REPAINT GATE, so this has to bump it -- the field shows
     * the live URL whenever the operator is not editing, and a page that navigated
     * on its own would otherwise keep the old address on the glass until something
     * else happened to ask for a repaint. Only on a real change: this is called
     * every tick, and a bump every tick is a repaint every tick. */
    if (strcmp(next, mw_live) == 0)
        return;
    memcpy(mw_live, next, (size_t)i + 1);
    mw_touch();
}

void menu_window_page_input(int focus)
{
    if (focus == mw_focus_seen)
        return;                 /* the same generation: nothing happened */
    mw_focus_seen = focus;
    if (focus > 0) {
        /* The operator editing the address is left alone: the page re-focusing
         * under them must not take the field away. */
        if (mw_target != MW_EDIT_URL) {
            mw_target_set(MW_EDIT_PAGE, 0);
            menu_keyboard_show();
        }
    } else if (mw_target == MW_EDIT_PAGE) {
        mw_target_set(MW_EDIT_NONE, 0);
        menu_keyboard_hide();
    }
}

int menu_window_rect(int *x, int *y, int *w, int *h)
{
    if (mw_state == MW_CLOSED)
        return 0;
    if (x) *x = MW_X;
    if (y) *y = MW_Y;
    if (w) *w = MW_W;
    /* Minimized is the title bar alone -- same origin, same width, one bar tall.
     * The plane follows this: menu_draw.c re-sets it up at this height, so the
     * minimized window is a smaller buffer on the glass, not a big one with its
     * bottom half ignored. */
    if (h) *h = (mw_state == MW_MIN) ? MW_TITLE_H : MW_H;
    return 1;
}

void menu_window_title_rect(int *x0, int *y0, int *x1, int *y1)
{
    if (x0) *x0 = MW_X;
    if (y0) *y0 = MW_Y;
    if (x1) *x1 = MW_X + MW_W;
    if (y1) *y1 = MW_Y + MW_TITLE_H;
}

/* The two chrome boxes, named left-to-right as the operator sees them: minimize
 * then close. Kept as one function so the hit test and the painter cannot drift
 * apart -- the painter asks these same two rectangles where to put its strokes. */
static void mw_close_rect(int *x0, int *y0, int *x1, int *y1)
{
    int rx = MW_X + MW_W - MW_BTN_GAP - MW_BTN;
    if (x0) *x0 = rx;
    if (y0) *y0 = MW_Y + MW_BTN_TOP;
    if (x1) *x1 = rx + MW_BTN;
    if (y1) *y1 = MW_Y + MW_BTN_TOP + MW_BTN;
}

static void mw_min_rect(int *x0, int *y0, int *x1, int *y1)
{
    int rx = MW_X + MW_W - MW_BTN_GAP - MW_BTN - MW_BTN_GAP - MW_BTN;
    if (x0) *x0 = rx;
    if (y0) *y0 = MW_Y + MW_BTN_TOP;
    if (x1) *x1 = rx + MW_BTN;
    if (y1) *y1 = MW_Y + MW_BTN_TOP + MW_BTN;
}

/* The back and forward boxes, in the URL row: left-to-right, back then forward.
 * Same one-definition rule as the two above. */
static void mw_back_rect(int *x0, int *y0, int *x1, int *y1)
{
    int rx = MW_X + MW_NAV_X0;
    if (x0) *x0 = rx;
    if (y0) *y0 = MW_Y + MW_TITLE_H + MW_NAV_TOP;
    if (x1) *x1 = rx + MW_NAV;
    if (y1) *y1 = MW_Y + MW_TITLE_H + MW_NAV_TOP + MW_NAV;
}

static void mw_fwd_rect(int *x0, int *y0, int *x1, int *y1)
{
    int rx = MW_X + MW_NAV_X0 + MW_NAV + MW_NAV_GAP;
    if (x0) *x0 = rx;
    if (y0) *y0 = MW_Y + MW_TITLE_H + MW_NAV_TOP;
    if (x1) *x1 = rx + MW_NAV;
    if (y1) *y1 = MW_Y + MW_TITLE_H + MW_NAV_TOP + MW_NAV;
}

/* The painter draws the boxes exactly where they are hit, so it asks these rather
 * than repeating the arithmetic. Both are pure functions of the constants, so the
 * paint thread cannot disagree with the touch thread's answer. */
void menu_window_close_box(int *x0, int *y0, int *x1, int *y1)
{
    mw_close_rect(x0, y0, x1, y1);
}

void menu_window_min_box(int *x0, int *y0, int *x1, int *y1)
{
    mw_min_rect(x0, y0, x1, y1);
}

void menu_window_back_box(int *x0, int *y0, int *x1, int *y1)
{
    mw_back_rect(x0, y0, x1, y1);
}

void menu_window_fwd_box(int *x0, int *y0, int *x1, int *y1)
{
    mw_fwd_rect(x0, y0, x1, y1);
}

void menu_window_url_rect(int *x0, int *y0, int *x1, int *y1)
{
    if (x0) *x0 = MW_X;
    if (y0) *y0 = MW_Y + MW_TITLE_H;
    if (x1) *x1 = MW_X + MW_W;
    if (y1) *y1 = MW_Y + MW_CONTENT_Y;
}

void menu_window_content_rect(int *x0, int *y0, int *x1, int *y1)
{
    if (x0) *x0 = MW_X;
    if (y0) *y0 = MW_Y + MW_CONTENT_Y;
    if (x1) *x1 = MW_X + MW_W;
    if (y1) *y1 = MW_Y + MW_H;
}

int menu_window_hit(int lx, int ly)
{
    int x0, y0, x1, y1;

    if (mw_state == MW_CLOSED)
        return MW_HIT_NONE;
    if (lx < MW_X || lx >= MW_X + MW_W)
        return MW_HIT_NONE;

    /* Minimized: the window IS its title bar, so the two boxes are live and
     * everything else is the title. A tap on the bar restores -- that is the way
     * back from a minimize that does not rely on remembering anything. */
    if (mw_state == MW_MIN) {
        if (ly < MW_Y || ly >= MW_Y + MW_TITLE_H)
            return MW_HIT_NONE;
        mw_close_rect(&x0, &y0, &x1, &y1);
        if (lx >= x0 && lx < x1 && ly >= y0 && ly < y1)
            return MW_HIT_CLOSE;
        mw_min_rect(&x0, &y0, &x1, &y1);
        if (lx >= x0 && lx < x1 && ly >= y0 && ly < y1)
            return MW_HIT_MIN;
        return MW_HIT_TITLE;
    }

    if (ly < MW_Y || ly >= MW_Y + MW_H)
        return MW_HIT_NONE;

    mw_close_rect(&x0, &y0, &x1, &y1);
    if (lx >= x0 && lx < x1 && ly >= y0 && ly < y1)
        return MW_HIT_CLOSE;
    mw_min_rect(&x0, &y0, &x1, &y1);
    if (lx >= x0 && lx < x1 && ly >= y0 && ly < y1)
        return MW_HIT_MIN;

    if (ly < MW_Y + MW_TITLE_H)
        return MW_HIT_TITLE;

    /* THE TWO NAV BOXES, ASKED BEFORE THE FIELD THEY SIT IN. They are inside the URL
     * row's rect, so asking the field first would make them unreachable: every tap
     * on them would raise the keyboard. Row-then-box is the same order the two title
     * boxes use, for the same reason. */
    mw_back_rect(&x0, &y0, &x1, &y1);
    if (lx >= x0 && lx < x1 && ly >= y0 && ly < y1)
        return MW_HIT_BACK;
    mw_fwd_rect(&x0, &y0, &x1, &y1);
    if (lx >= x0 && lx < x1 && ly >= y0 && ly < y1)
        return MW_HIT_FORWARD;

    if (ly < MW_Y + MW_CONTENT_Y)
        return MW_HIT_URL;
    /* THE KEYBOARD IS ASKED BEFORE THE PAGE, and only while it is up -- the same
     * point is the page underneath the moment it goes down (menu_keyboard_hit()
     * answers -1 then). Asking in this order is the whole of what keeps a tap on a
     * key from also being a click on the page behind it. */
    if (menu_keyboard_hit(lx, ly) >= 0)
        return MW_HIT_KEYBOARD;
    return MW_HIT_CONTENT;
}

int menu_window_pressed(void)
{
    return mw_pressed;
}

int menu_window_key_pressed(void)
{
    return mw_key_pressed;
}

/* What a released key does. The two targets differ in exactly one way, and it is
 * menu_window.h's: the URL field's characters land in a buffer this module owns and
 * is drawn from, and the page's are sent one at a time because the page draws them
 * and nothing here can read them back. */
static void mw_key_fire(int idx)
{
    const struct menu_key *k = menu_keyboard_key(idx);
    char c;

    if (!k)
        return;
    switch (k->act) {
    case MK_SHIFT:
        menu_keyboard_shift_set(!menu_keyboard_shift());
        break;
    case MK_BACK:
        if (mw_target == MW_EDIT_URL)
            mw_buf_back();
        else
            mw_push(MW_REQ_KEY, "Backspace", 0, 0);
        break;
    case MK_OK:
        /* OK is the end of the typing either way, so it lowers the keyboard --
         * "hides on OK" is what was asked for, and a keyboard that stayed up over
         * the page it just loaded would be the thing the operator has to dismiss
         * before reading it. */
        if (mw_target == MW_EDIT_URL) {
            if (mw_buf_len > 0)
                mw_push(MW_REQ_NAV, mw_buf, 0, 0);
        } else {
            mw_push(MW_REQ_KEY, "Enter", 0, 0);
        }
        mw_target_set(MW_EDIT_NONE, 0);
        menu_keyboard_hide();
        break;
    default:                    /* MK_CHAR and MK_SPACE */
        c = menu_keyboard_char(idx);
        if (!c)
            break;
        if (mw_target == MW_EDIT_URL) {
            mw_buf_ins(c);
        } else {
            char one[2];

            one[0] = c;
            one[1] = '\0';
            mw_push(MW_REQ_TEXT, one, 0, 0);
        }
        break;
    }
}

int menu_window_feed(int down, int lx, int ly)
{
    int hit;

    if (mw_state == MW_CLOSED)
        return MZ_FEED_NONE;
    hit = menu_window_hit(lx, ly);
    if (hit == MW_HIT_NONE)
        return MZ_FEED_NONE;

    if (down && mw_down && mw_down_hit == MW_HIT_CONTENT) {
        /* A MOVE, NOT A NEW PRESS. The same page cell and a press already down is a
         * finger that has not lifted -- this is the only place the run of reports a
         * held finger makes can be told from a fresh tap, and it is why the drag has
         * to be read here rather than at the release: the release alone cannot say
         * which way the finger went. mw_down_x/y is deliberately NOT updated -- the
         * click a tap sends is the point the operator aimed at. */
        int dx = lx - mw_move_x;
        int dy = ly - mw_move_y;

        mw_move_x = lx;
        mw_move_y = ly;
        mw_travel += (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
        if (mw_travel >= MW_TAP_SLOP) {
            /* Past the slop: this is a scroll. The travel BEFORE the slop is
             * deliberately dropped, which is what a touch driver does -- the first
             * few pixels pick the gesture up -- so the crossing report carries the
             * movement from the slop onward and the page follows from there. */
            mw_scrolled = 1;
            mw_acc_x += dx;
            mw_acc_y += dy;
            if (mw_acc_x <= -MW_SCROLL_STEP || mw_acc_x >= MW_SCROLL_STEP ||
                mw_acc_y <= -MW_SCROLL_STEP || mw_acc_y >= MW_SCROLL_STEP) {
                mw_push(MW_REQ_SCROLL, NULL, mw_acc_x, mw_acc_y);
                mw_acc_x = mw_acc_y = 0;
            }
        }
        mw_pressed = hit;
        return MZ_FEED_TAKEN;
    }

    if (down) {
        /* A FRESH PRESS: the origin of whatever gesture follows. */
        mw_down     = 1;
        mw_move_x   = lx;
        mw_move_y   = ly;
        mw_travel   = 0;
        mw_acc_x    = mw_acc_y = 0;
        mw_scrolled = 0;
        mw_down_hit = hit;
        mw_down_x   = lx;
        mw_down_y   = ly;
        mw_pressed  = hit;
        /* The keyboard's highlight is per KEY and not per cell: two keys are two
         * different marks on the glass and one MW_HIT_KEYBOARD, so the cell is not
         * enough to light the cap the finger is on. */
        mw_key_pressed = (hit == MW_HIT_KEYBOARD) ? menu_keyboard_hit(lx, ly) : -1;
        mw_down_key = mw_key_pressed;
        return MZ_FEED_TAKEN;
    }

    mw_pressed = MW_HIT_NONE;
    mw_key_pressed = -1;
    if (!mw_down)
        /* A release with no press behind it -- the window opened under a finger
         * that was already down. Swallowed all the same: the window is opaque, so
         * rbp must not learn about a press that ended on it. */
        return MZ_FEED_TAKEN;
    mw_down = 0;
    if (mw_scrolled) {
        /* THE PRESS WAS A DRAG, so nothing fires -- not the click it began on, and
         * not wherever it happens to have ended. A scroll that drifts up onto the
         * URL row must not raise the keyboard, which is exactly what the cell
         * comparison below would do. */
        mw_down_hit = MW_HIT_NONE;
        mw_down_key = -1;
        mw_scrolled = 0;
        return MZ_FEED_TAKEN;
    }
    if (hit != mw_down_hit) {
        /* Rolled off the cell it started on. Nothing fires, and this is the whole
         * reason mw_down_hit exists rather than re-reading the release's cell. */
        mw_down_hit = MW_HIT_NONE;
        mw_down_key = -1;
        return MZ_FEED_TAKEN;
    }

    switch (hit) {
    case MW_HIT_CLOSE:
        menu_window_close();
        break;
    case MW_HIT_MIN:
        /* The same box both ways, which is what makes the minimized bar's mark
         * live rather than decorative. */
        if (mw_state == MW_NORMAL)
            menu_window_minimize();
        else
            menu_window_restore();
        break;
    case MW_HIT_TITLE:
        /* The minimized window IS its title bar, so tapping anywhere on it is the
         * way back. Normal: nothing -- move and resize are not in this change. */
        if (mw_state == MW_MIN)
            menu_window_restore();
        break;
    case MW_HIT_BACK:
        /* Walking the page's own history is the browser's to do, not ours: it holds
         * the entries, and only it knows whether there is one at all. So this is a
         * request like a click, and it is -1 for back where a click's two ints are
         * a point -- mw_req's x is signed and a click's is never negative. */
        mw_push(MW_REQ_HIST, NULL, -1, 0);
        break;
    case MW_HIT_FORWARD:
        mw_push(MW_REQ_HIST, NULL, 1, 0);
        break;
    case MW_HIT_URL:
        /* THE FIELD IS THE KEYBOARD'S DOOR. Tapping it raises the keyboard with the
         * field as the target and SELECTS what is there, so the operator's first
         * character replaces the old address instead of appending to it. */
        mw_buf_set(mw_live);
        mw_target_set(MW_EDIT_URL, 1);
        menu_keyboard_show();
        break;
    case MW_HIT_KEYBOARD: {
        /* THE KEY MUST BE THE ONE THE PRESS BEGAN ON. The cell check above cannot
         * see this: fifty caps are one MW_HIT_KEYBOARD, so a finger that pressed 'q'
         * and slid onto 'w' before lifting is, by the cell alone, the same press --
         * and it would type the key it happened to be over at the lift, which is a
         * letter the operator never chose. The latch makes that typing neither. */
        int k = menu_keyboard_hit(lx, ly);

        if (k >= 0 && k == mw_down_key)
            mw_key_fire(k);
        break;
    }
    case MW_HIT_CONTENT:
    default:
        /* THE PAGE. A tap here is a CLICK at the page's own coordinate, which is
         * what the browser needs to follow a link -- the press's point, since a
         * release that rolled is rejected above. Tapping the page also ends a URL
         * edit, which is the behaviour of every browser with a URL bar. */
        if (mw_target != MW_EDIT_NONE) {
            mw_target_set(MW_EDIT_NONE, 0);
            menu_keyboard_hide();
        }
        mw_push(MW_REQ_POINT, NULL, mw_down_x - MW_X,
                mw_down_y - (MW_Y + MW_CONTENT_Y));
        mw_down_hit = MW_HIT_NONE;
        break;
    }
    mw_down_hit = MW_HIT_NONE;
    mw_down_key = -1;
    return MZ_FEED_TAKEN;
}
