/*
 * test_menu_dev.c -- menu_draw.c's device logic, with no framebuffer and no rbp.
 *
 * menu_draw.c is the half of the top menu that talks to the kernel and to rbp's frame
 * boundary: it opens /dev/fb0, measures the real geometry, mmaps it, and then, from
 * TWO callers at once -- its own thread and fb_shim.c's FBIO_WAITFORVSYNC interposer,
 * i.e. rbp's SCHED_FIFO render thread -- writes a 1280x56 image into a page rbp is
 * painting underneath. Until now none of that was covered: test_menu.c pins the
 * gesture, the geometry and the image, which are pure work against a local array, and
 * stops exactly where the device begins.
 *
 * THE SEAM IS THE SYSCALL BOUNDARY, test_cursor_dev.c's design and test_evdev.c's
 * before it. The PRODUCTION menu_draw.o is linked unmodified (-Wl,--wrap=syscall) and
 * what is faked is what the kernel would have said: the open of FB_DEV, the two
 * FBIOGET_* ioctls, and the mmap of the page. syscalls.h's real_* helpers are
 * `static inline` around syscall(), so wrapping the public symbol redirects every one
 * of them, and glibc's own internal syscalls do not go through it -- measured in
 * test_evdev.c, not assumed here -- so this fake sees the module's calls and nobody
 * else's.
 *
 * THE THREAD IS DRIVEN, NOT RACED, and that is the one piece of invention here.
 * menu_thread() is static and loops forever. Its iteration is
 *
 *     classify ; tick ; usleep(period)
 *
 * and the usleep is the LAST thing an iteration does, so __wrap_usleep() is a
 * handshake: it reports "parked", then blocks until the main thread grants one more
 * iteration. The main thread therefore has two states to work in -- the thread is
 * parked, and the thread is running -- and every assertion below is a statement about
 * what the module DID, not about who won a race. The alternative, ticking from the
 * main thread while the witness thread runs free, would make every claim here a claim
 * about timing, and the claim this file exists for is not a timing claim.
 *
 * WHAT IT PINS, and each of these is a comment in menu_draw.c that was only ever
 * argued:
 *
 *   1. THE PREWARM. The image is classified at map time, on the witness thread, before
 *      `ok` publishes the mapping -- so the panel's first appearance is a blit and not
 *      a 7.4 ms classify. Checked by opening the panel and having a VSYNC tick put the
 *      whole correct image on the page with the witness thread never having run since
 *      the open.
 *   2. THE VSYNC HOOK NEVER CLASSIFIES. A press transition made between two ticks
 *      leaves the page holding the PUBLISHED image -- the previous highlight -- after
 *      a tick from the main thread. That one tick of lag is the design (menu_draw.c's
 *      struct menu_map); what is being ruled out is the old behaviour, where the hook
 *      held the lock through a whole classification and the panel was absent for it.
 *   3. THE INCREMENTAL REBUILD IS EXACT. After the witness thread has taken the
 *      transition, the page matches the image for the new button PIXEL FOR PIXEL --
 *      over the whole panel, at every step of a slide across all seven buttons and back.
 *      This is where a missing copy of the front buffer, a wrong column mask or an
 *      off-by-one in a column rect would show: the columns that were not repainted
 *      would hold pixels of an image two presses old, and nothing on the unit would
 *      say so.
 *   4. CLOSED IS FREE. With the panel closed a tick writes nothing to the page, and
 *      rbp's own pixels survive it untouched.
 *
 * The page is compared against images built by the PRODUCTION painter
 * (menu_paint_cols()), not against a copy of the palette, so a palette change cannot
 * make this file pass against an image the module does not draw.
 *
 * Build + run: `make test` (see the Makefile's test_menu_dev rule).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "drmband.h"          /* struct drm_band: the plane the fake hands back */
#include "fbdev.h"
#include "menu_draw.h"
#include "menu_paint.h"
#include "menu_zone.h"
#include "pointsrc.h"
#include "prompt_paint.h"     /* the box's size and its painter, for the assertions */
#include "rbp_vu.h"           /* g_fader: what the drawer's handle is drawn at */
#include "side_paint.h"       /* the drawer's pixels, for the plane assertion */
#include "side_zone.h"        /* the drawer's gesture, driven below */

/* ---------------------------------------------------------------------------
 * Assertions and the log.
 * ------------------------------------------------------------------------- */

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

/* pointsrc_log() goes to a buffer instead of /tmp/pointsrc.log. The module logs the
 * gate line, the panel's open, and -- with MENU_VERBOSE=1 -- every pass it makes,
 * which is the only way to see from outside whether a transition was rebuilt whole or
 * by columns. */
#define LOG_MAX 8192
static char logbuf[LOG_MAX];
static size_t loglen;

void pointsrc_log(const char *fmt, ...)
{
    va_list ap;
    int n;

    if (loglen + 2 >= LOG_MAX)
        return;
    va_start(ap, fmt);
    n = vsnprintf(logbuf + loglen, LOG_MAX - loglen, fmt, ap);
    va_end(ap);
    if (n > 0)
        loglen += (size_t)n;
    if (loglen + 1 < LOG_MAX) {
        logbuf[loglen++] = '\n';
        logbuf[loglen] = 0;
    }
}

static int log_has(const char *s)
{
    return strstr(logbuf, s) != NULL;
}

/* What rbp would say about the two USB devices. The real one walks rbp's memory
 * (pointsrc.c's pointsrc_usb_state()); here it is a pair the scenario sets, which is
 * the whole point of the box being pure -- the dead row and the live row can be driven
 * without a player, a panel or a USB stick. Default: both live, and NEITHER NAMED -- the
 * designated initializer is here so the labels, which are what usb-watch.sh would have
 * written to /tmp/udev_usbN.label, stay empty and the buttons keep their numbers
 * (prompt_zone.h). A scenario that wants a named button fills label[] itself. */
static struct prompt_state usb_fake = { .live = { 1, 1 } };

int pointsrc_usb_state(struct prompt_state *S)
{
    *S = usb_fake;
    return 1;
}

/* A fresh window, so a check can be about the lines one action produced rather than
 * about everything the module has ever said. Used where the claim is a negative --
 * "this tick did not classify" -- which a line from an earlier scenario would
 * otherwise make untestable. */
static void log_reset(void)
{
    loglen = 0;
    logbuf[0] = 0;
}

/* ---------------------------------------------------------------------------
 * The faked framebuffer.
 * ------------------------------------------------------------------------- */

#define FB_W      1280
#define FB_H       800
#define FB_BPP      16
#define FB_PITCH   FB_W
#define FAKE_FD      42

/* THE PAGE'S STRIDE IS A PARAMETER, and it has to be, because at the identity
 * geometry the two strides the module juggles are the same number. menu_cache_view()
 * gives the image a stride of dw (the picture's width); menu_blit() copies it onto the
 * page a row at a time unless `pitch == dw && bx == 0`. On a 1280x800 panel the picture
 * IS the page, so both strides are 1280 and the contiguous shortcut is taken -- which
 * means a defect that confused the two would be invisible, and was: sabotaging
 * menu_cache_view() to use the PAGE's stride instead of the picture's passed this test
 * until `pad` existed. `make test` therefore runs this binary twice, once with pad 0
 * and once with a padded page, so the strided blit path is executed rather than argued.
 *
 * Padding is 16-bit pixels of a wider row, which is what a driver with an aligned
 * stride does; the panel is then no longer one contiguous run of the page. */
static int pad;                      /* extra pixels per page row; argv[1] */
static int pg_pitch;                 /* FB_W + pad, the page's stride in pixels */
static long pg_bytes;                /* what fix.smem_len reports */

/* What rbp's own frame looks like to the witness: a value that is not any palette
 * entry, so "this pixel still holds it" is unambiguous. */
#define PAGE_SENTINEL 0x1234

static unsigned short *page;         /* the module's view of the page */
static const char *fb_path = "/dev/fb0";
static int open_calls, ioctl_calls, mmap_calls;
static int fail_open_once;           /* see scenario_open_and_prewarm() */
static int var_xres = FB_W, var_yres = FB_H, var_bpp = FB_BPP;

/* The linker's --wrap gives every intercepted call this shape: the wrapper, and the
 * real one under __real_*. Declared here because it has no header -- the same
 * declaration test_cursor_dev.c carries. */
extern long __real_syscall(long number, ...);

/* Everything on the PAGE goes through pg_pitch; everything on the panel IMAGE goes
 * through FB_PITCH (menu_cache_view() makes that the picture's width). Keeping the two
 * named apart is the whole point of the padded run. */
static unsigned short px(int x, int y)
{
    return page[(size_t)y * pg_pitch + x];
}

static void fill_page(unsigned short v)
{
    size_t i, n = (size_t)pg_pitch * FB_H;

    for (i = 0; i < n; i++)
        page[i] = v;
}

long __wrap_syscall(long number, ...)
{
    va_list ap;
    long r;

    va_start(ap, number);
    switch (number) {
    case SYS_openat: {
        int dirfd = va_arg(ap, int);
        const char *path = va_arg(ap, const char *);
        int flags = va_arg(ap, int);
        unsigned int mode = va_arg(ap, unsigned int);

        if (path != NULL && strcmp(path, fb_path) == 0) {
            open_calls++;
            if (fail_open_once) {
                fail_open_once = 0;
                errno = ENODEV;      /* the retry path, which the unit needs for a
                                      * monitor that arrives after boot */
                r = -1;
            } else {
                r = FAKE_FD;
            }
        } else {
            va_end(ap);
            return __real_syscall(number, dirfd, path, flags, mode);
        }
        break;
    }
    case SYS_ioctl: {
        int fd = va_arg(ap, int);
        unsigned long req = va_arg(ap, unsigned long);
        void *arg = va_arg(ap, void *);

        if (fd != FAKE_FD) {
            va_end(ap);
            return __real_syscall(number, fd, req, arg);
        }
        ioctl_calls++;
        if (req == FBIOGET_VSCREENINFO) {
            struct fb_var_screeninfo *v = arg;

            memset(v, 0, sizeof *v);
            v->xres = v->xres_virtual = (unsigned int)var_xres;
            v->yres = v->yres_virtual = (unsigned int)var_yres;
            v->bits_per_pixel = (unsigned int)var_bpp;
            r = 0;
        } else if (req == FBIOGET_FSCREENINFO) {
            struct fb_fix_screeninfo *f = arg;

            memset(f, 0, sizeof *f);
            memcpy(f->id, "fakefb", 6);
            /* A stride wider than the visible width, which is the padded case. */
            f->line_length = (unsigned int)(pg_pitch * (FB_BPP / 8));
            f->smem_len = (unsigned int)pg_bytes;
            r = 0;
        } else {
            errno = EINVAL;
            r = -1;
        }
        break;
    }
#if defined(SYS_mmap2)
    case SYS_mmap2:
#else
    case SYS_mmap:
#endif
    {
        void *addr = va_arg(ap, void *);
        size_t len = va_arg(ap, size_t);
        int prot = va_arg(ap, int);
        int flags = va_arg(ap, int);
        int fd = va_arg(ap, int);
        long off = va_arg(ap, long);

        (void)addr; (void)prot; (void)flags; (void)off;
        if (fd != FAKE_FD) {
            printf("test_menu_dev: a real mmap on fd %d leaked through\n", fd);
            r = -ENOMEM;
            break;
        }
        mmap_calls++;
        if ((long)len > pg_bytes) {
            printf("test_menu_dev: the module asked for %ld bytes of page\n", (long)len);
            r = -ENOMEM;
            break;
        }
        r = (long)(intptr_t)page;
        break;
    }
    case SYS_close: {
        int fd = va_arg(ap, int);

        if (fd != FAKE_FD) {
            va_end(ap);
            return __real_syscall(number, fd);
        }
        r = 0;
        break;
    }
    default:
        va_end(ap);
        return __real_syscall(number);
    }
    va_end(ap);
    return r;
}

