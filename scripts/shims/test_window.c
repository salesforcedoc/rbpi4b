/*
 * test_window.c -- the browser window's unit test: no Pi, no device, no rbp, no
 * screen, no browser.
 *
 * It links the PRODUCTION menu_window.c and menu_window_paint.c, for test_menu.c's
 * reason: the rules pinned here are exactly the kind that get quietly rewritten in
 * a copy. Its surface is a local array -- there is no framebuffer, no plane, no
 * thread and no device anywhere in this file. menu_window.c's own header says why
 * that is possible: everything in it is a function of the rect and of the point
 * handed in.
 *
 * Four things are pinned, and each has a failure it is standing in front of.
 *
 * 1. The geometry. The window has to sit INSIDE the logical screen, and it has to
 *    leave the menu's swipe-down entry zone outside itself -- the strip is rows
 *    0..MZ_STRIP_Y1 across the middle third, and if the window ever grew over it the
 *    operator's way back to the menu would be a gesture the window swallowed. This
 *    is the one check here that is about the OTHER module's constants, and it is
 *    here because the window is the thing that grows.
 *
 * 2. The state machine. Minimize only from NORMAL (a minimize that conjured a title
 *    bar out of a closed window would put something on the glass the operator never
 *    asked for), restore only from MIN, and close from anywhere. The rect and the
 *    plane size follow the state, which is why the transitions are checked and not
 *    just the states.
 *
 * 3. The hit test, at every chrome cell and at the boundaries between them. An
 *    off-by-one on a box edge is a 28-px mark that only responds on 27 of them --
 *    invisible to any test that presses the middle of things.
 *
 * 4. The feed, which is where the X actually closes: a button fires on the release
 *    that is STILL ON the cell the press began on, a press that rolls off fires
 *    nothing, and a report for a point outside the rect is MZ_FEED_NONE so the menu
 *    and then rbp still get it. Plus the image: the chrome writes nothing in the
 *    page area (that is the frame's, and the loader owns it), nothing outside the
 *    window's own rows, a second identical paint changes nothing, and the X and the
 *    minimize are different marks rather than one mark drawn twice.
 *
 * 5. THE TYPING, which is the operator's own request -- "a URL bar and the keyboard
 *    to show up every time there's an input item, clicking should allow navigation".
 *    Three things, one each: the URL field raises the keyboard with the field as the
 *    target and selects what is there; a tap on the page is recorded as a CLICK at
 *    the page's own coordinate; and the page focusing an input raises the keyboard
 *    too, off a GENERATION rather than a flag -- so the keyboard can be dismissed on
 *    a field that is still focused and raised again by tapping it. What the taps
 *    produce is checked through the request ring, which is the interface
 *    menu_draw.c's tick hands to browser_link.c: nothing here needs a browser.
 *
 * Build + run (static, so no rootfs is needed to load it):
 *     make test
 * which is:  arm-linux-gnueabi-gcc -static -o test_window test_window.c
 *            menu_window.c menu_window_paint.c menu_paint.c menu_zone.c
 *            menu_keyboard.c   &&   qemu-arm ./test_window
 */
#define _GNU_SOURCE
#include "menu_window.h"
#include "menu_window_paint.h"
#include "menu_keyboard.h"
#include "menu_paint.h"
#include "menu_zone.h"

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

/* A value no class in the palette can produce, so a pixel still holding it was
 * never written. 0xa5a5 is a mid grey-magenta in RGB565 and is not in the palette. */
#define SENTINEL16 0xa5a5u

/* The window at its largest, 16 bpp, pitch = width. The page area makes this
 * 1120x600x2 = 1.3 MB of .bss, which is nothing for a host run. */
static unsigned short wfb[MW_W * MW_H];

/* The image as it stood after one paint, so "painting twice paints once" is a
 * comparison of the whole window rather than of the rows anyone remembered. */
static unsigned short whole[MW_W * MW_H];

static void view_of(struct menu_view *v, int h)
{
    memset(v, 0, sizeof *v);
    v->pix   = wfb;
    v->pitch = MW_W;
    v->fb_w  = MW_W;
    v->fb_h  = h;
    v->bpp   = 16;
    v->dw    = MW_W;
    v->dh    = h;
}

static void sentinel_fill(int h)
{
    int x, y;

    for (y = 0; y < h; y++)
        for (x = 0; x < MW_W; x++)
            wfb[(size_t)y * MW_W + x] = SENTINEL16;
}

static unsigned int at(int x, int y)
{
    return wfb[(size_t)y * MW_W + x];
}

/* THE RECTS menu_window.c HANDS BACK ARE IN THE MENU'S LOGICAL SPACE. The buffer
 * this test paints into is WINDOW-LOCAL, exactly as menu_window_paint.c's wp_box()
 * translates before it draws. One helper for that, so the test and the painter
 * cannot disagree about where a mark is -- which is the whole failure this file
 * exists to catch, and it caught it: reading the boxes' logical x as a buffer
 * column reads a different row entirely. */
#define WX(x) ((x) - MW_X)
#define WY(y) ((y) - MW_Y)

static int is_mark(unsigned int pv, int bpp)
{
    /* Ink, in either palette value the chrome uses for a mark: MENU_LABEL, or
     * MENU_LABEL_PRESSED when the box is the one under the finger. A face pixel is
     * neither, and neither is the sentinel. */
    return pv == menu_pixel(bpp, MENU_LABEL) ||
           pv == menu_pixel(bpp, MENU_LABEL_PRESSED);
}

/* --- 1. the geometry -------------------------------------------------------- */

static void test_geometry(void)
{
    int x, y, w, h, bx0, by0, bx1, by1;

    /* Inside the logical screen. */
    CHECK(MW_X >= 0 && MW_Y >= 0, "the window's origin is off-screen");
    CHECK(MW_X + MW_W <= MZ_LOGICAL_W, "the window runs off the right edge");
    CHECK(MW_Y + MW_H <= MZ_LOGICAL_H, "the window runs off the bottom");

    /* And the menu's way back is left alone: the strip is rows 0..MZ_STRIP_Y1 with
     * its entry zone across the middle third, and the window must not touch it. */
    CHECK(MW_Y > MZ_STRIP_Y1,
          "the window (y=%d) covers the menu's entry strip (rows 0..%d) -- the"
          " swipe-down gesture would start on the window",
          MW_Y, MZ_STRIP_Y1);

    /* The internal rows add up, so the page area is what is left over rather than a
     * second guess at the same number. */
    CHECK(MW_CONTENT_Y == MW_TITLE_H + MW_URL_H, "the content row is not the sum");
    CHECK(MW_CONTENT_H == MW_H - MW_CONTENT_Y, "the content height is not the rest");
    CHECK(MW_CONTENT_H > 0, "the window has no page area at all");

    /* The chrome boxes: square, inside the title bar, and clear of each other. They
     * must not overflow the bar's own height either -- a box taller than the bar it
     * sits in is a mark drawn over the url field. */
    CHECK(MW_BTN_TOP + MW_BTN <= MW_TITLE_H,
          "the chrome boxes (%d rows at +%d) do not fit the %d-row title bar",
          MW_BTN, MW_BTN_TOP, MW_TITLE_H);

    menu_window_reset();
    menu_window_open();
    CHECK(menu_window_rect(&x, &y, &w, &h) == 1, "an open window has no rect");
    CHECK(x == MW_X && y == MW_Y && w == MW_W && h == MW_H,
          "the open rect is %d,%d %dx%d", x, y, w, h);

    menu_window_close_box(&bx0, &by0, &bx1, &by1);
    CHECK(bx0 >= MW_X && bx1 <= MW_X + MW_W, "the close box is outside the window");
    CHECK(by0 >= MW_Y && by1 <= MW_Y + MW_TITLE_H,
          "the close box is outside the title bar");
    CHECK(bx1 == MW_X + MW_W - MW_BTN_GAP,
          "the close box is not inset from the window's right edge by MW_BTN_GAP");
    CHECK(bx1 - bx0 == MW_BTN && by1 - by0 == MW_BTN, "the close box is not square");
    CHECK(by0 == MW_Y + MW_BTN_TOP, "the close box is not at the bar's top inset");

    {
        int mx0, my0, mx1, my1;

        menu_window_min_box(&mx0, &my0, &mx1, &my1);
        CHECK(mx1 + MW_BTN_GAP <= bx0,
              "the minimize box (%d) is not clear of the close box (%d)", mx1, bx0);
        CHECK(my0 == by0 && my1 == by1, "the two boxes are not on the same rows");
        CHECK(mx1 - mx0 == MW_BTN, "the minimize box is not square");
        CHECK(mx0 >= MW_X, "the minimize box is off the left edge");
    }
}

