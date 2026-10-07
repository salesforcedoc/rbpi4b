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
 * THE DRAWERS TAKE THE SAME TWO ROUTES, and on the plane they get the band's image
 * treatment. A drawer is a `drm_band` of its own, so plane-vs-page is decided per
 * drawer exactly as above; what differs is where the pixels are built. The band is
 * built from three small glyph rows and published by swapping g.front/g.back under
 * g_lock; a drawer is 180x800 and its whole picture is rewritten on every fader
 * move, so the same swap is done for it -- `side_img[which][front,back]`, filled off
 * the lock by menu_side_build() (from menu_thread, just before the tick) and copied
 * into the live plane buffer by menu_side_publish() under it. Before that split the
 * paint ran UNDER g_lock onto the live, single-buffered plane, and the MENU_FILL bed
 * going down first made each repaint a whole-panel flash -- the flicker the operator
 * reported while riding a fader. On the page route the copy is per-tick and per-row,
 * for the reason everything else here is per-tick: rbp repaints over it and a region
 * this large has no damage witness.
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
#include "side_zone.h"         /* the edge drawers: which is out, and its geometry */
#include "side_paint.h"        /* ...and the image it is drawn with */
#include "prompt_zone.h"       /* the USB STOP chooser: its box, and whether it is up */
#include "prompt_paint.h"      /* ...and the image it is drawn with */
#include "fx_zone.h"           /* the Beat FX picker: its box, and whether it is up */
#include "fx_paint.h"          /* ...and the image it is drawn with */
#include "fxpad_zone.h"        /* the momentary X/Y pad: whether it is holding a finger */
#include "fxpad_paint.h"       /* ...and the HUD it draws over rbp's BPM cell */
#include "rbp_vu.h"            /* g_fader[]: the value the drawer's handle follows */
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

    /* THE EDGE DRAWERS' PLANES -- ONE PER EDGE, and that is the change the operator
     * asked for: *"allow for both side panels to be visable at the same time and to
     * accept input"*.
     *
     * They USED to be the band's slot a third time -- a single plane, handed over,
     * because DRM master is one per DEVICE and a second drm_band_setup() in a process
     * was refused EBUSY (the window's block above). That is no longer true: drmband.c
     * now holds the fd and master ONCE and REFCOUNTS them, so a second and third band
     * are a buffer and a plane each. Each drawer therefore has its own plane, and the
     * two can be out together.
     *
     * THE BAND'S PLANE IS STILL MUTUALLY EXCLUSIVE WITH THE DRAWERS', because the two
     * overlap on the glass at the top corners and two overlays cannot share a zpos.
     * `side_prev` is "was any drawer out at the last sync", which is what tells
     * "nothing to undo" from "the drawers just closed, give the band its plane back".
     *
     * `side_fail[which]` is the window's `win_fail` for the same reason and with the
     * same lifetime (cleared when that drawer closes, so a later open tries again); on
     * that route the drawer falls back to the page and keeps working, at the band's
     * fallback cost -- and per drawer, so one edge losing its plane does not cost the
     * other edge its own.
     *
     * `side_painted_*` are the drawer the PUBLISHED image is for (-1 for "no image
     * readable yet"), the pressed control and the fader value it holds. They are what
     * makes the BUILD happen on a CHANGE and not every tick -- the destinations are
     * single-buffered, so a gate fed anything that alternates by itself would tear or
     * blink (menu_window_repaint()'s rule, and the reason these are real values and not
     * a counter). They are written under the lock, by the builder that publishes and by
     * the close that invalidates.
     *
     * THE DRAWER'S IMAGE IS DOUBLE-BUFFERED, LIKE THE BAND'S. This is the same fix the
     * operator's band flicker got, and it exists for the same reason: side_paint()
     * rewrites every pixel of the 180x800 panel, and it used to do it into the LIVE,
     * single-buffered plane buffer from under g_lock -- so a vsync tick that could not
     * wait either showed a half-written panel or, worse, the flat colour the bed goes
     * down first. Measured: eight full-panel rewrites in one fader drag.
     *
     * `side_img[which][0]` is the PUBLISHED image (the only one a blitter reads) and
     * `[1]` the builder's scratch. The builder writes scratch off the lock and swaps
     * the two pointers under it; the tick then COPIES the front into whatever the
     * destination is. `side_pending[which]` says the destination has yet to receive the
     * current front -- which is what a fresh, uninitialised dumb buffer needs, and what
     * the page needs every tick.
     *
     * `side_buf_*` cache the image's geometry AT ALLOCATION. The builder runs off the
     * lock and must not read g.view or g.side[]: menu_frame_tick mutates both under it
     * (the plane teardown on a close, the page rebase on a fallback), so an off-lock read
     * there can hand side_paint() a dangling pointer. */
    struct drm_band side[2];
    int  side_ok[2];             /* this drawer's plane is set up and mapped */
    int  side_on[2];             /* ...and it is on the glass right now */
    int  side_fail[2];           /* this machine refused a drawer-sized plane */
    int  side_prev;              /* ANY drawer out at the last sync */
    int  side_painted_which[2];  /* the drawer the published image is for, or -1 */
    int  side_painted_hit[2];    /* ...and the control it is highlighted for */
    int  side_painted_v[2];      /* ...and the fader value it was drawn at */
    void  *side_img[2][2];       /* [drawer][front,back]; NULL when none could be had */
    size_t side_img_bytes;       /* one image, bytes */
    int    side_buf_w;           /* the image's geometry, cached at allocation ... */
    int    side_buf_h;
    int    side_buf_bpp;
    int    side_pending[2];      /* the destination still needs the front copied in */

    /* THE USB STOP CHOOSER'S PLANE -- a FOURTH drm_band, at the BOX'S OWN SIZE rather
     * than the panel's, which is what prompt_paint.h means by "the view's picture rect
     * IS the box": the plane is set up at PR_W x PR_H logical px scaled to the page,
     * and the box's own coordinates are the buffer's.
     *
     * It is its own slot and not a handover, for the reason drmband.c now refcounts the
     * fd and master: a fourth drm_band_setup() is a buffer and a plane (the two drawers
     * already coexist, measured at planes 127 and 138). It overlaps nothing either --
     * the box is raised from the band's own seventh column, so by the time it is up the
     * band is shut, and the funnel's first refusal (pointsrc.c) keeps a drawer or the
     * window from being opened under it.
     *
     * `prompt_fail` is the window's `win_fail` for the same reason and with the same
     * lifetime (cleared when the box closes, so a later open tries again); on that
     * route the box goes onto the page and keeps working, at the band's fallback cost.
     *
     * `prompt_painted_cell`, `prompt_painted_flash` and `prompt_painted_live` are the
     * state the PUBLISHED image holds -- the cell under the finger, the cell whose
     * hold is blinking on its lit half, and rbp's two device-liveness bits packed one
     * per bit. Three real values and not a counter: the plane is single-buffered, so a
     * gate fed anything that alternates by itself would repaint every tick and blink
     * (menu_window_repaint()'s rule). The liveness belongs in the gate because a device
     * appearing or going away is a change to the picture and nothing else would notice
     * it. AND THE FLASH BELONGS IN IT for the newest reason of the three: a running
     * hold lights its button for half of every PR_HOLD_FLASH_MS, and with the phase out
     * of the key the box would keep republishing the image it built when the finger
     * went down -- the button would light once and then sit there for three seconds
     * looking exactly like a tap that had been swallowed.
     *
     * SO DO THE TWO DEVICE NAMES, for the identical reason one step further out: a
     * button reads the name on the stick (prompt_zone.h), and a stick pulled while its
     * box is up removes the name file a moment before rbp stops reporting the media. The
     * window between those two is short and both events would repaint -- but the gate
     * has to see the thing that changed rather than rely on a second thing changing too
     * ([[a-dead-driver-flashes-the-chrome]] is the same trap). The key is the state's own
     * label array, so it is the string the button reads and not a second opinion about
     * it, and it is 72 bytes to compare.
     *
     * `prompt_valid` is "there is a real box in prompt_img[0]". It is what the show
     * waits on: a fresh dumb buffer holds nothing anyone can read, so the box must be
     * COPIED in before it goes on the glass, exactly as the drawer's must. */
    struct drm_band prompt;
    int  prompt_ok;              /* the box's plane is set up and mapped */
    int  prompt_on;              /* ...and it is on the glass right now */
    int  prompt_fail;            /* this machine refused a box-sized plane */
    int  prompt_valid;           /* a built box exists in prompt_img[0] */
    void  *prompt_img[2];        /* [front, back]; NULL when none could be had */
    size_t prompt_img_bytes;     /* one image, bytes */
    int    prompt_buf_w;         /* the image's geometry, cached at allocation ... */
    int    prompt_buf_h;
    int    prompt_buf_bpp;
    int    prompt_pending;       /* the destination still needs the front copied in */
    int    prompt_painted_cell;  /* the cell the published image is highlighted for */
    int    prompt_painted_flash; /* ...the cell it is lit on for the hold's blink... */
    int    prompt_painted_live;  /* ...and the liveness bits it was drawn with */
    char   prompt_painted_name[PROMPT_DEVICES][PR_LABEL_MAX + 1];
                                 /* ...and the two device names it was drawn with */

    /* THE BEAT FX PICKER'S PLANE -- a FIFTH drm_band, the prompt's arrangement at the
     * picker's own size, and for the same reason: fx_paint.h's contract is that the
     * view's picture rect IS the box, so the plane is FX_W x FX_H logical px scaled to
     * the page and the box's own coordinates are the buffer's.
     *
     * IT IS ITS OWN SLOT AND NOT THE PROMPT'S, reversed on evidence 2026-10-06 (the plan
     * of record records the reversal). The fear was that a fifth plane would be refused,
     * and the unit's own /sys/kernel/debug/dri/1/state lists plane[0] through plane[29]+
     * with five already held by this shim. Sharing the prompt's slot instead would make
     * the two boxes mutually exclusive by construction, and getting THAT wrong shows one
     * box's pixels in the other's buffer -- a far worse failure than a refused plane,
     * which costs nothing because the page route below carries it.
     *
     * The two cannot be raised at once in any case: the funnel's first refusal in
     * pointsrc.c keeps a second box from opening under either one.
     *
     * `fxlist_painted_row` is the one piece of state the published image holds -- the row
     * under the finger, 0 for none. It is a real value and not a counter for the reason
     * menu_window_repaint() gives: the plane is single-buffered, so a gate fed anything
     * that alternates by itself would repaint every tick and blink. There is nothing else
     * in it: fx_zone.h records why no row is marked as the current effect. */
    struct drm_band fxlist;
    int  fxlist_ok;              /* the picker's plane is set up and mapped */
    int  fxlist_on;              /* ...and it is on the glass right now */
    int  fxlist_fail;            /* this machine refused a picker-sized plane */
    int  fxlist_valid;           /* a built picker exists in fxlist_img[0] */
    void  *fxlist_img[2];        /* [front, back]; NULL when none could be had */
    size_t fxlist_img_bytes;     /* one image, bytes */
    int    fxlist_buf_w;         /* the image's geometry, cached at allocation ... */
    int    fxlist_buf_h;
    int    fxlist_buf_bpp;
    int    fxlist_pending;       /* the destination still needs the front copied in */
    int    fxlist_painted_row;   /* the row the published image is highlighted for */

    /* THE MOMENTARY PAD'S PLANE -- a SIXTH drm_band, the cell's own size, and the only
     * one in this file that is up for as long as a FINGER is down rather than for as long
     * as a box is open. That is the whole of its difference from the five above, and it
     * costs it the machinery they all share: there is no image pair and no publish gate
     * here, because the picture is a function of rbp's OWN pixels (which change under a
     * running deck) and of a finger (which changes between ticks) -- so "has it changed"
     * is always yes, and the tick copies and annotates every time it is up. menu_paint.h's
     * no-save-under argument applies twice over: the numbers underneath are the BPM.
     *
     * IT IS THE ONE PLANE THAT MUST NEVER OUTLIVE ITS GESTURE. A stale band is a strip of
     * our drawing over rbp's UI; a stale PAD would be an opaque rectangle sitting on the
     * operator's BPM readout with a frozen dot on it -- so the tick's very first act is to
     * take it off the glass whenever no finger is on the cell (menu_fxpad_plane_sync()),
     * before any route is decided. It is hidden and not torn down between gestures, which
     * is that function's own comment.
     *
     * `fxpad_fail` is the same once-only latch the picker's and the chooser's carry: a
     * machine that refuses this plane must not be asked again on every tick. */
    struct drm_band fxp;
    int  fxp_ok;                 /* the pad's plane is set up and mapped */
    int  fxp_on;                 /* ...and it is on the glass right now */
    int  fxp_fail;               /* this machine refused a cell-sized plane */

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