/* ---------------------------------------------------------------------------
 * The prewarm's claim, asked where it is actually decided.
 *
 * The image buffers are allocated by the first menu_build(). WITH the prewarm that
 * happens inside menu_fb_open_unlocked() and BEFORE `ok` publishes the mapping, so a
 * tick arriving at that instant finds no framebuffer yet and returns untouched.
 * WITHOUT it, the same allocation happens one iteration later: `ok` already set, and
 * the panel possibly open -- and that tick lands in the degraded `!front_valid`
 * branch, which paints straight onto the page with a whole-panel classification on
 * whichever thread arrived. That is the exact defect the prewarm removes, and it is
 * 7.4 ms on rbp's render thread.
 *
 * There is no way to observe that window from outside the module -- it opens and
 * closes between two parks of the witness thread -- so the question is asked from
 * inside it: the first image allocation calls the tick itself, which is the hook's
 * call with the hook's lock in the hook's place. And --wrap=malloc, like
 * --wrap=syscall, redirects the module's calls and not glibc's.
 *
 * WHAT THIS PINS IS THE ORDER, and the limit is worth stating rather than papering
 * over. The prewarm is protected twice -- built before `ok` is set, and built under
 * g_lock -- and this injection cannot separate them, because it fires before `ok` is
 * set and menu_fb_open_unlocked() makes no interposable call after it (so there is no
 * later point to inject at). Two sabotages confirm the limit: moving `ok = 1` to
 * before the publish still passes, and taking the lock OUT of menu_fb_open() still
 * passes, both because the tick's `!g.ok` check catches it first. What is pinned is
 * the ordering, and the ordering is what the defect needs: the lock's contribution is
 * that the view fields are never half-written, which `!g.ok` already covers in the
 * shipped order.
 * ------------------------------------------------------------------------- */

static int alloc_watch;              /* armed only across the open below */
static int alloc_seen;
static int alloc_tick_wrote;         /* what the tick did while the image was allocated */

extern void *__real_malloc(size_t n);

void *__wrap_malloc(size_t n)
{
    void *p = __real_malloc(n);

    if (!p || !alloc_watch || alloc_seen)
        return p;
    alloc_seen = 1;
    menu_frame_tick();
    alloc_tick_wrote = (px(640, 55) != PAGE_SENTINEL);
    return p;
}

/* ---------------------------------------------------------------------------
 * The thread handshake.
 * ------------------------------------------------------------------------- */

static pthread_mutex_t hs_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  hs_cv = PTHREAD_COND_INITIALIZER;
static int hs_parked, hs_go, hs_stop;

int __wrap_usleep(useconds_t usec)
{
    (void)usec;
    pthread_mutex_lock(&hs_lock);
    hs_parked = 1;
    pthread_cond_broadcast(&hs_cv);
    while (!hs_go && !hs_stop)
        pthread_cond_wait(&hs_cv, &hs_lock);
    hs_go = 0;
    pthread_mutex_unlock(&hs_lock);
    if (hs_stop)
        pthread_exit(NULL);
    return 0;
}

/* Let the witness thread run exactly one more iteration, and return with it parked
 * again. Between two calls of this the module is quiet and the main thread owns
 * everything it can see. */
static void run_tick(void)
{
    pthread_mutex_lock(&hs_lock);
    while (!hs_parked)
        pthread_cond_wait(&hs_cv, &hs_lock);
    hs_parked = 0;
    hs_go = 1;
    pthread_cond_broadcast(&hs_cv);
    while (!hs_parked)
        pthread_cond_wait(&hs_cv, &hs_lock);
    pthread_mutex_unlock(&hs_lock);
}

/* ---------------------------------------------------------------------------
 * The drawer's plane, SCRIPTED -- and this is the one place in this file where
 * drmband.o is not left to be the real object.
 *
 * WHY THE FAKE, IN ONE PARAGRAPH. Every other scenario here runs on the FALLBACK
 * route on purpose: with no /dev/dri/card1, drm_band_setup() fails exactly as it
 * fails on a unit whose plane cannot be had, so what the page assertions pin is the
 * route this shipped with. But the drawer's two riskiest claims cannot be seen from
 * there at all. `menu_side_plane_sync()`'s handover -- the band's plane torn down
 * BEFORE the drawer asks for the one master a process may hold -- is skipped
 * entirely while `g.band_ok` is 0, and the repaint gate is bypassed while
 * `g.side_ok` is 0, because with no plane the drawer is painted onto the page every
 * tick. `side_paint()` is then called once per tick whatever the gate does, so a
 * gate that had degenerated into "always" would be invisible. Both claims need a
 * plane that exists, and the only way to have one on a host is to hand the module a
 * struct that says so.
 *
 * SO THE WRAPPERS ARE ARMED, NOT ALWAYS ON. `plane_fake` is off for the whole of
 * the suite below, so `__wrap_drm_band_setup()` delegates to the real call and the
 * fallback route is untouched -- the block above still describes it. It is turned on
 * once, in scenario_drawer(), which is the last scenario, and nothing after it
 * exists to be disturbed. When it is off the other three wrappers delegate too;
 * they are never even reached, because an unset `band_ok`/`side_ok` guards every
 * show/hide/teardown in menu_draw.c.
 *
 * WHAT "THE BAND'S" MEANS HERE. The wrapper is handed a `struct drm_band *` and a
 * size, and nothing else distinguishes the surfaces -- all four are this same struct,
 * all four take their buffer from this fake. The SIZE does. The band asks for the full
 * picture WIDTH and menu_panel_ph() rows; a drawer asks for SZ_W (180) and the full
 * picture height; the chooser's box asks for prompt_paint_w() x prompt_paint_h(), which
 * is neither. So the classification below is by both dimensions and the box is the
 * else-arm of it -- and that is asserted rather than trusted: the first check of each
 * scenario is that the surface it is about is the one it thinks it is.
 * ------------------------------------------------------------------------- */

/* Both surfaces are handed this one buffer, and its size is the PAGE's, not the
 * drawer's, because the band is not a drawer: drm_band_setup() is asked for the
 * band at the full picture WIDTH and menu_panel_ph() rows (56), for the drawer at
 * SZ_W (180) and the full picture height, and for the chooser's box at
 * prompt_paint_w() x prompt_paint_h(). One buffer has to host all of them. */
#define PLANE_BUF_W  FB_W
#define PLANE_BUF_H  FB_H

static unsigned short plane_buf[PLANE_BUF_W * PLANE_BUF_H];

/* ...and a second one, for the stride the UNIT actually has. drm_band_setup() is asked
 * for a drawer 180 px wide, and the dumb buffer it maps has a 180 px pitch -- so
 * `g.side[i].pitch == side_buf_w` and menu_side_publish() takes its single-memcpy fast
 * path. The page-width buffer above takes the per-row loop instead, so without this
 * second run that memcpy would ship untested. Same size, same zeroing, only the stride
 * differs.

 * IT IS ALSO the prompt's fast path, and for the same reason: the box is 560 px wide
 * and a box-sized dumb buffer has a 560 px pitch, so menu_prompt_publish() takes its
 * one-memcpy arm. `plane_alt` is what makes that arm reachable. */
static unsigned short plane_alt[PLANE_BUF_W * PLANE_BUF_H];

static int plane_pitch_px = PLANE_BUF_W;  /* the stride the fake plane reports */
static int plane_use_alt;                 /* ...and which of the two buffers it is */

static int plane_fake;            /* armed only inside scenario_drawer() and its sequel */
static int plane_step;            /* a tag shared by every wrapped call, for order */
/* A machine that has room for the band and the drawers but refuses the chooser's box:
 * armed only by scenario_prompt()'s page-route half, to reach `prompt_fail`. */
static int plane_refuse_prompt;

/* WHICH SURFACE. A `struct drm_band` is handed to the wrapper with a size and nothing
 * else, so the size IS the identity -- see the block above. All four are counted
 * separately rather than lumped, because "the box got a plane" and "the band got its
 * plane back" are different claims and a shared counter could not tell them apart. */
enum { SURF_BAND = 0, SURF_SIDE, SURF_PROMPT };
static int surface_of(int w, int h)
{
    if (h == FB_H && w == SZ_W)
        return SURF_SIDE;
    if (w == FB_W)
        return SURF_BAND;
    return SURF_PROMPT;
}

static int setup_n[3], teardown_n[3];
static int show_n, hide_n;
static int last_show_x, last_show_y;   /* where the plane was last put, in fb px */
static int seq_band_teardown, seq_side_setup;

/* side_paint() is wrapped for one reason: the gate's claim is a COUNT, and "at
 * least one paint" is exactly what a broken gate produces. See scenario_drawer().
 * prompt_paint() is wrapped for the same claim about the box, in scenario_prompt(). */
static int side_paints;
static int side_paints_left, side_paints_right;
static int side_last_which = -1;
static int prompt_paints;

extern int  __real_drm_band_setup(struct drm_band *b, int w, int h, int bpp);
extern int  __real_drm_band_show(struct drm_band *b, int x, int y);
extern void __real_drm_band_hide(struct drm_band *b);
extern void __real_drm_band_teardown(struct drm_band *b);
extern void __real_side_paint(const struct menu_view *v, int side,
                              int pressed_hit, int fader_v);
extern void __real_prompt_paint(const struct menu_view *v,
                                const struct prompt_state *S, int pressed_cell,
                                int selected_cell);

int __wrap_drm_band_setup(struct drm_band *b, int w, int h, int bpp)
{
    if (!plane_fake)
        return __real_drm_band_setup(b, w, h, bpp);
    if (w > PLANE_BUF_W || h > PLANE_BUF_H || bpp != FB_BPP) {
        printf("test_menu_dev: the fake plane was asked for %dx%d %d bpp\n",
               w, h, bpp);
        return -1;
    }
    if (plane_refuse_prompt && surface_of(w, h) == SURF_PROMPT)
        return -1;
    memset(b, 0, sizeof *b);
    b->fd      = -1;              /* never used: show/hide/teardown are wrapped too */
    b->pix     = plane_use_alt ? plane_alt : plane_buf;
    b->pitch   = plane_pitch_px;  /* pixels per row, as struct menu_view counts it */
    b->map_len = sizeof plane_buf;
    b->w = w;
    b->h = h;
    b->bpp = bpp;
    plane_step++;
    setup_n[surface_of(w, h)]++;
    if (surface_of(w, h) == SURF_SIDE)
        seq_side_setup = plane_step;
    return 0;
}

int __wrap_drm_band_show(struct drm_band *b, int x, int y)
{
    if (!plane_fake)
        return __real_drm_band_show(b, x, y);
    plane_step++;
    show_n++;
    last_show_x = x;
    last_show_y = y;
    b->on = 1;
    b->on_x = x;
    b->on_y = y;
    return 0;
}