/* --- 2. the state machine --------------------------------------------------- */

static void test_states(void)
{
    int x, y, w, h;

    menu_window_reset();
    CHECK(menu_window_state() == MW_CLOSED, "reset does not close");
    CHECK(!menu_window_is_open(), "a reset window reads open");
    CHECK(menu_window_rect(&x, &y, &w, &h) == 0,
          "a closed window handed back a rect");
    CHECK(menu_window_rect(NULL, NULL, NULL, NULL) == 0,
          "a closed window did not survive null outs");

    /* Minimizing a closed window must not conjure a title bar onto the glass --
     * this is the one way the machine could put something up unasked. */
    menu_window_minimize();
    CHECK(menu_window_state() == MW_CLOSED, "minimize opened a closed window");
    menu_window_restore();
    CHECK(menu_window_state() == MW_CLOSED, "restore opened a closed window");

    menu_window_open();
    CHECK(menu_window_state() == MW_NORMAL && menu_window_is_open(),
          "open did not reach NORMAL");
    menu_window_open();
    CHECK(menu_window_state() == MW_NORMAL, "a second open moved the state");

    menu_window_minimize();
    CHECK(menu_window_state() == MW_MIN && menu_window_is_open(),
          "minimize did not reach MIN -- and MIN still counts as open");
    CHECK(menu_window_rect(&x, &y, &w, &h) == 1, "a minimized window has no rect");
    CHECK(x == MW_X && y == MW_Y && w == MW_W, "minimizing moved the window");
    CHECK(h == MW_TITLE_H,
          "a minimized window is %d rows, not the %d-row title bar it should be",
          h, MW_TITLE_H);

    menu_window_minimize();
    CHECK(menu_window_state() == MW_MIN, "a second minimize moved the state");

    menu_window_restore();
    CHECK(menu_window_state() == MW_NORMAL, "restore did not come back");
    CHECK(menu_window_rect(&x, &y, &w, &h) == 1 && h == MW_H,
          "the restored rect is not the full window");

    menu_window_close();
    CHECK(menu_window_state() == MW_CLOSED, "close did not close");
    menu_window_reset();
    CHECK(menu_window_state() == MW_CLOSED, "reset did not close");

    /* And close from MIN, which is the operator's second way out. */
    menu_window_open();
    menu_window_minimize();
    menu_window_close();
    CHECK(menu_window_state() == MW_CLOSED, "close from MIN did not close");
}

/* --- 3. the hit test -------------------------------------------------------- */

static void test_hit(void)
{
    int bx0, by0, bx1, by1, cx;

    menu_window_reset();
    CHECK(menu_window_hit(MW_X + MW_W / 2, MW_Y + MW_H / 2) == MW_HIT_NONE,
          "a closed window answered a hit inside its own rect");

    menu_window_open();

    /* Outside the rect, on all four sides -- including the pixels just off the
     * right and bottom edges, which are exclusive. */
    CHECK(menu_window_hit(MW_X - 1, MW_Y + MW_H / 2) == MW_HIT_NONE, "left spill");
    CHECK(menu_window_hit(MW_X + MW_W, MW_Y + MW_H / 2) == MW_HIT_NONE, "right spill");
    CHECK(menu_window_hit(MW_X + MW_W / 2, MW_Y - 1) == MW_HIT_NONE, "top spill");
    CHECK(menu_window_hit(MW_X + MW_W / 2, MW_Y + MW_H) == MW_HIT_NONE, "bottom spill");
    /* The corners are the window's, not the outside's. */
    CHECK(menu_window_hit(MW_X, MW_Y) == MW_HIT_TITLE, "the top-left corner is not the window's");
    CHECK(menu_window_hit(MW_X + MW_W - 1, MW_Y + MW_H - 1) == MW_HIT_CONTENT,
          "the bottom-right corner is not the window's");

    /* The two boxes: their four corners and their centres, and the pixel just
     * outside each edge -- which is what an off-by-one would get wrong. */
    menu_window_close_box(&bx0, &by0, &bx1, &by1);
    CHECK(menu_window_hit(bx0, by0) == MW_HIT_CLOSE, "the close box's own corner misses");
    CHECK(menu_window_hit(bx1 - 1, by1 - 1) == MW_HIT_CLOSE, "the close box's far corner misses");
    CHECK(menu_window_hit((bx0 + bx1) / 2, (by0 + by1) / 2) == MW_HIT_CLOSE,
          "the close box's centre misses");
    CHECK(menu_window_hit(bx0 - 1, by0 + 1) != MW_HIT_CLOSE, "one px left of the X hits it");
    CHECK(menu_window_hit(bx1, by0 + 1) != MW_HIT_CLOSE, "one px right of the X hits it");
    CHECK(menu_window_hit(bx0 + 1, by0 - 1) != MW_HIT_CLOSE, "one px above the X hits it");
    CHECK(menu_window_hit(bx0 + 1, by1) != MW_HIT_CLOSE, "one px below the X hits it");

    menu_window_min_box(&bx0, &by0, &bx1, &by1);
    CHECK(menu_window_hit(bx0, by0) == MW_HIT_MIN, "the minimize box's own corner misses");
    CHECK(menu_window_hit(bx1 - 1, by1 - 1) == MW_HIT_MIN, "the minimize box's far corner misses");
    CHECK(menu_window_hit(bx0 - 1, by0 + 1) != MW_HIT_MIN, "one px left of the - hits it");
    CHECK(menu_window_hit(bx1, by0 + 1) != MW_HIT_MIN, "one px right of the - hits it");

    /* The three bands under the boxes, and the boundaries between them. */
    cx = MW_X + 10;
    CHECK(menu_window_hit(cx, MW_Y) == MW_HIT_TITLE, "the title bar's first row");
    CHECK(menu_window_hit(cx, MW_Y + MW_TITLE_H - 1) == MW_HIT_TITLE, "the title bar's last row");
    CHECK(menu_window_hit(cx, MW_Y + MW_TITLE_H) == MW_HIT_URL, "the url field's first row");
    CHECK(menu_window_hit(cx, MW_Y + MW_CONTENT_Y - 1) == MW_HIT_URL, "the url field's last row");
    CHECK(menu_window_hit(cx, MW_Y + MW_CONTENT_Y) == MW_HIT_CONTENT, "the page's first row");
    CHECK(menu_window_hit(cx, MW_Y + MW_H - 1) == MW_HIT_CONTENT, "the page's last row");

    /* MINIMIZED: the window IS its title bar. The two boxes are still live, the rest
     * of the bar restores, and the rows below the bar are not the window's at all. */
    menu_window_minimize();
    menu_window_close_box(&bx0, &by0, &bx1, &by1);
    CHECK(menu_window_hit((bx0 + bx1) / 2, (by0 + by1) / 2) == MW_HIT_CLOSE,
          "the X is dead in the minimized bar");
    menu_window_min_box(&bx0, &by0, &bx1, &by1);
    CHECK(menu_window_hit((bx0 + bx1) / 2, (by0 + by1) / 2) == MW_HIT_MIN,
          "the minimize box is dead in the minimized bar");
    CHECK(menu_window_hit(cx, MW_Y + MW_TITLE_H - 1) == MW_HIT_TITLE,
          "the minimized bar is not tappable");
    CHECK(menu_window_hit(cx, MW_Y + MW_TITLE_H) == MW_HIT_NONE,
          "the minimized window answers a hit below its bar -- it is not there");
    CHECK(menu_window_hit(cx, MW_Y + MW_H - 1) == MW_HIT_NONE,
          "the minimized window answers a hit where the page used to be");

    /* THE KEYBOARD OVER THE PAGE, and the same point answering differently: with it
     * up, the page's own rows below MW_KB_Y0 are the keyboard's, and the rows above
     * are still the page's. A hit test that asked the page first would fire a click
     * on the link behind the cap the operator pressed. */
    menu_window_restore();
    CHECK(menu_window_hit(cx, MW_Y + MW_H - 1) == MW_HIT_CONTENT,
          "with no keyboard up the window's bottom row is not the page");
    menu_keyboard_show();
    CHECK(menu_keyboard_hit(cx, MW_Y + MW_H - 1) >= 0,
          "the keyboard is up and its own bottom row is not on it");
    CHECK(menu_window_hit(cx, MW_Y + MW_H - 1) == MW_HIT_KEYBOARD,
          "the keyboard is up and the page underneath still took the point");
    CHECK(menu_window_hit(cx, MW_Y + MW_CONTENT_Y) == MW_HIT_CONTENT,
          "the keyboard swallowed the top of the page too -- the page above it is "
          "still the operator's to click");
    menu_keyboard_hide();
    CHECK(menu_window_hit(cx, MW_Y + MW_H - 1) == MW_HIT_CONTENT,
          "lowering the keyboard did not give the page back");

    menu_window_reset();
}

