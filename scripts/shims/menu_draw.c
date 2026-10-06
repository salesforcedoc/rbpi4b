/*
 * menu_draw.c -- get the top menu onto the glass by the best route this machine
 * offers: a vc4 overlay plane that rbp's repaint cannot reach, and failing that
 * the real framebuffer, repaired every tick.
 *
 * THE PLANE IS THE ROUTE THIS WANTS AND THE PAGE IS THE ONE IT SETTLES FOR. A
 * plane is composited by the display controller out of its own buffer, so the
 * band is *presented* rather than *repaired* and the tick below stops repainting
 * altogether -- the difference between 99.07 % and all of it, and between a
 * visible shimmer and none. Everything in this file was written for the page and
 * is unchanged by the plane: drmband.h carries the proof that it composites on
 * this device and the reason the change is only where the band's writes land.
 * The page remains the route on any machine where the plane cannot be had, and
 * MENU_PLANE=0 is the A/B between them.
 *
 * WHY A THREAD, AND NOT fb_shim.c's PAN HOOK. This feature was planned onto the
 * FBIOPAN_DISPLAY interposer, on the strength of docs/13's note that with a
 * single buffer "the pan is a no-op that still goes through the shim". Measured
 * on the unit on 2026-09-29, that is not true of this configuration. Every
 * distinct framebuffer ioctl that reached the shim over a whole session, logged
 * at its first occurrence:
 *
 *     fb: ioctl 0x4600 first seen        FBIOGET_VSCREENINFO
 *     fb: ioctl 0x4602 first seen        FBIOGET_FSCREENINFO
 *     fb: ioctl 0x4601 first seen        FBIOPUT_VSCREENINFO
 *     fb: ioctl 0x4604 first seen        FBIOGETCMAP
 *     fb: ioctl 0x4605 first seen        FBIOPUTCMAP
 *     fb: ioctl 0x40044620 first seen    FBIO_WAITFORVSYNC
 *
 * 0x4606, FBIOPAN_DISPLAY, is not among them. With RB_DFB_PRESENT=off DirectFB's
 * layer surface IS the page (docs/13), and nothing ever pans it: the pan case in
 * fb_shim.c is dead code on this unit, and the panel could never have been drawn
 * from it. The panel's own geometry was not the obstacle -- this unit's vc4drmfb
 * reports xpanstep 1, ypanstep 1, so DirectFB's pan early-out is not what stops
 * it -- the call simply never happens.
 *
 * The vsync wait IS called, at a measured 57.1/s, and it is where the panel is
 * now composited from: fb_shim.c's FBIO_WAITFORVSYNC case calls menu_frame_tick()
 * BEFORE it performs the wait. The entry of the wait is the right side of rbp's
 * frame in either shape its loop can have -- in `draw; wait` it lands just after
 * the draw, and in `wait; draw` it lands just after the previous iteration's draw
 * too -- so the panel always goes up immediately after a completed frame, with the
 * whole inter-frame gap to be painted in. Painting AFTER the wait would be the
 * other side of that boundary: immediately before the next draw, and therefore
 * under it.
 *
 * SO: two callers of one tick, and that is deliberate. The vsync hook is the
 * frame boundary and does the work; the thread below remains because it can do the
 * two things the hook cannot. Mapping the framebuffer is a blocking open, ioctl
 * and mmap, and it retries for as long as the panel takes to appear -- that may
 * not run on rbp's RT render thread, and the hook has no way to schedule it. And
 * the hook's side of the frame is a reasoned conclusion, not an observed one: the
 * thread is what keeps the panel up if the conclusion is ever wrong, at the cost
 * of the witness checks below, which are a couple of dozen reads a tick and
 * nothing else. Whichever caller arrives first paints; the other finds the image
 * intact and does nothing.
 *
 * The thread is fb_cursor.c's shape, which is the compositor this port already
 * runs and has already measured on this glass. A detached thread ticks at a
 * period far shorter than rbp's frame, and each tick asks whether the panel is
 * still on the page and repaints it if it is not.
 *
 * The duty that buys comes straight out of fb_cursor.c's own measurement (its
 * header has the table): rbp repaints its ENTIRE frame at ~57 Hz into the one page
 * that is also the visible page, so anything composited between two frames is
 * erased by the next one and is visible for (frame - tick)/frame of the time --
 * 94% at a 1 ms tick, 97% at 0.5 ms, and 9% at 16 ms, which is how the arrow's
 * first version was debugged into the ground as "invisible". The panel is the same
 * physics over a larger region.
 *
 * WHY A WITNESS, AND NOT A FULL PASS EVERY TICK. fb_cursor.c paints every tick
 * unconditionally, which is free for a 12x19 arrow -- 228 cells -- and is what
 * makes it self-healing without ever asking "is the arrow still there?", a
 * question its own comment explains has no trustworthy answer. The panel is
 * 1280x56: 71,680 cells, 143 KB. A full read pass every tick would be ~1.4 GB/s
 * of uncached reads at the 0.1 ms period shipped, and almost all of it on ticks
 * where nothing has changed. (It was written as ~285 MB/s when the panel was 112
 * rows and the period 0.5 ms; the band halved and the period came down.)
 * menu_intact() reads a couple of dozen spread-out cells first and pays for the
 * full pass only when one of them is wrong. rbp's repaint of the band is
 * all-or-nothing, so one wrong cell is enough to know -- and unlike the arrow,
 * the question here ("does this pixel already hold the value the picture says it
 * should?") has an answer that does not depend on knowing who wrote it. What
 * remains is one full pass per rbp frame: ~57/s, ~17 MB/s, the number this was
 * costed at before the mechanism changed. (Both figures halve with the band: the
 * panel was 112 rows until 2026-09-29.)
 *
 * NO SAVE-UNDER, still, and for menu_paint.h's reason: rbp's next frame is the
 * restore. Nothing is drawn when the panel closes -- deliberately. The thing to
 * watch on the unit is a ghost that survives the close; that would mean rbp's
 * repaint is not as complete as docs/13 says, and the answer would be
 * cursor_paint.c's conditional restore rather than a change here.
 *
 * The framebuffer is measured with real_open/real_ioctl, bypassing the shim's own
 * FBIOGET_* interposition -- the one that makes rbp believe the geometry is its
 * logical 1280x800. That is fb_cursor.c:100-196's pattern and its reason: a panel
 * that is not that size has to be *measured*, and the picture rectangle inside it
 * comes from point_fit(), the same rule the DirectFB driver presents with.
 */
#define _GNU_SOURCE
#include "menu_draw.h"
#include "menu_paint.h"
#include "menu_window.h"
#include "menu_window_paint.h"
#include "menu_keyboard.h"
#include "browser_link.h"
#include "menu_zone.h"
#include "drmband.h"           /* the overlay plane, when this machine has one */
#include "fbdev.h"
#include "point_xform.h"
#include "pointsrc.h"          /* pointsrc_log */
#include "syscalls.h"
#include "envutil.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>             /* snprintf, for the window's command lines */
#include <stdlib.h>            /* malloc, for the rendered-image cache */
#include <string.h>
#include <sys/mman.h>          /* PROT_READ/PROT_WRITE/MAP_SHARED for real_mmap */
#include <time.h>
#include <unistd.h>

