/*
 * menu_window.h -- the browser window's geometry, state and hit test. PURE.
 *
 * This is menu_zone.h's sibling and is written to the same rule: everything here
 * is a function of the window's rect and of the point handed in, never of what is
 * already on the glass, and nothing here touches a framebuffer, a file or a clock.
 * That is what lets test_window.c pin every chrome cell on a host with no vc4, no
 * framebuffer and no browser.
 *
 * THE WINDOW IS NOT A SECOND PLANE. Measured on the unit 2026-10-04: DRM master is
 * one per device, so the menu band's plane setup is granted it and a second setup
 * in the same process is refused EBUSY. The window therefore *takes the band's
 * plane over* while it is open and gives it back when it closes -- see
 * menu_draw.c's menu_window_plane_sync(). The two are never wanted at once: the
 * window is opened from the menu, and opening it closes the panel.
 *
 * GEOMETRY, all in the menu's own logical 1280x800 space:
 *
 *     +-----------------------------------------------+  MW_TITLE_H (32)
 *     |  title                      [-] [X]           |
 *     +-----------------------------------------------+  MW_URL_H (28)
 *     |  url / status field                           |
 *     +-----------------------------------------------+
 *     |                                               |
 *     |  the page, 1120x600 of it                     |  MW_W x MW_H
 *     |                                               |
 *     +-----------------------------------------------+
 *
 * The window's rect is fixed, and deliberately so: 1120x600 at (80,80) is the
 * exact rectangle the Step-1 drill put on the glass and the operator confirmed, so
 * the size is a measured one rather than a guess. Move and resize are not in this
 * change.
 *
 * THE X AND THE MINIMIZE ARE SHAPES, NOT GLYPHS. The font atlas (menu_font.h) is
 * space, '0'-'9' and 'A'-'Z' -- no 'x', no punctuation -- and it is a GENERATED
 * file, so drawing chrome out of it would mean re-baking the font for two marks.
 * Both are drawn by menu_window_paint.c as strokes, which is also what
 * menu_zone.h does for its globe. The KEYBOARD's caps and the URL are drawn from
 * the atlas, which is why its character set was extended for them (see below).
 *
 * WHERE THE TYPING GOES, and there are two answers. The URL field is OURS: a tap on
 * it raises the keyboard with the field as the target, and characters accumulate in
 * this module's own buffer, which is what the field shows and what OK navigates to.
 * A text input INSIDE the page is the browser's: characters are sent to it one at a
 * time and nothing is kept here, because the page draws them and this process
 * cannot read them back. Which of the two is live is menu_window_target().
 *
 * THE WINDOW WRITES NOTHING. It is pure (menu_window.c's own rule), so a tap that
 * has something to say -- type this, navigate there, click here -- records a request
 * in a small ring and menu_draw.c's tick hands it to browser_link.c. That keeps the
 * file I/O in one module and lets test_window.c drive the whole interaction with no
 * browser, no /tmp and no device.
 */
#ifndef RBPI4B_MENU_WINDOW_H
#define RBPI4B_MENU_WINDOW_H

#include <stddef.h>         /* size_t */

#include "menu_zone.h"      /* MZ_LOGICAL_W, MZ_LOGICAL_H */

/* The window's rectangle: the one the Step-1 drill proved on this hardware. */
#define MW_X 80
#define MW_Y 80
#define MW_W 1120
#define MW_H 600

/* The chrome. All logical rows/cols, measured from the window's own top-left. */
#define MW_TITLE_H 32          /* the drag-free title bar */
#define MW_URL_H   28          /* the address/status field under it */
#define MW_BTN     28          /* the - and X boxes, square */
#define MW_BTN_TOP 2           /* their inset from the title bar's top edge */
#define MW_BTN_GAP 6           /* between the two of them */

/* THE BACK AND FORWARD BOXES, in the URL row and not the title bar -- where every
 * browser puts them, and the only place a tap on them cannot be confused with a
 * tap on the window's own chrome. They are square and smaller than MW_BTN because
 * MW_URL_H is 28 and a box filling it edge to edge would read as a segment of the
 * row rather than as a button in it. */
#define MW_NAV      22         /* the boxes, square */
#define MW_NAV_X0   6          /* the back box's inset from the window's left edge */
#define MW_NAV_GAP  2          /* between the two of them */
#define MW_NAV_TOP  3          /* their inset inside the URL row: (28 - 22) / 2 */
/* Where the address text starts, leaving the two boxes and a gap clear of it. The
 * field's own rect is unchanged -- only its text moves -- so the tap that raises
 * the keyboard is still the whole row, buttons included, and a tap that lands on a
 * button is answered by the hit test before the field is ever asked. */
#define MW_URL_TEXT_X (MW_NAV_X0 + MW_NAV + MW_NAV_GAP + MW_NAV + 8)