/* --- 4. the feed, and the image --------------------------------------------- */

static void test_feed(void)
{
    int bx0, by0, bx1, by1, mx0, my0, mx1, my1, cx, cy;

    menu_window_reset();
    CHECK(menu_window_feed(1, MW_X + 10, MW_Y + 10) == MZ_FEED_NONE,
          "a closed window took a press");

    menu_window_open();
    menu_window_close_box(&bx0, &by0, &bx1, &by1);
    cx = (bx0 + bx1) / 2;
    cy = (by0 + by1) / 2;

    /* OUTSIDE the rect is not ours: the menu and then rbp get this report, which is
     * what keeps the swipe-down gesture and rbp's own touches working while the
     * window is up. */
    CHECK(menu_window_feed(1, MW_X - 5, MW_Y + MW_H / 2) == MZ_FEED_NONE,
          "a press left of the window was taken -- the menu would lose it");
    CHECK(menu_window_feed(0, MW_X - 5, MW_Y + MW_H / 2) == MZ_FEED_NONE,
          "a release left of the window was taken");
    CHECK(menu_window_feed(1, MW_X + MW_W / 2, MZ_STRIP_Y1) == MZ_FEED_NONE,
          "a press in the menu's own strip was taken by the window");
    /* A press on the page is TAKEN with no action: the window is opaque, so a
     * report it let through would land on rbp's UI underneath, invisibly. */
    CHECK(menu_window_feed(1, MW_X + 10, MW_Y + MW_H - 10) == MZ_FEED_TAKEN,
          "a press on the page was not taken");
    CHECK(menu_window_feed(0, MW_X + 10, MW_Y + MW_H - 10) == MZ_FEED_TAKEN,
          "a release on the page was not taken");
    CHECK(menu_window_state() == MW_NORMAL, "a press on the page changed the state");

    /* THE X: press and release on the box closes it. */
    CHECK(menu_window_feed(1, cx, cy) == MZ_FEED_TAKEN, "the press on the X was not taken");
    CHECK(menu_window_pressed() == MW_HIT_CLOSE,
          "the X is not what the finger is on, so it would not be drawn pressed");
    CHECK(menu_window_state() == MW_NORMAL, "the X fired on the PRESS, not the release");
    CHECK(menu_window_feed(0, cx, cy) == MZ_FEED_TAKEN, "the release on the X was not taken");
    CHECK(menu_window_state() == MW_CLOSED, "the release on the X did not close the window");
    CHECK(menu_window_pressed() == MW_HIT_NONE, "the finger is still on a closed window");

    /* ROLLED OFF: the press began on the X and the release is elsewhere on the
     * window. Nothing fires -- this is the whole reason the press's own cell is
     * remembered rather than the release's cell being re-read. */
    menu_window_open();
    CHECK(menu_window_feed(1, cx, cy) == MZ_FEED_TAKEN, "the second press on the X");
    CHECK(menu_window_feed(0, MW_X + 10, MW_Y + MW_H - 10) == MZ_FEED_TAKEN,
          "the release off the X was not taken");
    CHECK(menu_window_state() == MW_NORMAL,
          "rolling off the X and lifting closed the window anyway");
    CHECK(menu_window_pressed() == MW_HIT_NONE, "the finger is still down after a lift");

    /* And a release with no press behind it -- the window opened under a finger that
     * was already down -- is swallowed and does nothing. */
    CHECK(menu_window_feed(0, cx, cy) == MZ_FEED_TAKEN,
          "a stray release on the X was not taken");
    CHECK(menu_window_state() == MW_NORMAL, "a stray release closed the window");

    /* THE MINIMIZE, then the two ways back: the same box again, and the bar. */
    menu_window_min_box(&mx0, &my0, &mx1, &my1);
    menu_window_feed(1, (mx0 + mx1) / 2, (my0 + my1) / 2);
    menu_window_feed(0, (mx0 + mx1) / 2, (my0 + my1) / 2);
    CHECK(menu_window_state() == MW_MIN, "the minimize box did not minimize");

    menu_window_feed(1, (mx0 + mx1) / 2, (my0 + my1) / 2);
    menu_window_feed(0, (mx0 + mx1) / 2, (my0 + my1) / 2);
    CHECK(menu_window_state() == MW_NORMAL, "the minimize box did not restore");

    menu_window_minimize();
    menu_window_feed(1, MW_X + 10, MW_Y + 2);
    menu_window_feed(0, MW_X + 10, MW_Y + 2);
    CHECK(menu_window_state() == MW_NORMAL, "a tap on the minimized bar did not restore");

    /* The X still works from the minimized bar. */
    menu_window_minimize();
    menu_window_close_box(&bx0, &by0, &bx1, &by1);
    menu_window_feed(1, (bx0 + bx1) / 2, (by0 + by1) / 2);
    menu_window_feed(0, (bx0 + bx1) / 2, (by0 + by1) / 2);
    CHECK(menu_window_state() == MW_CLOSED, "the X did not close the minimized window");

    /* reset() clears the finger as well as the state: a stale press must not survive
     * into the next window and fire on its first release. */
    menu_window_open();
    menu_window_close_box(&bx0, &by0, &bx1, &by1);
    menu_window_feed(1, (bx0 + bx1) / 2, (by0 + by1) / 2);
    menu_window_reset();
    CHECK(menu_window_pressed() == MW_HIT_NONE, "reset left a finger down");
    menu_window_open();
    CHECK(menu_window_feed(0, (bx0 + bx1) / 2, (by0 + by1) / 2) == MZ_FEED_TAKEN,
          "a release after reset was not swallowed");
    CHECK(menu_window_state() == MW_NORMAL, "a stale press closed a fresh window");

    menu_window_reset();
}

