/*
 * test_cursor_dev.c -- fb_cursor.c's device logic, with no framebuffer.
 *
 * fb_cursor.c is the half of the pointer path that talks to the kernel: it opens
 * /dev/fb0, asks it for the real geometry, mmaps it, and then writes into a page
 * rbp is simultaneously painting. None of that is reachable on a workstation, and
 * until now none of it was covered at all -- test_cursor.c pins the glyph and the
 * conditional restore, which are pure pixel work against a local array, and stops
 * exactly where the device begins. What was untested is the part that changed when
 * monitors became swappable: whether a framebuffer that is not there yet is
 * retried, and where inside a mismatched page the arrow is put.
 *
 * THE SEAM IS THE SYSCALL BOUNDARY, exactly as in test_evdev.c, and for the same
 * reason: an `ops` struct of function pointers in fb_cursor.c would put test-only
 * indirection into the module that ships and would leave the real open/ioctl/mmap
 * path unexercised. The PRODUCTION fb_cursor.o is linked unmodified
 * (-Wl,--wrap=syscall) and what is faked is what the kernel would have said. Two
 * things make that work:
 *
 *   - syscalls.h's real_* helpers are `static inline` around syscall(), so
 *     wrapping the public symbol redirects every one of them -- and glibc's own
 *     internal syscalls do NOT go through it (measured in test_evdev.c, not
 *     assumed here), so this fake sees the module's calls and no one else's.
 *   - the thread runs for real. A real pthread, the real retry loop, the real
 *     paint pass, the real call into cursor_paint(). Only the kernel is faked.
 *
 * clock_gettime is NOT wrapped here, unlike test_evdev. Nothing in fb_cursor.c,
 * cursor_paint.c or point_xform.c reads a clock -- the module's only timer is the
 * sleep it hands to usleep() -- so a virtual clock would be machinery with nothing
 * to hold. The test drives the thread by making the *sleep* the tick instead.
 *
 * THE THREAD IS STOPPED FROM INSIDE, and that is the one piece of invention here.
 * cursor_thread() is static and loops forever; fb_cursor_start() detaches it, so
 * there is nothing to join. __wrap_usleep() therefore counts ticks and calls
 * pthread_exit() when the scenario's script is exhausted -- which is legal, since
 * the wrapped function is itself called from that thread. It is also exact: the
 * usleep() is the *last* thing an iteration does, so exiting there means every
 * paint, restore and log line of that tick has already happened, and the page this
 * file inspects afterwards is the page that tick left behind. The alternative -- a
 * timeout in the main thread and a race against the loop -- would make every
 * assertion below a statement about timing rather than about what the module did.
 *
 * The faked framebuffer is a real heap buffer, filled with a non-uniform pattern
 * and kept beside a pristine copy, so the assertions are made about *pixels that
 * changed* rather than about the module's internal idea of where it drew. That is
 * what lets this file pin the claim the operator cares about -- the arrow never
 * enters the bars -- without knowing anything about the glyph.
 *
 * WHAT IS CHECKED, each of them a claim the code makes in a comment:
 *
 *   1. The identity is intact. On a framebuffer that is already the logical
 *      1280x800 the picture rectangle is the whole page and a logical (100,100)
 *      puts the arrow tip at page (100,100) -- the behaviour verified on the unit
 *      before any of this existed.
 *   2. The arrow lands in the *picture*, not the page. On a 1280x720 fb the UI is
 *      fitted to 1152x720 at x=64, and a logical (640,400) must land at page
 *      (640,360) -- which is 64 px right of where the same code with the bar
 *      offset dropped would put it, so the assertion has teeth. On the picture's
 *      right edge the 12-cell glyph must be *cut off* at it rather than spilling
 *      into the bar, which is what pins the width the module hands
 *      cursor_paint(); and at the bottom-right corner only the tip cell is in
 *      bounds at all. In every case the bars must be byte-for-byte what they were.
 *   3. `stretch` fills the page, and the arrow follows it there.
 *   4. A padded stride is a *row* stride, not the picture width. On a 1024x768 fb
 *      with line_length 2112 the pitch is 1056 px while the picture is 1024 wide,
 *      so a row placed by the picture width lands on a different row. This is the
 *      one case where pitch and dw disagree, and it is why the two are separate.
 *   5. A framebuffer that is not there yet is retried, not fatal: three ENOENT
 *      opens, one log line, and the arrow is drawn once it appears.
 *   6. The retry log is loud on the first attempt and then once a minute (60
 *      attempts at 5 s), so a permanently absent fb costs one line a minute
 *      rather than one per attempt -- and a framebuffer that never appears does
 *      not end the thread.
 *   7. A format or geometry the module cannot draw in is refused rather than
 *      guessed at, and nothing is written anywhere: no mmap, no paint.
 *   8. The device it draws on is FB_DEV, not the /dev/fb0 it was hard-coded to.
 *      RB_FB_DEV was already end-to-end in rb.conf, start-rb.sh and fbdev.c, so
 *      that hard-coding was the one place the knob did not reach.
 *   9. The arrow comes off the screen when the pointer goes away -- the mouse-
 *      unplugged path, which is a restore and not just a stopped repaint.
 *  10. A press inverts it, which is the only click feedback there is.
 *
 * WHAT IS NOT CHECKED, and cannot be from here:
 *
 *   - The compositing rule itself (what a restore may and may not put back) is
 *     cursor_paint.c's, pinned by test_cursor.c, which is the right home for it:
 *     it is a rule about pixels, not about a device.
 *   - The fit arithmetic is point_fit()'s, pinned by test_point, and its agreement
 *     with the driver's second copy of the same rule is pinned by
 *     tools/fit-crosscheck.sh. This file calls the real function and asserts the
 *     four rectangles it produces; it does not re-derive them.
 *   - Anything about a real framebuffer, a real mode-set or a real hotplug. Those
 *     are S10.7 and S10.9 on the unit; this file is what makes the *code* path
 *     they exercise a thing that can be reasoned about at all.
 *   - The frame period and the redraw rate. cursor_thread's 0.5 ms is a measured
 *     value with its measurement in the source; nothing here would catch it going
 *     wrong, and nothing here pretends to.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "cursor_paint.h"
#include "fb_cursor.h"
#include "fbdev.h"
#include "point_xform.h"
#include "pointsrc.h"

/* ==========================================================================
 * Reporting, the same shape test_evdev.c uses: a count and a line per failure.
 * ========================================================================== */
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