/* Where the page itself goes, inside the window. */
#define MW_CONTENT_Y (MW_TITLE_H + MW_URL_H)
#define MW_CONTENT_H (MW_H - MW_CONTENT_Y)

/* The window's state. MINIMIZED is not "hidden": it keeps the title bar on the
 * glass, so the operator's way back is the thing they were looking at rather than
 * a mark they have to remember. */
enum {
    MW_CLOSED = 0,
    MW_NORMAL,
    MW_MIN
};

/* What a logical point lands on. MW_HIT_NONE covers everywhere outside the
 * window's rect, which is how a tap that is not the window's is left for the menu
 * and then for rbp. MW_HIT_KEYBOARD is only ever returned while the keyboard is up,
 * so the same point is the page when it is down. */
enum {
    MW_HIT_NONE = 0,
    MW_HIT_CLOSE,
    MW_HIT_MIN,
    MW_HIT_BACK,
    MW_HIT_FORWARD,
    MW_HIT_URL,
    MW_HIT_TITLE,
    MW_HIT_KEYBOARD,
    MW_HIT_CONTENT
};

/* WHAT THE KEYBOARD IS TYPING INTO. MW_EDIT_NONE means it is down; the URL field is
 * this module's own buffer, and a page input is the browser's. */
enum {
    MW_EDIT_NONE = 0,
    MW_EDIT_URL,
    MW_EDIT_PAGE
};

/* A REQUEST FOR THE BROWSER, recorded by a tap and drained by menu_draw.c's tick.
 * One struct for all four because the ring is fixed-size, and the text field is the
 * URL or the character run a single key types. */
enum {
    MW_REQ_NONE = 0,
    MW_REQ_NAV,        /* text: the URL to load                      */
    MW_REQ_TEXT,       /* text: characters to type into the page     */
    MW_REQ_KEY,        /* text: one named key -- "Backspace", "Enter" */
    MW_REQ_POINT,      /* x, y: a page coordinate to click           */
    MW_REQ_HIST,       /* x: -1 back, +1 forward -- the page's own
                        * history, which only the browser can walk  */
    MW_REQ_SCROLL      /* x, y: a page scroll DELTA, in page pixels,
                        * signed: +x right, +y down. One request is
                        * one movement of the finger, not a wheel
                        * notch, so the page follows the finger.    */
};
#define MW_REQ_TEXT_MAX 256
#define MW_REQ_RING      8

/* A DRAG INSIDE THE PAGE IS A SCROLL; A TAP IS A CLICK, AND THEY ARRIVE THE SAME
 * WAY. The touch panel reports a held finger as a run of `down` reports, so without
 * this every drag would ALSO fire the click it started on and a scroll down a
 * listing would navigate wherever the finger landed. MW_TAP_SLOP is the total travel
 * (|dx| + |dy|) past which the press was a drag and the click is dropped; it is
 * generous on purpose -- a finger that wanders 15 px is tapping a link, not
 * scrolling a page. MW_SCROLL_STEP is how much un-sent travel accumulates before a
 * request goes out: the panel reports faster than the browser needs, and one wheel
 * event per report would be a round trip per pixel. */
#define MW_TAP_SLOP     16
#define MW_SCROLL_STEP  12
/* The URL buffer, and it is shorter than a request's so a filled field always fits
 * a request whole -- a truncated URL that navigated would be worse than one that did
 * not. */
#define MW_URL_MAX     160

struct mw_req {
    int  kind;                      /* MW_REQ_* */
    int  x, y;                      /* MW_REQ_POINT, page coordinates */
    char text[MW_REQ_TEXT_MAX];     /* MW_REQ_NAV/TEXT/KEY */
};

/* The window's rect for its CURRENT state: the full 1120x600 when normal, the
 * title bar alone when minimized. Returns 0 when closed (and touches nothing). */
int menu_window_rect(int *x, int *y, int *w, int *h);

/* State, and the transitions. Opening is what the menu's globe asks for;
 * closing and minimizing are what the chrome's two boxes ask for. Every one of
 * these is safe to call in any state. */
int  menu_window_state(void);
int  menu_window_is_open(void);     /* NORMAL or MIN */
void menu_window_open(void);        /* -> NORMAL */
void menu_window_close(void);       /* -> CLOSED */
void menu_window_minimize(void);    /* NORMAL -> MIN */
void menu_window_restore(void);     /* MIN -> NORMAL */
void menu_window_reset(void);       /* -> CLOSED, whatever it was */

/* What is at (lx, ly), in the menu's logical space. Closed reads MW_HIT_NONE for
 * every point, which is what makes "the window is not there" one comparison. */
int  menu_window_hit(int lx, int ly);

/* The cell under the finger, so the painter draws the pressed box. Written by the
 * touch thread on the press and cleared on the release, read by the paint thread --
 * menu_zone.c's one-writer rule, and one int, so a paint racing a tap sees the cell
 * before or after it and never torn. */
int  menu_window_pressed(void);