static void test_paint(void)
{
    struct menu_view v;
    int bx0, by0, bx1, by1, mx0, my0, mx1, my1, x, y, i, unwritten;
    int bpp = 16;

    /* NORMAL, nothing pressed. The window is opened FIRST: a closed window paints
     * nothing at all, and every check below would pass for the wrong reason. */
    menu_window_reset();
    menu_window_open();
    sentinel_fill(MW_H);
    view_of(&v, MW_H);
    menu_window_paint(&v, MW_HIT_NONE, -1);

    /* The page area is the LOADER's, not the chrome's: the chrome must leave the
     * page's INTERIOR alone, or a repaint of a border would erase the page it is
     * drawn over -- and the page is a browser frame that cannot be re-fetched
     * cheaply. The window's own outer ring is the one exception and is the point:
     * the frame is drawn last, over everything, and its bottom row and right column
     * necessarily land in the page's rows. */
    unwritten = 0;
    for (y = MW_CONTENT_Y; y < MW_H - 1; y++)
        for (x = 1; x < MW_W - 1; x++)
            if (at(x, y) != SENTINEL16)
                unwritten++;
    CHECK(unwritten == 0, "the chrome wrote %d pixels of the page's interior",
          unwritten);

    /* The ring it is allowed to write is exactly one pixel wide. */
    unwritten = 0;
    for (y = MW_CONTENT_Y; y < MW_H; y++)
        if (at(MW_W - 1, y) != menu_pixel(bpp, MENU_BORDER))
            unwritten++;
    for (x = 0; x < MW_W; x++)
        if (at(x, MW_H - 1) != menu_pixel(bpp, MENU_BORDER))
            unwritten++;
    CHECK(unwritten == 0, "%d pixels of the window's bottom/right edge are not the"
          " border class", unwritten);

    /* ...and every row ABOVE it is written, so nothing of whatever was on the glass
     * before shows through the window's own furniture. */
    unwritten = 0;
    for (y = 0; y < MW_CONTENT_Y; y++)
        for (x = 0; x < MW_W; x++)
            if (at(x, y) == SENTINEL16)
                unwritten++;
    CHECK(unwritten == 0, "%d pixels of the window's own rows were never written",
          unwritten);

    /* The bars and the edge. */
    CHECK(at(2, 2) == menu_pixel(bpp, MENU_BTN), "the title bar is not the bar class");
    CHECK(at(2, MW_TITLE_H + 2) == menu_pixel(bpp, MENU_FILL),
          "the url field is not the fill class");
    CHECK(at(0, 0) == menu_pixel(bpp, MENU_BORDER), "the window's edge is not drawn");
    CHECK(at(MW_W - 1, MW_H - 1) == menu_pixel(bpp, MENU_BORDER),
          "the window's bottom-right edge is not drawn");
    CHECK(at(2, MW_CONTENT_Y - 1) == menu_pixel(bpp, MENU_DIV),
          "there is no divider under the url field");

    /* THE TWO MARKS ARE DIFFERENT MARKS. Both boxes have ink at their centre, so the
     * centre cannot tell them apart -- the diagonals can: the X has ink off its own
     * centre line, the minimize has only its middle row. If someone ever draws both
     * from one helper, this is what says so. */
    menu_window_close_box(&bx0, &by0, &bx1, &by1);
    CHECK(is_mark(at(WX(bx0 + 4), WY(by0 + 4)), bpp), "the X has no ink at its top-left");
    CHECK(is_mark(at(WX(bx1 - 5), WY(by0 + 4)), bpp), "the X has no ink at its top-right");
    CHECK(is_mark(at(WX((bx0 + bx1) / 2), WY((by0 + by1) / 2)), bpp), "the X has no centre");
    CHECK(at(WX(bx0 + 4), WY(by0 + 14)) == menu_pixel(bpp, MENU_BTN),
          "the X's face between its strokes is not the button face");

    menu_window_min_box(&mx0, &my0, &mx1, &my1);
    CHECK(!is_mark(at(WX(mx0 + 4), WY(my0 + 4)), bpp),
          "the minimize box has a diagonal in it -- it is being drawn as an X");
    CHECK(is_mark(at(WX((mx0 + mx1) / 2), WY((my0 + my1) / 2)), bpp),
          "the minimize box has no stroke");
    CHECK(is_mark(at(WX(mx0 + 4), WY((my0 + my1) / 2)), bpp),
          "the minimize stroke does not run the width of its box");

    /* Both boxes have an outline and a face that is not the bar they sit on, or a box
     * is invisible until it is pressed. */
    CHECK(at(WX(bx0), WY(by0)) == menu_pixel(bpp, MENU_LABEL),
          "the close box has no outline");
    CHECK(at(WX(mx0), WY(my0)) == menu_pixel(bpp, MENU_LABEL),
          "the minimize box has no outline");
    CHECK(at(WX(bx0 - MW_BTN_GAP + 1), WY(by0 + 2)) == menu_pixel(bpp, MENU_BTN),
          "the gap between the two boxes is not the bar's own face");

    /* PAINTING IT TWICE PAINTS IT ONCE, which is the module's stated rule and the
     * reason a press and a state change can both ask for a repaint. */
    memcpy(whole, wfb, sizeof whole);
    for (i = 0; i < 3; i++)
        menu_window_paint(&v, MW_HIT_NONE, -1);
    CHECK(memcmp(whole, wfb, sizeof whole) == 0,
          "a second identical paint changed the chrome");

    /* The pressed cell changes the image, and only on the box it names. */
    menu_window_paint(&v, MW_HIT_CLOSE, -1);
    CHECK(at(WX((bx0 + bx1) / 2), WY((by0 + by1) / 2)) == menu_pixel(bpp, MENU_LABEL_PRESSED),
          "the pressed X is not drawn dark-on-accent");
    CHECK(at(2, 2) == menu_pixel(bpp, MENU_BTN),
          "pressing the X changed the title bar too");
    menu_window_paint(&v, MW_HIT_NONE, -1);
    CHECK(at(WX((bx0 + bx1) / 2), WY((by0 + by1) / 2)) == menu_pixel(bpp, MENU_LABEL),
          "the X did not come back after the finger left it");

    /* MINIMIZED: the bar and nothing below it, so the plane can be 32 rows and the
     * buffer is not a 600-row one with its bottom half ignored. */
    sentinel_fill(MW_TITLE_H);
    view_of(&v, MW_TITLE_H);
    menu_window_reset();
    menu_window_open();
    menu_window_minimize();
    menu_window_paint_chrome(&v, MW_HIT_NONE, -1);
    unwritten = 0;
    for (y = 0; y < MW_TITLE_H; y++)
        for (x = 0; x < MW_W; x++)
            if (at(x, y) == SENTINEL16)
                unwritten++;
    CHECK(unwritten == 0, "the minimized bar left %d pixels unwritten", unwritten);
    menu_window_close_box(&bx0, &by0, &bx1, &by1);
    CHECK(is_mark(at(WX(bx0 + 4), WY(by0 + 4)), bpp),
          "the minimized bar has no X in it -- there would be no way out of it");
    CHECK(at(0, MW_TITLE_H - 1) == menu_pixel(bpp, MENU_BORDER),
          "the minimized bar has no bottom edge");

    /* A closed window paints NOTHING: it has no rect, and a painter that fell back to
     * the full size would draw a window the state says is not there. */
    sentinel_fill(MW_H);
    view_of(&v, MW_H);
    menu_window_close();
    menu_window_paint_chrome(&v, MW_HIT_NONE, -1);
    unwritten = 0;
    for (y = 0; y < MW_H; y++)
        for (x = 0; x < MW_W; x++)
            if (at(x, y) != SENTINEL16)
                unwritten++;
    CHECK(unwritten == 0, "a closed window painted %d pixels", unwritten);

    menu_window_reset();
}