/* ==========================================================================
 * The faked framebuffer
 *
 * One /dev/fbN whose geometry is whatever the scenario set, backed by a heap
 * buffer the module mmaps. The pattern is deliberately not uniform: a diff
 * against an all-zero page would score a bar the module scribbled as "still
 * zero" and miss it.
 * ========================================================================== */
static char *fb_page;               /* the fake framebuffer, as the module sees it */
static char *fb_pristine;           /* the same bytes, untouched, to diff against */
static size_t page_bytes;           /* pitch * height * bytes-per-pixel */

static unsigned int var_xres = 1280, var_yres = 800, var_bpp = 16;
static unsigned int fix_ll = 2560, fix_smem;
static int pitch_px;                /* fix_ll / (var_bpp/8), what the module computes */

static const char *fb_path = "/dev/fb0";
static long open_attempts;          /* opens of fb_path */
static long open_other;             /* opens of anything else */
static long open_fail_left;         /* refuse this many before handing one out */
static long mmap_calls, mmap_len;
static long ioctl_calls;

#define FAKE_FD 42                  /* nothing else in this child holds fd 42 */

static void setup_fb(int w, int h, int bpp, int ll)
{
    size_t alloc, i;

    var_xres = (unsigned int)w;
    var_yres = (unsigned int)h;
    var_bpp = (unsigned int)bpp;
    fix_ll = (unsigned int)ll;
    pitch_px = ll / (bpp / 8);
    fix_smem = (unsigned int)(pitch_px * (bpp / 8) * h);
    page_bytes = (size_t)pitch_px * (size_t)(bpp / 8) * (size_t)h;
    alloc = page_bytes > 0 ? page_bytes : 1;   /* calloc(1,0) is not worth relying on */

    free(fb_page);
    free(fb_pristine);
    fb_page = calloc(1, alloc);
    fb_pristine = calloc(1, alloc);
    if (fb_page == NULL || fb_pristine == NULL) {
        printf("test_cursor_dev: out of memory for the fake page\n");
        _exit(1);
    }
    for (i = 0; i < page_bytes; i++)
        fb_pristine[i] = fb_page[i] = (char)((i * 7 + i / 3) & 0xff);
}

/* The changed-pixel bounding box, in pixels, by row stride. Returns the count of
 * changed pixels and fills the box. Comparing whole *bytes* rather than pixels is
 * deliberate: a 16 bpp write that landed one byte off would still be one changed
 * pixel if this compared pixels, and that is exactly the sort of thing this file
 * exists to catch. */
static long changed_box(int *x0, int *y0, int *x1, int *y1);