#define MENU_FB_DEFAULT  "/dev/fb0"

/* The tick period, in ms, and the floor under it in microseconds. The floor is
 * the granularity of nanosleep on this target -- a typo cannot turn this into a
 * busy loop. POINT_MENU_MS is a bench knob and deliberately not in start-rb.sh's
 * SHIM_VARS, like POINT_MENU_KEY_EXTRA: an ad-hoc value must not be overwritten
 * by the shipped empty string.
 *
 * THE DEFAULT IS 0.1 ms, and that is a measurement rather than the 0.5 ms this
 * inherited from fb_cursor.c (where 0.5 ms is where the *arrow* reaches ~97%
 * visible -- a different object, and at the 112-row panel it was also the right
 * answer here, because the miss was dominated by a 550 us repair copy that no
 * period could shorten). At the 56-row band the copy is 82 us and the period is
 * what is left, so the same instrument was pointed at it: `work/mm10.sh` ran the
 * shipped build and then `POINT_MENU_MS=0.1` on the unit, both measured with the
 * panel verifiably open, 12,000 fbpanel samples (~7 s) each, same build, one
 * variable:
 *
 *     period   duty     miss runs   longest   total miss   open - closed CPU
 *     0.5 ms   97.64%   275         1465 us   2.62%        +62 ticks/15 s (4.13%)
 *     0.1 ms   99.11%   107          908 us   0.94%        +88 ticks/15 s (5.87%)
 *
 * So five times the wakes buy +1.5 points of duty, halve the miss runs and cut
 * the worst miss by a third -- and the *paired* cost of the whole panel rises by
 * 1.7 points of one core, which is the price to state plainly. The misses that
 * remain are a repair in flight, not a blank panel: at 0.1 ms not one of the
 * 12,000 samples caught both of the band's border pairs missing. 100% is still
 * F5's (the structural composite) and not this knob's -- but it is not the
 * plane's either, because the plane does not do it with a number at all: a band
 * that is never erased has nothing to repair, so on a machine with a plane the
 * tick stops repainting entirely and the period stops mattering (MENU_PLANE,
 * drmband.h). This knob is what the fallback route is tuned by. One literal to
 * revert, or `POINT_MENU_MS=0.5` in the environment for a session. */
#define MENU_MS_DEFAULT  0.1
#define MENU_US_MIN      50L

/* How long to wait between attempts while there is no usable framebuffer. A
 * monitor plugged in long after boot -- or one whose fb the kernel tore down and
 * rebuilt -- must not cost the panel for the life of the process; 5 s is slower
 * than any of those transitions and faster than an impatient operator. */
#define MENU_RETRY_US    5000000L

struct menu_map {
    struct menu_view view;
    int  ok;                     /* 1 = mapped and drawable, 0 = not yet */
    int  painted_open;           /* the panel was open at the last tick */
    int  painted_pressed;        /* ...and this is the button it was painted with */
    int  paints;                 /* full passes since the process started */
    int  verbose;                /* MENU_VERBOSE: log every pass and what it was */

    /* THE PLANE THE BAND IS PRESENTED ON, when this machine has one, and the
     * reason this file stops having to repair anything.
     *
     * The band used to be rewritten into rbp's own framebuffer at 10 kHz because
     * rbp's 57.1 Hz repaint kept erasing it, and the miss run was bounded by the
     * tick -- 16.67 ms ("60 fps") measured 53.83 % duty against 0.1 ms's 99.07 %,
     * i.e. 49x *worse*, so no tick setting could be the cure. A plane is scanned
     * out by the display controller from a buffer rbp never writes, so the same
     * pixels are simply not erased and the tick has nothing to repair.
     *
     * NOTHING ABOVE THIS POINT CHANGES FOR IT. The image, the cache, the witness
     * and the blit all work off `view`, and when the band is up `view.pix` is the
     * plane's buffer instead of the page -- the same pixels at the same pitch in
     * the same format (drmband.h), so the classifier never learns which
     * destination it is drawing for. That is what makes this a small change
     * rather than a second renderer. */
    struct drm_band band;
    int  band_ok;                /* the plane is set up and mapped */
    int  band_on;                /* ...and it is on the glass right now */
    int  band_off;               /* MENU_PLANE=0: the fb0 route, deliberately */
    int  band_bx, band_by;       /* where the band's rect goes on the CRTC */

    /* THE BROWSER WINDOW'S PLANE -- the SAME SLOT as the band's, and only ever one
     * of the two at a time (see the block above menu_band_setup_from_page()). While
     * `win_ok` is set the band's plane does not exist at all and `band_ok` is 0, so
     * the two cannot both be believed.
     *
     * `win_h` is the height the plane was set up at, which is what makes a minimize
     * visible here: the state asks for MW_TITLE_H rows and the plane has to be
     * rebuilt rather than resized. And `win_painted_*` are the state and the pressed
     * cell the chrome currently in the buffer was drawn for -- the same
     * paint-when-it-changed rule the panel image uses, at one int each. */
    struct drm_band win;
    int  win_ok;                 /* the window's plane is set up and mapped */
    int  win_on;                 /* ...and it is on the glass right now */
    int  win_h;                  /* the height it was set up at (600 or 32) */
    int  win_fail;                /* this machine refused a window-sized plane */
    int  win_painted_state;      /* the state the chrome in the buffer is for */
    int  win_painted_hit;        /* ...and the cell it is highlighted for */
    int  win_painted_key;        /* ...and the cap of the keyboard it is for */
    unsigned win_painted_seq;    /* ...and the url field's text generation */
    int  win_painted_kb;         /* ...and whether the keyboard was up */

    /* The page, and the picture rect inside it, kept only so that a plane which
     * will not go up can be abandoned mid-session: the view goes back to these
     * and the band is a copy again, on the tick that finds out. */
    void *fb_pix;
    int   fb_pitch, fb_bx, fb_by;

    /* THE IMAGE, DOUBLE-BUFFERED.
     *
     * This is the fix for the flicker the operator reported, and it is a bug fix
     * rather than a budget. The classification is ~7 ms and the blit ~0.3 ms, and
     * both used to run under g_lock from whichever caller arrived -- including
     * rbp's SCHED_FIFO render thread, through the vsync hook. So while the witness
     * thread was classifying, every vsync tick failed its trylock and returned
     * without painting: the panel was not late, it was ABSENT for the whole pass,
     * 40% of a frame. A finger sliding across seven buttons crosses six boundaries,
     * so six such windows per slide -- and work/fbpanel.c's run-length histogram
     * was written to show exactly those ~7 ms runs.
     *
     * So the image is built in `back`, off the lock, and published by swapping the
     * two pointers under it. The vsync hook is then left with two things it can do
     * -- blit the published image, or read fifteen witness points -- and both are
     * bounded. A press transition costs at most one tick of highlight lag and ZERO
     * blanking.
     *
     * The swap is safe because a buffer is only ever the destination of a write
     * while it is NOT the front: after the swap the classifier's next build goes
     * into the buffer the blitter has just stopped reading, and the lock is what
     * makes "just stopped" exact. Only the witness thread ever writes either
     * pointer, and only under the lock.
     *
     * `front_pressed` is what the image in `front` was drawn for, which is not
     * necessarily what the finger is on right now: the blitter paints what the front
     * holds and compares the page against that same image, so a lagging classify
     * shows an old highlight rather than a damaged panel. The staleness is at most
     * one tick of the period below -- 0.1 ms shipped -- against the 7 ms blank it
     * replaced. */
    void  *front;                /* published; the only buffer a blitter reads */
    void  *back;                 /* the classifier's scratch; never read by a blitter */
    size_t img_bytes;
    int    front_pressed;
    int    front_valid;          /* 0 until the first classification succeeds */
};