/* --- 5. the typing, the page click, and the ring ----------------------------- */

/* Press and release one key by index, through the feed -- the whole path a finger
 * takes, so the shift's one-shotness and the page's click coordinate are exercised
 * where they actually happen. */
static void tap_key(int i)
{
    int kx0, ky0, kx1, ky1, x, y;

    menu_keyboard_key_rect(i, &kx0, &ky0, &kx1, &ky1);
    x = (kx0 + kx1) / 2;
    y = (ky0 + ky1) / 2;
    menu_window_feed(1, x, y);
    menu_window_feed(0, x, y);
}

static int key_of(char c)
{
    int i;

    for (i = 0; i < menu_keyboard_count(); i++) {
        const struct menu_key *k = menu_keyboard_key(i);

        if (k && k->act == MK_CHAR && k->ch == c)
            return i;
    }
    return -1;
}

static int special_of(int act)
{
    int i;

    for (i = 0; i < menu_keyboard_count(); i++) {
        const struct menu_key *k = menu_keyboard_key(i);

        if (k && k->act == act)
            return i;
    }
    return -1;
}

static void tap_field(void)
{
    int ux0, uy0, ux1, uy1;

    menu_window_url_rect(&ux0, &uy0, &ux1, &uy1);
    /* CLEAR OF THE NAV BOXES: they live in the field's own row, so a tap at the
     * row's left edge is now the back button and not the field. MW_URL_TEXT_X is
     * where the address text starts, which is exactly the first column that is the
     * field's and not a box's. */
    menu_window_feed(1, ux0 + MW_URL_TEXT_X + 20, uy0 + 5);
    menu_window_feed(0, ux0 + MW_URL_TEXT_X + 20, uy0 + 5);
}

/* One tap on a nav box. Built the same way every other tap in this file is -- press
 * and release on the same point -- so the latch rule is exercised and not bypassed. */
static void tap_nav(int forward)
{
    int bx0, by0, bx1, by1;

    if (forward)
        menu_window_fwd_box(&bx0, &by0, &bx1, &by1);
    else
        menu_window_back_box(&bx0, &by0, &bx1, &by1);
    menu_window_feed(1, (bx0 + bx1) / 2, (by0 + by1) / 2);
    menu_window_feed(0, (bx0 + bx1) / 2, (by0 + by1) / 2);
}