static long changed_box(int *x0, int *y0, int *x1, int *y1)
{
    int bpp_bytes = (int)var_bpp / 8;
    long n = 0;
    int x, y;

    *x0 = *y0 = 0x7fffffff;
    *x1 = *y1 = -1;
    for (y = 0; y < (int)var_yres; y++) {
        char *row = fb_page + (size_t)y * (size_t)pitch_px * (size_t)bpp_bytes;
        char *pris = fb_pristine + (size_t)y * (size_t)pitch_px * (size_t)bpp_bytes;
        for (x = 0; x < pitch_px; x++) {
            if (memcmp(row + (size_t)x * (size_t)bpp_bytes,
                       pris + (size_t)x * (size_t)bpp_bytes,
                       (size_t)bpp_bytes) == 0)
                continue;
            n++;
            if (x < *x0) *x0 = x;
            if (x > *x1) *x1 = x;
            if (y < *y0) *y0 = y;
            if (y > *y1) *y1 = y;
        }
    }
    return n;
}

/* True when nothing changed outside the picture rectangle point_fit() gives --
 * the assertion behind "the arrow never enters the bars". */
static int outside_picture(int dw, int dh, int bx, int by)
{
    int bpp_bytes = (int)var_bpp / 8;
    int x, y;

    for (y = 0; y < (int)var_yres; y++) {
        char *row = fb_page + (size_t)y * (size_t)pitch_px * (size_t)bpp_bytes;
        char *pris = fb_pristine + (size_t)y * (size_t)pitch_px * (size_t)bpp_bytes;
        for (x = 0; x < pitch_px; x++) {
            if (x >= bx && x < bx + dw && y >= by && y < by + dh)
                continue;
            if (memcmp(row + (size_t)x * (size_t)bpp_bytes,
                       pris + (size_t)x * (size_t)bpp_bytes,
                       (size_t)bpp_bytes) != 0)
                return 0;
        }
    }
    return 1;
}

static unsigned int pixel_at(int x, int y);

static unsigned int pixel_at(int x, int y)
{
    char *p = fb_page + (size_t)y * (size_t)pitch_px * ((size_t)var_bpp / 8)
                      + (size_t)x * ((size_t)var_bpp / 8);

    if (x < 0 || y < 0 || x >= pitch_px || y >= (int)var_yres)
        return 0xdeadbeefu;
    if (var_bpp == 16)
        return *(unsigned short *)p;
    return *(unsigned int *)p;
}

/* ==========================================================================
 * The two pointsrc_* symbols fb_cursor.c uses
 *
 * These are the whole of pointsrc.c's interface to this module, so stubbing them
 * keeps the entire pointer-device layer out of the link -- and, more to the
 * point, keeps it from opening /dev/input on the machine running this test.
 *
 * pointsrc_log() goes to a buffer instead of /tmp/pointsrc.log, which is what
 * lets the retry-gating assertions be about the module's own output rather than
 * about a file the test would have to find. The newline is appended here because
 * the production logger is what terminates a line and several calls in
 * fb_cursor.c pass a format with no "\n" in it.
 * ========================================================================== */
#define LOG_MAX 16384
static char log_buf[LOG_MAX];
static size_t log_len;
static long log_truncated;