static struct menu_map g;

/* Two callers, one image. The thread below ticks far more often than the vsync
 * wait does, and the wait -- a real frame boundary on this unit, measured at
 * 57.1/s -- ticks from rbp's own render thread, so the two can arrive together.
 * The paint is idempotent, so a lost race between them is harmless: what the lock
 * buys is that two passes over the same page do not interleave. A tick that finds
 * it taken returns immediately, because whoever holds it is painting this same
 * image right now.
 *
 * trylock, not lock. The vsync caller is rbp's render thread at SCHED_FIFO 98;
 * making it wait behind a normal-priority painter would be a priority inversion
 * for no gain, and the pass it would be waiting for is the one it wanted. */
/* ---------------- THE BROWSER WINDOW'S PLANE -------------------------------
 *
 * The window is NOT a second plane, and this block is where that is implemented
 * rather than wished away. Measured on the unit 2026-10-04: DRM master is one per
 * DEVICE, so the band's `drm_band_setup()` is granted it and a second setup in the
 * same process is refused EBUSY (a separate process is refused EACCES, 5/5,
 * because rbp's DirectFB holds the display). The first setup in a process gets
 * master; every later one does not, whoever asks.
 *
 * So the band and the window are MUTUALLY EXCLUSIVE HOLDERS OF A SINGLE PLANE, and
 * menu_window_plane_sync() hands the slot over: opening the window tears the band's
 * plane down and sets the window's up at the window's own size; closing it tears
 * the window's down and sets the band's back up. `drm_band_teardown()` drops
 * master as it goes, so the next setup retakes it -- which is what makes the
 * handover a teardown-and-setup and not a resize.
 *
 * The two routes, told apart in the log by which ids appear:
 *   window closed -> the band, as it always was:
 *                    `menu plane: crtc 102 plane 127 fb N 1280x56 16 bpp pitch ...`
 *   window open   -> the window, at 1120x600 -- or the title bar's 32 rows when it
 *                    is minimized, which is a genuinely smaller plane rather than
 *                    a tall one with its bottom half ignored.
 * Both carry the page's own bpp and RGB565, so the palette, the chrome and every
 * pixel rule in menu_window_paint.c are the same code on either buffer.
 *
 * WHAT OPENS THE WINDOW, NOW THAT THE MENU DOES NOT. The browser is abandoned and the
 * panel's globe cell went with it (menu_zone.h's block), so `MENU_WINDOW=1` below is no
 * longer a debug route but the ONLY route in: it opens the window at startup, with the
 * panel shut and no gesture in front of it. It is kept because the module still ships
 * and still works, and because it is how the chrome was pressed on the glass when the
 * X and the minimize were settled (that is the same reason, one tense later).
 *
 * THE MENU IS DEGRADED WHILE THE WINDOW IS OPEN, and that is stated rather than
 * hidden: the band has no plane, so a swipe-down draws the panel on the page and it
 * flickers exactly as it did before the plane existed. The alternative -- letting
 * the window refuse to coexist with the panel -- would mean the menu's own gesture
 * silently did nothing. It is visible, it is correct, and it ends when the window
 * does. */
static void menu_window_view(struct menu_view *wv);
static int  menu_band_setup_from_page(void);
static int  menu_panel_ph(void);      /* defined below; the band's own height */

/* Set the band's plane up from the PAGE's geometry, and point the view at it.
 *
 * Split out of menu_fb_open_unlocked() because the window's close path needs
 * exactly this: while the window is open the band has NO plane -- the window took
 * the slot -- so giving it back is a fresh setup and not a re-show. `g.view`'s
 * dw/dh/bpp are the page's throughout (the window never touches `g.view`; it paints
 * through a view of its own), so the size is still the right one to ask for. */
static int menu_band_setup_from_page(void)
{
    if (g.band_off)
        return 0;
    if (drm_band_setup(&g.band, g.view.dw, menu_panel_ph(), g.view.bpp) != 0)
        return -1;
    /* The band's own rectangle, and no longer the picture's: the plane is placed on
     * the CRTC at the picture's top-left corner and its buffer IS the band, so the
     * view moves to its origin. menu_blit()'s strided path handles whatever pitch
     * the driver chose for it. */
    g.band_ok    = 1;
    g.band_on    = 0;
    g.view.pix   = g.band.pix;
    g.view.pitch = g.band.pitch;
    g.view.bx    = 0;
    g.view.by    = 0;
    return 0;
}

/* A view over the window's plane buffer, WINDOW-LOCAL: origin 0,0 and dw/dh the
 * plane's own size. Deliberately not `g.view` -- while the window is up that still
 * describes the page for the close path, and nothing about the window should be
 * able to corrupt it. */
static void menu_window_view(struct menu_view *wv)
{
    memset(wv, 0, sizeof *wv);
    wv->pix   = g.win.pix;
    wv->pitch = g.win.pitch;
    wv->fb_w  = g.win.w;
    wv->fb_h  = g.win.h;
    wv->bpp   = g.win.bpp;
    wv->dw    = g.win.w;
    wv->dh    = g.win.h;
}

/* The window's chrome, the url text and the keyboard, over whatever the page area
 * already holds. Called on a change and NOT every tick: all of it is a pure function
 * of the state, the pressed cell, the pressed cap and the field's text, so a repaint
 * that would write the same pixels is skipped by comparing those. */
static void menu_window_repaint(void)
{
    struct menu_view wv;
    int st  = menu_window_state();
    int hit = menu_window_pressed();
    int key = menu_window_key_pressed();
    unsigned seq = menu_window_field_seq();

    if (g.win_painted_state == st && g.win_painted_hit == hit &&
        g.win_painted_key == key && g.win_painted_seq == seq)
        return;
    menu_window_view(&wv);
    if (menu_view_ok(&wv))
        menu_window_paint_chrome(&wv, hit, key);
    g.win_painted_state = st;
    g.win_painted_hit   = hit;
    g.win_painted_key   = key;
    g.win_painted_seq   = seq;
}

/* The page area, black. Loaded over when there is a frame file, so "no frame" reads
 * as a black page and not as whatever the dumb buffer was created with -- a fresh
 * dumb buffer is not reliably zeroed, and a plane of uninitialised memory on the
 * glass is the one failure this cannot leave to chance. */
