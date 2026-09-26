/*
 * fb_cursor.c — see fb_cursor.h.
 *
 * This is the only part of the port that writes to /dev/fb0 without being asked
 * to. Two things make that safe to do here and nowhere else:
 *
 *  - It reads the geometry with raw syscalls, so it sees the *real* page
 *    (1280x800, 16 bpp, pitch 2560 on the measured Pi) and not the lie fb_shim.c
 *    tells rbp and DirectFB. Everything it does — the mmap length, the pixel
 *    pitch, the format — is derived from that read, so a panel that came up at
 *    another depth or size cannot silently turn the arrow into garbage.
 *  - It never assumes it owns the pixel. The page is the one rbp paints its UI
 *    into, so every write is paired with a save-under and every restore is
 *    conditional on the pixel still being ours; see cursor_paint.h. The arrow is
 *    also repainted every tick rather than left to survive on its own, because
 *    rbp's repaints will otherwise erase it — and an erased arrow is invisible
 *    for good, since nothing else on this target draws a pointer.
 *
 * It draws nothing unless a *relative* device is driving the pointer: a real
 * touchscreen reports where it was touched and does not need an arrow, and an
 * arrow parked at the last touch would be clutter on the one target that does
 * not need it.
 */
#define _GNU_SOURCE
#include "fb_cursor.h"
#include "cursor_paint.h"
#include "fbdev.h"
#include "point_xform.h"
#include "pointsrc.h"
#include "syscalls.h"
#include "envutil.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <string.h>
#include <sys/mman.h>          /* PROT_READ/PROT_WRITE/MAP_SHARED for real_mmap */
#include <unistd.h>

#define FB_DEV "/dev/fb0"
/* 0.5 ms, i.e. ~33x rbp's own frame rate, and the measurement is the reason.
 *
 * The arrow is composited into the page rbp paints, so it is erased once per rbp
 * frame and can only be redrawn between frames. Measured on the unit by writing a
 * 12x19 block at (640,400) and counting the redraw intervals that found it
 * damaged:
 *
 *     redraw every 16 ms  ->  damaged in 91% of intervals, arrow visible  9%
 *     redraw every  1 ms  ->  damaged in  6%                   visible 94%
 *     redraw every  0.2 ms -> damaged in  1.7%                 visible 98%
 *
 * rbp repaints at about 60 Hz (all three numbers are the same ~16 ms per repaint,
 * which is also why the UI looks static: it repaints the same content). At the
 * 16 ms rate the arrow was on screen for roughly 1.5 ms in every 16 and no
 * framebuffer capture ever contained one — which is how the first version of this
 * was debugged into the ground. At 0.5 ms it is visible ~97% of the time.
 *
 * This costs almost nothing because cursor_paint() writes only the cells that are
 * not already ours: a tick that finds the arrow intact is a read pass over 228
 * cells and no writes at all. */
#define CURSOR_MS_DEFAULT 0.5
/* The floor on the redraw period, in microseconds. A value of 0 (or a typo) means
 * "as fast as this can go", not "spin a core": the loop still sleeps this long,
 * which is about the granularity of nanosleep on this target anyway. */
#define CURSOR_US_MIN 50L

struct cursor_state {
    void *pix;
    int fb_w, fb_h, bpp;
    int pitch;                                  /* in pixels, not bytes */
    unsigned int saved[CURSOR_W * CURSOR_H];
    int drawn, x, y, pressed;
};

static struct cursor_state g;