void pointsrc_log(const char *fmt, ...)
{
    va_list ap;
    int n;

    if (log_len + 2 >= sizeof log_buf) {
        log_truncated++;
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(log_buf + log_len, sizeof log_buf - log_len - 1, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    log_len += (size_t)n;
    if (log_len > 0 && log_buf[log_len - 1] != '\n')
        log_buf[log_len++] = '\n';
    log_buf[log_len] = '\0';
}

/* How many times `needle` appears. Every gating assertion is a count rather than
 * a "does it appear": "loud once" and "loud seventy times" are the same
 * substring, and only the count tells them apart. */
static int log_count(const char *needle)
{
    const char *p = log_buf;
    int n = 0;
    size_t len = strlen(needle);

    while ((p = strstr(p, needle)) != NULL) {
        n++;
        p += len;
    }
    return n;
}

/* The pointer source, scripted: one entry per iteration, the last of which stops
 * the thread.
 *
 * Each call takes a snapshot of the page *as it stands before this tick paints*,
 * which is how a scenario sees a moment it cannot see from outside -- the state
 * left by the previous tick. Entry i's snapshot is therefore "after tick i-1", and
 * the state after the *last* tick is read by the caller once the thread is gone.
 * Two scenarios need exactly that: the unplugged one (drawn, then erased) and the
 * press one (unpressed, then inverted). */
struct tick {
    int present, x, y, down;
};

#define TICKS_MAX 8
static struct tick script[TICKS_MAX];
static int script_n;
static int script_i;
static int tick_done;                 /* set on the last entry: stop after it */

static int seen_n;
static long seen_px[TICKS_MAX];       /* changed-pixel count before tick i */
static int seen_box[TICKS_MAX][4];    /* and its box */
static int seen_watch[TICKS_MAX];     /* and the watched pixel's value */

/* Where a scenario wants one particular pixel watched across the ticks -- the
 * arrow tip, which is the pixel the press inversion shows up in. Off-page by
 * default, which is harmless: pixel_at() answers a sentinel and no scenario
 * asserts on it unless it set the point. */
static int watch_x, watch_y;

int pointsrc_cursor(int *logical_x, int *logical_y, int *down)
{
    struct tick *t;

    if (seen_n < TICKS_MAX) {
        seen_px[seen_n] = changed_box(&seen_box[seen_n][0], &seen_box[seen_n][1],
                                      &seen_box[seen_n][2], &seen_box[seen_n][3]);
        seen_watch[seen_n] = (int)pixel_at(watch_x, watch_y);
        seen_n++;
    }
    if (script_i >= script_n) {
        /* A scenario that ran out of script is a bug in this file, not a module
         * behaviour: stop rather than replay the last entry for ever. */
        tick_done = 1;
        return 0;
    }
    t = &script[script_i];
    if (t->present) {
        *logical_x = t->x;
        *logical_y = t->y;
        *down = t->down;
    }
    tick_done = (script_i == script_n - 1);
    script_i++;
    return t->present;
}

/* ==========================================================================
 * The faked kernel
 *
 * Everything the module asks of the kernel goes through __wrap_syscall();
 * anything else falls through to the real one.
 * ========================================================================== */
extern long __real_syscall(long number, ...);

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
            open_attempts++;
            if (open_fail_left > 0) {
                open_fail_left--;
                errno = ENOENT;
                r = -1;
            } else {
                r = FAKE_FD;
            }
        } else {
            open_other++;
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
            v->xres = v->xres_virtual = var_xres;
            v->yres = v->yres_virtual = var_yres;
            v->bits_per_pixel = var_bpp;
            r = 0;
        } else if (req == FBIOGET_FSCREENINFO) {
            struct fb_fix_screeninfo *f = arg;
            memset(f, 0, sizeof *f);
            memcpy(f->id, "fakefb", 6);
            f->line_length = fix_ll;
            f->smem_len = fix_smem;
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
            printf("test_cursor_dev: a real mmap on fd %d leaked through\n", fd);
            r = -ENOMEM;
            break;
        }
        mmap_calls++;
        mmap_len = (long)len;
        r = (long)fb_page;
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
        /* Loud rather than silent: this file models everything fb_cursor.c does
         * today, so an unmodelled call means the module grew a new one and the
         * fake is now guessing about the answer. */
        printf("test_cursor_dev: unmodelled syscall %ld\n", number);
        return -1;
    }
    va_end(ap);
    return r;
}

/* ==========================================================================
 * The tick, and the stop
 *
 * usleep() is the last call of every iteration of the module's loops -- the retry
 * loop's backoff and the paint loop's period both end there -- so it is both the
 * natural clock for a scripted test and the place where the thread can be ended
 * exactly at a tick boundary. It costs no real time; the main thread waits with a
 * real nanosleep, which is what gives this thread the CPU.
 * ========================================================================== */
static int thread_gone;
static long sleep_cap = 100000;      /* backstop: a scenario bug must not hang */
static long sleeps;
static int exit_script, exit_cap;

int __wrap_usleep(useconds_t usec)
{
    (void)usec;
    if (tick_done || ++sleeps > sleep_cap) {
        exit_script = tick_done;
        exit_cap = !tick_done;
        thread_gone = 1;
        pthread_exit(NULL);
    }
    return 0;
}

/* ==========================================================================
 * Running one scenario
 * ========================================================================== */
static void wait_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* Start the module's thread and wait for it to leave. The bound is real time and
 * generous: a scenario that never reaches its exit condition is a bug in this
 * file, and it should fail loudly rather than hang the suite. */
static int run_thread(void)
{
    int i;

    thread_gone = sleeps = 0;
    exit_script = exit_cap = 0;
    if (fb_cursor_start() != 0) {
        printf("test_cursor_dev: fb_cursor_start() refused\n");
        return 0;
    }
    for (i = 0; i < 5000 && !thread_gone; i++)
        wait_ms(1);
    return thread_gone;
}

/* Arm the pointer script, so a scenario reads as a list of what the pointer did
 * rather than as a pile of resets. */
static void arm(const struct tick *ticks, int n)
{
    int i;

    if (n > TICKS_MAX)
        n = TICKS_MAX;
    memset(script, 0, sizeof script);
    for (i = 0; i < n; i++)
        script[i] = ticks[i];
    script_n = n;
    script_i = 0;
    tick_done = 0;
    seen_n = 0;
    memset(seen_px, 0, sizeof seen_px);
    memset(seen_box, 0, sizeof seen_box);
    memset(seen_watch, 0, sizeof seen_watch);
    watch_x = watch_y = -1;
}

/* ==========================================================================
 * The scenarios
 * ========================================================================== */

/* 1. The identity. Nothing about this changed, and the whole point of the
 *    rectangle work is that on the measured panel it still does not. */