void __wrap_drm_band_hide(struct drm_band *b)
{
    if (!plane_fake) {
        __real_drm_band_hide(b);
        return;
    }
    plane_step++;
    hide_n++;
    b->on = 0;
}

void __wrap_drm_band_teardown(struct drm_band *b)
{
    if (!plane_fake) {
        __real_drm_band_teardown(b);
        return;
    }
    plane_step++;
    teardown_n[surface_of(b->w, b->h)]++;
    if (surface_of(b->w, b->h) == SURF_BAND)
        seq_band_teardown = plane_step;
    b->pix = NULL;
    b->on  = 0;
}

void __wrap_side_paint(const struct menu_view *v, int side, int pressed_hit,
                       int fader_v)
{
    side_paints++;
    if (side == SZ_RIGHT)
        side_paints_right++;
    else
        side_paints_left++;
    side_last_which = side;
    __real_side_paint(v, side, pressed_hit, fader_v);
}

void __wrap_prompt_paint(const struct menu_view *v, const struct prompt_state *S,
                         int pressed_cell, int flash_cell)
{
    prompt_paints++;
    __real_prompt_paint(v, S, pressed_cell, flash_cell);
}

/* ---------------------------------------------------------------------------
 * Driving the drawer, in the funnel's own terms.
 *
 * `side_feed_any()` says which control a swallowed report was over and what the
 * caller must do about it; pointsrc.c is the caller that turns that into a
 * K_FADER/OP_VALUE send and a write to the shared g_fader. This test is standing in
 * for pointsrc.c here, and it applies the documented half of that contract -- the
 * fader write -- because the drawer's handle is DRAWN at g_fader[ch] and the plane
 * assertion below is about the pixels. The key sends (CUE/PLAY) are not modelled:
 * nothing about the plane depends on them.
 *
 * This is not a convenience. It is what makes the gate's claim honest: the value the
 * gate compares has to arrive the way the shipped one does, through the gesture, or
 * the test would be poking g_fader and then congratulating itself. */
static void side_drive(int down, int x, int y)
{
    int which = -1, act = SZ_ACT_NONE, v = 0;

    (void)side_feed_any(SZ_PTR_MAIN, down, x, y, &which, &act, &v);
    if (act == SZ_ACT_FADER && (which == SZ_LEFT || which == SZ_RIGHT)) {
        int ch = side_channel(which);

        g_fader[ch] = v;
        g_fader_seen[ch] = 1;
    }
}

/* ---------------------------------------------------------------------------
 * The reference images, and the comparison.
 *
 * Built by the production painter, once each, so what the page is compared against is
 * what the module would draw -- not a copy of the palette that could drift.
 * ------------------------------------------------------------------------- */

/* One image per `pressed`: 0 (no cell) and every column. Sized off the column count
 * rather than written as a literal, so a slide cannot index past the end of this. */
static unsigned short ref[MZ_COLS + 1][FB_PITCH * MZ_PANEL_H];
static int ref_built = -1;

static struct menu_view ref_view(void)
{
    struct menu_view v;

    memset(&v, 0, sizeof v);
    v.pix = ref[0];
    v.pitch = FB_PITCH;
    v.fb_w = FB_W;
    v.fb_h = FB_H;
    v.bpp = FB_BPP;
    v.bx = 0;
    v.by = 0;
    v.dw = FB_W;
    v.dh = FB_H;
    return v;
}

static const unsigned short *ref_image(int pressed)
{
    struct menu_view v = ref_view();

    if (ref_built != pressed) {
        v.pix = ref[pressed];
        memset(ref[pressed], 0, sizeof ref[pressed]);
        menu_paint_cols(&v, pressed, (1u << MZ_COLS) - 1u);
        ref_built = pressed;
    }
    return ref[pressed];
}

/* One memcmp when the page is unpadded, a row at a time when it is not -- because a
 * padded page makes the panel a run per row and no longer one run. The comparison is
 * against an image built by the production painter at the picture's own stride, so it
 * is the same bytes either way; only the addressing differs, and that difference is
 * the thing the padded run exists to exercise. */
static int page_is_image(int pressed, const char *what, int line)
{
    const unsigned short *want = ref_image(pressed);
    long bad = 0;
    int x, y, firstx = -1, firsty = -1;
    unsigned int got = 0, wantv = 0;

    checks++;
    /* The fast path is only valid while the two strides agree; with padding it would
     * be comparing the page's row tails against the image's next row. */
    if (pg_pitch == FB_PITCH &&
        memcmp(page, want, (size_t)FB_PITCH * MZ_PANEL_H * sizeof page[0]) == 0)
        return 1;
    for (y = 0; y < MZ_PANEL_H; y++)
        for (x = 0; x < FB_W; x++) {
            unsigned short w = want[(size_t)y * FB_PITCH + x];
            unsigned short g = px(x, y);

            if (g != w) {
                bad++;
                if (firstx < 0) {
                    firstx = x;
                    firsty = y;
                    got = g;
                    wantv = w;
                }
            }
        }
    if (bad == 0)
        return 1;
    failures++;
    printf("FAIL %s:%d: %s: the page is not the image for pressed=%d"
           " (%ld of %d panel pixels wrong; first at %d,%d: %#06x, wanted %#06x)\n",
           __FILE__, line, what, pressed, bad, FB_W * MZ_PANEL_H,
           firstx, firsty, got, wantv);
    return 0;
}

/* Anything below the panel is rbp's page, not ours. A sentinel written there must
 * survive every tick. */
static int outside_untouched(const char *what, int line)
{
    int x, y, bad = 0;

    checks++;
    for (y = MZ_PANEL_H; y < FB_H; y++)
        for (x = 0; x < FB_W; x++)
            if (px(x, y) != PAGE_SENTINEL) {
                bad++;
                if (bad == 1)
                    printf("FAIL %s:%d: %s: wrote %#06x at %d,%d, below the panel\n",
                           __FILE__, line, what, px(x, y), x, y);
            }
    if (bad) {
        failures++;
        if (bad > 1)
            printf("FAIL %s:%d: %s: %d pixels below the panel were written\n",
                   __FILE__, line, what, bad);
    }
    return bad == 0;
}

#define PAGE_IS_IMAGE(p, w) page_is_image((p), (w), __LINE__)
#define OUTSIDE_CLEAN(w)    outside_untouched((w), __LINE__)

/* ---------------------------------------------------------------------------
 * Driving the gesture, in logical coordinates -- menu_zone.h's space.
 * ------------------------------------------------------------------------- */

#define BTN_MID(i) ((menu_button_x0(i) + menu_button_x1(i)) / 2)
/* The middle of a CELL as the gesture names them, which is now the middle of a column
 * for every cell there is. It stays a separate name from BTN_MID() because the gesture
 * numbers cells from 1 and the column geometry from 0, and mixing the two up slides to
 * the neighbouring button without failing anything else. */
#define CELL_MID(b) BTN_MID((b) - 1)

/* The rows the gesture is driven at, DERIVED rather than written as the literals
 * they were. They were BTN_ROW 55 and BELOW_ROW 107, and both of those describe the
 * 96-row button band this feature was built with: at 56 rows the band is 8..47, so a
 * finger at 55 is below it and reads as no button at all -- which is twenty-four
 * failures in one go, every one of them "the finger on button N reads as 0".
 *
 * BELOW_ROW has two jobs and they bound it from both sides, so both are stated
 * rather than left to the number that happens to satisfy them: it must be past the
 * panel's buttons (it is the border the dismissal press lands on), it must be at
 * least MZ_SWIPE_PX below STRIP_ROW (open_panel() swipes from there), and it must be
 * less than MZ_CLOSE_PX from BTN_ROW (slide_to(0) moves to it and must not dismiss).
 * The asserts are the point of the exercise: a future band change moves the number
 * and cannot silently invalidate the gesture. */
#define BTN_ROW    ((MZ_BTN_Y0 + MZ_BTN_Y1) / 2)   /* 27: inside 8..47 */
#define STRIP_ROW  10                              /* inside 0..55 */
#define BELOW_ROW  (MZ_BTN_Y1 + 23)                /* 70: past the buttons */

_Static_assert(MZ_BTN_Y0 <= BTN_ROW && BTN_ROW <= MZ_BTN_Y1,
               "BTN_ROW must be inside the button band");
_Static_assert(BELOW_ROW > MZ_BTN_Y1, "BELOW_ROW must be past the panel's buttons");
_Static_assert(STRIP_ROW + MZ_SWIPE_PX <= BELOW_ROW,
               "open_panel()'s swipe must travel far enough to open the panel");
_Static_assert(BELOW_ROW - BTN_ROW < MZ_CLOSE_PX,
               "slide_to(0) must not travel far enough to dismiss the panel");

static int feed(int down, int x, int y)
{
    int b = 0;

    (void)menu_feed(down, x, y, &b);
    return b;
}

/* Down in the strip, then down to the border below the button band -- 97 px, well
 * past MZ_SWIPE_PX and with no sideways travel -- which opens it. It lands on no
 * button, so the swipe leaves menu_pressed() at 0 and the image the prewarm builds
 * is the neutral one; a slide then has to make every highlight change explicitly.
 * The press stays DOWN afterwards, exactly as the operator's finger does, and that is
 * not a detail: see slide_to(). */
static void open_panel(void)
{
    menu_reset();
    feed(1, 640, STRIP_ROW);
    feed(1, 640, BELOW_ROW);
    CHECK(menu_is_open(), "the swipe did not open the panel");
    CHECK(menu_pressed() == 0, "the swipe landed on button %d", menu_pressed());
}

/* THE SLIDE IS ONE PRESS, and it has to be. A lift while the panel is open ends it
 * -- menu_zone.c's rule is that only a press which BEGAN with the panel out is a
 * press *on* the panel, and only that one dismisses it on release -- so a test that
 * released between buttons would be asserting against a closed panel. A finger
 * moving from one button to the next never lifts, so neither does this. The moves
 * are horizontal except for `b == 0`, which goes to the border below the band: 52 px
 * of travel from the button row, short of the 56 that MZ_CLOSE_PX dismisses on. */
static void slide_to(int b)
{
    if (b == 0)
        feed(1, BTN_MID(2), BELOW_ROW);
    else
        feed(1, CELL_MID(b), BTN_ROW);
    CHECK(menu_is_open(), "sliding to %d closed the panel", b);
    CHECK(menu_pressed() == b, "the finger on cell %d reads as %d", b, menu_pressed());
}

/* The operator's dismissal: a press that begins with the panel out, on the border
 * rather than on a button, and therefore fires nothing when it lifts. The leading
 * release ends whatever press is live -- the slide's, which began CLOSED, so the
 * lift leaves the panel standing (menu_zone.c's release branch). */
static void press_and_dismiss(void)
{
    feed(0, BTN_MID(2), BELOW_ROW);
    feed(1, BTN_MID(2), BELOW_ROW);
    CHECK(menu_pressed() == 0, "a finger over the border reads as button %d",
          menu_pressed());
    feed(0, BTN_MID(2), BELOW_ROW);
    CHECK(!menu_is_open(), "the lift did not close the panel");
}