static void menu_window_page_clear(void)
{
    int bpp = g.win.bpp / 8, y;

    if (g.win.pix == NULL || bpp <= 0)
        return;
    for (y = MW_CONTENT_Y; y < g.win.h; y++)
        memset((unsigned char *)g.win.pix + (size_t)y * g.win.pitch * bpp, 0,
               (size_t)g.win.w * bpp);
}

/* THE LIVE HALF -- the page, the page's status and the operator's taps, all through
 * browser_link.c, which is the only module that touches a file. Runs under g_lock on
 * the paint thread, which is what makes it the request ring's one consumer.
 *
 * THE ORDER IS LOAD-THEN-PAINT, and it is load-bearing exactly once: when the
 * keyboard goes down the rows it was covering have to be filled from a frame before
 * the chrome is drawn over them, or the panel below the page keeps a keyboard's
 * pixels until the next page change. That is why a keyboard transition forces the
 * frame cache to be re-read (bl_reset) -- the page has not changed, but the part of
 * it this window shows has. */
static void menu_window_live(void)
{
    struct mw_req rq;
    char url[MW_URL_MAX];
    int focus = 0, kb, x, y, w, h, rows;
    unsigned char *page;

    if (!g.win_ok || g.win.pix == NULL)
        return;
    if (!menu_window_rect(&x, &y, &w, &h) || h <= MW_CONTENT_Y)
        return;                     /* minimized: a title bar has no page */
    if (g.win.bpp != 16 && g.win.bpp != 32)
        return;

    /* What the page is doing. The URL is what the field shows when the operator is
     * not editing it, and the focus generation is what raises the keyboard for the
     * page's own text inputs -- the operator's "every time there's an input item". */
    /* THE TWO ARE MUTUALLY EXCLUSIVE, and that is not tidiness. A dead driver leaves
     * a status file that is STALE BUT READABLE, so both of these fire; and because
     * the two strings differ, menu_window_set_url()'s change gate sees a change on
     * every tick, bumps the field's sequence, and the whole chrome is repainted at
     * tick rate into a single-buffered plane. That is the flashing title bar. */
    if (!bl_online()) {
        menu_window_set_url("no browser on the host -- start rbrowser.py");
    } else if (bl_status_read(url, (int)sizeof url, &focus)) {
        menu_window_set_url(url);
        menu_window_page_input(focus);
    }

    kb = menu_keyboard_is_up();
    if (kb != g.win_painted_kb) {
        /* The keyboard covers the bottom of the page and the page does not know it:
         * re-read the whole frame, because nothing in the file changed and the part
         * of it that is wanted has. */
        bl_reset();
        g.win_painted_kb = kb;
    }
    rows = kb ? (MW_KB_Y0 - MW_CONTENT_Y) : MW_CONTENT_H;
    page = (unsigned char *)g.win.pix +
           (size_t)MW_CONTENT_Y * g.win.pitch * (g.win.bpp / 8);
    (void)bl_frame_load(page, g.win.pitch, g.win.w, rows, g.win.bpp);

    menu_window_repaint();

    /* THE OPERATOR'S TAPS, on their way out. One command per line, and the line IS
     * the protocol -- browser_link.h says why it is text. */
    while (menu_window_take_req(&rq)) {
        char line[MW_REQ_TEXT_MAX + 32];

        switch (rq.kind) {
        case MW_REQ_NAV:
            snprintf(line, sizeof line, "nav %s", rq.text);
            break;
        case MW_REQ_TEXT:
            snprintf(line, sizeof line, "text %s", rq.text);
            break;
        case MW_REQ_KEY:
            snprintf(line, sizeof line, "key %s", rq.text);
            break;
        case MW_REQ_POINT:
            snprintf(line, sizeof line, "click %d %d", rq.x, rq.y);
            break;
        case MW_REQ_HIST:
            snprintf(line, sizeof line, "hist %d", rq.x);
            break;
        case MW_REQ_SCROLL:
            /* A delta, so both numbers are signed -- and the driver does not negate
             * them: a finger dragging DOWN moves the page's content down, which is
             * what a wheel reports as a negative delta, and the browser's own
             * convention is applied there rather than guessed at here. */
            snprintf(line, sizeof line, "scroll %d %d", rq.x, rq.y);
            break;
        default:
            continue;
        }
        if (!bl_cmd(line))
            pointsrc_log("window: could not write '%s' -- is the browser running?",
                         line);
    }
}

/* The window's plane, up at the size the window's CURRENT state asks for.
 *
 * Returns 0 when the plane is up and painted -- whether it was just set up or was
 * already right -- and -1 when this machine will not give one up. Minimizing
 * changes the requested height, so it is a teardown-and-setup here rather than a
 * resize: there is no resize in this API, and a 32-row bar does not want a 600-row
 * buffer scanned out behind it. */
static int menu_window_plane_up(void)
{
    int x, y, w, h;

    if (!menu_window_rect(&x, &y, &w, &h))
        return -1;
    if (g.win_ok && g.win_h == h)
        return 0;
    if (g.win_ok) {
        drm_band_teardown(&g.win);
        g.win_ok = g.win_on = 0;
    }
    if (drm_band_setup(&g.win, w, h, g.view.bpp) != 0) {
        pointsrc_log("window: no plane for a %dx%d window -- it cannot be drawn on"
                     " this unit", w, h);
        return -1;
    }
    g.win_ok = 1;
    g.win_h  = h;
    /* The load-bearing line, as it was for the Step-1 gate: the ids say whether the
     * kernel took a window this size. */
    pointsrc_log("window: plane %u fb %u %dx%d %d bpp pitch %d px (%zu bytes) at %p",
                 g.win.plane, g.win.fb, g.win.w, g.win.h, g.win.bpp, g.win.pitch,
                 g.win.map_len, g.win.pix);

    menu_window_page_clear();
    /* The page in the fresh dump buffer is not the page in the browser, and the
     * frame cache cannot know that -- a NEW buffer has to be filled from the frame
     * even though the frame's sequence has not changed. */
    bl_reset();
    /* The chrome is drawn BEFORE the show, so what goes on the glass the first time
     * is the finished window and not the page with no border round it. -1 is not a
     * state, so this repaints whatever the last window left in these two. */
    g.win_painted_state = -1;
    g.win_painted_hit   = MW_HIT_NONE;
    g.win_painted_key   = -1;
    g.win_painted_seq   = 0;
    menu_window_repaint();

    if (drm_band_show(&g.win, x, y) == 0) {
        g.win_on = 1;
        pointsrc_log("window: on the glass at (%d,%d) %dx%d", x, y, w, h);
    } else {
        pointsrc_log("window: SETPLANE failed: %s -- the plane exists but will not"
                     " go up", strerror(errno));
    }
    return 0;
}

/* Bring the plane into line with the window's state. Called under g_lock from the
 * tick, so the PAINT thread owns the handover and pointsrc only ever writes the
 * state -- one thread mutates the plane, and it is the one that already owns it. */