static void scenario_identity(void)
{
    static const struct tick ticks[] = { { 1, 100, 100, 0 } };
    int x0, y0, x1, y1;

    setup_fb(1280, 800, 16, 2560);
    arm(ticks, 1);

    CHECK(run_thread(), "the thread never finished");
    CHECK(exit_script, "the thread was stopped by the backstop, not the script");
    CHECK(log_count("picture 1280x800 at 0,0") == 1,
          "the picture rectangle was not the whole page: %d matching line(s)",
          log_count("picture 1280x800 at 0,0"));
    CHECK(log_count("cursor: open /dev/fb0") == 0,
          "a log line about a missing fb, on a fb that was there");
    CHECK(log_count("cursor: first paint at (100,100)") == 1,
          "the arrow was not painted at the logical position: %d line(s)",
          log_count("cursor: first paint at (100,100)"));

    CHECK(changed_box(&x0, &y0, &x1, &y1) > 0, "nothing was drawn at all");
    CHECK(x0 == 100 && y0 == 100, "the tip is at %d,%d, expected 100,100", x0, y0);
    CHECK(x1 == 111 && y1 == 118,
          "the glyph box is %d,%d..%d,%d, expected 100,100..111,118",
          x0, y0, x1, y1);
}

/* 2. The arrow is placed inside the picture rectangle, not the page. The x the
 *    bar offset adds is the assertion: with bx dropped the tip would be at 576. */
static void scenario_fit_centre(void)
{
    static const struct tick ticks[] = { { 1, 640, 400, 0 } };
    int x0, y0, x1, y1;

    setup_fb(1280, 720, 16, 2560);
    arm(ticks, 1);

    CHECK(run_thread(), "the thread never finished");
    CHECK(log_count("picture 1152x720 at 64,0") == 1,
          "the picture rectangle is not 1152x720 at 64,0: %d line(s)",
          log_count("picture 1152x720 at 64,0"));

    CHECK(changed_box(&x0, &y0, &x1, &y1) > 0, "nothing was drawn at all");
    CHECK(x0 == 640 && y0 == 360,
          "the tip is at %d,%d, expected 640,360 -- those extra 64 px are what "
          "proves the bar offset is applied", x0, y0);
    CHECK(x1 == 651 && y1 == 378,
          "the glyph box is %d,%d..%d,%d, expected 640,360..651,378",
          x0, y0, x1, y1);
    CHECK(outside_picture(1152, 720, 64, 0), "the arrow left the picture rectangle");
}

/* 2b. The right edge. This is the case that pins the *clamping bound*: the
 *     picture is 1152 columns wide and ends at page x=1215, so an arrow whose tip
 *     is on that last column must be cut off at it. The glyph is 12 cells wide,
 *     so with the page's width handed to cursor_paint() instead of the picture's,
 *     eleven of those cells land in the right-hand bar -- which is why the
 *     assertion is about the box's right edge and about the bars, not about the
 *     tip. The pointer is at (1279,400) rather than the bottom corner on purpose:
 *     at the very bottom row only the glyph's first cell is in bounds, and a
 *     one-cell arrow cannot show a clamping difference at all. */
static void scenario_fit_right_edge(void)
{
    static const struct tick ticks[] = { { 1, 1279, 400, 0 } };
    int x0, y0, x1, y1;

    setup_fb(1280, 720, 16, 2560);
    arm(ticks, 1);

    CHECK(run_thread(), "the thread never finished");
    CHECK(changed_box(&x0, &y0, &x1, &y1) > 0, "nothing was drawn at all");
    CHECK(y0 == 360, "the tip's row is %d, expected 360", y0);
    CHECK(x0 == 1215 && x1 == 1215,
          "the box is %d..%d, expected the picture's last column only (1215): "
          "the glyph reaches 11 columns past its tip, and those columns are the "
          "right-hand bar", x0, x1);
    CHECK(outside_picture(1152, 720, 64, 0),
          "the arrow drew into the right-hand bar: the picture ends at page "
          "x=1215 and nothing at or past 1216 may change");
}

/* 2c. And the bottom-right corner, where only the tip cell is in bounds. This is
 *     the clamp in the other axis, and it is the state a pointer pinned into the
 *     corner actually sits in. */
static void scenario_fit_corner(void)
{
    static const struct tick ticks[] = { { 1, 1279, 799, 0 } };
    int x0, y0, x1, y1;

    setup_fb(1280, 720, 16, 2560);
    arm(ticks, 1);

    CHECK(run_thread(), "the thread never finished");
    CHECK(changed_box(&x0, &y0, &x1, &y1) > 0, "nothing was drawn at all");
    CHECK(x0 == 1215 && y0 == 719,
          "the tip is at %d,%d, expected 1215,719 (bx 64 + the picture's last "
          "column 1151)", x0, y0);
    CHECK(x1 == 1215 && y1 == 719,
          "the glyph was not clamped to the picture: box %d,%d..%d,%d, expected "
          "the single tip pixel at 1215,719", x0, y0, x1, y1);
    CHECK(outside_picture(1152, 720, 64, 0),
          "the arrow drew into the bars");
}