/* The page, damaged the way rbp damages it: its own frame written over the whole
 * panel. A sentinel rather than a copy of rbp's UI, because what the witness asks is
 * "does this pixel hold the image's value", not "who wrote it". */
static void clobber(void)
{
    fill_page(PAGE_SENTINEL);
}

/* ---------------------------------------------------------------------------
 * The scenarios.
 * ------------------------------------------------------------------------- */

static void scenario_open_and_prewarm(void)
{
    /* The first open was made to fail in main(), BEFORE the thread existed -- and it
     * has to be armed there rather than here. Setting it here would be a race: the
     * thread menu_draw_start() creates opens the framebuffer immediately, and this
     * scenario would then depend on which thread got there first. It was written that
     * way once and `make test` caught it -- usually the thread lost, and one run in
     * several read open_calls == 1 and two spurious failures.
     *
     * With it armed first, the thread's very first open fails and it parks in its
     * retry loop -- the path the unit needs for a monitor that appears after boot, so
     * this covers that too -- leaving the mapping not yet made while the panel is
     * ALREADY open. The main thread therefore holds the one state it cannot otherwise
     * reach: the world as it is before the framebuffer exists. */
    open_panel();

    /* One iteration. The thread retries the open -- which succeeds now, and builds
     * the first image inside it, under g_lock and before `ok` -- then classifies and
     * ticks. The injected tick in the middle of that allocation is the claim. */
    alloc_seen = 0;
    alloc_tick_wrote = -1;
    alloc_watch = 1;
    run_tick();
    alloc_watch = 0;

    /* THE PREWARM. A tick that can see the framebuffer can already see a finished
     * image, because `ok` is published after the build. Without the prewarm this same
     * tick paints the whole panel straight onto the page -- the 7.4 ms classify on
     * rbp's render thread that the prewarm exists to remove. See the wrapper above
     * for exactly how much of that this measures. */
    CHECK(alloc_seen, "no image allocation was seen -- the injection never fired");
    CHECK(alloc_tick_wrote == 0,
          "a tick arriving while the image was being built WROTE to the page: the"
          " framebuffer was visible before a finished image existed");

    CHECK(open_calls == 2, "the framebuffer was opened %d times (the retry took two)",
          open_calls);
    CHECK(mmap_calls == 1, "the page was mapped %d times", mmap_calls);
    CHECK(!log_has("image cache"),
          "the module found no room for the image cache and fell back to painting"
          " straight onto the page");
    CHECK(log_has("drawing the panel"), "the module did not report drawing the panel");
    CHECK(log_has("no panel will be drawn yet"),
          "the failed first open was not reported");

    /* ...and the panel's first appearance is a blit of that image, put there by a
     * tick the witness thread never classified for. */
    PAGE_IS_IMAGE(0, "the panel's first image, from the prewarm");
    OUTSIDE_CLEAN("the panel's first image");
    CHECK(log_has("panel open, painted"), "the module did not log the open");
}

static void scenario_closed(void)
{
    /* With the panel closed nothing is written: the whole page -- panel rows included
     * -- keeps rbp's pixels through as many ticks as you like. */
    press_and_dismiss();
    clobber();
    log_reset();
    run_tick();
    run_tick();
    CHECK(px(4, 4) == PAGE_SENTINEL, "a tick while closed wrote at 4,4");
    CHECK(px(640, 55) == PAGE_SENTINEL, "a tick while closed wrote into the panel");
    CHECK(page[0] == PAGE_SENTINEL, "a tick while closed wrote at 0,0");
    CHECK(!log_has("classify"), "a tick while closed classified");
}

static void scenario_hook_does_not_classify(void)
{
    /* THE HIGHLIGHT LAGS BY A TICK, DELIBERATELY, AND THE HOOK NEVER CLASSIFIES.
     *
     * A finger arrives on button 3. No tick has run since, so the published image is
     * still the neutral one. The vsync hook's tick -- the main thread here -- must
     * put THAT image on the page and not the new one: the classify belongs to the
     * witness thread, and the hook's whole job is a bounded blit. Before the double
     * buffer the hook either did the classify itself, on rbp's render thread, or
     * failed its trylock and left the panel ABSENT for the 7 ms it took. */
    open_panel();                      /* the closed scenario dismissed it */
    slide_to(3);
    clobber();
    log_reset();
    menu_frame_tick();
    PAGE_IS_IMAGE(0, "the hook's tick after a press it has not classified for");
    OUTSIDE_CLEAN("the hook's tick after a press");
    /* The image above could in principle be right by accident -- rbp's page
     * happened to hold it. This is the claim itself: the hook ran no classify. */
    CHECK(!log_has("classify"), "the vsync hook classified the panel");

    /* ...and once the witness thread has taken the transition, the page is the new
     * image, whole -- rebuilt by columns, not reclassified. */
    log_reset();
    run_tick();
    PAGE_IS_IMAGE(3, "the witness tick after the press");
    CHECK(log_has("columns 0x4"),
          "the transition to button 3 was not rebuilt by column");
    CHECK(!log_has("whole panel for"), "button 3 was classified whole-panel");
}

static void scenario_slide(void)
{
    /* A SLIDE ACROSS ALL SEVEN BUTTONS AND BACK, the gesture the operator reported the
     * flicker for -- and the one where a wrong incremental rebuild has six chances
     * to leave a pixel of an older image on the page. Every step is compared against
     * the whole panel drawn fresh. The seven are walked in order and then in a
     * second, out-of-order pass (and the 0 between them is the border, which must
     * dismiss nothing), so the boundaries crossed are not all the same pair twice. */
    static const int order[] = { 1, 2, 3, 4, 5, 6, 7, 0, 7, 1, 6, 2, 0 };
    unsigned int i;

    /* The prewarm's own whole-panel classify is in the log from before this
     * scenario; the claim below is about the slide. */
    log_reset();

    for (i = 0; i < sizeof order / sizeof order[0]; i++) {
        char what[64];

        slide_to(order[i]);
        clobber();
        run_tick();
        snprintf(what, sizeof what, "after moving to %d", order[i]);
        PAGE_IS_IMAGE(order[i], what);
    }
    CHECK(menu_is_open(), "the slide closed the panel");

    /* Every one of the twelve transitions was incremental -- twelve column rebuilds
     * and no whole panel. The one transition that has nothing to carry over is the
     * prewarm's, and that is not in this window. */
    CHECK(!log_has("whole panel for"), "a slide transition was classified whole-panel;"
          " the mask is not doing its job");
    CHECK(log_has("columns 0x"), "no slide transition logged a column rebuild");
}

static void scenario_close_and_reopen(void)
{
    /* CLOSING DRAWS NOTHING, and reopening needs no rebuild either: the neutral image
     * is published before the next tick can see it, so the panel comes back whole. */
    press_and_dismiss();
    clobber();
    log_reset();
    run_tick();
    CHECK(px(640, 55) == 0x1234, "the tick after the close wrote into the panel");
    CHECK(log_has("panel closed"), "the module did not log the close");
    CHECK(!log_has("classify"), "the tick after the close classified");

    open_panel();
    clobber();
    menu_frame_tick();
    PAGE_IS_IMAGE(0, "the first tick after reopening");
    OUTSIDE_CLEAN("the first tick after reopening");
}

/* ---------------------------------------------------------------------------
 * The drawers, with a plane that exists.
 *
 * This is the last scenario and the only one that arms the fake plane, so the
 * fallback route everything above pins is untouched by it. What it settles:
 *
 *   - a drawer that has never been out costs the tick one int read and no plane;
 *   - the first open sets up the plane at the drawer's OWN width, paints into it
 *     ONCE and shows it once;
 *   - a static drawer does not repaint -- the exact count, not "at least one",
 *     because a gate that had degenerated to "always" would still pass an "at
 *     least" and would tear a single-buffered plane;
 *   - one fader step through the real gesture repaints exactly once, and a resting
 *     finger none;
 *   - the outward sweep on the drawer's own background closes it, gives the plane
 *     back, and the band's plane is restored ONCE across many ticks (the side_prev
 *     guard), not on every one;
 *   - the next open takes the band's plane DOWN BEFORE it asks for its own -- the
 *     one-master rule -- and that is in the log and in the call order, not merely in
 *     a comment;
 *   - the mirror holds: the right drawer is its own plane, its own pixels.
 * ------------------------------------------------------------------------- */

#define SZ_ENTRY_LX  10     /* inside the entry column, 0..SZ_ENTRY_W-1 */
/* The row the swipe is driven at, and it has ONE constraint beyond clearing
 * SZ_ENTRY_Y0 (56, the USB STOP gate): the finger comes to rest at
 * SZ_ENTRY_LX + SZ_SWIPE_PX, which is lx 66, and it must land on the panel's own
 * BACKGROUND rather than on a control -- lx 66 is inside the SYNC/nudge column
 * (16..163) for every row those cover, and inside the fader lane's x range (44..135)
 * for every row IT covers. It must also not be a control's row, or the opening swipe
 * would leave that control highlighted and the first frame painted would not be the
 * neutral one this test's reference image assumes. 122 is the gap between SYNC (ends
 * 115) and the nudge pair (starts 128); the other candidate gap, 200..205 between the
 * nudge pair and the lane, is only six rows wide. */
#define SZ_ENTRY_LY  122
/* The ONLY dismissal left: an outward sweep on the drawer's OWN background. The
 * press must begin on SZ_HIT_BG -- clear of the buttons' rows and right of the fader
 * lane -- and travel at least SZ_CLOSE_PX outward with a dominant axis, which for the
 * left drawer means toward smaller lx. A press anywhere else, and in particular one on
 * the glass rbp draws for itself, does nothing at all (side_zone.c). */
#define SZ_BG_LX     140    /* on the drawer's own background, right of the fader lane */
#define SZ_BG_LY     400    /* between the nudge pair (ends 199) and CUE (starts 628) */
#define SZ_FAD_LX    89     /* the middle of the fader track, 74..105 */
#define SZ_FAD_LY    500    /* inside the track and the lane, 206..608 */

_Static_assert(SZ_ENTRY_LX <= SZ_ENTRY_W - 1, "the entry probe must be in the column");
_Static_assert(SZ_ENTRY_LY >= SZ_ENTRY_Y0, "the entry probe must clear USB STOP's rows");
_Static_assert(SZ_ENTRY_LX + SZ_SWIPE_PX <= SZ_W, "the opening swipe must stay on the panel");
_Static_assert(SZ_ENTRY_LY > SZ_SYNC_Y1 && SZ_ENTRY_LY < SZ_NUDGE_Y0,
               "the swipe must come to rest between SYNC and the nudge pair, on no"
               " control, or the first frame painted is not the neutral one");
_Static_assert(SZ_ENTRY_LX + SZ_SWIPE_PX > SZ_BTN_X0 - 1 && SZ_ENTRY_LX < SZ_BTN_X0,
               "the swipe must land inside the button column, so this test would catch"
               " a landing that a control had grown over");
_Static_assert(SZ_BG_LX < SZ_W && SZ_BG_LX > SZ_FADER_GX1,
               "the dismissal probe must begin on the drawer's own background, clear of"
               " the fader lane -- a press in the lane is a drag and must not dismiss");