static void menu_window_plane_sync(void)
{
    if (menu_window_state() == MW_CLOSED) {
        g.win_fail = 0;
        if (!g.win_ok)
            return;
        drm_band_teardown(&g.win);
        g.win_ok = g.win_on = g.win_h = 0;
        g.win_painted_state = MW_CLOSED;
        g.win_painted_hit   = MW_HIT_NONE;
        g.win_painted_key   = -1;
        g.win_painted_kb    = 0;
        /* The frame cache goes with it: the next open gets a fresh plane full of a
         * fresh frame, and a cached sequence would tell it there was nothing new. */
        bl_reset();
        /* Back to the page FIRST, and this is not tidiness: the band's buffer is
         * gone while the window holds the slot, so a view still pointing at it
         * would be a dangling mapping the next paint would write through. */
        g.view.pix   = g.fb_pix;
        g.view.pitch = g.fb_pitch;
        g.view.bx    = g.fb_bx;
        g.view.by    = g.fb_by;
        pointsrc_log("window: closed -- the plane goes back to the band");
        /* The band's buffer is a FRESH dumb buffer, so the image in `front` is not
         * in it: the tick's own witness sees that on the next pass and re-blits,
         * which is why nothing is blitted here. */
        if (menu_band_setup_from_page() != 0)
            pointsrc_log("menu: the plane did not come back to the band -- it stays"
                         " on the page");
        return;
    }

    /* THE HANDOVER, IN THIS ORDER. The window's plane and the band's are the same
     * slot on this hardware, so the band has to be GONE before the window asks --
     * otherwise the window's SET_MASTER is refused EBUSY, which is exactly how the
     * one-master-per-device rule shows itself (the block above). */
    if (g.band_ok) {
        drm_band_teardown(&g.band);
        g.band_ok = g.band_on = 0;
        g.view.pix   = g.fb_pix;
        g.view.pitch = g.fb_pitch;
        g.view.bx    = g.fb_bx;
        g.view.by    = g.fb_by;
        pointsrc_log("window: open -- the plane is handed over from the band");
    }
    /* Once, not per tick. A window that cannot get a plane cannot get one on the
     * next tick either, and an unguarded retry here would be a setup/teardown pair
     * at the tick period -- on the same thread that rbp's framebuffer presents from.
     * The flag clears when the window closes, so a later open tries again. */
    if (g.win_fail)
        return;
    if (menu_window_plane_up() != 0) {
        g.win_fail = 1;
        return;
    }
    menu_window_repaint();
    /* Re-shown, not shown once, for drm_band_show()'s own reason: it is idempotent
     * at the same place, so this costs one integer compare in the steady state and
     * a plane something later knocked down comes back by itself. */
    if (!g.win_on && drm_band_show(&g.win, MW_X, MW_Y) == 0)
        g.win_on = 1;
}

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;


/* A monotonic microsecond clock, for the two passes worth timing: the classify
 * (menu_build) and the blit (menu_frame_tick). Both are recorded in a comment as
 * well, because a log line only exists when MENU_VERBOSE=1 is set; the point of
 * keeping the clock here is that a pass which ever stops being bounded has to be
 * visible in a debugger and not only in a log.
 *
 * `long` is four bytes on the target, so this product wraps every 71.6 min of
 * uptime. That was audited on 2026-09-29, when pointsrc.c's nanosecond clock was
 * found to be wrapping (see shim_now_ms() there): it is safe HERE only because
 * every use of it is a difference of two readings taken microseconds apart, and
 * such a difference cannot straddle the wrap in practice. Nothing may ever test
 * one of these values for sign or magnitude, as pointsrc.c's old `down_ns > 0`
 * did -- that is the whole reason it was wrong. */
static long menu_now_us(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long)t.tv_sec * 1000000L + t.tv_nsec / 1000L;
}

/* The panel's height in fb px, and the stride of one image row in bytes. The same
 * two numbers menu_paint.c scales its layout by, computed the same way
 * (menu_panel_rect()'s rule), because the cache has to be exactly the panel. */
static int menu_panel_ph(void)
{
    int ph = (MZ_PANEL_H * g.view.dh) / MZ_LOGICAL_H;

    return ph < 1 ? 1 : ph;
}

/* The cache is the panel's rectangle and no more: a second view of the same picture
 * -- same dw/dh, so the panel comes out the same height -- whose "framebuffer" is the
 * cache, whose stride is the picture's width, and whose origin is its own top-left
 * corner. menu_paint() then writes rows 0..ph-1 at columns 0..dw-1 of it, which is
 * exactly what the blit below expects. */
static struct menu_view menu_cache_view(void *buf)
{
    struct menu_view cv = g.view;

    cv.pix = buf;
    cv.pitch = cv.dw;
    cv.fb_w = cv.dw;
    cv.fb_h = cv.dh;
    cv.bx = 0;
    cv.by = 0;
    return cv;
}

/* Build the image for `pressed` into the back buffer. NO LOCK, NO PUBLICATION, and
 * therefore safe to be slow -- which is the entire reason it is separate from
 * menu_publish(). Returns 0 when there is an image to publish, -1 when there is not
 * (nothing to build into, or the published image is already this one). */
static int menu_build(int pressed)
{
    struct menu_view cv;
    size_t n;
    long t0;
    int full;

    if (!g.front) {
        n = (size_t)g.view.dw * (size_t)(g.view.bpp / 8) * (size_t)menu_panel_ph();
        g.front = malloc(n);
        g.back = malloc(n);
        if (!g.front || !g.back) {
            /* A unit with no room for 572 KB still gets a panel; it just pays the
             * per-pixel classifier on every frame, on whichever thread arrives.
             * Logged once, on the first attempt. */
            free(g.front);
            free(g.back);
            g.front = g.back = NULL;
            if (g.paints < 1)
                pointsrc_log("menu: no %zu-byte image cache (%s); painting straight"
                             " onto the page every frame", n * 2, strerror(errno));
            return -1;
        }
        g.img_bytes = n;
        g.front_valid = 0;       /* a new mapping means a new image */
    }

    full = !g.front_valid;
    if (!full && pressed == g.front_pressed)
        return -1;               /* the published image is already this one */

    cv = menu_cache_view(g.back);
    t0 = menu_now_us();

    if (full) {
        menu_paint(&cv, pressed);
        /* THE CLASSIFY/BLIT DISCRIMINATOR. A whole-panel classify is the ~7.4 ms
         * pass the flicker was made of; a column rebuild is ~2 ms, and the two are
         * the only paths here. Which one a transition took is not visible from the
         * image -- both end with the correct panel -- so it is logged, under the
         * same bench knob as the blit, and the drill reads it.
         *
         * One line per transition and not per tick: a build with nothing to do
         * returns above without reaching here. */
        if (g.verbose)
            pointsrc_log("menu: classify %ld us: whole panel for %d",
                         menu_now_us() - t0, pressed);
    } else {
        /* ONLY THE COLUMNS THAT CHANGED. The old button loses its highlight and the
         * new one gains it, and those two columns are the whole of the difference
         * (menu_paint.h's menu_paint_cols()). The rest of the image is carried over
         * by the copy -- warm memory, 286 KB and 164 us at the 112-row band this was
         * measured against, 143 KB now -- against the 7.4 ms a whole-panel
         * reclassification costs. work/fbwrite.c has both numbers. */
        unsigned int mask = 0;

        memcpy(g.back, g.front, g.img_bytes);
        if (g.front_pressed > 0)
            mask |= 1u << (g.front_pressed - 1);
        if (pressed > 0)
            mask |= 1u << (pressed - 1);
        menu_paint_cols(&cv, pressed, mask);
        /* The interval covers the copy as well as the paint: they are one pass over
         * the image and the number worth having is what the transition cost, not
         * what the classifier's inner loop cost inside it. */
        if (g.verbose)
            pointsrc_log("menu: classify %ld us: columns %#x, now %d",
                         menu_now_us() - t0, mask, pressed);
    }
    return 0;
}