/* 3. `stretch` is the other policy: the picture is the whole page, and the arrow
 *    follows it to the page's last column -- which under `fit` was 1151 + 64. */
static void scenario_stretch(void)
{
    static const struct tick ticks[] = { { 1, 1279, 799, 0 } };
    int x0, y0, x1, y1;

    setenv("DFB_PRESENT_FIT", "stretch", 1);
    setup_fb(1280, 720, 16, 2560);
    arm(ticks, 1);

    CHECK(run_thread(), "the thread never finished");
    CHECK(log_count("picture 1280x720 at 0,0 (stretched)") == 1,
          "the stretched rectangle was not the whole page: %d line(s)",
          log_count("picture 1280x720 at 0,0 (stretched)"));

    CHECK(changed_box(&x0, &y0, &x1, &y1) > 0, "nothing was drawn at all");
    CHECK(x0 == 1279 && y0 == 719,
          "the tip is at %d,%d, expected the page corner 1279,719", x0, y0);
    CHECK(outside_picture(1280, 720, 0, 0), "the arrow left the page");
}

/* 4. A padded stride. line_length 2112 with 16 bpp is a pitch of 1056 px against
 *    a picture only 1024 wide, so a row drawn at the picture's width lands on a
 *    different row -- which is what the y assertion catches. */
static void scenario_padded_stride(void)
{
    static const struct tick ticks[] = { { 1, 640, 400, 0 } };
    int x0, y0, x1, y1;

    setup_fb(1024, 768, 16, 2112);
    arm(ticks, 1);

    CHECK(run_thread(), "the thread never finished");
    CHECK(log_count("pitch 1056") == 1,
          "the log does not report the pitch as 1056 px: %d line(s)",
          log_count("pitch 1056"));
    CHECK(log_count("picture 1024x640 at 0,64") == 1,
          "the picture rectangle is not 1024x640 at 0,64: %d line(s)",
          log_count("picture 1024x640 at 0,64"));
    CHECK(mmap_len == (long)fix_smem && fix_smem == 1056 * 2 * 768,
          "the mapping is %ld bytes, expected smem_len %u", mmap_len, fix_smem);

    CHECK(changed_box(&x0, &y0, &x1, &y1) > 0, "nothing was drawn at all");
    CHECK(x0 == 512 && y0 == 384,
          "the tip is at %d,%d, expected 512,384 -- a row placed by the picture's "
          "1024 px width instead of the 1056 px stride lands 3 rows up",
          x0, y0);
    CHECK(x1 == 523 && y1 == 402,
          "the glyph box is %d,%d..%d,%d, expected 512,384..523,402",
          x0, y0, x1, y1);
    CHECK(outside_picture(1024, 640, 0, 64),
          "the arrow drew outside the picture, in the vertical bars");
}

/* 5. A framebuffer that is not there yet. Three ENOENTs, one log line (the loud
 *    first attempt, then the silent ones), and an arrow once it appears. */
static void scenario_absent_fb(void)
{
    static const struct tick ticks[] = { { 1, 100, 100, 0 } };

    open_fail_left = 3;
    setup_fb(1280, 800, 16, 2560);
    arm(ticks, 1);

    CHECK(run_thread(), "the thread never finished");
    CHECK(open_attempts == 4,
          "the fb was opened %ld time(s), expected 4 (3 refusals then a success)",
          open_attempts);
    CHECK(log_count("cursor: open /dev/fb0") == 1,
          "the retry logged %d line(s) for 3 failures: only the first attempt is "
          "supposed to be loud", log_count("cursor: open /dev/fb0"));
    CHECK(log_count("first paint") == 1,
          "the arrow was never drawn after the fb appeared");
    CHECK(ioctl_calls > 0, "the fb was opened but never asked for its geometry");
}

/* 6. And the gate itself: 70 refusals reach the 60-attempt mark, so exactly two
 *    lines -- the first attempt and the once-a-minute one. */
static void scenario_retry_gate(void)
{
    static const struct tick ticks[] = { { 1, 100, 100, 0 } };

    open_fail_left = 70;
    setup_fb(1280, 800, 16, 2560);
    arm(ticks, 1);

    CHECK(run_thread(), "the thread never finished");
    CHECK(open_attempts == 71, "the fb was opened %ld time(s), expected 71",
          open_attempts);
    CHECK(log_count("cursor: open /dev/fb0") == 2,
          "%d log line(s) for 70 refusals: expected 2 -- the first attempt and "
          "the one at attempt 60", log_count("cursor: open /dev/fb0"));
    CHECK(log_count("first paint") == 1,
          "the arrow was never drawn after the fb appeared");
}