_Static_assert(SZ_BG_LY > SZ_NUDGE_Y1 && SZ_BG_LY < SZ_CUE_Y0,
               "the dismissal probe's row must be on no control, or the press would"
               " grab one instead of starting a sweep");
_Static_assert(SZ_FAD_LX >= SZ_FADER_GX0 && SZ_FAD_LX <= SZ_FADER_GX1,
               "the fader probe must be inside the grab lane");
_Static_assert(SZ_FAD_LY >= SZ_FADER_GY0 && SZ_FAD_LY <= SZ_FADER_GY1,
               "the fader probe must be on the lane");
_Static_assert(SZ_W <= PLANE_BUF_W && FB_H <= PLANE_BUF_H,
               "the fake plane's buffer must be at least the drawer's size");
_Static_assert(MZ_PANEL_H < FB_H, "the band's plane is the shorter one; see the "
               "wrappers' h == FB_H discriminator");

static void side_drive_local(int side, int down, int lx, int ly)
{
    side_drive(down, side_abs_x(side, lx), ly);
}

/* The plane holds exactly the image the shipped painter makes for this drawer, at
 * the plane's own size and stride. Built by that same painter, so it cannot drift
 * with the palette -- and compared over the whole buffer, which is safe because
 * side_paint() clips every write to dw x dh and the fake buffer starts zeroed. */
static int plane_is_drawer(int which, int hit, int v, const char *what, int line)
{
    static unsigned short want[PLANE_BUF_W * PLANE_BUF_H];
    struct menu_view sv;
    int pw = (SZ_W * FB_W) / MZ_LOGICAL_W;

    checks++;
    memset(want, 0, sizeof want);
    memset(&sv, 0, sizeof sv);
    sv.pix   = want;
    sv.pitch = plane_pitch_px;
    sv.fb_w  = plane_pitch_px;
    sv.fb_h  = PLANE_BUF_H;
    sv.bpp   = FB_BPP;
    sv.dw    = pw;
    sv.dh    = FB_H;
    __real_side_paint(&sv, which, hit, v);
    if (memcmp(plane_use_alt ? plane_alt : plane_buf, want, sizeof want) == 0)
        return 1;
    failures++;
    printf("FAIL %s:%d: %s: the plane is not the drawer's image"
           " (which=%d hit=%d v=%d)\n", __FILE__, line, what, which, hit, v);
    return 0;
}

#define PLANE_IS_DRAWER(w, h, v, what) plane_is_drawer((w), (h), (v), (what), __LINE__)

/* The PAGE route's version of the same claim: the drawer's strip is on the page, at the
 * page's own stride, and NOTHING ELSE is -- every pixel outside the strip must still
 * hold the sentinel the page was filled with. That second half is what makes this a
 * test of the placement and not just of the pixels: menu_side_publish()'s page arm
 * writes a run per row from an offset of its own arithmetic, and an off-by-one in it
 * would leave a column of sentinel inside the strip or a column of drawer beside it.
 * Both would be invisible to a comparison of the strip alone.
 *
 * The drawer's own image has a strip width of pw and a FULL picture height, because on
 * this route the panel is the whole picture's height; fb_bx is 0 at this geometry (the
 * plane test below says so too), which is why the strip starts at 0 for the left drawer. */
static int page_is_drawer(int which, int hit, int v, const char *what, int line)
{
    static unsigned short want[FB_PITCH * FB_H];
    static unsigned short strip[FB_W * FB_H];
    struct menu_view sv;
    int pw = (SZ_W * FB_W) / MZ_LOGICAL_W;
    int x0 = (which == SZ_LEFT) ? 0 : FB_W - pw;
    long bad = 0;
    int x, y, fx = -1, fy = -1;
    unsigned int got = 0, wantv = 0;

    checks++;
    memset(strip, 0, sizeof strip);
    memset(&sv, 0, sizeof sv);
    sv.pix   = strip;
    sv.pitch = pw;                /* the image is built tight, one drawer wide */
    sv.fb_w  = pw;
    sv.fb_h  = FB_H;
    sv.bpp   = FB_BPP;
    sv.dw    = pw;
    sv.dh    = FB_H;
    __real_side_paint(&sv, which, hit, v);

    memset(want, 0, sizeof want);
    for (y = 0; y < FB_H; y++)
        for (x = 0; x < FB_W; x++)
            want[(size_t)y * FB_PITCH + x] =
                (x >= x0 && x < x0 + pw) ? strip[(size_t)y * pw + (x - x0)]
                                         : PAGE_SENTINEL;

    for (y = 0; y < FB_H; y++)
        for (x = 0; x < FB_W; x++) {
            unsigned short w = want[(size_t)y * FB_PITCH + x];
            unsigned short g = px(x, y);

            if (g != w) {
                bad++;
                if (fx < 0) { fx = x; fy = y; got = g; wantv = w; }
            }
        }
    if (bad == 0)
        return 1;
    failures++;
    printf("FAIL %s:%d: %s: the page is not the %s drawer's strip"
           " (%ld of %d page pixels wrong; first at %d,%d: %#06x, wanted %#06x)\n",
           __FILE__, line, what, which == SZ_LEFT ? "left" : "right",
           bad, FB_W * FB_H, fx, fy, got, wantv);
    return 0;
}

#define PAGE_IS_DRAWER(w, h, v, what) page_is_drawer((w), (h), (v), (what), __LINE__)

/* A swipe inward from the entry column: down, far enough in, then the lift. The
 * LIFT IS NOT OPTIONAL -- a finger still down would make the next press of this
 * test read as a move, not a new press, and every subsequent assertion here would
 * be about a gesture that never started. */
static void side_open(int side)
{
    side_drive_local(side, 1, SZ_ENTRY_LX, SZ_ENTRY_LY);
    side_drive_local(side, 1, SZ_ENTRY_LX + SZ_SWIPE_PX + 4, SZ_ENTRY_LY);
    side_drive_local(side, 0, SZ_ENTRY_LX + SZ_SWIPE_PX + 4, SZ_ENTRY_LY);
}

/* The operator's dismissal, and now the ONLY one: a press on the drawer's own
 * BACKGROUND that sweeps outward past SZ_CLOSE_PX. It used to be a press off every
 * open panel -- the "tap away to close" -- and the operator reversed that ("the side
 * bar should stay up until i swipe them away"), so a press on the glass is no longer
 * this module's at all. The sweep closes on its MOTION edge, so the lift afterwards is
 * a courtesy that keeps this harness's latches clean. */
static void side_swipe_away(int side)
{
    side_drive_local(side, 1, SZ_BG_LX, SZ_BG_LY);
    side_drive_local(side, 1, SZ_BG_LX - SZ_CLOSE_PX - 4, SZ_BG_LY);
    side_drive_local(side, 0, SZ_BG_LX - SZ_CLOSE_PX - 4, SZ_BG_LY);
}

static void scenario_drawer(void)
{
    int pw = (SZ_W * FB_W) / MZ_LOGICAL_W;

    /* scenario_close_and_reopen() left the band's panel OPEN, and a drawer may not
     * arm while it is -- the ONE SURFACE gate. Close it the operator's way. */
    press_and_dismiss();
    CHECK(!menu_is_open(), "the band's panel would not close before the drawer test");

    /* A drawer that has never been out is free: the sync's first line is a return,
     * and the fake is not armed yet, so this is the shipped fallback route talking. */
    log_reset();
    run_tick();
    run_tick();
    CHECK(setup_n[SURF_SIDE] == 0 && setup_n[SURF_BAND] == 0,
          "a drawer that was never open set up a plane (%d side, %d band)",
          setup_n[SURF_SIDE], setup_n[SURF_BAND]);
    CHECK(side_paints == 0, "a shut drawer painted %d times", side_paints);

    plane_fake = 1;

    /* --- 1. the first open: the band has no plane to give up --------------- */
    log_reset();
    side_open(SZ_LEFT);
    CHECK(side_is_open(SZ_LEFT), "the inward swipe did not open the left drawer");
    CHECK(!log_has("handed over from the band"),
          "the drawer claimed the band's plane when the band had none");

    run_tick();
    CHECK(setup_n[SURF_SIDE] == 1, "the drawer's plane was set up %d times", setup_n[SURF_SIDE]);
    CHECK(side_paints == 1, "the drawer's first tick painted %d times", side_paints);
    CHECK(side_paints_left == 1 && side_paints_right == 0,
          "the left drawer painted %d/%d left/right", side_paints_left, side_paints_right);
    CHECK(side_last_which == SZ_LEFT, "the plane was drawn for drawer %d", side_last_which);
    CHECK(show_n == 1, "the drawer's plane was shown %d times", show_n);
    CHECK(last_show_x == 0, "the left drawer's plane was shown at x %d", last_show_x);
    PLANE_IS_DRAWER(SZ_LEFT, SZ_HIT_NONE, g_fader[side_channel(SZ_LEFT)],
                    "the first tick with the left drawer out");

    /* --- 2. a static frame does not repaint, and neither does the show ---- */
    run_tick();
    run_tick();
    run_tick();
    CHECK(side_paints == 1,
          "a static drawer repainted: %d paints over four ticks", side_paints);
    CHECK(show_n == 1, "the plane was re-shown %d times on a steady tick", show_n);

    /* --- 3. one fader step, through the real gesture ---------------------- */
    side_drive_local(SZ_LEFT, 1, SZ_FAD_LX, SZ_FAD_LY);
    CHECK(g_fader[side_channel(SZ_LEFT)] == side_fader_v(SZ_FAD_LY),
          "the jump-to-touch did not reach g_fader (%d, wanted %d)",
          g_fader[side_channel(SZ_LEFT)], side_fader_v(SZ_FAD_LY));
    run_tick();
    CHECK(side_paints == 2, "one fader step painted %d times, not once", side_paints - 1);
    run_tick();
    CHECK(side_paints == 2, "a resting finger on the fader repainted");
    PLANE_IS_DRAWER(SZ_LEFT, SZ_HIT_FADER, g_fader[side_channel(SZ_LEFT)],
                    "the tick after the fader step");

    /* The lift on the lane leaves the drawer out -- riding a fader is the drawer's
     * most common use, and a re-swipe per nudge would make it useless. */
    side_drive_local(SZ_LEFT, 0, SZ_FAD_LX, SZ_FAD_LY);
    CHECK(side_is_open(SZ_LEFT), "lifting off the fader dismissed the drawer");
    run_tick();                       /* the cap un-highlights: one more, and one only */
    CHECK(side_paints == 3, "the fader's lift painted %d times, not once",
          side_paints - 2);
    run_tick();
    run_tick();
    CHECK(side_paints == 3, "the drawer repainted with nothing changed");
    PLANE_IS_DRAWER(SZ_LEFT, SZ_HIT_NONE, g_fader[side_channel(SZ_LEFT)],
                    "the drawer after the fader was let go");

    /* --- 4. the outward sweep gives the plane back, once ------------------ */
    log_reset();
    side_swipe_away(SZ_LEFT);
    CHECK(!side_is_open(SZ_LEFT), "the outward sweep did not close the drawer");
    run_tick();
    CHECK(teardown_n[SURF_SIDE] == 1, "the drawer's plane was torn down %d times", teardown_n[SURF_SIDE]);
    CHECK(log_has("left drawer closed"), "the close was not logged");
    CHECK(setup_n[SURF_BAND] == 1, "the band's plane did not come back (%d)", setup_n[SURF_BAND]);
    /* ...and ONCE. The restore is behind the side_prev transition, so a hundred
     * ticks cost one setup -- unguarded it would be a setup/teardown pair per tick
     * on the thread rbp presents from. */
    run_tick();
    run_tick();
    run_tick();
    CHECK(setup_n[SURF_BAND] == 1, "the band's plane was re-set-up on a steady tick (%d)",
          setup_n[SURF_BAND]);
    CHECK(teardown_n[SURF_SIDE] == 1, "the drawer's plane was torn down again (%d)", teardown_n[SURF_SIDE]);

    /* --- 5. the next open TAKES the band's plane before asking ------------ */
    log_reset();
    side_open(SZ_LEFT);
    CHECK(side_is_open(SZ_LEFT), "the second swipe did not open the drawer");
    run_tick();
    CHECK(teardown_n[SURF_BAND] == 1, "the band's plane was not given up (%d)", teardown_n[SURF_BAND]);
    CHECK(log_has("handed over from the band"), "the handover was not logged");
    CHECK(seq_band_teardown < seq_side_setup,
          "the drawer asked for the plane before the band gave it up"
          " (band teardown %d, drawer setup %d)", seq_band_teardown, seq_side_setup);
    CHECK(setup_n[SURF_SIDE] == 2, "the drawer's plane was set up %d times, not twice",
          setup_n[SURF_SIDE]);
    CHECK(teardown_n[SURF_SIDE] == 1, "the reopen tore a plane down that was already gone (%d)",
          teardown_n[SURF_SIDE]);
    PLANE_IS_DRAWER(SZ_LEFT, SZ_HIT_NONE, g_fader[side_channel(SZ_LEFT)],
                    "the reopened left drawer");

    /* --- 6. the mirror: the right drawer is its own plane, in its own place, and
     *        the pixels in it are the right drawer's, not the left one's ------- */
    side_swipe_away(SZ_LEFT);
    CHECK(!side_is_open(SZ_LEFT), "the second sweep did not close the drawer");
    run_tick();

    log_reset();
    side_open(SZ_RIGHT);
    CHECK(side_is_open(SZ_RIGHT), "the right drawer did not open");
    CHECK(!side_is_open(SZ_LEFT), "opening the right drawer left the left one out");
    run_tick();
    CHECK(setup_n[SURF_SIDE] == 3, "the right drawer's plane was set up %d times", setup_n[SURF_SIDE]);
    CHECK(side_last_which == SZ_RIGHT, "the plane was drawn for drawer %d", side_last_which);
    /* The buffer the left drawer had just been using, armed to be repainted whatever
     * it held -- so this is the mirror's own proof at the plane level: the same
     * memory, and what is in it now is the right drawer's image. */
    PLANE_IS_DRAWER(SZ_RIGHT, SZ_HIT_NONE, g_fader[side_channel(SZ_RIGHT)],
                    "the right drawer in a buffer the left one had used");
    /* WHERE it was put is the part the pixels cannot show: the panel's drawing is
     * mirror-symmetric, so a plane placed at the wrong edge would carry an identical
     * buffer. This is the placement claim, and it is a different one from the paint's.
     * g.view.bx is 0 at this geometry, so the left drawer's plane belongs at 0 and the
     * right one a panel's width in from the page's right edge. */
    CHECK(last_show_x == FB_W - pw,
          "the right drawer's plane was shown at x %d, wanted %d (pw %d)",
          last_show_x, FB_W - pw, pw);

    /* Leave nothing behind: the last close gives the plane back to the band. */
    side_swipe_away(SZ_RIGHT);
    run_tick();
    CHECK(!side_is_open(SZ_RIGHT), "the right drawer would not close");
    CHECK(teardown_n[SURF_SIDE] == 3, "the drawer's plane was torn down %d times", teardown_n[SURF_SIDE]);
    CHECK(setup_n[SURF_BAND] == 3, "the band's plane did not come back at the end (%d)",
          setup_n[SURF_BAND]);
    plane_fake = 0;
}