static void test_input(void)
{
    struct mw_req rq;
    int kq, ka, kshift, kdel, kok, ksp;
    int ux0, uy0, ux1, uy1, cx0, cy0, cx1, cy1;
    unsigned seq;

    kq = key_of('q');
    ka = key_of('a');
    kshift = special_of(MK_SHIFT);
    kdel = special_of(MK_BACK);
    kok = special_of(MK_OK);
    ksp = special_of(MK_SPACE);
    CHECK(kq >= 0 && ka >= 0 && kshift >= 0 && kdel >= 0 && kok >= 0 && ksp >= 0,
          "the keyboard's table is missing a key this test drives");

    menu_window_reset();
    menu_window_open();
    menu_window_set_url("example.com");

    /* With nothing focused the field is the PAGE's url, and it is not selected -- a
     * selection there would be an inversion the operator never asked for. */
    CHECK(menu_window_target() == MW_EDIT_NONE, "a fresh window is editing something");
    CHECK(!menu_keyboard_is_up(), "a fresh window has the keyboard up");
    CHECK(strcmp(menu_window_field(), "example.com") == 0,
          "with nothing focused the field shows '%s', not the page's url",
          menu_window_field());
    CHECK(!menu_window_field_sel(), "the live url is drawn as a selection");

    /* THE FIELD IS THE KEYBOARD'S DOOR, and the press that opens it SELECTS what is
     * there -- that is what makes typing a new address one tap and then characters
     * rather than one tap and then thirty backspaces. */
    menu_window_url_rect(&ux0, &uy0, &ux1, &uy1);
    CHECK(menu_window_hit(ux0 + MW_URL_TEXT_X + 20, uy0 + 5) == MW_HIT_URL,
          "the url field's own row is not the field");
    CHECK(menu_window_feed(1, ux0 + MW_URL_TEXT_X + 20, uy0 + 5) == MZ_FEED_TAKEN,
          "the press on the field was not taken");
    CHECK(menu_window_state() == MW_NORMAL, "the field fired on the PRESS");
    CHECK(menu_window_feed(0, ux0 + MW_URL_TEXT_X + 20, uy0 + 5) == MZ_FEED_TAKEN,
          "the release on the field was not taken");
    CHECK(menu_window_target() == MW_EDIT_URL, "the field is not the edit target");
    CHECK(menu_keyboard_is_up(), "tapping the url field did not raise the keyboard");
    CHECK(menu_window_field_sel(),
          "the field was not SELECTED -- the first character would append to the "
          "old address instead of replacing it");
    CHECK(strcmp(menu_window_field(), "example.com") == 0,
          "the selected field shows '%s'", menu_window_field());

    /* The keyboard is up, so the bottom of the window is the keyboard and not the
     * page -- the same point that was MW_HIT_CONTENT a moment ago. */
    menu_window_content_rect(&cx0, &cy0, &cx1, &cy1);
    CHECK(menu_window_hit(cx0 + 10, cy1 - 10) == MW_HIT_KEYBOARD,
          "the raised keyboard does not own the bottom of the page");
    CHECK(menu_window_hit(cx0 + 10, cy0 + 10) == MW_HIT_CONTENT,
          "the raised keyboard swallowed the part of the page above it");

    /* The pressed KEY is per key, not per cell: two caps are two marks and one
     * MW_HIT_KEYBOARD, so the cell cannot say which one is lit. */
    {
        int kx0, ky0, kx1, ky1, wx0, wy0, wx1, wy1;
        int kw = key_of('w');

        CHECK(kw >= 0, "the keyboard has no 'w'");
        menu_keyboard_key_rect(kq, &kx0, &ky0, &kx1, &ky1);
        menu_keyboard_key_rect(kw, &wx0, &wy0, &wx1, &wy1);
        CHECK(wy0 == ky0 && wx0 == kx1,
              "this test assumes 'w' is the cap right of 'q'");

        menu_window_feed(1, (kx0 + kx1) / 2, (ky0 + ky1) / 2);
        CHECK(menu_window_key_pressed() == kq,
              "the finger is on key %d, not %d, so the wrong cap would light",
              menu_window_key_pressed(), kq);
        CHECK(menu_window_pressed() == MW_HIT_KEYBOARD,
              "the keyboard is not the pressed cell");

        /* SLID ONTO THE NEXT CAP AND LIFTED. Every key is one MW_HIT_KEYBOARD, so
         * the chrome's roll-off rule cannot refuse this -- the cell never changed.
         * Only the key-level latch can, and it must type NEITHER key: the operator
         * pressed 'q' and never chose 'w', which is what the finger happened to be
         * over at the lift. */
        menu_window_feed(0, (wx0 + wx1) / 2, (wy0 + wy1) / 2);
        CHECK(menu_window_key_pressed() == -1, "the finger is still on a key after a lift");
        CHECK(strcmp(menu_window_field(), "example.com") == 0,
              "a finger that slid from 'q' onto 'w' and lifted typed '%s' into the "
              "url", menu_window_field());
    }

    /* The first character REPLACES the selection. */
    tap_key(kq);
    CHECK(strcmp(menu_window_field(), "q") == 0,
          "the first character left the field as '%s', not 'q' -- the selection "
          "was not replaced", menu_window_field());
    CHECK(!menu_window_field_sel(), "the field is still selected after typing");

    /* Nothing has been sent to the browser yet: the URL field's text is OURS, and it
     * becomes a request when OK is tapped. */
    CHECK(menu_window_take_req(&rq) == 0,
          "a character typed into url field became a browser request");

    /* Backspace edits the buffer, and a space is a character like any other. */
    tap_key(key_of('a'));
    tap_key(ksp);
    tap_key(key_of('b'));
    CHECK(strcmp(menu_window_field(), "qa b") == 0,
          "the buffer reads '%s', not 'qa b'", menu_window_field());
    tap_key(kdel);
    CHECK(strcmp(menu_window_field(), "qa ") == 0,
          "DEL left the buffer as '%s'", menu_window_field());

    /* SHIFT is one-shot through the whole path: the cap shows the capital and the
     * tap types it, and the next letter is lower case again. */
    tap_key(kshift);
    CHECK(menu_keyboard_shift(), "the SHIFT tap did not arm the shift");
    tap_key(kq);
    CHECK(strcmp(menu_window_field(), "qa Q") == 0,
          "the armed shift typed '%s' into the url, not 'qa Q'",
          menu_window_field());
    tap_key(key_of('z'));
    CHECK(strcmp(menu_window_field(), "qa Qz") == 0,
          "the shift was not one-shot: the buffer reads '%s'",
          menu_window_field());
    CHECK(!menu_keyboard_shift(), "the shift survived the letter it armed");

    /* OK NAVIGATES and lowers the keyboard -- "hides on OK" is the operator's own
     * word, and a keyboard left up over the page it just loaded is furniture they
     * would have to dismiss before reading it. */
    tap_key(kok);
    CHECK(menu_window_target() == MW_EDIT_NONE, "OK left the field as the target");
    CHECK(!menu_keyboard_is_up(), "OK left the keyboard on the glass");
    CHECK(menu_window_take_req(&rq) == 1, "OK recorded no request");
    CHECK(rq.kind == MW_REQ_NAV, "OK recorded request %d, not a navigation", rq.kind);
    CHECK(strcmp(rq.text, "qa Qz") == 0,
          "the navigation carries '%s', not what the field showed", rq.text);
    CHECK(menu_window_take_req(&rq) == 0, "OK recorded more than one request");

    /* An empty field does not navigate: a tap on OK with nothing typed would load
     * the empty URL, which on this browser is a request the operator did not make. */
    tap_field();
    CHECK(menu_window_field_sel(), "the second tap on the field did not select it");
    tap_key(kdel);              /* the selection makes this clear the field */
    CHECK(strcmp(menu_window_field(), "") == 0,
          "DEL on a selected field left '%s'", menu_window_field());
    tap_key(kok);
    CHECK(menu_window_take_req(&rq) == 0, "OK navigated an empty field");
    CHECK(!menu_keyboard_is_up(), "OK on an empty field left the keyboard up");

    /* THE PAGE'S OWN INPUT, off a GENERATION rather than a flag. The failure this
     * stands in front of is the operator's second attempt: dismiss the keyboard on
     * a field that is still focused, then tap it again. A flag never changes and
     * the second tap raises nothing. */
    menu_window_page_input(0);
    CHECK(menu_window_target() == MW_EDIT_NONE, "focus 0 set an edit target");
    CHECK(!menu_keyboard_is_up(), "focus 0 raised the keyboard");

    menu_window_page_input(1);
    CHECK(menu_window_target() == MW_EDIT_PAGE,
          "the page focusing an input did not become the target");
    CHECK(menu_keyboard_is_up(),
          "the page focusing an input did not raise the keyboard -- the operator's "
          "'the keyboard should show up every time there's an input item'");

    /* The SAME generation again is nothing: this is called every tick, and a
     * generation that re-fired would raise the keyboard the operator had just
     * dismissed, once a frame, forever. */
    menu_window_page_input(1);
    CHECK(menu_window_target() == MW_EDIT_PAGE,
          "the same focus generation changed the target");

    /* Dismissed, then focused AGAIN -- the field never blurred, so a flag would be
     * unchanged here and the second tap would raise nothing. */
    menu_window_page_input(0);
    CHECK(menu_window_target() == MW_EDIT_NONE, "the page blur left an edit target");
    CHECK(!menu_keyboard_is_up(), "the page blur left the keyboard up");
    menu_window_page_input(2);
    CHECK(menu_window_target() == MW_EDIT_PAGE,
          "re-focusing the same field did not raise the keyboard again");
    CHECK(menu_keyboard_is_up(), "the second focus did not raise the keyboard");

    /* A page input is the BROWSER's: characters go out one at a time and nothing is
     * kept here, because the page draws them and this process cannot read them back. */
    tap_key(ka);
    CHECK(menu_window_take_req(&rq) == 1, "a character typed into the page vanished");
    CHECK(rq.kind == MW_REQ_TEXT, "a page character recorded request %d", rq.kind);
    CHECK(strcmp(rq.text, "a") == 0, "a page character sent '%s'", rq.text);
    CHECK(menu_window_target() == MW_EDIT_PAGE,
          "typing into the page changed the target");
    /* The field is NOT holding the page's characters: with the page as the target it
     * shows the page's own url, because those characters are the page's and this
     * process cannot read them back. */
    CHECK(strcmp(menu_window_field(), "example.com") == 0,
          "a page character landed in a buffer of ours ('%s')",
          menu_window_field());

    tap_key(kdel);
    CHECK(menu_window_take_req(&rq) == 1, "DEL in the page recorded nothing");
    CHECK(rq.kind == MW_REQ_KEY && strcmp(rq.text, "Backspace") == 0,
          "DEL in the page sent %d/'%s', not Backspace", rq.kind, rq.text);

    tap_key(kok);
    CHECK(menu_window_take_req(&rq) == 1, "OK in the page recorded nothing");
    CHECK(rq.kind == MW_REQ_KEY && strcmp(rq.text, "Enter") == 0,
          "OK in the page sent %d/'%s', not Enter", rq.kind, rq.text);
    CHECK(menu_window_target() == MW_EDIT_NONE, "OK in the page left the target");
    CHECK(!menu_keyboard_is_up(), "OK in the page left the keyboard up");

    /* A PAGE FOCUS DOES NOT STEAL THE ADDRESS BAR: the operator editing the url is
     * left alone, even as the page underneath focuses a field of its own. */
    tap_field();
    CHECK(menu_window_target() == MW_EDIT_URL, "the field did not take the edit");
    menu_window_page_input(7);
    CHECK(menu_window_target() == MW_EDIT_URL,
          "the page stole the url field out from under the operator");
    CHECK(menu_keyboard_is_up(), "the url edit lost its keyboard");

    /* A TAP ON THE PAGE ends the url edit and is recorded as a CLICK at the page's
     * own coordinate -- the press's point, which is what the browser follows a link
     * with. "Clicking should allow navigation" is the operator's own word. */
    menu_window_content_rect(&cx0, &cy0, &cx1, &cy1);
    CHECK(menu_window_feed(1, cx0 + 100, cy0 + 50) == MZ_FEED_TAKEN,
          "a press on the page was not taken");
    CHECK(menu_window_feed(0, cx0 + 100, cy0 + 50) == MZ_FEED_TAKEN,
          "a release on the page was not taken");
    CHECK(menu_window_target() == MW_EDIT_NONE, "tapping the page left the url edit on");
    CHECK(!menu_keyboard_is_up(), "tapping the page left the keyboard up");
    CHECK(menu_window_take_req(&rq) == 1, "the page tap recorded no click");
    CHECK(rq.kind == MW_REQ_POINT, "the page tap recorded request %d", rq.kind);
    CHECK(rq.x == 100 && rq.y == 50,
          "the page click went to %d,%d -- the browser's page origin is the "
          "content rect's top-left, not the window's", rq.x, rq.y);

    /* The ring is drained in order and then empty. */
    CHECK(menu_window_take_req(&rq) == 0, "the ring still had a request in it");

    /* A second tap on the field re-seeds the buffer from the page's own url, and OK
     * navigates THAT -- the field is never left holding what a previous edit typed. */
    tap_field();
    tap_key(kok);
    CHECK(menu_window_take_req(&rq) == 1,
          "OK on a field re-seeded from the page's url recorded nothing");
    CHECK(rq.kind == MW_REQ_NAV && strcmp(rq.text, "example.com") == 0,
          "the re-seeded field navigated %d/'%s'", rq.kind, rq.text);

    /* A RESET EMPTIES THE RING: a request recorded before it belongs to a window
     * that is gone, and replaying it would click a page that is no longer there. */
    menu_window_page_input(9);
    tap_key(ka);
    CHECK(menu_window_take_req(&rq) == 1, "a page character recorded nothing");
    tap_key(ka);
    menu_window_reset();
    CHECK(menu_window_take_req(&rq) == 0, "reset left a request in the ring");
    CHECK(menu_window_target() == MW_EDIT_NONE, "reset left an edit target");
    CHECK(!menu_keyboard_is_up(), "reset left the keyboard up");

    /* The field's sequence is the REPAINT gate: the live url changing has to bump it,
     * or a page that navigated keeps the old address on the glass. */
    menu_window_open();
    menu_window_set_url("one.example");
    seq = menu_window_field_seq();
    menu_window_set_url("one.example");
    CHECK(menu_window_field_seq() == seq,
          "re-setting the same url bumped the repaint sequence -- a repaint every "
          "tick, since this is called on every one");
    menu_window_set_url("two.example");
    CHECK(menu_window_field_seq() != seq,
          "a new url did not bump the repaint sequence -- the field would keep "
          "showing the old address");
    CHECK(strcmp(menu_window_field(), "two.example") == 0,
          "the field shows '%s' after the page navigated", menu_window_field());

    menu_window_reset();
}