/* The one lock, defined with its comment further down. Declared here because the
 * drawers' builder -- which sits above that comment -- takes it to publish an image:
 * a tentative definition followed by the real one is one object. */
static pthread_mutex_t g_lock;

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
 * open FILE, so the band's `drm_band_setup()` is granted it and a second setup in
 * the same process -- which opened the card again -- was refused EBUSY (a separate
 * process is refused EACCES, 5/5, because rbp's DirectFB holds the display).
 *
 * THE DRAWERS ARE NO LONGER BOUND BY THAT, and this block is kept for the history
 * and for the window. drmband.c now holds the fd and master ONCE and refcounts
 * them, so a second and third band are a buffer and a plane each -- which is what
 * lets both edge drawers own a plane at the same time (the operator's "allow for
 * both side panels to be visable at the same time"). The WINDOW has not been moved
 * over to that: it is abandoned work (menu_window.h) and rewriting a dead path to
 * share a device would be churn, so it still hands the slot over with the band and
 * still cannot coexist with a drawer.
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
static long menu_now_us(void);        /* ...and this: the drawers time themselves */

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

/* ---------------- THE EDGE DRAWERS' PLANE --------------------------------
 *
 * Same slot, third holder, same handover as the window's above -- and the same
 * measurement is behind it, so none of that argument is repeated here. What is new
 * is only that a drawer is FULL HEIGHT, so the band is not merely covered while one
 * is out: there is no band at all until the drawer closes. Opening a drawer tears
 * the band's plane down; closing it tears the drawer's down and sets the band's up
 * again from the page.
 *
 * EACH DRAWER HAS ITS OWN PLANE, and the older note here that a second plane was
 * impossible is wrong and is kept only as a warning: DRM master is one per DEVICE, but
 * drmband.c now acquires the fd and the master ONCE and REFCOUNTS them, so a second and
 * third drm_band_setup() each get a buffer and a plane of their own -- which is what
 * lets both drawers be out at once (measured: plane 127 and plane 138). What is still
 * exclusive is the BAND's plane, because the band and a drawer overlap at the top
 * corners of the glass and two overlays cannot share a zpos.
 *
 * A DRAWER AND THE BAND CANNOT BOTH BE UP, and that reaches further than this file:
 * side_zone.c's closed-entry arm refuses while the band is open, menu_zone.c's
 * refuses while a drawer is open, and pointsrc.c's funnel refuses while the window
 * is. One surface, gated in all three places the question can be asked. */

/* The drawer's width in fb px -- the panel's SZ_W of logical px at the page's own
 * scale, the same rule menu_panel_ph() applies to the band's height. */
static int menu_side_pw(void)
{
    int pw = (SZ_W * g.view.dw) / MZ_LOGICAL_W;

    return pw < 1 ? 1 : pw;
}

/* The value the handle is drawn at: the SHARED fader position, so the drawer follows
 * the hardware fader when there is one and the hardware fader follows the drawer when
 * there is not -- one number, in fader_state.c, written by both (rbp_vu.h). 1023 is
 * both the seed and the legal maximum, so an untouched machine draws the handle at
 * the top, which is exactly what rbp's mixer is doing there. */
static int menu_side_value(int which)
{
    int v = g_fader[side_channel(which)];

    if (v < 0)
        v = 0;
    if (v > 1023)
        v = 1023;
    return v;
}

/* A view over one drawer, whichever route it is being drawn on.
 *
 * ON A PLANE the buffer IS the panel: origin 0,0, and dw/dh its own size, so
 * side_paint.c's pixels are buffer-local and the mirror is the only thing that has to
 * know which edge this is. Deliberately not `g.view`, which describes the page for
 * the band's own path throughout.
 *
 * ON THE PAGE there is no buffer of the drawer's own, so the view's origin is moved to
 * the panel's SCREEN rectangle instead -- the pointer is offset and the pitch stays
 * the page's. That is what makes the same side_paint() call work on both routes: the
 * writer only ever sees a base, a stride and a width. This is the MENU_PLANE=0 route
 * and the side_fail fallback, and it shimmers exactly as the band's does there. */
static void menu_side_view(struct menu_view *sv, int which)
{
    memset(sv, 0, sizeof *sv);
    if (g.side_ok[which]) {
        sv->pix   = g.side[which].pix;
        sv->pitch = g.side[which].pitch;
        sv->fb_w  = g.side[which].w;
        sv->fb_h  = g.side[which].h;
        sv->bpp   = g.side[which].bpp;
        sv->dw    = g.side[which].w;
        sv->dh    = g.side[which].h;
        return;
    }
    sv->dw  = menu_side_pw();
    sv->dh  = g.view.dh;
    sv->pix = (unsigned char *)g.fb_pix
              + ((size_t)g.fb_by * (size_t)g.fb_pitch
                 + (size_t)(g.fb_bx
                            + (which == SZ_LEFT ? 0 : g.view.dw - menu_side_pw())))
                * (size_t)(g.view.bpp / 8);
    sv->pitch = g.fb_pitch;
    sv->fb_w  = sv->dw;
    sv->fb_h  = sv->dh;
    sv->bpp   = g.view.bpp;
}

/* The drawer's image, and ONLY when it has changed.
 *
 * `which` is the drawer on the glass, its currently pressed control is the highlight
 * and its channel's shared fader value is the handle's position -- three real values,
 * and the difference of any one of them is the repaint. Nothing derived and nothing
 * that alternates by itself: the plane is single-buffered, so a gate that flipped
 * every tick would repaint every tick and blink (menu_window_repaint()'s rule, and
 * the trap that cost real time on the band).
 *
 * THIS IS NOW THE NO-CACHE FALLBACK. When the image pair could be allocated the tick
 * publishes with menu_side_publish() instead, and this direct paint is reached only on a
 * unit with no room for the pair. */
static void menu_side_repaint(int which)
{
    struct menu_view sv;
    int hit = side_pressed(which);
    int v   = menu_side_value(which);

    if (g.side_painted_which[which] == which && g.side_painted_hit[which] == hit &&
        g.side_painted_v[which] == v)
        return;
    menu_side_view(&sv, which);
    if (side_paint_ok(&sv))
        side_paint(&sv, which, hit, v);
    g.side_painted_which[which] = which;
    g.side_painted_hit[which]   = hit;
    g.side_painted_v[which]     = v;
}

/* A view over one drawer's SCRATCH image, for the off-lock build. Deliberately not
 * menu_side_view() above: that one reads g.side[which].pix/pitch and g.view, and
 * menu_frame_tick mutates both under g_lock -- so reading them from the builder, which
 * runs off the lock, is a race that can hand side_paint() a pointer into a plane being
 * torn down. Every scalar this needs was cached at allocation.
 *
 * fb_w/fb_h are load-bearing and not decoration: side_paint_ok() rejects a view whose
 * rectangle does not fit inside its framebuffer, and it rejects it SILENTLY -- the paint
 * simply does not happen and the drawer comes out blank. */
static void menu_side_buf_view(struct menu_view *sv, void *buf)
{
    memset(sv, 0, sizeof *sv);
    sv->pix   = buf;
    sv->pitch = g.side_buf_w;
    sv->fb_w  = g.side_buf_w;
    sv->fb_h  = g.side_buf_h;
    sv->bpp   = g.side_buf_bpp;
    sv->dw    = g.side_buf_w;
    sv->dh    = g.side_buf_h;
}

/* Build one drawer's image OFF THE LOCK, as menu_build() does for the band, and publish
 * it by swapping the two pointers UNDER it. This is the fix for the flicker the operator
 * reported on the fader.
 *
 * It reads only side_is_open()/side_pressed()/menu_side_value() -- the same off-lock
 * gesture reads menu_build() already makes, and menu_side_value() only ever touches
 * fader_state.c's ints, which nothing here locks. It writes only side_img[which][1],
 * which is never the published front. The swap is the release; the tick's trylock is the
 * acquire. A stale unlocked read of the published tuple costs one redundant or one late
 * build and nothing else, which is the property menu_build() already accepts. */
static void menu_side_build(int which)
{
    struct menu_view sv;
    int hit, v;
    void *t;
    long t0;

    if (!g.side_img[which][0])
        return;                      /* no cache: the tick paints direct, as it shipped */

    if (!side_is_open(which))
        return;

    hit = side_pressed(which);
    v   = menu_side_value(which);
    if (g.side_painted_which[which] == which &&
        g.side_painted_hit[which] == hit &&
        g.side_painted_v[which] == v)
        return;                      /* the published image is already this one */

    menu_side_buf_view(&sv, g.side_img[which][1]);
    if (!side_paint_ok(&sv))
        return;

    t0 = menu_now_us();
    side_paint(&sv, which, hit, v);
    if (g.verbose)
        pointsrc_log("side: build %ld us: %s drawer for %d/%d",
                     menu_now_us() - t0, which == SZ_LEFT ? "left" : "right", hit, v);

    pthread_mutex_lock(&g_lock);
    t = g.side_img[which][0];
    g.side_img[which][0] = g.side_img[which][1];
    g.side_img[which][1] = t;
    g.side_painted_which[which] = which;
    g.side_painted_hit[which]   = hit;
    g.side_painted_v[which]     = v;
    g.side_pending[which]       = 1;
    pthread_mutex_unlock(&g_lock);
}

/* Put the published drawer image on whatever is carrying it. The caller holds g_lock.
 *
 * ON A PLANE it copies only when there is something new to copy -- one memcpy when the
 * plane's stride is the image's width (this port's case: a 180 px plane at 16 bpp, pitch
 * 180 px) and a row at a time otherwise, the same split menu_blit() makes for the band.
 *
 * ON THE PAGE it copies EVERY tick, because rbp's own repaint erases the drawer and there
 * is no witness for a region this big. That is the band's degraded rule, and it is why
 * this cannot simply honour side_pending[] on both routes: pending means "the destination
 * has not had the CURRENT front", which on the page is true again one frame later. */
static void menu_side_publish(int which)
{
    const unsigned char *src;
    int bpp = g.side_buf_bpp / 8;

    if (g.side_painted_which[which] != which)
        return;                      /* no published image for this drawer yet */
    src = g.side_img[which][0];
    if (!src)
        return;

    if (g.side_ok[which]) {
        int y;
        long t0;

        if (!g.side_pending[which])
            return;
        t0 = menu_now_us();
        if (g.side[which].pitch == g.side_buf_w) {
            memcpy(g.side[which].pix, src, g.side_img_bytes);
        } else {
            for (y = 0; y < g.side_buf_h; y++)
                memcpy((unsigned char *)g.side[which].pix
                           + (size_t)y * (size_t)g.side[which].pitch * (size_t)bpp,
                       src + (size_t)y * (size_t)g.side_buf_w * (size_t)bpp,
                       (size_t)g.side_buf_w * (size_t)bpp);
        }
        g.side_pending[which] = 0;
        if (g.verbose)
            pointsrc_log("side: blit %ld us: %s drawer", menu_now_us() - t0,
                         which == SZ_LEFT ? "left" : "right");
        return;
    }

    /* Page route. Always, and per row: the page's stride is not the image's. */
    {
        int y;
        unsigned char *dst = (unsigned char *)g.fb_pix
            + ((size_t)g.fb_by * (size_t)g.fb_pitch
               + (size_t)(g.fb_bx
                          + (which == SZ_LEFT ? 0 : g.view.dw - g.side_buf_w)))
              * (size_t)(g.view.bpp / 8);

        for (y = 0; y < g.side_buf_h; y++)
            memcpy(dst + (size_t)y * (size_t)g.fb_pitch * (size_t)bpp,
                   src + (size_t)y * (size_t)g.side_buf_w * (size_t)bpp,
                   (size_t)g.side_buf_w * (size_t)bpp);
    }
}

/* Everything a drawer does per tick. One entry point for both routes so the tick
 * cannot take one and forget the other. */
static void menu_side_live(int which)
{
    /* The buffered route, and the one this ships on: the image was built off the lock by
     * menu_side_build() and this only COPIES it -- bounded, on both destinations. */
    if (g.side_img[which][0]) {
        menu_side_publish(which);
        return;
    }

    /* No image pair (the allocation failed): the pre-existing direct routes, unchanged,
     * with the flicker they always had. A unit with no room for 576 KB still gets its
     * drawers. */
    if (!g.side_ok[which]) {
        /* No plane: the drawer is painted onto the page EVERY tick, because rbp's own
         * repaint erases it and there is no witness for a region this big to say
         * whether it survived. That is the band's degraded path exactly, with its cost
         * and its shimmer, and it is only ever reached on a machine where the plane
         * cannot be had. */
        struct menu_view sv;

        menu_side_view(&sv, which);
        if (side_paint_ok(&sv))
            side_paint(&sv, which, side_pressed(which), menu_side_value(which));
        return;
    }
    menu_side_repaint(which);
}

/* Bring the planes into line with the drawers' state. Called under g_lock from the
 * tick, exactly as menu_window_plane_sync() is and for the same reason: one thread
 * mutates the planes and it is the paint thread. pointsrc.c only ever writes the
 * gesture state, which is what side_is_open() reads.
 *
 * EACH DRAWER HAS ITS OWN PLANE and is brought up or given back on its own, which is
 * what makes the operator's "both side panels at the same time" work: nothing here
 * hands a slot over any more. What IS still exclusive is the BAND's plane, because
 * the band and a drawer overlap at the top corners of the glass and two overlays
 * cannot share a zpos -- so the band's goes down while ANY drawer is out and comes
 * back, freshly read off the live page, when the last one goes. */
static void menu_side_plane_sync(void)
{
    int i, any = side_any_open();

    if (any && g.band_ok) {
        drm_band_teardown(&g.band);
        g.band_ok = g.band_on = 0;
        /* Back to the page in the same breath: the band's buffer is gone, so a view
         * still pointing at it would be a dangling mapping. */
        g.view.pix   = g.fb_pix;
        g.view.pitch = g.fb_pitch;
        g.view.bx    = g.fb_bx;
        g.view.by    = g.fb_by;
        pointsrc_log("side: a drawer is out -- the plane is handed over from the band");
    }

    for (i = 0; i < 2; i++) {
        if (!side_is_open(i)) {
            /* Shut: give back whatever this drawer had, and clear the "refused"
             * latch so a later open tries the plane again. */
            if (g.side_ok[i]) {
                drm_band_teardown(&g.side[i]);
                g.side_ok[i] = g.side_on[i] = 0;
                pointsrc_log("side: %s drawer closed -- its plane is released",
                             i == SZ_LEFT ? "left" : "right");
            }
            /* The published image is stale the instant the drawer is shut, whether or not
             * it had a plane of its own: clearing this only inside the branch above would
             * leave the page route's tuple live across a close, and the next open would
             * show the previous session's drawer for a tick. */
            g.side_painted_which[i] = -1;
            g.side_painted_hit[i]   = SZ_HIT_NONE;
            g.side_painted_v[i]     = -1;
            g.side_pending[i]       = 0;
            g.side_fail[i] = 0;
            continue;
        }

        /* Once, not per tick, for win_fail's reason: a plane that will not go up will
         * not go up on the next tick either, and an unguarded retry would be a
         * setup/teardown pair at the tick period, on the thread rbp presents from. */
        if (!g.side_ok[i] && !g.side_fail[i] && !g.band_off) {
            if (drm_band_setup(&g.side[i], menu_side_pw(), g.view.dh, g.view.bpp) != 0)
                g.side_fail[i] = 1;
            else {
                g.side_ok[i] = 1;
                g.side_on[i] = 0;
                /* A fresh dumb buffer holds nothing anyone can read. The image has to be
                 * COPIED into it before it is shown, and the front that gets copied is
                 * built on the witness thread -- so mark the copy pending rather than
                 * resetting the published tuple (which the builder may have just set), and
                 * let the show below wait for a front to exist. Arming a paint here
                 * instead would be the old bug back: a flag does not initialise a
                 * buffer. */
                g.side_pending[i] = 1;
            }
        }
        /* Painted before it is shown, never after: what goes on the glass the first time
         * must be the finished drawer and not the uninitialised buffer it was created
         * with (menu_band_present()'s rule). With a cached image that means waiting for
         * the witness thread to publish one and menu_side_live() above to copy it in --
         * `side_painted_which[i] == i` is exactly "there is a real drawer in that buffer
         * now". The cost is at most one tick of absence, and only when the vsync hook
         * reaches here before the builder has run. */
        menu_side_live(i);
        if (g.side_ok[i] && !g.side_on[i] && g.side_painted_which[i] == i &&
            drm_band_show(&g.side[i],
                          g.fb_bx + (i == SZ_LEFT ? 0
                                     : g.view.dw - menu_side_pw()),
                          g.fb_by) == 0)
            g.side_on[i] = 1;
    }

    /* The band's plane comes back whether or not a drawer ever had one: on the
     * fallback route the band's was torn down for a setup that then failed. A band
     * that is not on the glass costs nothing, and this is what makes a swipe-down
     * work again the moment the last drawer goes. */
    if (!any && g.side_prev) {
        if (!g.band_ok && menu_band_setup_from_page() != 0)
            pointsrc_log("menu: the plane did not come back to the band -- it stays"
                         " on the page");
        else
            pointsrc_log("side: no drawer is out -- the plane goes back to the band");
    }
    g.side_prev = any;
}

/* ---------------- THE USB STOP CHOOSER'S PLANE ---------------------------
 *
 * The fourth holder of a drm_band, and the one that is not a handover at all. Its
 * argument is prompt_zone.h's, and it is not repeated: the box is the answer to the
 * operator's *"when you have USB stop, put up a prompt for USB1, USB2 or Cancel"*, and
 * everything below is the same publish-a-finished-image rule the band and the drawers
 * already keep, at the box's own size. */

/* The box's size in fb px -- PR_W/PR_H logical px at the page's own scale, the same
 * rule menu_side_pw() applies to the drawer's width. */
static int menu_prompt_pw(void)
{
    return prompt_paint_w(g.view.dw);
}

static int menu_prompt_ph(void)
{
    return prompt_paint_h(g.view.dh);
}

/* Where the box lands on the page. Centred, because that is what the logical geometry
 * already says it is (PR_X0/PR_Y0 are the screen's centre less half the box) -- so this
 * is the same statement in framebuffer pixels and not a second opinion. */
static int menu_prompt_bx(void)
{
    return g.fb_bx + (g.view.dw - menu_prompt_pw()) / 2;
}

static int menu_prompt_by(void)
{
    return g.fb_by + (g.view.dh - menu_prompt_ph()) / 2;
}

/* A view over the box, whichever route it is being drawn on. ON A PLANE the buffer IS
 * the box: origin 0,0 and dw/dh its own size, because that is the contract
 * prompt_paint.h states. ON THE PAGE the origin moves to the box's screen rectangle and
 * the pitch stays the page's, so the same prompt_paint() call works on both -- exactly
 * menu_side_view()'s split. */
static void menu_prompt_view(struct menu_view *pv)
{
    memset(pv, 0, sizeof *pv);
    if (g.prompt_ok) {
        pv->pix   = g.prompt.pix;
        pv->pitch = g.prompt.pitch;
        pv->fb_w  = g.prompt.w;
        pv->fb_h  = g.prompt.h;
        pv->bpp   = g.prompt.bpp;
        pv->dw    = g.prompt.w;
        pv->dh    = g.prompt.h;
        return;
    }
    pv->dw  = (unsigned)menu_prompt_pw();
    pv->dh  = menu_prompt_ph();
    pv->pix = (unsigned char *)g.fb_pix
              + ((size_t)menu_prompt_by() * (size_t)g.fb_pitch
                 + (size_t)menu_prompt_bx()) * (size_t)(g.view.bpp / 8);
    pv->pitch = g.fb_pitch;
    pv->fb_w  = pv->dw;
    pv->fb_h  = pv->dh;
    pv->bpp   = g.view.bpp;
}

/* A view over the box's SCRATCH image, for the off-lock build. Deliberately not
 * menu_prompt_view() above: that one reads g.prompt.pix/pitch and g.view, and
 * menu_frame_tick mutates both under g_lock, so reading them from the builder would be
 * the same race menu_side_buf_view() exists to avoid. Every scalar here was cached at
 * allocation, and fb_w/fb_h are load-bearing -- prompt_paint_ok() rejects a view whose
 * rectangle does not fit inside its framebuffer, and it rejects it SILENTLY. */
static void menu_prompt_buf_view(struct menu_view *pv, void *buf)
{
    memset(pv, 0, sizeof *pv);
    pv->pix   = buf;
    pv->pitch = g.prompt_buf_w;
    pv->fb_w  = g.prompt_buf_w;
    pv->fb_h  = g.prompt_buf_h;
    pv->bpp   = g.prompt_buf_bpp;
    pv->dw    = g.prompt_buf_w;
    pv->dh    = g.prompt_buf_h;
}

/* Build the box OFF THE LOCK and publish it by swapping the two pointers under it --
 * menu_build()'s and menu_side_build()'s rule, and here for the same reason.
 *
 * The liveness is what makes this more than a highlight: rbp's answer about the two
 * devices is part of the picture (prompt_paint.c draws a row whose device is absent in
 * the OFF palette), so pointsrc_usb_state() is re-read on every build and the answer it
 * was read at is what the published tuple records. A device the operator stops
 * elsewhere therefore changes the box on the next tick, with nothing to invalidate. */
static void menu_prompt_build(void)
{
    struct menu_view pv;
    struct prompt_state S;
    int cell, flash, live;
    void *t;
    long t0;

    if (!g.prompt_img[0])
        return;                      /* no cache: the tick paints direct, as it shipped */
    if (!prompt_is_open())
        return;

    pointsrc_usb_state(&S);
    cell  = prompt_pressed();
    flash = prompt_hold_flash() ? prompt_hold_cell() : 0;
    live = (S.live[0] ? 1 : 0) | (S.live[1] ? 2 : 0);
    /* FOUR THINGS MAKE THE PICTURE, so four things are in the key. The flash is one
     * of them and it is the one that is easy to forget: a hold moves the box nowhere and
     * changes no other value, so if the phase were not here the box would go on
     * publishing the image it built when the finger went down and the button would
     * simply light once ([[a-dead-driver-flashes-the-chrome]] is the same trap: a gate
     * that cannot see the thing that changed). The names are the fourth and they are in
     * the key for exactly that reason (the field's own note above). */
    if (g.prompt_valid && g.prompt_painted_cell == cell &&
        g.prompt_painted_flash == flash && g.prompt_painted_live == live &&
        !memcmp(g.prompt_painted_name, S.label, sizeof g.prompt_painted_name))
        return;                      /* the published image is already this one */

    menu_prompt_buf_view(&pv, g.prompt_img[1]);
    if (!prompt_paint_ok(&pv))
        return;

    t0 = menu_now_us();
    prompt_paint(&pv, &S, cell, flash);
    if (g.verbose)
        pointsrc_log("prompt: build %ld us: cell %d, flash %d, usb 1 %s '%s',"
                     " usb 2 %s '%s'",
                     menu_now_us() - t0, cell, flash,
                     S.live[0] ? "ready" : "absent",
                     prompt_cell_text(&S, PR_CELL_USB1),
                     S.live[1] ? "ready" : "absent",
                     prompt_cell_text(&S, PR_CELL_USB2));

    pthread_mutex_lock(&g_lock);
    t = g.prompt_img[0];
    g.prompt_img[0] = g.prompt_img[1];
    g.prompt_img[1] = t;
    g.prompt_painted_cell  = cell;
    g.prompt_painted_flash = flash;
    g.prompt_painted_live  = live;
    memcpy(g.prompt_painted_name, S.label, sizeof g.prompt_painted_name);
    g.prompt_valid = 1;
    g.prompt_pending = 1;
    pthread_mutex_unlock(&g_lock);
}

/* Put the published box on whatever is carrying it. The caller holds g_lock.
 *
 * ON A PLANE it copies only when there is something new, one memcpy when the plane's
 * stride is the box's width and a row at a time otherwise -- the split menu_side_publish()
 * and menu_blit() both make.
 *
 * ON THE PAGE it copies EVERY tick, because rbp repaints the middle of the performance
 * screen 57 times a second and there is no witness for a region this size. The same rule
 * as the drawer's, and it is why prompt_pending cannot simply be honoured on both. */
static void menu_prompt_publish(void)
{
    const unsigned char *src;
    int bpp = g.prompt_buf_bpp / 8;

    if (!g.prompt_valid)
        return;                      /* no finished box exists yet */
    src = g.prompt_img[0];
    if (!src)
        return;

    if (g.prompt_ok) {
        int y;
        long t0;

        if (!g.prompt_pending)
            return;
        t0 = menu_now_us();
        if (g.prompt.pitch == g.prompt_buf_w) {
            memcpy(g.prompt.pix, src, g.prompt_img_bytes);
        } else {
            for (y = 0; y < g.prompt_buf_h; y++)
                memcpy((unsigned char *)g.prompt.pix
                           + (size_t)y * (size_t)g.prompt.pitch * (size_t)bpp,
                       src + (size_t)y * (size_t)g.prompt_buf_w * (size_t)bpp,
                       (size_t)g.prompt_buf_w * (size_t)bpp);
        }
        g.prompt_pending = 0;
        if (g.verbose)
            pointsrc_log("prompt: blit %ld us", menu_now_us() - t0);
        return;
    }

    /* Page route. Always, and per row: the page's stride is not the box's. */
    {
        int y;
        unsigned char *dst = (unsigned char *)g.fb_pix
            + ((size_t)menu_prompt_by() * (size_t)g.fb_pitch
               + (size_t)menu_prompt_bx()) * (size_t)(g.view.bpp / 8);

        for (y = 0; y < g.prompt_buf_h; y++)
            memcpy(dst + (size_t)y * (size_t)g.fb_pitch * (size_t)bpp,
                   src + (size_t)y * (size_t)g.prompt_buf_w * (size_t)bpp,
                   (size_t)g.prompt_buf_w * (size_t)bpp);
    }
}

/* The no-cache fallback: paint the box straight into its destination, as the drawers
 * did before they were given an image pair. Reached only on a unit with no room for
 * another 607 KB. */
static void menu_prompt_repaint(void)
{
    struct menu_view pv;
    struct prompt_state S;
    int cell, flash, live;

    pointsrc_usb_state(&S);
    cell  = prompt_pressed();
    flash = prompt_hold_flash() ? prompt_hold_cell() : 0;
    live = (S.live[0] ? 1 : 0) | (S.live[1] ? 2 : 0);
    if (g.prompt_valid && g.prompt_painted_cell == cell &&
        g.prompt_painted_flash == flash && g.prompt_painted_live == live &&
        !memcmp(g.prompt_painted_name, S.label, sizeof g.prompt_painted_name))
        return;
    menu_prompt_view(&pv);
    if (prompt_paint_ok(&pv))
        prompt_paint(&pv, &S, cell, flash);
    g.prompt_valid = 1;
    g.prompt_painted_cell  = cell;
    g.prompt_painted_flash = flash;
    g.prompt_painted_live  = live;
    memcpy(g.prompt_painted_name, S.label, sizeof g.prompt_painted_name);
}

/* Everything the box does per tick. One entry point for both routes, so the tick
 * cannot take one and forget the other. */
static void menu_prompt_live(void)
{
    if (!prompt_is_open())
        return;

    /* The buffered route, and the one this ships on: the image was built off the lock by
     * menu_prompt_build() and this only COPIES it. The show waits for prompt_valid, so a
     * fresh plane is never seen holding the uninitialised buffer it was created with --
     * the cost is at most one tick of absence. */
    if (g.prompt_img[0]) {
        menu_prompt_publish();
        if (g.prompt_valid && g.prompt_ok && !g.prompt_on &&
            drm_band_show(&g.prompt, menu_prompt_bx(), menu_prompt_by()) == 0)
            g.prompt_on = 1;
        return;
    }

    if (!g.prompt_ok) {
        struct menu_view pv;
        struct prompt_state S;

        pointsrc_usb_state(&S);
        menu_prompt_view(&pv);
        if (prompt_paint_ok(&pv))
            prompt_paint(&pv, &S, prompt_pressed(),
                         prompt_hold_flash() ? prompt_hold_cell() : 0);
        g.prompt_valid = 1;
        return;
    }
    menu_prompt_repaint();
}

/* Bring the box's plane into line with the box. Called under g_lock from the tick,
 * after the window's and the drawers' syncs so that anything they had has already been
 * given back -- and before the band's path, which the box's being up skips entirely.
 *
 * A machine that refuses a box-sized plane gets the page route and keeps working at the
 * band's fallback cost; prompt_fail is the once-only latch that stops it retrying at the
 * tick period, on the thread rbp presents from. */
static void menu_prompt_plane_sync(void)
{
    if (!prompt_is_open()) {
        if (g.prompt_ok) {
            drm_band_teardown(&g.prompt);
            g.prompt_ok = g.prompt_on = 0;
            pointsrc_log("prompt: the box closed -- its plane is released");
        }
        /* Cleared unconditionally, inside and outside the branch above: a closed box's
         * image is stale whether or not it ever had a plane, and leaving the tuple live
         * across a close would show the previous box for a tick. */
        g.prompt_valid   = 0;
        g.prompt_pending = 0;
        g.prompt_fail    = 0;
        return;
    }

    if (!g.prompt_ok && !g.prompt_fail) {
        if (drm_band_setup(&g.prompt, menu_prompt_pw(), menu_prompt_ph(),
                           g.view.bpp) != 0) {
            g.prompt_fail = 1;
            pointsrc_log("prompt: no plane for the box -- it goes on the page");
        } else {
            g.prompt_ok = 1;
            g.prompt_on = 0;
            /* A fresh dumb buffer holds nothing anyone can read, so the image has to be
             * COPIED in before the box is shown -- which is what prompt_pending says, and
             * prompt_valid, which the show waits on, is what says there is a real box to
             * copy. Arming a paint here instead would be the old bug back: a flag does
             * not initialise a buffer. */
            g.prompt_pending = 1;
        }
    }

    menu_prompt_live();
}

/* ---------------- THE BEAT FX PICKER'S PLANE -----------------------------
 *
 * The fifth holder of a drm_band and, like the chooser's, not a handover at all. Its
 * argument is fx_zone.h's and is not repeated: the box is the answer to the operator's
 * *"when i touch the beat fx have a popup menu with all the fx available so i can
 * select"*, and everything below is the same publish-a-finished-image rule the band,
 * the drawers and the chooser keep, at the box's own size.
 *
 * IT IS THE CHOOSER'S CODE WITH ONE FIELD, which is the whole difference between the
 * two boxes: the picker has a row under a finger and nothing else -- no liveness, no
 * hold -- so where the chooser keys its published image on a three-value tuple this
 * one keys on a single int. Written out rather than shared because the shared version
 * would be a function taking a pointer to a slot in `struct g` and a callback for
 * "what is the picture now", and that indirection would cost more than the ~120
 * duplicated lines here do -- the same judgement prompt_paint.c records about the
 * pixel accessor. */

static int menu_fxlist_pw(void)
{
    return fxlist_paint_w(g.view.dw);
}

static int menu_fxlist_ph(void)
{
    return fxlist_paint_h(g.view.dh);
}

/* WHERE THE BOX GOES, and it is fx_zone.h's business and not this file's: the picker is
 * drawn in rbp's BEAT FX plate's own rectangle, straight over the panel, so its place is
 * that rectangle and nothing is derived here. It is NOT centred, which is what these two
 * functions did before the operator saw the first build and asked for it to sit on the
 * panel ("the same dimensions of the beatfx box ... you don't need a title menu"). The logical->page
 * scale is the drawers' (menu_side_pw() does the same for SZ_W), with fx_zone.h's
 * FX_X0/FX_Y0 as the logical origin -- the same origin fxlist_paint() subtracts before
 * scaling, so the picture and the plane cannot land one pixel apart. */
static int menu_fxlist_bx(void)
{
    return g.fb_bx + (FX_X0 * g.view.dw) / MZ_LOGICAL_W;
}

static int menu_fxlist_by(void)
{
    return g.fb_by + (FX_Y0 * g.view.dh) / MZ_LOGICAL_H;
}

static void menu_fxlist_view(struct menu_view *pv)
{
    memset(pv, 0, sizeof *pv);
    if (g.fxlist_ok) {
        pv->pix   = g.fxlist.pix;
        pv->pitch = g.fxlist.pitch;
        pv->fb_w  = g.fxlist.w;
        pv->fb_h  = g.fxlist.h;
        pv->bpp   = g.fxlist.bpp;
        pv->dw    = g.fxlist.w;
        pv->dh    = g.fxlist.h;
        return;
    }
    pv->dw  = (unsigned)menu_fxlist_pw();
    pv->dh  = (unsigned)menu_fxlist_ph();
    pv->pix = (unsigned char *)g.fb_pix
              + ((size_t)menu_fxlist_by() * (size_t)g.fb_pitch
                 + (size_t)menu_fxlist_bx()) * (size_t)(g.view.bpp / 8);
    pv->pitch = g.fb_pitch;
    pv->fb_w  = pv->dw;
    pv->fb_h  = pv->dh;
    pv->bpp   = g.view.bpp;
}

/* Over the picker's SCRATCH image, for the off-lock build -- menu_prompt_buf_view()'s
 * function verbatim and for its reason: menu_frame_tick mutates g.fxlist.pix and
 * g.view under g_lock, so reading either from the builder would be the race that
 * function exists to avoid. fb_w/fb_h are load-bearing: fxlist_paint_ok() rejects a
 * view whose rectangle does not fit inside its framebuffer, and it rejects it SILENTLY. */
static void menu_fxlist_buf_view(struct menu_view *pv, void *buf)
{
    memset(pv, 0, sizeof *pv);
    pv->pix   = buf;
    pv->pitch = g.fxlist_buf_w;
    pv->fb_w  = g.fxlist_buf_w;
    pv->fb_h  = g.fxlist_buf_h;
    pv->bpp   = g.fxlist_buf_bpp;
    pv->dw    = g.fxlist_buf_w;
    pv->dh    = g.fxlist_buf_h;
}

/* Build the picker OFF THE LOCK and publish it by swapping the two pointers under it.
 * ONE VALUE MAKES THE PICTURE -- the row under the finger -- so the gate is that one
 * comparison, and it is a transition gate rather than a per-tick one, which is what
 * keeps the plane from blinking. */
static void menu_fxlist_build(void)
{
    struct menu_view pv;
    int row;
    void *t;
    long t0;

    if (!g.fxlist_img[0])
        return;                      /* no cache: the tick paints direct, as it shipped */
    if (!fxlist_is_open())
        return;

    row = fxlist_pressed();
    if (g.fxlist_valid && g.fxlist_painted_row == row)
        return;                      /* the published image is already this one */

    menu_fxlist_buf_view(&pv, g.fxlist_img[1]);
    if (!fxlist_paint_ok(&pv))
        return;

    t0 = menu_now_us();
    fxlist_paint(&pv, row);
    if (g.verbose)
        pointsrc_log("fxlist: build %ld us: row %d", menu_now_us() - t0, row);

    pthread_mutex_lock(&g_lock);
    t = g.fxlist_img[0];
    g.fxlist_img[0] = g.fxlist_img[1];
    g.fxlist_img[1] = t;
    g.fxlist_painted_row = row;
    g.fxlist_valid = 1;
    g.fxlist_pending = 1;
    pthread_mutex_unlock(&g_lock);
}

/* Put the published picker on whatever is carrying it. The caller holds g_lock.
 * menu_prompt_publish()'s two routes and its two reasons: a plane copies only when
 * there is something new, and the page copies EVERY tick because rbp repaints the
 * middle of the performance screen 57 times a second and there is no witness for a
 * region this size. */
static void menu_fxlist_publish(void)
{
    const unsigned char *src;
    int bpp = g.fxlist_buf_bpp / 8;

    if (!g.fxlist_valid)
        return;                      /* no finished picker exists yet */
    src = g.fxlist_img[0];
    if (!src)
        return;

    if (g.fxlist_ok) {
        int y;
        long t0;

        if (!g.fxlist_pending)
            return;
        t0 = menu_now_us();
        if (g.fxlist.pitch == g.fxlist_buf_w) {
            memcpy(g.fxlist.pix, src, g.fxlist_img_bytes);
        } else {
            for (y = 0; y < g.fxlist_buf_h; y++)
                memcpy((unsigned char *)g.fxlist.pix
                           + (size_t)y * (size_t)g.fxlist.pitch * (size_t)bpp,
                       src + (size_t)y * (size_t)g.fxlist_buf_w * (size_t)bpp,
                       (size_t)g.fxlist_buf_w * (size_t)bpp);
        }
        g.fxlist_pending = 0;
        if (g.verbose)
            pointsrc_log("fxlist: blit %ld us", menu_now_us() - t0);
        return;
    }

    /* Page route. Always, and per row: the page's stride is not the box's. */
    {
        int y;
        unsigned char *dst = (unsigned char *)g.fb_pix
            + ((size_t)menu_fxlist_by() * (size_t)g.fb_pitch
               + (size_t)menu_fxlist_bx()) * (size_t)(g.view.bpp / 8);

        for (y = 0; y < g.fxlist_buf_h; y++)
            memcpy(dst + (size_t)y * (size_t)g.fb_pitch * (size_t)bpp,
                   src + (size_t)y * (size_t)g.fxlist_buf_w * (size_t)bpp,
                   (size_t)g.fxlist_buf_w * (size_t)bpp);
    }
}

/* The no-cache fallback: paint the picker straight into its destination. Reached only on
 * a unit with no room for another image pair. */
static void menu_fxlist_repaint(void)
{
    struct menu_view pv;
    int row = fxlist_pressed();

    if (g.fxlist_valid && g.fxlist_painted_row == row)
        return;
    menu_fxlist_view(&pv);
    if (fxlist_paint_ok(&pv))
        fxlist_paint(&pv, row);
    g.fxlist_valid = 1;
    g.fxlist_painted_row = row;
}

/* Everything the picker does per tick. One entry point for both routes, so the tick
 * cannot take one and forget the other. */
static void menu_fxlist_live(void)
{
    if (!fxlist_is_open())
        return;

    /* The buffered route: the image was built off the lock by menu_fxlist_build() and
     * this only COPIES it. The show waits for fxlist_valid, so a fresh plane is never
     * seen holding the uninitialised buffer it was created with. */
    if (g.fxlist_img[0]) {
        menu_fxlist_publish();
        if (g.fxlist_valid && g.fxlist_ok && !g.fxlist_on &&
            drm_band_show(&g.fxlist, menu_fxlist_bx(), menu_fxlist_by()) == 0)
            g.fxlist_on = 1;
        return;
    }

    if (!g.fxlist_ok) {
        struct menu_view pv;

        menu_fxlist_view(&pv);
        if (fxlist_paint_ok(&pv))
            fxlist_paint(&pv, fxlist_pressed());
        g.fxlist_valid = 1;
        return;
    }
    menu_fxlist_repaint();
}

/* Bring the picker's plane into line with the picker. Called under g_lock from the tick,
 * after the chooser's sync, and the same once-only `fxlist_fail` latch keeps a machine
 * that refuses the plane off the tick period. */
static void menu_fxlist_plane_sync(void)
{
    if (!fxlist_is_open()) {
        if (g.fxlist_ok) {
            drm_band_teardown(&g.fxlist);
            g.fxlist_ok = g.fxlist_on = 0;
            pointsrc_log("fxlist: the picker closed -- its plane is released");
        }
        /* Cleared unconditionally, inside and outside the branch above: a closed
         * picker's image is stale whether or not it ever had a plane, and leaving the
         * tuple live across a close would show the previous picker for a tick. */
        g.fxlist_valid   = 0;
        g.fxlist_pending = 0;
        g.fxlist_fail    = 0;
        return;
    }

    if (!g.fxlist_ok && !g.fxlist_fail) {
        if (drm_band_setup(&g.fxlist, menu_fxlist_pw(), menu_fxlist_ph(),
                           g.view.bpp) != 0) {
            g.fxlist_fail = 1;
            pointsrc_log("fxlist: no plane for the picker -- it goes on the page");
        } else {
            g.fxlist_ok = 1;
            g.fxlist_on = 0;
            /* A fresh dumb buffer holds nothing anyone can read, so the image has to be
             * COPIED in before the picker is shown -- what fxlist_pending says, and
             * fxlist_valid, which the show waits on, is what says there is a real
             * picker to copy. */
            g.fxlist_pending = 1;
        }
    }

    menu_fxlist_live();
}

/* ---------------- THE MOMENTARY PAD'S PLANE -------------------------------
 *
 * The sixth holder of a drm_band and, uniquely, NOT a handover and not a box: it is up
 * for exactly as long as a finger is on rbp's BPM detail cell, and its whole job is to
 * put a frame and a dot over pixels that are still rbp's. fxpad_paint.h has the argument
 * for why that means COPYING rbp's own cell in every tick rather than drawing over it.
 *
 * WHAT IT DOES NOT HAVE, AND WHY. No image pair, no publish gate, no `painted_*` tuple:
 * the five boxes above all key their image on a handful of values that only change when
 * the operator does something, and each of them has a comment about how a gate fed
 * something that alternates by itself would repaint every tick and blink. That reasoning
 * INVERTS here. The picture is a function of rbp's pixels (which move under a playing
 * track -- the BPM readout is live) and of the finger (which moves every report), so it
 * is genuinely different every tick while it is up, and painting it every tick is not a
 * gate that failed -- it is the only honest answer. The cost is the cell: 160x137 at the
 * page's scale, ~44 KB of copy a tick, which is nothing against rbp repainting the whole
 * screen 57 times a second underneath it.
 *
 * THE ONE THING IT INHERITS IS THE MUTUAL EXCLUSION. The pad cannot be engaged while any
 * box, drawer or window is up -- pointsrc.c's press gate asks every one of them before
 * it lets a finger take the cell -- so this plane and theirs are never up together, and
 * the tick below does not have to arbitrate between them. */

static int menu_fxp_pw(void)
{
    return fxpad_paint_w(g.view.dw);
}

static int menu_fxp_ph(void)
{
    return fxpad_paint_h(g.view.dh);
}

/* WHERE IT GOES, and fxpad_zone.h's FXPAD_* are rbp's OWN cell coordinates -- the copy
 * lands exactly on the black square rbp drew, which is the whole point of measuring them
 * rather than deriving them. The origin is subtracted before scaling, as fxlist_paint()
 * does with FX_X0, so the picture and the plane cannot land a pixel apart. */
static int menu_fxp_bx(void)
{
    return g.fb_bx + (FXPAD_X0 * g.view.dw) / MZ_LOGICAL_W;
}

static int menu_fxp_by(void)
{
    return g.fb_by + (FXPAD_Y0 * g.view.dh) / MZ_LOGICAL_H;
}

/* rbp's OWN PIXELS for the cell -- the copy's source, and the reason this box is not
 * like the other five. It is the page: the live framebuffer rbp is painting, addressed at
 * the same screen rectangle the plane will cover, so the HUD is drawn over what rbp has
 * just put there rather than over a snapshot from some earlier tick. Its origin is the
 * cell's top-left and its size the plane's, which is what makes fxpad_paint()'s copy a
 * straight point-for-point move. */
static void menu_fxp_page_view(struct menu_view *sv)
{
    memset(sv, 0, sizeof *sv);
    sv->pix   = (unsigned char *)g.fb_pix
                + ((size_t)menu_fxp_by() * (size_t)g.fb_pitch
                   + (size_t)menu_fxp_bx()) * (size_t)(g.view.bpp / 8);
    sv->pitch = g.fb_pitch;
    sv->dw    = menu_fxp_pw();
    sv->dh    = menu_fxp_ph();
    sv->fb_w  = sv->dw;
    sv->fb_h  = sv->dh;
    sv->bpp   = g.view.bpp;
}

/* Where the pad's HUD is drawn into: its plane's buffer, or -- on a machine that refused
 * one -- the page itself, in which case there is nothing to copy (the destination IS
 * rbp's pixels) and menu_fxpad_live() passes a NULL source. This is MENU_PLANE=0's route
 * and a refused sixth plane's fallback, and it shimmers exactly as the band's does there:
 * rbp's next frame erases the two marks and this redraws them. */
static void menu_fxp_view(struct menu_view *sv)
{
    memset(sv, 0, sizeof *sv);
    if (g.fxp_ok) {
        sv->pix   = g.fxp.pix;
        sv->pitch = g.fxp.pitch;
        sv->fb_w  = g.fxp.w;
        sv->fb_h  = g.fxp.h;
        sv->bpp   = g.fxp.bpp;
        sv->dw    = g.fxp.w;
        sv->dh    = g.fxp.h;
        return;
    }
    menu_fxp_page_view(sv);
}

/* Everything the pad does per tick. One entry point for both routes, so the tick cannot
 * take one and forget the other -- menu_fxlist_live()'s shape. */
static void menu_fxpad_live(void)
{
    struct menu_view dv, pv;
    int mx = 0, my = 0;
    int mark = fxpad_mark(&mx, &my);
    long t0;

    menu_fxp_view(&dv);
    if (!fxpad_paint_ok(&dv))
        return;

    t0 = menu_now_us();
    if (g.fxp_ok) {
        menu_fxp_page_view(&pv);
        fxpad_paint(&dv, &pv, mark, mx, my);
    } else {
        /* No plane: the destination is the page and the marks go straight onto rbp's
         * pixels, so there is nothing to copy and the source is deliberately NULL. */
        fxpad_paint(&dv, NULL, mark, mx, my);
    }
    if (g.verbose)
        pointsrc_log("fxpad: hud %ld us: mark %d at %d,%d", menu_now_us() - t0, mark, mx, my);

    /* Shown, not shown once, for drm_band_show()'s own reason: it is idempotent while it
     * is already up in the same place, so a tick may call it every frame, and the first
     * tick that has a real image is the first that shows it -- a fresh dumb buffer holds
     * nothing anyone can read. */
    if (!g.fxp_on && drm_band_show(&g.fxp, menu_fxp_bx(), menu_fxp_by()) == 0)
        g.fxp_on = 1;
}

/* Bring the pad's plane into line with the pad. Called under g_lock from the tick, and
 * FIRST there, before any route is decided -- which is the one thing this box does
 * differently from the five above. Their argument for going last is that each gives its
 * predecessor the chance to release what it holds; this one has the opposite obligation,
 * because it is the only plane whose right to be on the glass depends on a finger that
 * can be gone by the time the tick runs. A pad left up would be an opaque rectangle over
 * the operator's own readout, so it is released before anything else can return early.
 *
 * SHOWN AND HIDDEN PER GESTURE, SET UP ONCE. The five above tear their plane down when
 * their box closes, and the reason they can is that a box is up for a deliberate session:
 * the setup and the teardown are paid once for a minute of use. The pad's lifetime is a
 * TAP -- engage, drag, release, unwound inside a second -- and it is used dozens of times
 * in a set, so paying create_dumb/AddFB/set_plane and their reverse on every finger would
 * be paying the expensive half of this on the part the operator will feel. What actually
 * takes the rectangle off the glass is drm_band_hide() (one ioctl), so that is what the
 * idle tick does: the buffer stays mapped and hidden, and the next gesture paints it
 * before showing it, which is what keeps a stale frame off the glass.
 *
 * The one thing NOT given up is the refusal latch: a machine that cannot have this plane
 * says so once and goes on the page, as the picker's does. */
static void menu_fxpad_plane_sync(void)
{
    if (!fxpad_busy()) {
        if (g.fxp_on) {
            drm_band_hide(&g.fxp);
            g.fxp_on = 0;
        }
        return;
    }

    if (!g.fxp_ok && !g.fxp_fail) {
        if (drm_band_setup(&g.fxp, menu_fxp_pw(), menu_fxp_ph(), g.view.bpp) != 0) {
            g.fxp_fail = 1;
            pointsrc_log("fxpad: no plane for the pad -- its HUD goes on the page");
        } else {
            g.fxp_ok = 1;
            g.fxp_on = 0;
        }
    }

    menu_fxpad_live();
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

    /* THE DRAWERS' IMAGES, beside the band's and INDEPENDENTLY of them. A failure here
     * must NOT touch this function's return value: the -1 above is the BAND's "no image
     * cache" signal, and returning it because a drawer could not get 576 KB would drop
     * the band to the page as well. A drawer with no image keeps the direct-paint route
     * it shipped with, and side_painted_which stays -1 for it.
     *
     * Retried every call while it fails, exactly as the band's allocation above is; the
     * log line is the once-only part. A pair is stored WHOLE and never freed once valid,
     * so the tick -- which reads only side_img[i][0] -- sees either NULL or a pair it can
     * safely copy from. */
    if (!g.side_img[SZ_LEFT][0] || !g.side_img[SZ_RIGHT][0]) {
        static int side_alloc_logged;
        size_t sn = (size_t)menu_side_pw() * (size_t)g.view.dh
                    * (size_t)(g.view.bpp / 8);
        int i;

        g.side_buf_w     = menu_side_pw();
        g.side_buf_h     = g.view.dh;
        g.side_buf_bpp   = g.view.bpp;
        g.side_img_bytes = sn;

        for (i = 0; i < 2; i++) {
            void *a, *b;

            if (g.side_img[i][0] && g.side_img[i][1])
                continue;            /* this drawer's pair is already good */

            if (g.side_img[i][0] || g.side_img[i][1]) {
                /* Half a pair left by a failed attempt. Never published, so safe to
                 * drop; nothing else frees these. */
                free(g.side_img[i][0]);
                free(g.side_img[i][1]);
                g.side_img[i][0] = g.side_img[i][1] = NULL;
            }

            a = malloc(sn);
            b = malloc(sn);
            if (!a || !b) {
                free(a);
                free(b);
                if (!side_alloc_logged) {
                    side_alloc_logged = 1;
                    pointsrc_log("side: no %zu-byte drawer image (%s); the drawer"
                                 " paints straight onto its plane", sn * 2,
                                 strerror(errno));
                }
                continue;
            }
            g.side_img[i][0] = a;
            g.side_img[i][1] = b;
        }
    }

    /* THE CHOOSER'S IMAGE, on exactly the drawers' terms: its own pair, its own once-only
     * log, and INDEPENDENT of this function's return value for the same reason (the -1
     * below is the BAND's signal, and returning it because the box could not get 607 KB
     * would drop the band to the page as well). With no pair the box takes
     * menu_prompt_repaint()'s direct-paint route and still works.
     *
     * Sized from prompt_paint_w()/prompt_paint_h() -- the SAME functions the plane is set
     * up from and the same ones menu_prompt_bx()/by() centre with -- so the three cannot
     * disagree about how big the box is. */
    if (!g.prompt_img[0]) {
        static int prompt_alloc_logged;
        size_t pn = (size_t)prompt_paint_w(g.view.dw)
                    * (size_t)prompt_paint_h(g.view.dh)
                    * (size_t)(g.view.bpp / 8);
        void *a, *b;

        g.prompt_buf_w     = prompt_paint_w(g.view.dw);
        g.prompt_buf_h     = prompt_paint_h(g.view.dh);
        g.prompt_buf_bpp   = g.view.bpp;
        g.prompt_img_bytes = pn;

        if (g.prompt_img[1]) {
            free(g.prompt_img[1]);   /* half a pair from a failed attempt */
            g.prompt_img[1] = NULL;
        }
        a = malloc(pn);
        b = malloc(pn);
        if (!a || !b) {
            free(a);
            free(b);
            if (!prompt_alloc_logged) {
                prompt_alloc_logged = 1;
                pointsrc_log("prompt: no %zu-byte box image (%s); the box paints"
                             " straight onto its plane", pn * 2, strerror(errno));
            }
        } else {
            g.prompt_img[0] = a;
            g.prompt_img[1] = b;
        }
    }

    /* THE PICKER'S IMAGE, on exactly the chooser's terms: its own pair, its own
     * once-only log, and independent of this function's return value for the same
     * reason. 340 x 535 at the page's own scale is ~364 KB an image, so the pair is
     * ~728 KB -- the largest of the four caches, and still the smallest thing here: a
     * machine that cannot spare it takes menu_fxlist_repaint()'s direct-paint route and
     * the picker still works. */
    if (!g.fxlist_img[0]) {
        static int fxlist_alloc_logged;
        size_t fn = (size_t)fxlist_paint_w(g.view.dw)
                    * (size_t)fxlist_paint_h(g.view.dh)
                    * (size_t)(g.view.bpp / 8);
        void *a, *b;

        g.fxlist_buf_w     = fxlist_paint_w(g.view.dw);
        g.fxlist_buf_h     = fxlist_paint_h(g.view.dh);
        g.fxlist_buf_bpp   = g.view.bpp;
        g.fxlist_img_bytes = fn;

        if (g.fxlist_img[1]) {
            free(g.fxlist_img[1]);   /* half a pair from a failed attempt */
            g.fxlist_img[1] = NULL;
        }
        a = malloc(fn);
        b = malloc(fn);
        if (!a || !b) {
            free(a);
            free(b);
            if (!fxlist_alloc_logged) {
                fxlist_alloc_logged = 1;
                pointsrc_log("fxlist: no %zu-byte picker image (%s); the picker paints"
                             " straight onto its plane", fn * 2, strerror(errno));
            }
        } else {
            g.fxlist_img[0] = a;
            g.fxlist_img[1] = b;
        }
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
    /* `side_prev` is "was any drawer out at the last sync". A zeroed struct would
     * claim one had been, and the tick's first pass would try to give the band a
     * plane it had never lost. The per-drawer `side_painted_which` is -1 and not 0
     * for a different reason: 0 is SZ_LEFT, and these are "which drawer is this
     * buffer's image for". */
    g.side_prev = -1;
    g.side_painted_which[SZ_LEFT] = g.side_painted_which[SZ_RIGHT] = -1;
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
 * The points come from menu_paint.c's menu_witness_point(), and the 2026-10-06
 * restyle moved every one of them -- which is exactly the change that comment warns
 * about, so it is worth saying what they are now and why. Fourteen are the BUTTON
 * OUTLINES: each of the seven buttons' top edge row and bottom edge row, at that
 * button's own centre x. The fifteenth is the BED, in the padding between two
 * buttons. All of them miss the labels' ink, which is what lets the comparison below
 * be against an exact palette value: menu_class_at() at a glyph-free point is the
 * whole truth about that pixel. test_menu.c asserts that property, against this same
 * list, at ten panel sizes.
 *
 * The mix matters, and it matters MORE now that the bed is black. A witness that
 * sampled only the bed could be fooled for the life of the session by rbp happening
 * to draw black there -- the failure would be silent and permanent, because "intact"
 * means "do not paint" -- and black is a colour rbp's static UI is full of, where the
 * old near-black blue-grey was not. That is why the bulk of the points is the white
 * outline: a false "intact" now needs rbp to hold our exact light border colour at
 * fourteen separate points spread across the whole band. The single bed point can
 * only ever ADD a "damaged", never hide one, since one matching point cannot carry a
 * vote on its own. A false "damaged" costs one full pass, which is what the tick
 * after an rbp frame does anyway. */
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

    /* THE PAD'S PLANE FIRST, before any route is decided, and it is the only one here
     * that goes first. The five boxes below each give their predecessor the chance to
     * release what it holds before returning early; this one cannot, because what it
     * holds is a rectangle sitting on the operator's own BPM readout and its right to be
     * there expires with a FINGER, not with a box. If the sync were reached only on the
     * path that no drawer is out, a finger that lifted while a drawer was open -- which
     * the pad's own gate makes unlikely but which nothing in this file can forbid --
     * would leave the dot frozen over the panel until something else
     * happened. Here, one call releases it on the first tick after the finger goes,
     * whatever else the tick is about to do. An idle pad costs this one comparison. */
    menu_fxpad_plane_sync();

    /* The window next, and it DECIDES THE ROUTE: while it is open it owns the
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

    /* Then the drawers, and they decide the route the same way the window does: while
     * one is out it owns the plane (or, on a machine that refused it, paints the page
     * itself), and the band's path below is not reached. The window's check above is
     * what keeps the two from fighting for the slot -- the funnel refuses to arm a
     * drawer while the window is open (pointsrc.c), so a drawer cannot be out here
     * unless the window is shut. */
    menu_side_plane_sync();
    if (side_any_open()) {
        pthread_mutex_unlock(&g_lock);
        return;
    }

    /* Then the chooser's box, and it decides the route a third time. It is raised from
     * the band's own seventh column, so the band is already shut when it opens -- but
     * the two DO overlap on the glass (the box is centred and the band is at the top
     * edge, and both are opaque planes), so the rule is stated and kept: while the box
     * is up the band's whole path below -- present, witness, blit -- is not reached at
     * all, exactly as the window's and the drawers' checks above do it.
     *
     * The box is the LAST of the three because it is the newest and the smallest: each
     * check above gives its predecessor the chance to release what it holds, and the
     * box holds nothing anyone else wants. */
    menu_prompt_plane_sync();
    if (prompt_is_open()) {
        /* And the band's plane goes with it, because the check above returns before the
         * `!open` branch below that would normally hide it. In practice the band is
         * already shut by the time the box opens -- the tap that raises the box is the
         * same release that closes the panel -- but "in practice" is not a reason to
         * leave an opaque plane on the glass over a box the operator is being asked to
         * read, and the two rects are far enough apart that a stale band would be visible
         * rather than merely wrong. `painted_open` is deliberately NOT cleared: the band's
         * own state is the band's business, and a band that is somehow still open resumes
         * cleanly when the box goes. */
        if (g.band_on) {
            drm_band_hide(&g.band);
            g.band_on = 0;
        }
        pthread_mutex_unlock(&g_lock);
        return;
    }

    /* Then the picker, the fifth and last holder, and the same shape a fourth time. It
     * is raised from rbp's own BEAT FX panel rather than from the band, so the band is
     * not necessarily shut when it opens -- hence the hide below, which the chooser's
     * block above also needs and for the same reason: the picker is centred and opaque
     * and the band is at the top edge and opaque, so both on the glass at once would
     * show one through the other.
     *
     * THE CHOOSER AND THE PICKER CANNOT BOTH BE UP: pointsrc.c closes the picker on the
     * keycode path that raises the chooser, and the funnel's ladder asks the chooser
     * before the picker so the chooser's own feed swallows a tap that would raise the
     * picker. So this is reached only with the chooser shut. */
    menu_fxlist_plane_sync();
    if (fxlist_is_open()) {
        if (g.band_on) {
            drm_band_hide(&g.band);
            g.band_on = 0;
        }
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
        /* Then the drawers' images, off the lock the same way and for the same reason,
         * and BEFORE the tick so the tick publishes the image this iteration just made.
         * A drawer that is shut returns immediately. */
        menu_side_build(SZ_LEFT);
        menu_side_build(SZ_RIGHT);
        /* Then the chooser's box, on the same terms and in the same place: built off
         * the lock, before the tick, so the tick publishes what this iteration just
         * made. A shut box returns immediately. It is built here and not in the touch
         * thread because its picture depends on rbp's answer about the two devices
         * (pointsrc_usb_state()), which is a read this thread already knows how to make
         * safely, and because the touch thread must stay the one that OWNS the gesture
         * -- the box is the only surface in this shim with two writers otherwise. */
        menu_prompt_build();
        /* Then the picker, on the same terms and in the same place. It has no rbp read
         * behind it -- the ONE value that makes its picture is the row under the finger
         * -- but it is built here for the same reason the chooser is: the tick publishes
         * what this iteration just made, and a build left to the tick would be a build
         * on the lock the tick is holding. A shut picker returns immediately. */
        menu_fxlist_build();
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