/* ---------------------------------------------------------------------------
 * The three routes a drawer's image can take, which the scenario above never walks.
 *
 * scenario_drawer() runs the SHIPPED one -- a plane, a page-width stride, and the
 * per-row copy -- and it is the only scenario that arms the fake at all. What is left
 * untested there is everything in menu_side_publish()'s two arms that the unit does not
 * reach only because the host's fake plane is page-wide:
 *
 *   1. THE PAGE ROUTE, with the image cache present. The drawers used to paint the page
 *      straight; now the built image is COPIED onto it every tick, because rbp's own
 *      repaint erases the drawer and there is no damage witness for a region 180x800 px.
 *      Nothing in the suite above has ever had a drawer open with `side_ok` false and
 *      the cache allocated, which is the MENU_PLANE=0 fallback this port ships to a
 *      machine whose plane cannot be had.
 *   2. THE PITCH-MATCHING PLANE. On the unit the drawer's dumb buffer is 180 px wide and
 *      its pitch is 180 px, so the copy is ONE memcpy. Here the fake's plane is FB_W
 *      wide, so the per-row loop runs instead and that memcpy would ship untested.
 *   3. THE FIRST FRAME AFTER A FRESH SETUP. drm_band_setup() maps a dumb buffer holding
 *      nothing anyone can read. The show has to wait for an image; a flag alone does not
 *      initialise a buffer, and showing it would put 180x800 px of heap on the glass.
 * ------------------------------------------------------------------------- */
static void scenario_drawer_routes(void)
{
    int pw = (SZ_W * FB_W) / MZ_LOGICAL_W;

    /* scenario_drawer() left the BAND's plane up in this module's state -- the fake had
     * just given it back -- with the fake turned off behind it. Hand it back down the
     * way the module only ever does: with a drawer. (The real teardown is a memset for a
     * band with no fd, so leaving it would be safe; it would just be the wrong code
     * giving it back, and this scenario's ledger would be measuring the wrong object.) */
    setup_n[SURF_SIDE] = setup_n[SURF_BAND] = teardown_n[SURF_SIDE] = teardown_n[SURF_BAND] = 0;
    show_n = hide_n = side_paints = side_paints_left = side_paints_right = 0;
    side_last_which = -1;

    plane_fake = 1;
    side_open(SZ_LEFT);
    run_tick();
    CHECK(teardown_n[SURF_BAND] == 1, "the band's plane was not given up to the drawer (%d)",
          teardown_n[SURF_BAND]);
    CHECK(setup_n[SURF_SIDE] == 1, "the drawer's plane was set up %d times", setup_n[SURF_SIDE]);

    /* The fake goes off with the drawer still out, so the module is on the PAGE route in
     * both directions from here: the drawer's plane is released by the real call, and the
     * band's is never set up again. */
    log_reset();
    plane_fake = 0;
    side_swipe_away(SZ_LEFT);
    run_tick();
    CHECK(!side_is_open(SZ_LEFT), "the drawer would not close with the fake off");
    CHECK(setup_n[SURF_BAND] == 0, "the band's plane came back on a machine that has none (%d)",
          setup_n[SURF_BAND]);

    /* --- 1. the page route, WITH the image cache (the MENU_PLANE=0 fallback) ------ */
    side_open(SZ_LEFT);
    CHECK(side_is_open(SZ_LEFT), "the drawer did not open on the page route");
    fill_page(PAGE_SENTINEL);
    setup_n[SURF_SIDE] = 0;
    run_tick();
    CHECK(setup_n[SURF_SIDE] == 0, "the page route set up a plane (%d)", setup_n[SURF_SIDE]);
    CHECK(px(0, 400) != PAGE_SENTINEL, "the drawer never reached the page");
    PAGE_IS_DRAWER(SZ_LEFT, SZ_HIT_NONE, g_fader[side_channel(SZ_LEFT)],
                   "the first tick with the drawer out, on the page");

    /* rbp's own repaint erases it, and there is no damage witness for a region this
     * big -- so the copy is per TICK, not per change. Overwriting the page with the
     * sentinel is what that repaint looks like from here. The image does NOT change
     * across it: the copy is repeated, the build is not. */
    {
        int painted_before = side_paints;

        fill_page(PAGE_SENTINEL);
        menu_frame_tick();
        PAGE_IS_DRAWER(SZ_LEFT, SZ_HIT_NONE, g_fader[side_channel(SZ_LEFT)],
                       "the page route's every-tick copy");
        CHECK(side_paints == painted_before,
              "the page route repainted the drawer (%d new paints) -- the copy is"
              " per-tick but the build is not", side_paints - painted_before);
    }

    side_swipe_away(SZ_LEFT);
    run_tick();
    CHECK(!side_is_open(SZ_LEFT), "the drawer would not close off the page");

    /* --- 2. the plane the UNIT has: 180 px wide, 180 px pitch, one memcpy --------- */
    plane_pitch_px = pw;
    plane_use_alt  = 1;
    plane_fake     = 1;
    side_open(SZ_LEFT);
    run_tick();
    CHECK(setup_n[SURF_SIDE] == 1, "the narrow plane was set up %d times", setup_n[SURF_SIDE]);
    CHECK(last_show_x == 0, "the left drawer's narrow plane was shown at x %d", last_show_x);
    PLANE_IS_DRAWER(SZ_LEFT, SZ_HIT_NONE, g_fader[side_channel(SZ_LEFT)],
                    "the pitch-matching plane the unit actually has");
    /* ...and it tracks a changing image, not just the first one. */
    side_drive_local(SZ_LEFT, 1, SZ_FAD_LX, SZ_FAD_LY);
    run_tick();
    PLANE_IS_DRAWER(SZ_LEFT, SZ_HIT_FADER, g_fader[side_channel(SZ_LEFT)],
                    "the pitch-matching plane after a fader step");
    side_drive_local(SZ_LEFT, 0, SZ_FAD_LX, SZ_FAD_LY);

    side_swipe_away(SZ_LEFT);
    plane_pitch_px = PLANE_BUF_W;
    plane_use_alt  = 0;
    run_tick();
    CHECK(!side_is_open(SZ_LEFT), "the drawer would not close off the narrow plane");

    /* --- 3. the first frame: painted before it is shown, never after --------------- */
    side_open(SZ_LEFT);
    CHECK(side_is_open(SZ_LEFT), "the drawer did not open for the defer test");
    show_n = 0;
    /* The tick BY HAND, so no builder has run for this open: this is the vsync hook
     * arriving at a plane that was set up in this very tick. */
    menu_frame_tick();
    CHECK(setup_n[SURF_SIDE] == 2, "the plane was set up %d times, wanted 2", setup_n[SURF_SIDE]);
    CHECK(show_n == 0, "the plane was SHOWN holding a buffer nothing had written");

    run_tick();                      /* the builder runs first in this iteration */
    CHECK(show_n == 1, "the plane was not shown once a real image existed (%d)", show_n);
    PLANE_IS_DRAWER(SZ_LEFT, SZ_HIT_NONE, g_fader[side_channel(SZ_LEFT)],
                    "the drawer deferred until its image existed");

    side_swipe_away(SZ_LEFT);
    run_tick();
    CHECK(!side_is_open(SZ_LEFT), "the drawer would not close at the end of the routes");
}