/* Which KEY the finger is down on, or -1. The chrome's highlight is the cell above
 * and the keyboard's is this, because fifty caps are one cell. Same thread rules. */
int  menu_window_key_pressed(void);

/* ONE TOUCH REPORT, in menu_zone.h's MZ_FEED_* vocabulary so pointsrc.c can ask
 * the window and the menu with the same two lines of code. The action is taken
 * here rather than handed back, because this module is the one that owns the state
 * machine: the X closes, the box minimizes, the minimized bar restores, the URL
 * field raises the keyboard, a cap types, and a tap on the page is recorded as a
 * click for the browser.
 *
 * CLOSED READS MZ_FEED_NONE -- and so does a point outside the rect, which is what
 * leaves the menu and then rbp their own. Everything inside the rect is TAKEN,
 * never TAP: the window is opaque on the glass, so a press it let through would
 * land on whatever rbp has under it, invisibly. That is the same argument the
 * panel's MZ_FEED_TAKEN arm makes (menu_zone.h).
 *
 * A button fires on the release that is STILL ON the cell the press began on --
 * menu_zone.c's rule, and the reason a finger that rolls off the X and lifts
 * elsewhere does not close the window. It is the same rule that keeps a finger
 * rolling from one cap to the next from typing either of them.
 *
 * THE RELEASE DOES NOT KNOW WHERE THE PAGE WANTS ITS CLICK, so the press's own
 * coordinate is what a page click carries: the operator aimed at a link with the
 * press, and by the time the finger lifts it may have travelled ten pixels across
 * that link and still be on the same cell. */
int  menu_window_feed(int down, int lx, int ly);

/* The title bar's rect, for the painter's own use. */
void menu_window_title_rect(int *x0, int *y0, int *x1, int *y1);

/* The two chrome boxes' rects. The painter draws them here; the hit test above
 * answers here. One definition, so the mark and the target cannot drift apart. */
void menu_window_close_box(int *x0, int *y0, int *x1, int *y1);
void menu_window_min_box(int *x0, int *y0, int *x1, int *y1);

/* The back and forward boxes, in the URL row. Same rule as the two above: the
 * mark and the target come from one definition. */
void menu_window_back_box(int *x0, int *y0, int *x1, int *y1);
void menu_window_fwd_box(int *x0, int *y0, int *x1, int *y1);

/* The URL field's rect -- the painter draws the text inside it and the tap that
 * raises the keyboard is the tap on it. */
void menu_window_url_rect(int *x0, int *y0, int *x1, int *y1);

/* --- what the operator is typing into -------------------------------------- */

/* The target, and what the field shows. `field` is the edit buffer while the URL is
 * the target and the page's own URL the rest of the time -- which is the whole of
 * why the status line from the host exists. */
int  menu_window_target(void);
const char *menu_window_field(void);
int  menu_window_field_sel(void);      /* the buffer is selected whole: the field
                                        * draws it inverted and the next character
                                        * replaces it, which is what makes typing a
                                        * new address one tap instead of thirty */

/* Bumped by ANY change to what the field shows -- the text, the selection, the
 * target and the page's own URL. The painter is pure and its caller decides whether
 * to run it by comparing what it last drew against what is true now, and "what is
 * true now" is a string. A counter makes that comparison an int, which is what
 * menu_draw.c's repaint gate needs. */
unsigned menu_window_field_seq(void);

/* The page's URL and its focused input, as reported by the host through
 * browser_link.c. The second is the operator's "the keyboard should show up every
 * time there's an input item": the page focusing a field is the page asking for a
 * keyboard, and this is how it asks.
 *
 * THE INPUT IS A SEQUENCE, NOT A FLAG, and that is the whole of the subtlety.
 * `focus` is the page's focus generation: 0 while no text input is focused, and a
 * number that only ever grows each time one GAINS focus. A plain flag cannot answer
 * the question the operator will actually ask -- dismiss the keyboard on a field
 * that is still focused and tap that same field again -- because the flag never
 * changes and the second tap would raise nothing. The generation does change, so it
 * does. A repeated identical value is ignored, which is what makes it safe to call
 * this on every tick. */
void menu_window_set_url(const char *s);
void menu_window_page_input(int focus);

/* The page's own rectangle inside the window, in logical space. */
void menu_window_content_rect(int *x0, int *y0, int *x1, int *y1);

/* --- the request ring ------------------------------------------------------ */

/* One request, or 0 when there is none. Written by the touch thread on a tap and
 * read by the tick thread that forwards it, so it is a single-producer /
 * single-consumer ring: the producer writes the slot and then publishes the head,
 * the consumer reads the slot and then publishes the tail. Each index is one int
 * written by one thread, which is the whole of the synchronisation and is enough for
 * a counter -- the same argument menu_zone.c's state makes. */
int  menu_window_take_req(struct mw_req *out);

#endif /* RBPI4B_MENU_WINDOW_H */