/* Publish what menu_build() built. The caller holds g_lock, or is the only thread
 * that touches the front -- the two call sites are menu_fb_open_unlocked(), under the
 * lock it was already holding, and menu_classify_publish() below. */
static void menu_publish(int pressed)
{
    void *t = g.front;

    g.front = g.back;
    g.back = t;
    g.front_pressed = pressed;
    g.front_valid = 1;
}

/* Build and publish, off the lock and then briefly on it: what the witness thread
 * does once per tick. */
static void menu_classify_publish(int pressed)
{
    if (menu_build(pressed) != 0)
        return;
    pthread_mutex_lock(&g_lock);
    menu_publish(pressed);
    pthread_mutex_unlock(&g_lock);
}

/* Open, measure and map. Called once, and again on every retry while it fails.
 *
 * `loud` gates the diagnostics: the first attempt explains itself and the caller
 * then passes it true once a minute, so a unit with no usable framebuffer cannot
 * write a line every 5 s for the life of the session. The success path below logs
 * unconditionally, so a framebuffer that appears later is still visible in the
 * log -- silence here never means success, and the return value is what the
 * caller retries on. */
static int menu_fb_open_unlocked(int loud)
{
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    const char *dev = env_str("FB_DEV", MENU_FB_DEFAULT);
    long len;
    int fd, stretch;

    fd = real_open(dev, O_RDWR, 0);
    if (fd < 0) {
        if (loud)
            pointsrc_log("menu: open %s: %s (no panel will be drawn yet)",
                         dev, strerror(errno));
        return -1;
    }
    memset(&var, 0, sizeof var);
    memset(&fix, 0, sizeof fix);
    if (real_ioctl(fd, FBIOGET_VSCREENINFO, &var) != 0 ||
        real_ioctl(fd, FBIOGET_FSCREENINFO, &fix) != 0) {
        if (loud)
            pointsrc_log("menu: %s is not a framebuffer (%s)", dev, strerror(errno));
        real_close(fd);
        return -1;
    }

    /* The same two depths menu_paint.c will draw in, refused rather than guessed
     * at: a wrong pitch or format paints stripes over the UI. */
    if (var.bits_per_pixel != 16 && var.bits_per_pixel != 32) {
        if (loud)
            pointsrc_log("menu: %d bpp is not a format this can draw in",
                         var.bits_per_pixel);
        real_close(fd);
        return -1;
    }
    if (var.xres == 0 || var.yres == 0 || fix.line_length == 0) {
        if (loud)
            pointsrc_log("menu: fb geometry is %ux%u pitch %u",
                         var.xres, var.yres, fix.line_length);
        real_close(fd);
        return -1;
    }

    g.view.fb_w = (int)var.xres;
    g.view.fb_h = (int)var.yres;
    g.view.bpp = (int)var.bits_per_pixel;
    g.view.pitch = (int)(fix.line_length / (var.bits_per_pixel / 8));

    len = fix.smem_len > 0 ? (long)fix.smem_len
                           : (long)g.view.pitch * g.view.fb_h * (g.view.bpp / 8);
    g.view.pix = real_mmap(NULL, (size_t)len, PROT_READ | PROT_WRITE, MAP_SHARED,
                           fd, 0);
    /* Not needed after the mapping: a shared mapping outlives the fd, and holding
     * it would leave the panel owning an fd forever. */
    real_close(fd);
    if (g.view.pix == (void *)-1) {
        g.view.pix = NULL;
        if (loud)
            pointsrc_log("menu: mmap %s (%ld bytes): %s", dev, len, strerror(errno));
        return -1;
    }

    /* The rectangle the present path blits the UI into -- point_fit()'s rule, the
     * same one fb_cursor.c uses for the arrow, and the identity on a framebuffer
     * that is already 1280x800. DFB_PRESENT_FIT is the one policy in it: under
     * "stretch" the picture is the whole page. */
    stretch = strcmp(env_str("DFB_PRESENT_FIT", "fit"), "stretch") == 0;
    point_fit(POINT_LOGICAL_W, POINT_LOGICAL_H, g.view.fb_w, g.view.fb_h, stretch,
              &g.view.dw, &g.view.dh, &g.view.bx, &g.view.by);

    if (!menu_view_ok(&g.view)) {
        if (loud)
            pointsrc_log("menu: %dx%d %d bpp leaves no room for the panel",
                         g.view.fb_w, g.view.fb_h, g.view.bpp);
        g.view.pix = NULL;
        return -1;
    }

    /* THE PLANE, IF THERE IS ONE. Attempted once, here, so that a machine without
     * it never pays for it again and a machine with it has it before the first
     * tick. Kept in the fb's geometry first, because the fallback below needs to
     * know where the page and its picture rect are. */
    g.fb_pix   = g.view.pix;
    g.fb_pitch = g.view.pitch;
    g.fb_bx    = g.view.bx;
    g.fb_by    = g.view.by;
    g.band_bx  = g.view.bx;
    g.band_by  = g.view.by;
    g.band_ok  = 0;
    g.band_on  = 0;
    g.band_off = !env_flag("MENU_PLANE", 1);
    /* The window is opened, not shown: the plane it needs is set up by the tick, on
     * this thread's own terms, so that the one writer of the plane is the paint
     * thread and never the touch thread (menu_window_plane_sync()). MENU_WINDOW is the
     * only opener left, now that the menu's globe cell is gone -- see the block above. */
    if (env_flag("MENU_WINDOW", 0)) {
        menu_window_open();
        pointsrc_log("window: MENU_WINDOW=1 -- open at startup (the menu has no"
                     " cell for this any more; this is the only way in)");
    }
    if (g.band_off) {
        pointsrc_log("menu: MENU_PLANE=0 -- the band is repaired on the page, as it"
                     " was before the plane existed");
    } else {
        /* A failure here is not fatal and is already logged by drm_band_setup(): the
         * view still points at the page, which is the route this shipped with and
         * the one menu_band_present() completes if it ever gets that far. */
        (void)menu_band_setup_from_page();
    }

    pointsrc_log("menu: %dx%d %d bpp pitch %d at %p; picture %dx%d at %d,%d%s;"
                 " drawing the panel",
                 g.view.fb_w, g.view.fb_h, g.view.bpp, g.view.pitch, g.view.pix,
                 g.view.dw, g.view.dh, g.view.bx, g.view.by,
                 stretch ? " (stretched)" : "");

    /* THE FIRST IMAGE IS BUILT HERE, with the lock still held and before `ok`
     * publishes the mapping. Two things come out of that. The panel's first
     * appearance costs a blit instead of a 7.4 ms classification -- the image that
     * would otherwise be built on the tick the swipe arrives, which is the one
     * moment the operator is looking at it. And a classification can never run on
     * rbp's render thread even in the window between the map and the first tick: a
     * tick that can see the framebuffer can already see a finished image.
     *
     * Nothing is published on failure, and then the tick paints straight onto the
     * page -- the degraded path menu_frame_tick() documents. */
    if (menu_build(0) == 0)
        menu_publish(0);

    g.ok = 1;
    return 0;
}