/* ---------------------------------------------------------------------------
 * The USB STOP chooser's plane -- the FOURTH band, and the three claims about it
 * that nothing before this scenario could make.
 *
 * WHY IT NEEDS A SCENARIO OF ITS OWN. The box is a surface with a lifetime of its
 * own: it is raised from the band's seventh column (pointsrc.c), it outlives the tap
 * that raised it by up to PR_TIMEOUT_MS, and it is the only surface in this shim
 * whose PICTURE depends on something outside the shim -- rbp's own answer about the
 * two USB devices (pointsrc_usb_state()). Three consequences of that are pinned
 * nowhere else: a fresh box-sized plane can be SHOWN holding an uninitialised buffer
 * if the defer is wrong, the repaint gate must notice a device appearing or going
 * away and not only a finger moving, and the band's plane has to be taken off the
 * glass while the box is up.
 *
 * THE BOX IS RAISED BY CALLING prompt_open() DIRECTLY, and that is the one thing
 * this scenario does not do the shipped way. The raise lives in pointsrc.c's
 * menu_fire(), which is static and not linked here -- pointsrc.c is the input half
 * and brings the whole device layer with it. prompt_open() is exactly what that
 * function calls and nothing else happens on the way, so the state machine under
 * test is the shipped one; what is NOT covered here is that the seventh column is
 * what reaches this call, and that is scenario-level truth about pointsrc.c that no
 * test in this file can see either way.
 *
 * The liveness is `usb_fake`, the stub above: it is the whole reason the box's
 * picture can be driven on a host with no rbp, and it is what makes the "a dead button
 * is a different picture" claim testable at all.
 *
 * THE GESTURE ITSELF IS NOT TESTED HERE, and since 2026-10-07 that is worth saying out
 * loud. The box is two buttons held for three seconds (prompt_zone.h), and the whole of
 * that -- the hold, the flash phase, the threshold, the release that sends nothing -- is
 * pinned by test_prompt.c against the production prompt_zone.c with a clock the test owns.
 * What is pinned HERE is the DRAW thread's half: that the picture the module's state asks
 * for is the picture on the plane, that the repaint gate notices a device appearing or
 * going away and a button lighting up, and that the box's plane is taken off the glass and
 * given back at the right moments. Nothing in this file calls prompt_tick() -- the tick
 * lives in pointsrc.c's read loop, which is the input half and is not linked here -- so no
 * hold can COMPLETE in this scenario, which is what makes the "let go early" leg below a
 * real test of the early-release rule rather than of a clock nobody moved.
 * ------------------------------------------------------------------------- */

static int box_pw(void) { return prompt_paint_w(FB_W); }
static int box_ph(void) { return prompt_paint_h(FB_H); }

/* A point comfortably inside a cell, in LOGICAL coordinates -- prompt_feed() takes the
 * same space the panel reports in, so these are the PR_CELL_* rects and not page px. */
static int box_cell_x(int c) { return PR_CELL_X0(c) + 10; }
static int box_cell_y(int c) { return PR_CELL_Y0(c) + 10; }

/* The box's rect on the PAGE, which is what the fallback route writes into. Centred,
 * because that is what the logical geometry says and what menu_prompt_bx()/by()
 * reproduce in page pixels. */
static int box_page_x0(void) { return (FB_W - box_pw()) / 2; }
static int box_page_y0(void) { return (FB_H - box_ph()) / 2; }

/* The plane holds exactly the image the shipped painter makes for this state, at the
 * plane's own size and stride -- built by that same painter, so it cannot drift with
 * the palette.
 *
 * ONLY THE BOX'S OWN RECT IS COMPARED, and that is not a weakening. On the unit the
 * plane IS the box: drm_band_setup() is asked for menu_prompt_pw() x menu_prompt_ph()
 * and the dumb buffer it maps has exactly that stride, so there is no pixel of this
 * surface outside the rect. This fake hands the module the PAGE's buffer instead --
 * one allocation shared by all four surfaces -- so the columns past the box's width
 * hold whatever the band or a drawer left there, which is not this surface's business
 * and must not be asserted on. Where "nothing else was written" IS a real claim it is
 * made against the page, by BOX_PAGE_IS below, on a page that is clobbered first. */
static int box_plane_is(const struct prompt_state *S, int cell, int flash,
                        const char *what, int line)
{
    static unsigned short want[PLANE_BUF_W * PLANE_BUF_H];
    const unsigned short *got = plane_use_alt ? plane_alt : plane_buf;
    struct menu_view sv;
    int pw = box_pw(), ph = box_ph();
    int y, bad = 0;

    checks++;
    memset(want, 0, sizeof want);
    memset(&sv, 0, sizeof sv);
    sv.pix   = want;
    sv.pitch = plane_pitch_px;
    sv.fb_w  = pw;
    sv.fb_h  = ph;
    sv.bpp   = FB_BPP;
    sv.dw    = pw;
    sv.dh    = ph;
    __real_prompt_paint(&sv, S, cell, flash);

    for (y = 0; y < ph; y++) {
        if (memcmp(got + (size_t)y * plane_pitch_px,
                   want + (size_t)y * plane_pitch_px,
                   (size_t)pw * sizeof *got) == 0)
            continue;
        bad++;
    }
    if (bad == 0)
        return 1;
    failures++;
    printf("FAIL %s:%d: %s: the plane is not the chooser's box"
           " (%d of %d rows wrong; cell=%d flash=%d usb1=%d usb2=%d)\n",
           __FILE__, line, what, bad, ph, cell, flash, S->live[0], S->live[1]);
    return 0;
}

#define BOX_PLANE_IS(s, c, a, what) box_plane_is((s), (c), (a), (what), __LINE__)

/* The PAGE route's version of the same claim, and the same second half as
 * PAGE_IS_DRAWER: the box is on the page at its own rect, and every pixel outside it
 * still holds the sentinel the page was filled with. That is what tests the
 * PLACEMENT -- menu_prompt_publish()'s page arm writes a run per row from an offset of
 * its own arithmetic, and an off-by-one would leave a column of sentinel inside the
 * box or a column of box beside it, neither of which a comparison of the box alone
 * could see. */
static int box_page_is(const struct prompt_state *S, int cell, int flash,
                       const char *what, int line)
{
    static unsigned short want[FB_PITCH * FB_H];
    static unsigned short img[PR_W * PR_H];
    struct menu_view sv;
    int x0 = box_page_x0(), y0 = box_page_y0();
    int pw = box_pw(), ph = box_ph();
    long bad = 0;
    int x, y, fx = -1, fy = -1;
    unsigned int got = 0, wantv = 0;

    checks++;
    memset(img, 0, sizeof img);
    memset(&sv, 0, sizeof sv);
    sv.pix   = img;
    sv.pitch = PR_W;              /* the image is built tight, the box's own width */
    sv.fb_w  = pw;
    sv.fb_h  = ph;
    sv.bpp   = FB_BPP;
    sv.dw    = pw;
    sv.dh    = ph;
    __real_prompt_paint(&sv, S, cell, flash);

    memset(want, 0, sizeof want);
    for (y = 0; y < FB_H; y++)
        for (x = 0; x < FB_W; x++) {
            unsigned short v = PAGE_SENTINEL;

            if (x >= x0 && x < x0 + pw && y >= y0 && y < y0 + ph)
                v = img[(size_t)(y - y0) * PR_W + (x - x0)];
            want[(size_t)y * FB_PITCH + x] = v;
        }

    for (y = 0; y < FB_H; y++)
        for (x = 0; x < FB_W; x++) {
            unsigned short w = want[(size_t)y * FB_PITCH + x];
            unsigned short g = px(x, y);

            if (g != w) {
                bad++;
                if (fx < 0) { fx = x; fy = y; got = g; wantv = w; }
            }
        }
    if (bad == 0)
        return 1;
    failures++;
    printf("FAIL %s:%d: %s: the page is not the chooser's box"
           " (%ld of %d page pixels wrong; first at %d,%d: %#06x, wanted %#06x)\n",
           __FILE__, line, what, bad, FB_W * FB_H, fx, fy, got, wantv);
    return 0;
}

#define BOX_PAGE_IS(s, c, a, what) box_page_is((s), (c), (a), (what), __LINE__)

/* The raise the shipped path makes, minus the funnel that reaches it. */
static void box_open(void)
{
    prompt_open(0);               /* the clock is pointsrc.c's; this module keeps none */
    CHECK(prompt_is_open(), "the chooser would not open");
}

