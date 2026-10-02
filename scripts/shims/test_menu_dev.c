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

#include "fbdev.h"
#include "menu_draw.h"
#include "menu_paint.h"
#include "menu_zone.h"
#include "pointsrc.h"

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
 * The reference images, and the comparison.
 *
 * Built by the production painter, once each, so what the page is compared against is
 * what the module would draw -- not a copy of the palette that could drift.
 * ------------------------------------------------------------------------- */

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
        feed(1, BTN_MID(b - 1), BTN_ROW);
    CHECK(menu_is_open(), "sliding to %d closed the panel", b);
    CHECK(menu_pressed() == b, "the finger on button %d reads as %d", b, menu_pressed());
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

    /* Every one of the thirteen transitions was incremental -- thirteen column
     * rebuilds and no whole panel. The one transition that has nothing to carry
     * over is the prewarm's, and that is not in this window. */
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