/* 7. A framebuffer that never appears does not end the thread. The stop here is
 *    the harness's backstop rather than the script, and that is the assertion:
 *    had the module exited on the first failure there would be no attempts to
 *    count. The retry loop spends one sleep per attempt, so N sleeps is N+1
 *    opens -- the extra one is the open that was in flight when the cap hit. */
static void scenario_never_appears(void)
{
    static const struct tick ticks[] = { { 1, 100, 100, 0 } };

    open_fail_left = 1000000;
    sleep_cap = 130;
    setup_fb(1280, 800, 16, 2560);
    arm(ticks, 1);

    CHECK(run_thread(), "the thread never finished");
    CHECK(exit_cap && !exit_script,
          "the thread stopped for the wrong reason (script=%d cap=%d)",
          exit_script, exit_cap);
    CHECK(open_attempts == sleep_cap + 1,
          "the thread made %ld attempt(s) in %ld ticks: it is not retrying every "
          "tick", open_attempts, sleep_cap);
    CHECK(log_count("cursor: open /dev/fb0") == 3,
          "%d log line(s) in %ld attempts: expected 3 (attempts 0, 60, 120)",
          log_count("cursor: open /dev/fb0"), open_attempts);
    CHECK(log_count("first paint") == 0, "an arrow was drawn with no fb");
    CHECK(mmap_calls == 0, "the fb was mmapped after a failed open");
}

/* 8. What cannot be drawn in is refused rather than guessed at. An 8 bpp fb is
 *    not a format this module has a pixel writer for, so it must not map one --
 *    and it must keep trying, in case the next mode is usable. */
static void scenario_refused_format(void)
{
    static const struct tick ticks[] = { { 1, 100, 100, 0 } };

    sleep_cap = 5;
    setup_fb(1280, 800, 8, 1280);
    arm(ticks, 1);

    CHECK(run_thread(), "the thread never finished");
    CHECK(log_count("cursor: 8 bpp is not a format this can draw in") == 1,
          "the format was not refused: %d line(s)",
          log_count("cursor: 8 bpp is not a format this can draw in"));
    CHECK(mmap_calls == 0, "a framebuffer in an unusable format was mmapped");
    CHECK(open_attempts == sleep_cap + 1,
          "the thread stopped retrying: %ld attempt(s)", open_attempts);
}

/* 8b. The same for a geometry that is there but meaningless. */
static void scenario_refused_geometry(void)
{
    static const struct tick ticks[] = { { 1, 100, 100, 0 } };

    sleep_cap = 5;
    setup_fb(0, 0, 16, 2560);
    arm(ticks, 1);

    CHECK(run_thread(), "the thread never finished");
    CHECK(log_count("cursor: fb geometry is 0x0 pitch 2560") == 1,
          "the degenerate geometry was not refused: %d line(s)",
          log_count("cursor: fb geometry is 0x0 pitch 2560"));
    CHECK(mmap_calls == 0, "a framebuffer with no geometry was mmapped");
}

/* 9. FB_DEV is the device, not a name compiled in. /dev/fb1 is a real
 *    configuration rather than a hypothetical: it is what a monitor on the Pi's
 *    second micro-HDMI port is. */
static void scenario_fb_dev(void)
{
    static const struct tick ticks[] = { { 1, 100, 100, 0 } };

    setenv("FB_DEV", "/dev/fb1", 1);
    fb_path = "/dev/fb1";
    setup_fb(1280, 800, 16, 2560);
    arm(ticks, 1);

    CHECK(run_thread(), "the thread never finished");
    CHECK(open_attempts == 1, "/dev/fb1 was opened %ld time(s), expected 1",
          open_attempts);
    CHECK(open_other == 0,
          "%ld open(s) of something else: FB_DEV was ignored and the hard-coded "
          "/dev/fb0 was used", open_other);
    CHECK(log_count("picture 1280x800 at 0,0") == 1, "the fb was not mapped");
    CHECK(log_count("first paint") == 1, "no arrow was drawn");
}

/* 10. The mouse-unplugged path. Tick 0 draws; tick 1 reports no relative device,
 *     which must take the arrow back off rather than leave it frozen on screen.
 *     Both states are read as *pixels*: entry 1's snapshot is the page after tick
 *     0's paint, and the box after the run is the page after tick 1 erased it. */