static void scenario_prompt(void)
{
    struct prompt_state live2 = { .live = { 1, 1 } };
    struct prompt_state usb1only = { .live = { 1, 0 } };
    int x0, y0;

    /* The routes above left the left drawer shut and the band's plane back in its
     * hands. Nothing here needs the band, so put it away first and start from a glass
     * that holds only the page. */
    press_and_dismiss();
    run_tick();
    CHECK(!menu_is_open() && !side_any_open(),
          "the glass was not clear before the chooser test");
    CHECK(!prompt_is_open(), "the chooser was open before its scenario");

    /* --- 1. a shut box costs nothing ---------------------------------------- */
    plane_fake = 1;
    usb_fake = live2;
    setup_n[SURF_PROMPT] = teardown_n[SURF_PROMPT] = 0;
    prompt_paints = 0;
    log_reset();
    run_tick();
    run_tick();
    CHECK(setup_n[SURF_PROMPT] == 0, "a shut box set up a plane (%d)",
          setup_n[SURF_PROMPT]);
    CHECK(prompt_paints == 0, "a shut box painted %d times", prompt_paints);

    /* --- 2. the raise: a plane of its own, at its own size ------------------ */
    x0 = box_page_x0();
    y0 = box_page_y0();
    CHECK(box_pw() != FB_W && box_ph() != MZ_PANEL_H && box_ph() != FB_H,
          "the box is not distinguishable from the band or a drawer (%dx%d)",
          box_pw(), box_ph());

    box_open();
    run_tick();
    CHECK(setup_n[SURF_PROMPT] == 1, "the box's plane was set up %d times",
          setup_n[SURF_PROMPT]);
    CHECK(prompt_paints == 1, "the box painted %d times on its first tick",
          prompt_paints);
    CHECK(last_show_x == x0 && last_show_y == y0,
          "the box was shown at %d,%d, not centred at %d,%d",
          last_show_x, last_show_y, x0, y0);
    BOX_PLANE_IS(&live2, 0, 0, "the box on its own plane");

    /* --- 3. the gate: a steady tick builds nothing --------------------------- */
    run_tick();
    run_tick();
    CHECK(prompt_paints == 1, "a steady tick repainted the box (%d)", prompt_paints);
    CHECK(setup_n[SURF_PROMPT] == 1, "a steady tick re-set-up the box's plane (%d)",
          setup_n[SURF_PROMPT]);

    /* --- 4. the finger, the hold let go early, and the way out -------------- */
    /* The highlight is driven through the module's own gesture and not by poking the
     * row: prompt_feed() is what pointsrc.c's funnel calls, so what the plane holds is
     * what the operator's finger would have made.
     *
     * THE PRESS IS STAMPED, because a press on a live button now starts the three-second
     * hold and the module has to know WHEN. Nothing in this file calls prompt_tick() --
     * the tick lives in pointsrc.c's read loop, which is the input half and is not linked
     * here -- so the hold cannot complete in this scenario, which is exactly what makes
     * "let go early" testable at all: the release below must send nothing because 1000 ms
     * is short of the operator's three seconds, and not because this file's clock never
     * moved. */
    {
        int act = PR_ACT_NONE;

        prompt_feed(&live2, 1, box_cell_x(PR_CELL_USB1), box_cell_y(PR_CELL_USB1),
                    0, &act);
        run_tick();
        CHECK(prompt_paints == 2, "the box did not repaint for a finger on USB 1 (%d)",
              prompt_paints);
        /* THE LIT BUTTON IS THE FINGER'S CELL, and it arrives through the flash and not
         * through prompt_pressed(): the cell whose hold is running reports 0 as the
         * finger's own cell precisely so that the blink has somewhere to blink to
         * (prompt_zone.h). What that means HERE is that the picture is the pressed one,
         * which is the same pair of pixels either way -- the plane cannot tell them
         * apart, and neither can the operator, which is the point. */
        BOX_PLANE_IS(&live2, 0, PR_CELL_USB1, "the box with USB 1 held");

        /* LET GO EARLY: nothing is sent, and the box STAYS UP. This is the operator's own
         * "if they release it before three seconds don't eject it", and it is the one
         * release in the module that leaves the box standing -- so it is pinned here as a
         * change of picture back to idle and not as a close. */
        prompt_feed(&live2, 0, box_cell_x(PR_CELL_USB1), box_cell_y(PR_CELL_USB1),
                    1000, &act);
        run_tick();
        CHECK(prompt_is_open(), "letting go early closed the box");
        CHECK(act == PR_ACT_NONE, "letting go early answered %d", act);
        CHECK(teardown_n[SURF_PROMPT] == 0,
              "the box's plane was released on a hold let go early (%d)",
              teardown_n[SURF_PROMPT]);
        CHECK(prompt_paints == 3, "letting go early did not repaint the box (%d)",
              prompt_paints);
        BOX_PLANE_IS(&live2, 0, 0, "the box after a hold let go early");

        /* ...and the way out, which is now a tap anywhere that is not a button: the press
         * begins off every cell, so its release dismisses and answers nothing. There is no
         * CANCEL and no OK any more (prompt_zone.h). */
        prompt_feed(&live2, 1, 10, 10, 0, &act);
        prompt_feed(&live2, 0, 10, 10, 1000, &act);
        run_tick();
        CHECK(act == PR_ACT_NONE, "a tap outside answered %d", act);
        CHECK(!prompt_is_open(), "a tap outside did not put the box away");
        CHECK(teardown_n[SURF_PROMPT] == 1, "the box's plane was not released (%d)",
              teardown_n[SURF_PROMPT]);
        CHECK(prompt_paints == 3, "the close repainted the box (%d)", prompt_paints);
    }

    /* --- 5. rbp's answer is part of the picture, so it is part of the gate --- */
    usb_fake = usb1only;
    box_open();
    run_tick();
    CHECK(setup_n[SURF_PROMPT] == 2, "the second open did not get a plane (%d)",
          setup_n[SURF_PROMPT]);
    CHECK(prompt_paints == 4, "the box did not build with USB 2 absent (%d)",
          prompt_paints);
    BOX_PLANE_IS(&usb1only, 0, 0, "the box with USB 2 absent");
    /* ...and the dim row really is a different picture, which is the only reason the
     * liveness belongs in the gate at all. */
    {
        static unsigned short a[PLANE_BUF_W * PLANE_BUF_H];
        struct menu_view sv;

        memset(a, 0, sizeof a);
        memset(&sv, 0, sizeof sv);
        sv.pix = a; sv.pitch = plane_pitch_px;
        sv.fb_w = box_pw(); sv.fb_h = box_ph(); sv.bpp = FB_BPP;
        sv.dw = box_pw(); sv.dh = box_ph();
        __real_prompt_paint(&sv, &live2, 0, 0);
        CHECK(memcmp(a, plane_buf, sizeof a) != 0,
              "the absence of USB 2 made no difference to the picture");
    }
    /* A device GOING AWAY is the same kind of change in the other direction. */
    usb_fake = live2;
    run_tick();
    CHECK(prompt_paints == 5, "USB 2 coming back did not rebuild the box (%d)",
          prompt_paints);
    BOX_PLANE_IS(&live2, 0, 0, "the box with USB 2 back");
    prompt_close();
    run_tick();
    CHECK(teardown_n[SURF_PROMPT] == 2, "the box's plane was not released (%d)",
          teardown_n[SURF_PROMPT]);

    /* --- 6. the pitch-matching plane, which is the one the unit has ---------- */
    /* drmband.c allocates the buffer it was asked for, so on the unit the box's plane
     * has the box's own WIDTH for a stride and `g.prompt.pitch == prompt_buf_w` --
     * menu_prompt_publish()'s single memcpy. The fake above reports the PAGE's stride
     * and takes the per-row loop instead, so without this leg the memcpy would ship
     * untested, exactly the gap the drawers' second buffer exists for.
     *
     * The pitch has to be set BEFORE the setup, because that is the only moment
     * drm_band_setup() reads it -- so this is a leg of its own, with its own open and
     * close, rather than a knob flipped under a plane that already exists. */
    plane_pitch_px = box_pw();
    box_open();
    run_tick();
    CHECK(setup_n[SURF_PROMPT] == 3, "the narrow box plane was set up %d times",
          setup_n[SURF_PROMPT]);
    CHECK(prompt_paints == 6, "the box did not repaint for the narrow plane (%d)",
          prompt_paints);
    BOX_PLANE_IS(&live2, 0, 0, "the box down the pitch-matching memcpy path");
    prompt_close();
    run_tick();
    plane_pitch_px = PLANE_BUF_W;
    CHECK(teardown_n[SURF_PROMPT] == 3, "the narrow box plane was not released (%d)",
          teardown_n[SURF_PROMPT]);

    /* --- 7. the band's plane is taken off the glass while the box is up ------ */
    /* THE STATE THIS CONSTRUCTS IS NOT REACHABLE from the shipped gesture, and that is
     * stated rather than glossed: the tap that raises the box is the same release that
     * closes the band, so on the unit the band is already shut. But menu_frame_tick()'s
     * early return is what makes the band's own path unreachable while the box is up,
     * and a band left on the glass over a box the operator is being asked to read is
     * exactly the kind of thing that would only ever be noticed on the unit. So it is
     * built here, and what is pinned is the defence and not the sequence. */
    open_panel();
    run_tick();
    run_tick();
    {
        int hides = hide_n;
        int paints = prompt_paints;

        box_open();
        run_tick();
        CHECK(hide_n > hides, "the band's plane was left on the glass over the box");
        CHECK(prompt_paints == paints + 1, "the box did not paint over the band (%d)",
              prompt_paints);
    }

    prompt_close();
    run_tick();
    press_and_dismiss();         /* the band open_panel() left standing */
    run_tick();
    CHECK(!menu_is_open() && !side_any_open(),
          "the glass was not clear before the fallback route");

    /* --- 8. the fallback: a machine that refuses the box a plane -------------- */
    /* The same claim as the drawers' page route, and it has to keep working: on a unit
     * that will not grant a fourth plane the box goes onto the page every tick --
     * prompt_pending cannot be honoured there, because rbp repaints over it and there
     * is no damage witness for a region this size. */
    setup_n[SURF_PROMPT] = teardown_n[SURF_PROMPT] = 0;
    plane_refuse_prompt = 1;
    usb_fake = live2;
    box_open();
    run_tick();
    CHECK(setup_n[SURF_PROMPT] == 0,
          "the refused box setup was counted as a plane (%d)", setup_n[SURF_PROMPT]);
    /* The page is written EVERY tick on this route, so this is the second tick's
     * output and not the first's -- which is why the clobber is repeated. */
    clobber();
    run_tick();
    BOX_PAGE_IS(&live2, 0, 0, "the box on the page, on a machine with no fourth plane");

    prompt_close();
    run_tick();
    CHECK(teardown_n[SURF_PROMPT] == 0,
          "a box that never had a plane tore one down (%d)", teardown_n[SURF_PROMPT]);

    /* ...and the refusal is a LATCH, not a verdict: the close clears it, so a later
     * open tries once more -- exactly what win_fail and side_fail do, and the whole
     * reason prompt_fail is cleared in the close arm. */
    plane_refuse_prompt = 0;
    box_open();
    run_tick();
    CHECK(setup_n[SURF_PROMPT] == 1,
          "a later open did not try the box's plane again (%d)", setup_n[SURF_PROMPT]);
    prompt_close();
    run_tick();
    CHECK(teardown_n[SURF_PROMPT] == 1, "the box's plane was not released (%d)",
          teardown_n[SURF_PROMPT]);
}

int main(int argc, char **argv)
{
    int rc;

    /* The page's stride, and the only thing the argument changes. See the FB_W block
     * above for what the padded run is for; `make test` runs both. */
    pad = argc > 1 ? atoi(argv[1]) : 0;
    if (pad < 0)
        pad = 0;
    pg_pitch = FB_W + pad;
    pg_bytes = (long)pg_pitch * FB_H * (FB_BPP / 8);

    printf("test_menu_dev: %dx%d 16 bpp, page stride %d px%s\n",
           FB_W, FB_H, pg_pitch, pad ? " (padded by the argument)" : "");

    /* The page is a real mapping, so the module's real_mmap() has something to return
     * that behaves like one. Ours is readable and writable; the module only ever
     * writes. */
    page = mmap(NULL, (size_t)pg_bytes, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    fill_page(PAGE_SENTINEL);

    setenv("MENU_VERBOSE", "1", 1);      /* the classify/blit discriminator */
    unsetenv("POINT_MENU");              /* the shipped default: enabled */
    unsetenv("FB_DEV");
    unsetenv("DFB_PRESENT_FIT");

    /* BEFORE the thread exists: the witness thread opens the framebuffer the instant
     * it starts, and scenario_open_and_prewarm() needs it to fail. See there. */
    fail_open_once = 1;

    rc = menu_draw_start();
    if (rc != 0) {
        printf("test_menu_dev: menu_draw_start() returned %d\n", rc);
        return 1;
    }

    scenario_open_and_prewarm();
    scenario_closed();
    scenario_hook_does_not_classify();
    scenario_slide();
    scenario_close_and_reopen();
    /* LAST, because it is the only one that arms the fake plane -- everything above
     * is the fallback route, and this is the one that has to be told the plane
     * exists. See its block. */
    scenario_drawer();
    /* ...and then the routes scenario_drawer() cannot reach: the page route with the
     * cache present, the unit's own pitch-matching plane, and the defer-show. */
    scenario_drawer_routes();
    /* ...and LAST of all the chooser's box, which is the only scenario that uses the
     * fake plane for a surface other than the band and the drawers, and the only one
     * whose picture depends on rbp's own answer (the usb_fake stub). */
    scenario_prompt();

    /* Stop the thread from inside, the way it was started: the wrapped usleep is
     * called BY that thread, so exiting there is legal and leaves nothing running. */
    pthread_mutex_lock(&hs_lock);
    hs_stop = 1;
    hs_go = 1;
    pthread_cond_broadcast(&hs_cv);
    pthread_mutex_unlock(&hs_lock);

    printf("test_menu_dev: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

/* Built and run twice by `make test`: once at the identity geometry, once with pad. */