/* THE BACK AND FORWARD BOXES, and they are the one pair of cells in this window
 * that overlap another: they sit INSIDE the URL field's rect, so a hit test that
 * asked the field first would make them unreachable -- every tap on them would
 * raise the keyboard instead. That ordering is what this test exists to pin, along
 * with the geometry (inside the row, inside the window, not overlapping each other)
 * and the two requests a tap records. */
static void test_nav(void)
{
    struct mw_req rq;
    int ax0, ay0, ax1, ay1, fx0, fy0, fx1, fy1, ux0, uy0, ux1, uy1;

    menu_window_reset();
    menu_window_open();
    menu_window_set_url("example.com");

    menu_window_back_box(&ax0, &ay0, &ax1, &ay1);
    menu_window_fwd_box(&fx0, &fy0, &fx1, &fy1);
    menu_window_url_rect(&ux0, &uy0, &ux1, &uy1);

    CHECK(ax0 >= MW_X && fx1 <= MW_X + MW_W, "a nav box is outside the window");
    CHECK(ay0 >= uy0 && ay1 <= uy1, "a nav box is outside the url row");
    CHECK(ax1 <= fx0, "the back and forward boxes overlap");
    CHECK(ax0 > ux0, "the nav boxes start on the window's own edge, not inset from it");
    CHECK(MW_URL_TEXT_X >= fx1 - MW_X + 8,
          "the address text starts under the forward box -- the two would be legible "
          "on top of one another");

    /* The cells, and the one-pixel misses. */
    CHECK(menu_window_hit(ax0, ay0) == MW_HIT_BACK, "the back box's own corner misses");
    CHECK(menu_window_hit(ax1 - 1, ay1 - 1) == MW_HIT_BACK, "the back box's far corner misses");
    CHECK(menu_window_hit(ax0 - 1, ay0 + 1) != MW_HIT_BACK, "one px left of back hits it");
    CHECK(menu_window_hit(ax1, ay0 + 1) != MW_HIT_BACK, "one px right of back hits it");
    CHECK(menu_window_hit(ax0 + 1, ay0 - 1) != MW_HIT_BACK, "one px above back hits it");
    CHECK(menu_window_hit(ax0 + 1, ay1) != MW_HIT_BACK, "one px below back hits it");
    CHECK(menu_window_hit(fx0, fy0) == MW_HIT_FORWARD, "the forward box's own corner misses");
    CHECK(menu_window_hit(fx1 - 1, fy1 - 1) == MW_HIT_FORWARD,
          "the forward box's far corner misses");
    CHECK(menu_window_hit(fx1, fy0 + 1) != MW_HIT_FORWARD, "one px right of forward hits it");
    CHECK(menu_window_hit(fx0 - 1, fy0 + 1) != MW_HIT_FORWARD, "one px left of forward hits it");

    /* A tap on each records the walk it names, and does NOT raise the keyboard: the
     * boxes are inside the field's row, so this is the whole of the ordering. */
    CHECK(!menu_keyboard_is_up(), "the keyboard was up before the back tap");
    tap_nav(0);
    CHECK(menu_window_take_req(&rq) == 1, "the back tap recorded nothing");
    CHECK(rq.kind == MW_REQ_HIST, "the back tap recorded request %d, not a history walk",
          rq.kind);
    CHECK(rq.x == -1, "the back tap asked for %d, not -1", rq.x);
    CHECK(!menu_keyboard_is_up(), "the back tap raised the keyboard -- the box is inside "
          "the field's row and was not asked first");
    CHECK(menu_window_target() == MW_EDIT_NONE, "the back tap started a url edit");

    tap_nav(1);
    CHECK(menu_window_take_req(&rq) == 1, "the forward tap recorded nothing");
    CHECK(rq.kind == MW_REQ_HIST && rq.x == 1,
          "the forward tap recorded %d/%d, not a history walk of +1", rq.kind, rq.x);
    CHECK(menu_window_take_req(&rq) == 0, "the two nav taps recorded more than two requests");

    /* MINIMIZED: the row the boxes live in is gone with the page, so they are not
     * there to hit -- the same answer the field itself gives. */
    menu_window_minimize();
    CHECK(menu_window_hit((ax0 + ax1) / 2, (ay0 + ay1) / 2) == MW_HIT_NONE,
          "the back box is live in the minimized bar -- its row is not on the glass");
    CHECK(menu_window_hit((fx0 + fx1) / 2, (fy0 + fy1) / 2) == MW_HIT_NONE,
          "the forward box is live in the minimized bar");
    menu_window_restore();
}