/* The mapping is committed under the lock, so that a tick arriving from the vsync
 * hook sees either no framebuffer at all or a complete one -- never g.ok set with
 * the view still half-written. The open/ioctl/mmap themselves are slow, and hold
 * the lock while they run; the tick's trylock turns that into skipped ticks, which
 * is the right answer while there is nothing to draw into yet. */
static int menu_fb_open(int loud)
{
    int rc;

    pthread_mutex_lock(&g_lock);
    rc = menu_fb_open_unlocked(loud);
    pthread_mutex_unlock(&g_lock);
    return rc;
}

/* Is the panel's image still on the page?
 *
 * The points come from menu_paint.c's menu_witness_point(): fractions of the panel
 * rect in sixteenths, so they fall in the same classes on a panel that is not
 * 1280x800 -- the four corners and two mid-border points are the frame, and the rest
 * are spread over the button band, on the column centres where a label is drawn and
 * between them where the seam is, at 5/16 and 11/16 of the height. Both of those are
 * chosen to miss the labels' ink, which is what lets the comparison below be against
 * an exact palette value: menu_class_at() at a glyph-free point is the whole truth
 * about that pixel. test_menu.c asserts that property against this same list.
 *
 * The mix matters. A witness that sampled only the fill could be fooled for the
 * life of the session by rbp happening to draw its own dark grey there -- the
 * failure would be silent and permanent, because "intact" means "do not paint".
 * Sampling the frame as well means a false "intact" needs rbp's static UI to hold
 * our exact light border colour at six separate points, one of them a corner of the
 * screen. A false "damaged" costs one full pass, which is what the tick after an
 * rbp frame does anyway. */
static int menu_intact(const struct menu_view *v, int pressed)
{
    struct menu_layout L;
    unsigned int pal[8];
    int i, n = menu_witness_count();

    /* ONE LAYOUT FOR ALL FIFTEEN POINTS, and that is the other half of this check's
     * cost. Each point used to go through menu_class_at(), i.e. a whole rebuild of
     * the panel's geometry -- MZ_COLS columns of scaled arithmetic and a walk of every
     * label's advances -- fifteen times per tick, at two thousand ticks a second.
     * The layout is a function of the view alone and the view does not change, so
     * building it once is not an optimisation of the arithmetic; it is not doing the
     * arithmetic. */
    menu_layout_make(v, &L);
    /* The palette, likewise: a pure function of (bpp, class), resolved once so a
     * switch stays out of the loop. */
    for (i = 0; i < 8; i++)
        pal[i] = menu_pixel(v->bpp, i);

    for (i = 0; i < n; i++) {
        int fx, fy;

        menu_witness_point(v, i, &fx, &fy);
        if (menu_get(v, fx, fy) != pal[menu_class_in(&L, fx, fy, pressed)])
            return 0;
    }
    return 1;
}

/* Put the published image on the page. THE ONLY WRITE A TICK EVER MAKES, and it is
 * bounded by construction: one memcpy of g.img_bytes, split into a row each only
 * when the page's stride is not the picture's width.
 *
 * A row is a contiguous run of pixels in the image and in the page alike, so the
 * whole panel is one memcpy whenever `pitch == dw && bx == 0` -- this port's case,
 * where the present picture is the identity on a 1280x800 page. The strided path is
 * not dead code kept for luck: a framebuffer whose stride is padded, or a picture
 * letterboxed inside one, takes it, and it is what fb_cursor.c composites with.
 *
 * What is read here is `front`, and `front` is only ever the PUBLISHED image -- the
 * classifier builds into `back` and swaps the two pointers under g_lock. A blit can
 * therefore never read a half-built image, whatever the witness thread is doing; that
 * is the whole of what the double buffer buys, and it is why this function can be
 * called from rbp's render thread without a classify ever running there. */
static void menu_blit(void)
{
    int bpp = g.view.bpp / 8;
    const unsigned char *src = g.front;

    if (g.view.pitch == g.view.dw && g.view.bx == 0) {
        memcpy((unsigned char *)g.view.pix
                   + (size_t)g.view.by * (size_t)g.view.pitch * (size_t)bpp,
               src, g.img_bytes);
        return;
    }
    {
        int y, ph = menu_panel_ph();

        for (y = 0; y < ph; y++)
            memcpy((unsigned char *)g.view.pix
                       + ((size_t)(g.view.by + y) * (size_t)g.view.pitch
                          + (size_t)g.view.bx) * (size_t)bpp,
                   src + (size_t)y * (size_t)g.view.dw * (size_t)bpp,
                   (size_t)g.view.dw * (size_t)bpp);
    }
}

/* Put the finished band on the glass, when there is a plane for it.
 *
 * ON THE TRANSITION AND NOT IN THE LOOP: once `band_on` is set this returns
 * without an ioctl, so a session's ten thousand ticks a second cost nothing for
 * it. That is the whole of what the plane buys -- a band that is never erased has
 * nothing to repair, so the ticks that used to be repaints are now two integer
 * reads and fifteen witness points over a buffer nobody else writes.
 *
 * If the plane will not take it, the image has just been drawn into a buffer that
 * nothing is scanning out, so the page has to be drawn a second time -- once, here,
 * on the way to the route this shipped with. */
static void menu_band_present(void)
{
    if (!g.band_ok || g.band_on)
        return;

    if (drm_band_show(&g.band, g.band_bx, g.band_by) == 0) {
        g.band_on = 1;
        return;
    }

    g.band_ok    = 0;
    g.view.pix   = g.fb_pix;
    g.view.pitch = g.fb_pitch;
    g.view.bx    = g.fb_bx;
    g.view.by    = g.fb_by;
    drm_band_teardown(&g.band);
    /* The window's plane is the same slot, so it goes with the band's -- a plane
     * that will not carry the band will not carry a 1120x600 window either. */
    if (g.win_ok) {
        drm_band_teardown(&g.win);
        g.win_ok = g.win_on = g.win_h = 0;
    }
    if (g.front_valid)
        menu_blit();
    else
        menu_paint(&g.view, g.painted_pressed);
    pointsrc_log("menu: the plane would not take the band -- back on the page");
}