static int map_fb(struct cursor_state *s)
{
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    long len;
    int fd;

    fd = real_open(FB_DEV, O_RDWR, 0);
    if (fd < 0) {
        pointsrc_log("cursor: open %s: %s (no pointer will be drawn)\n",
                     FB_DEV, strerror(errno));
        return -1;
    }
    memset(&var, 0, sizeof var);
    memset(&fix, 0, sizeof fix);
    if (real_ioctl(fd, FBIOGET_VSCREENINFO, &var) != 0 ||
        real_ioctl(fd, FBIOGET_FSCREENINFO, &fix) != 0) {
        pointsrc_log("cursor: %s is not a framebuffer (%s)\n", FB_DEV, strerror(errno));
        real_close(fd);
        return -1;
    }

    /* The two formats this target can produce: vc4drmfb gives RGB565 here, and
     * 32 bpp is the other thing drm_fb_helper is asked for. Anything else is
     * refused rather than guessed at, because a wrong pitch or format draws
     * stripes over the UI instead of an arrow. */
    if (var.bits_per_pixel != 16 && var.bits_per_pixel != 32) {
        pointsrc_log("cursor: %d bpp is not a format this can draw in\n",
                     var.bits_per_pixel);
        real_close(fd);
        return -1;
    }
    if (var.xres == 0 || var.yres == 0 || fix.line_length == 0) {
        pointsrc_log("cursor: fb geometry is %ux%u pitch %u\n",
                     var.xres, var.yres, fix.line_length);
        real_close(fd);
        return -1;
    }

    s->fb_w = (int)var.xres;
    s->fb_h = (int)var.yres;
    s->bpp = (int)var.bits_per_pixel;
    s->pitch = (int)(fix.line_length / (var.bits_per_pixel / 8));

    len = fix.smem_len > 0 ? (long)fix.smem_len
                           : (long)s->pitch * s->fb_h * (s->bpp / 8);
    s->pix = real_mmap(NULL, (size_t)len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    /* The fd is not needed after the mapping: a shared mapping outlives it, and
     * holding it open would leave the cursor thread owning an fd forever. */
    real_close(fd);
    if (s->pix == (void *)-1) {
        s->pix = NULL;
        pointsrc_log("cursor: mmap %s (%ld bytes): %s\n", FB_DEV, len, strerror(errno));
        return -1;
    }

    pointsrc_log("cursor: %dx%d %d bpp pitch %d at %p; drawing a pointer\n",
                 s->fb_w, s->fb_h, s->bpp, s->pitch, s->pix);
    return 0;
}

/* Logical pointer position -> a pixel on this fb. The two agree exactly on the
 * measured Pi (the forced mode makes the fb rbp's logical surface), but the
 * scale is applied anyway so a panel that came up at 1280x720 puts the arrow
 * where the click lands rather than 40 rows below it. */
static int scale_to(int v, int logical, int real)
{
    int r = (int)((long)v * real / logical);
    if (r < 0)
        r = 0;
    if (r > real - 1)
        r = real - 1;
    return r;
}

static void *cursor_thread(void *arg)
{
    long period_us;
    (void)arg;

    /* Seconds are too coarse and integers too blunt for this one: the useful
     * range is 0.05-2 ms, and env_int() would round every value below a
     * millisecond to zero. env_double() shares env_int()'s contract — unset,
     * empty or unparseable all fall back to the default rather than to 0 — so a
     * typo in rb.local.conf cannot turn this into a busy loop. */
    period_us = (long)(env_double("POINT_CURSOR_MS", CURSOR_MS_DEFAULT) * 1000.0);
    if (period_us < CURSOR_US_MIN)
        period_us = CURSOR_US_MIN;

    if (map_fb(&g) != 0)
        return NULL;

    for (;;) {
        int lx, ly, down, fx, fy;

        if (!pointsrc_cursor(&lx, &ly, &down)) {
            /* Nothing relative attached. If an arrow is still on the screen —
             * the mouse was just unplugged — take it back off. */
            if (g.drawn) {
                cursor_restore(g.pix, g.pitch, g.fb_w, g.fb_h, g.bpp,
                               g.x, g.y, g.pressed, g.saved);
                g.drawn = 0;
            }
            usleep((useconds_t)period_us);
            continue;
        }

        fx = scale_to(lx, POINT_LOGICAL_W, g.fb_w);
        fy = scale_to(ly, POINT_LOGICAL_H, g.fb_h);

        /* A move (or a press, which inverts the arrow — the only click feedback
         * there is) has to take the old arrow off first: its cells are at the old
         * position, and the restore is what puts rbp's pixels back there. */
        if (g.drawn && (fx != g.x || fy != g.y || down != g.pressed))
            cursor_restore(g.pix, g.pitch, g.fb_w, g.fb_h, g.bpp,
                           g.x, g.y, g.pressed, g.saved);

        /* Then paint, every tick, whether or not anything moved. This is the
         * mechanism that keeps the arrow on the screen: rbp paints its UI into
         * this same page, so any redraw over the arrow erases it. Painting
         * unconditionally is cheap and self-healing — cursor_paint() writes only
         * the cells that are not already ours, so an intact arrow costs a read
         * pass and an erased one comes straight back — and it is also more
         * reliable than asking "is the arrow still there?" first, because that
         * question has no trustworthy answer: our outline is black, and so is a
         * good deal of rbp's UI (see cursor_paint.h). */
        cursor_paint(g.pix, g.pitch, g.fb_w, g.fb_h, g.bpp,
                     fx, fy, down, g.saved);
        if (!g.drawn)
            pointsrc_log("cursor: first paint at (%d,%d)", fx, fy);
        g.x = fx;
        g.y = fy;
        g.pressed = down;
        g.drawn = 1;

        usleep((useconds_t)period_us);
    }
    return NULL;
}

int fb_cursor_start(void)
{
    static int started;
    pthread_t tid;

    if (started)
        return 0;
    started = 1;

    if (!env_flag("POINT_CURSOR", 1)) {
        pointsrc_log("cursor: POINT_CURSOR=0 — no pointer is drawn\n");
        return 0;
    }
    if (pthread_create(&tid, NULL, cursor_thread, NULL) != 0) {
        started = 0;
        pointsrc_log("cursor: cannot start the paint thread: %s\n", strerror(errno));
        return -1;
    }
    pthread_detach(tid);
    return 0;
}