static void scenario_unplugged(void)
{
    static const struct tick ticks[] = { { 1, 100, 100, 0 }, { 0, 0, 0, 0 } };
    int x0, y0, x1, y1;

    setup_fb(1280, 800, 16, 2560);
    arm(ticks, 2);

    CHECK(run_thread(), "the thread never finished");
    CHECK(log_count("first paint at (100,100)") == 1,
          "the arrow was never drawn in the first place");
    CHECK(seen_n >= 2, "the pointer was consulted %d time(s), expected 2", seen_n);
    CHECK(seen_px[1] > 0 && seen_box[1][0] == 100 && seen_box[1][1] == 100,
          "after the first tick the page holds no arrow at 100,100: %ld pixel(s), "
          "box %d,%d..%d,%d", seen_px[1], seen_box[1][0], seen_box[1][1],
          seen_box[1][2], seen_box[1][3]);

    CHECK(changed_box(&x0, &y0, &x1, &y1) == 0,
          "%ld pixel(s) are still changed after the pointer went away: the arrow "
          "was left on the screen (box %d,%d..%d,%d)", seen_px[1], x0, y0, x1, y1);
}

/* 11. A press inverts the arrow -- the only click feedback there is. Tick 0 draws
 *     unpressed, tick 1 draws pressed, and the tip pixel is compared across the
 *     two. The values are cursor_pixel()'s for 16 bpp: the outline is dark until
 *     the button is down, and white while it is. */
static void scenario_press(void)
{
    static const struct tick ticks[] = { { 1, 100, 100, 0 }, { 1, 100, 100, 1 } };

    setup_fb(1280, 800, 16, 2560);
    arm(ticks, 2);
    watch_x = watch_y = 100;          /* the tip, where the inversion shows */

    CHECK(run_thread(), "the thread never finished");
    CHECK(seen_n >= 2, "the pointer was consulted %d time(s), expected 2", seen_n);
    CHECK(seen_watch[1] == 0x0000,
          "the unpressed outline tip is 0x%04x, expected the dark 0x0000",
          (unsigned)seen_watch[1]);
    CHECK(pixel_at(100, 100) == 0xffff,
          "the pressed tip is 0x%04x, expected the inverted 0xffff -- a press has "
          "to change the pixel or there is no click feedback at all",
          pixel_at(100, 100));
    CHECK(pixel_at(100, 100) != (unsigned)seen_watch[1],
          "the tip pixel did not change on press");
}

/* ========================================================================== */

struct scenario {
    const char *name;
    void (*fn)(void);
};

static const struct scenario scenarios[] = {
    { "identity on 1280x800",    scenario_identity         },
    { "fit, centre",             scenario_fit_centre       },
    { "fit, right edge",         scenario_fit_right_edge   },
    { "fit, bottom-right corner", scenario_fit_corner      },
    { "stretch",                 scenario_stretch          },
    { "padded stride",           scenario_padded_stride    },
    { "fb absent, then appears", scenario_absent_fb        },
    { "retry log gating",        scenario_retry_gate       },
    { "fb never appears",        scenario_never_appears    },
    { "refused format",          scenario_refused_format   },
    { "refused geometry",        scenario_refused_geometry },
    { "FB_DEV",                  scenario_fb_dev           },
    { "pointer unplugged",       scenario_unplugged        },
    { "press inverts the arrow", scenario_press            },
};

int main(void)
{
    int n = (int)(sizeof scenarios / sizeof scenarios[0]);
    int failed = 0;

    for (int i = 0; i < n; i++) {
        pid_t pid;
        int st;

        printf("-- %s\n", scenarios[i].name);
        fflush(stdout);

        pid = fork();
        if (pid < 0) {
            printf("test_cursor_dev: fork: %s\n", strerror(errno));
            return 1;
        }
        if (pid == 0) {
            /* A virgin module: fb_cursor_start() is once-per-process (its
             * `started` flag is a function static) and the thread it spawns is
             * detached, so one scenario per child is the only way to run
             * thirteen of them. It is also what keeps each scenario's faked
             * syscall counters its own. */
            checks = failures = 0;
            scenarios[i].fn();
            printf("   %d checks, %d failures\n", checks, failures);
            if (failures > 0) {
                /* The module's own account of what it did, which is where a
                 * failure's cause usually is. Printed only on failure so a
                 * passing run stays two lines per scenario. */
                printf("   -- the module's log --\n%s", log_buf);
                if (log_truncated)
                    printf("   (log truncated: %ld line(s) dropped)\n",
                           log_truncated);
            }
            fflush(stdout);
            _exit(failures == 0 ? 0 : 1);
        }

        if (waitpid(pid, &st, 0) != pid) {
            printf("test_cursor_dev: waitpid: %s\n", strerror(errno));
            return 1;
        }
        if (!WIFEXITED(st)) {
            printf("   scenario died on signal %d\n", WTERMSIG(st));
            failed++;
        } else if (WEXITSTATUS(st) != 0) {
            failed++;
        }
    }

    printf("test_cursor_dev: %d scenario(s), %d failed\n", n, failed);
    return failed == 0 ? 0 : 1;
}