void menu_frame_tick(void)
{
    int open, fp;

    if (!g.ok)
        return;
    if (pthread_mutex_trylock(&g_lock) != 0)
        return;

    /* The window first, and it DECIDES THE ROUTE: while it is open it owns the
     * process's one plane, so the band's whole path below -- present, witness,
     * blit -- is not reached at all. What is reached is the page fallback, which is
     * what lets a swipe-down still draw a (flickering) panel outside the window's
     * rectangle. See the block above menu_band_setup_from_page(). */
    menu_window_plane_sync();
    if (menu_window_is_open()) {
        /* While it is open the window owns the process's one plane, so the band's
         * whole path below -- present, witness, blit -- is not reached at all. What
         * is reached is the LIVE half: the page from the browser, the status that
         * raises the keyboard for the page's own inputs, and the operator's taps on
         * their way out. It runs under the lock because this is the request ring's
         * one consumer. */
        menu_window_live();
        pthread_mutex_unlock(&g_lock);
        return;
    }

    open = menu_is_open();

    if (!open) {
        /* The whole cost of a closed menu: one int read, one more to notice it
         * just closed. Nothing is drawn -- rbp's next frame restores the band,
         * and a "restore" here would be a write to a region we do not own.
         *
         * The plane is the exception, and it is not optional: RGB565 has no alpha,
         * so a plane left up is an opaque strip over rbp's UI for the life of the
         * process. Switching it off is one ioctl, on this transition. */
        if (g.band_on) {
            drm_band_hide(&g.band);
            g.band_on = 0;
        }
        if (g.painted_open) {
            g.painted_open = 0;
            pointsrc_log("menu: panel closed");
        }
    } else if (!g.front_valid) {
        /* No image cache: there is nowhere to build one, so the panel is painted
         * straight onto the page -- the behaviour this had before the cache existed,
         * and only on a unit with no room for 572 KB. This is the one path left that
         * can classify on rbp's render thread, which is why it is worth its own
         * branch rather than being folded in below. */
        int pressed = menu_pressed();

        menu_paint(&g.view, pressed);
        g.paints++;
        g.painted_open = 1;
        g.painted_pressed = pressed;
        menu_band_present();
    } else {
        /* Paint when the image the front holds has changed (the panel just opened,
         * or a different button is under the finger) and otherwise only when the
         * witness says rbp has painted over it. Note which image is judged intact:
         * the PUBLISHED one, not the one the finger implies. A classify that is still
         * running makes the highlight a tick stale, and blitting the front against
         * the front's own image is what keeps that from reading as damage and
         * turning into a repaint storm. */
        fp = g.front_pressed;
        if (!g.painted_open || fp != g.painted_pressed ||
            !menu_intact(&g.view, fp)) {
            long t0 = menu_now_us();
            long us;

            menu_blit();
            us = menu_now_us() - t0;

            /* The picture rect, which is where the band is on the SCREEN in either
             * route -- with a plane the view's own bx/by are the plane buffer's
             * origin and say nothing about the glass. The route is named here
             * because this is the one line that tells a log reader which of the two
             * the session actually got. */
            if (!g.painted_open)
                pointsrc_log("menu: panel open, painted at %d,%d on the %s",
                             g.fb_bx, g.fb_by, g.band_ok ? "plane" : "page");
            /* The cost of a pass, measured here on 2026-09-29 and recorded rather
             * than printed: with the panel open and nothing touching it, `menu: paint
             * N took M us` over 2048 passes read **236-432 us, mean ~310** for the
             * steady state, which is the band's copy onto the page -- rbp has just
             * dirtied those lines, so the destination is cold, which is why this is
             * slower than fbwrite.c's warm-cache 164 us. Those figures are the
             * 112-row band (286 KB); the panel is 56 rows now and the copy with it,
             * so they want re-taking rather than halving on paper. `us` is kept
             * because this
             * runs on rbp's render thread when it arrives from the vsync hook -- a
             * pass that ever stops being bounded has to be visible in a debugger, not
             * just in a log -- and MENU_VERBOSE=1 is what prints it. */
            if (g.verbose)
                pointsrc_log("menu: blit %ld us (pressed=%d, %d total)",
                             us, fp, g.paints + 1);
            (void)us;
            g.paints++;
            g.painted_open = 1;
            g.painted_pressed = fp;
        }
        /* After the paint, never before it: what goes on the glass the first time
         * must be the finished band and not the empty buffer the plane was
         * created with. */
        menu_band_present();
    }

    pthread_mutex_unlock(&g_lock);
}

static void *menu_thread(void *arg)
{
    long period_us;
    int attempts;
    (void)arg;

    /* Seconds are too coarse and integers too blunt for this one: the useful range
     * is 0.05-2 ms, and env_int() would round every value below a millisecond to
     * zero. env_double() shares env_int()'s contract -- unset, empty or
     * unparseable all fall back to the default rather than to 0 -- so a typo in
     * rb.local.conf cannot turn this into a busy loop. */
    period_us = (long)(env_double("POINT_MENU_MS", MENU_MS_DEFAULT) * 1000.0);
    if (period_us < MENU_US_MIN)
        period_us = MENU_US_MIN;

    /* Retry rather than exit, which is fb_cursor.c's rule and for the same reason:
     * a monitor that appears long after boot, or a framebuffer the kernel tears
     * down and rebuilds on a hotplug, must not cost the panel for the life of the
     * process. There is no second chance -- menu_draw_start() has already set
     * `started`, so nothing will call this again. */
    attempts = 0;
    while (menu_fb_open(attempts == 0 || attempts % 60 == 0) != 0) {
        attempts++;
        usleep(MENU_RETRY_US);
    }

    for (;;) {
        /* The classify first, then the tick, and both on this thread. In this order
         * the image this thread just built is the image it then blits, so a press
         * transition reaches the glass within one period. The vsync hook calls
         * menu_frame_tick() alone and never classifies -- that is the whole point of
         * the split (struct menu_map). */
        menu_classify_publish(menu_is_open() ? menu_pressed() : 0);
        menu_frame_tick();
        usleep((useconds_t)period_us);
    }
    return NULL;
}

int menu_draw_start(void)
{
    static int started;
    pthread_t tid;

    if (started)
        return 0;
    started = 1;

    /* Read once, here, because menu_frame_tick() runs from two threads and a
     * getenv() in the hot path is a lock and a write to the process's environment
     * block from rbp's render thread. A bench knob: it prints the measured cost of
     * every pass -- one line per rbp frame, ~57/s -- and is deliberately not in
     * start-rb.sh's SHIM_VARS, like POINT_MENU_MS and POINT_MENU_KEY_EXTRA. */
    g.verbose = env_flag("MENU_VERBOSE", 0);

    /* The same gate the input half reads (pointsrc.c's POINT_MENU): with the menu
     * off the panel can never open, so there is nothing to composite and no
     * reason for a thread. Note this is what makes RB_POINT_MENU=0 the baseline
     * for the A/B -- with it off, not one pixel of the panel is ever written. */
    if (!env_flag("POINT_MENU", 1)) {
        pointsrc_log("menu: POINT_MENU=0 -- no panel is drawn and no touch is"
                     " swallowed\n");
        return 0;
    }
    if (pthread_create(&tid, NULL, menu_thread, NULL) != 0) {
        started = 0;
        pointsrc_log("menu: cannot start the paint thread: %s\n", strerror(errno));
        return -1;
    }
    pthread_detach(tid);
    return 0;
}