/* SWIPE-TO-SCROLL. A drag inside the page is a scroll, a tap is a click, and the
 * ONLY thing that separates them is travel: the touch panel reports a held finger as
 * a run of `down` reports, so a drag is not "a press with moves in between" that
 * this module can see as one event -- each report arrives as a press, and the run has
 * to be recognised by where the finger went. The two failures this pins are the two
 * the operator would see: a scroll that ALSO clicks (navigating to whatever was
 * under the finger when it landed) and a scroll that wanders onto the address row
 * and raises the keyboard. */
static void test_scroll(void)
{
    struct mw_req rq;
    int cx0, cy0, cx1, cy1;
    int x, y;

    menu_window_reset();
    menu_window_open();
    menu_window_content_rect(&cx0, &cy0, &cx1, &cy1);
    x = cx0 + 200;
    y = cy0 + 100;

    /* A TAP THAT WANDERS A FEW PIXELS IS STILL A TAP, and it clicks the PRESS's
     * point -- the finger drifted, the operator aimed at the link. */
    CHECK(menu_window_feed(1, x, y) == MZ_FEED_TAKEN, "the tapped press was not taken");
    CHECK(menu_window_feed(1, x + 5, y + 5) == MZ_FEED_TAKEN, "a small drift was not taken");
    CHECK(menu_window_take_req(&rq) == 0, "a drift under the slop recorded a scroll (%d/%d)",
          rq.x, rq.y);
    CHECK(menu_window_feed(0, x + 5, y + 5) == MZ_FEED_TAKEN, "the tap's release was not taken");
    CHECK(menu_window_take_req(&rq) == 1, "a tap with a small drift recorded nothing");
    CHECK(rq.kind == MW_REQ_POINT, "a tap with a small drift recorded %d, not a click", rq.kind);
    CHECK(rq.x == x - MW_X && rq.y == y - (MW_Y + MW_CONTENT_Y),
          "the click went to %d/%d -- a small drift moved it off the press point",
          rq.x, rq.y);

    /* A DRAG: nothing before the slop (a tap that jitters is a tap), a scroll
     * request once past it, and NO click at the release. */
    CHECK(menu_window_feed(1, x, y) == MZ_FEED_TAKEN, "the drag's press was not taken");
    CHECK(menu_window_feed(1, x, y + 8) == MZ_FEED_TAKEN, "a move was not taken");
    CHECK(menu_window_take_req(&rq) == 0,
          "a move under the slop recorded a scroll (%d/%d)", rq.x, rq.y);
    CHECK(menu_window_feed(1, x, y + 24) == MZ_FEED_TAKEN, "a move past the slop was not taken");
    CHECK(menu_window_take_req(&rq) == 1, "the drag past the slop recorded nothing");
    CHECK(rq.kind == MW_REQ_SCROLL, "the drag recorded request %d, not a scroll", rq.kind);
    CHECK(rq.x == 0 && rq.y == 16,
          "the drag recorded %d/%d -- the finger moved +16 px past the slop and the page "
          "has to move with it", rq.x, rq.y);
    CHECK(menu_window_feed(0, x, y + 24) == MZ_FEED_TAKEN, "the drag's release was not taken");
    CHECK(menu_window_take_req(&rq) == 0,
          "the drag ALSO recorded a click at %d/%d -- a scroll would follow the link it "
          "happened to end on", rq.x, rq.y);

    /* A DRAG THAT WANDERS OFF THE PAGE STILL FIRES NOTHING -- not the click it began
     * on, and not the cell it ended on. A scroll that drifts up onto the address row
     * must not raise the keyboard. */
    CHECK(menu_window_feed(1, x, cy0 + 8) == MZ_FEED_TAKEN, "the wandering press was not taken");
    CHECK(menu_window_feed(1, x, cy0 - 14) == MZ_FEED_TAKEN,
          "the wandering move was not taken -- the drag left the page and the menu took it");
    CHECK(menu_window_take_req(&rq) == 1, "the wandering drag recorded nothing");
    CHECK(rq.kind == MW_REQ_SCROLL && rq.y < 0,
          "the wandering drag recorded %d/%d -- a finger moving UP is a scroll down",
          rq.kind, rq.y);
    CHECK(menu_window_hit(x, cy0 - 14) == MW_HIT_URL,
          "the wandering drag did not end on the address row -- the test pins nothing");
    CHECK(menu_window_feed(0, x, cy0 - 14) == MZ_FEED_TAKEN, "the wandering release was not taken");
    CHECK(menu_window_take_req(&rq) == 0,
          "the wandering drag recorded a second request (%d) -- ending on the address row "
          "would have raised the keyboard", rq.kind);
    CHECK(!menu_keyboard_is_up(), "the wandering drag raised the keyboard");
    CHECK(menu_window_target() == MW_EDIT_NONE, "the wandering drag started a url edit");

    /* AND THE NEXT TAP IS A TAP AGAIN -- the drag latch is per gesture, not sticky. */
    CHECK(menu_window_feed(1, x, y) == MZ_FEED_TAKEN, "the tap after a scroll was not taken");
    CHECK(menu_window_feed(0, x, y) == MZ_FEED_TAKEN, "the tap after a scroll was not released");
    CHECK(menu_window_take_req(&rq) == 1, "the tap after a scroll recorded nothing");
    CHECK(rq.kind == MW_REQ_POINT, "the tap after a scroll recorded %d, not a click", rq.kind);

    menu_window_reset();
}

int main(void)
{
    test_geometry();
    test_states();
    test_hit();
    test_feed();
    test_scroll();
    test_paint();
    test_input();
    test_nav();

    printf("test_window: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
