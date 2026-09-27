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

/* The fb the driver opens, and so the fb the arrow must be drawn on. It was
 * hard-coded here while rb.conf, start-rb.sh and fbdev.c all carried the knob
 * end-to-end, which made RB_FB_DEV=/dev/fb1 move the driver and leave the arrow
 * painting into fb0. From RB_FB_DEV, which SHIM_VARS exports as FB_DEV. */
#define FB_DEV_DEFAULT "/dev/fb0"
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

/* How long to wait between attempts while there is no usable framebuffer.  A
 * monitor plugged in long after boot -- or one whose fb the kernel tore down and
 * rebuilt -- must not cost the arrow for the life of the process, and 5 s is
 * slower than any of those transitions and faster than an impatient operator. */
#define CURSOR_RETRY_US 5000000L

struct cursor_state {
    void *pix;
    int fb_w, fb_h, bpp;
    int pitch;                                  /* in pixels, not bytes */
    /* Where the present path puts the UI inside that fb. On a panel that is
     * already the logical 1280x800 this is the whole page -- dw==fb_w, dh==fb_h,
     * bx==by==0 -- so every coordinate below is the identity and this file draws
     * exactly what it drew before it knew about other panels. Computed once, by
     * map_fb(), out of point_fit(). */
    int dw, dh, bx, by;
    unsigned int saved[CURSOR_W * CURSOR_H];
    int drawn, x, y, pressed;
};

static struct cursor_state g;

/* Open and map the fb, and work out the rectangle the UI occupies inside it.
 *
 * `loud` is what keeps a persistent failure from filling the log one line per
 * 5 s for the life of the session: the caller passes it true for the first
 * attempt and then once a minute, and every diagnostic below is gated on it.
 * The return value is what the caller retries on, so silence here never means
 * success. */
static int map_fb(struct cursor_state *s, const char *dev, int loud)
{
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    long len;
    int fd, stretch;

    fd = real_open(dev, O_RDWR, 0);
    if (fd < 0) {
        if (loud)
            pointsrc_log("cursor: open %s: %s (no pointer will be drawn yet)\n",
                         dev, strerror(errno));
        return -1;
    }
    memset(&var, 0, sizeof var);
    memset(&fix, 0, sizeof fix);
    if (real_ioctl(fd, FBIOGET_VSCREENINFO, &var) != 0 ||
        real_ioctl(fd, FBIOGET_FSCREENINFO, &fix) != 0) {
        if (loud)
            pointsrc_log("cursor: %s is not a framebuffer (%s)\n", dev, strerror(errno));
        real_close(fd);
        return -1;
    }

    /* The two formats this target can produce: vc4drmfb gives RGB565 here, and
     * 32 bpp is the other thing drm_fb_helper is asked for. Anything else is
     * refused rather than guessed at, because a wrong pitch or format draws
     * stripes over the UI instead of an arrow. */
    if (var.bits_per_pixel != 16 && var.bits_per_pixel != 32) {
        if (loud)
            pointsrc_log("cursor: %d bpp is not a format this can draw in\n",
                         var.bits_per_pixel);
        real_close(fd);
        return -1;
    }
    if (var.xres == 0 || var.yres == 0 || fix.line_length == 0) {
        if (loud)
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
        if (loud)
            pointsrc_log("cursor: mmap %s (%ld bytes): %s\n", dev, len, strerror(errno));
        return -1;
    }

    /* The rectangle the present path blits the UI into, so the arrow follows the
     * picture instead of spreading across the whole page. It is point_fit() -- the
     * same rule as the driver's fbdev_present_fit(), and see that header for why
     * the two copies must move together -- and on a matching fb it is the identity.
     *
     * GEOMETRY decides it, deliberately, and not the mode name. The mode the
     * driver actually presents with is not visible from here: rb.conf ships
     * DFB_PRESENT=off and the driver upgrades that to its scale rung by itself
     * when the fb disagrees with the shim's 1280x800, so reading the env would
     * say "off" on precisely the panel that needs the fit. Geometry gives the
     * right answer for everything that is reached automatically, because each of
     * those puts the picture in this rectangle on a mismatched fb and in the
     * whole page on a matching one.
     *
     * What it does not give is the right rectangle for an explicitly selected
     * letterbox, crop or rotate on a mismatched fb: those have rectangles of their
     * own, and the arrow can land outside the picture under them. Nothing selects
     * them -- the automatic upgrade never does -- and the arrow was already wrong
     * under them, so that is a known limitation rather than a regression. Doing it
     * properly means the driver publishing its choice somewhere this process can
     * read, which is a larger change than the arrow is worth.
     *
     * DFB_PRESENT_FIT is the one thing here that is a policy and not a geometry:
     * "stretch" makes the driver fill both axes, which is the whole page, and the
     * arrow has to be told. */
    stretch = strcmp(env_str("DFB_PRESENT_FIT", "fit"), "stretch") == 0;
    point_fit(POINT_LOGICAL_W, POINT_LOGICAL_H, s->fb_w, s->fb_h, stretch,
              &s->dw, &s->dh, &s->bx, &s->by);

    if (loud)
        pointsrc_log("cursor: %dx%d %d bpp pitch %d at %p; picture %dx%d at %d,%d%s; "
                     "drawing a pointer\n",
                     s->fb_w, s->fb_h, s->bpp, s->pitch, s->pix,
                     s->dw, s->dh, s->bx, s->by, stretch ? " (stretched)" : "");
    return 0;
}

/* Logical pointer position -> a pixel inside the picture rectangle. `real` is the
 * rectangle's width or height (g.dw/g.dh), not the page's, so the result is
 * relative to the picture's top-left and the caller adds g.bx/g.by. On the
 * measured Pi the rectangle is the whole page and this is the identity; on a
 * panel that came up at another size it is what puts the arrow where the click
 * lands rather than a proportional distance off it.
 *
 * Clamping to real-1 is the second half of the confinement: the logical position
 * is already clamped to the logical space, but a rounding that landed on the
 * last row/column of the picture must not spill into the bar past it. */
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
    const char *dev;
    long period_us;
    int attempts;
    (void)arg;

    /* Seconds are too coarse and integers too blunt for this one: the useful
     * range is 0.05-2 ms, and env_int() would round every value below a
     * millisecond to zero. env_double() shares env_int()'s contract — unset,
     * empty or unparseable all fall back to the default rather than to 0 — so a
     * typo in rb.local.conf cannot turn this into a busy loop. */
    period_us = (long)(env_double("POINT_CURSOR_MS", CURSOR_MS_DEFAULT) * 1000.0);
    if (period_us < CURSOR_US_MIN)
        period_us = CURSOR_US_MIN;

    dev = env_str("FB_DEV", FB_DEV_DEFAULT);

    /* Retry rather than exit, which is pointsrc.c's rule and for the same reason:
     * a monitor that appears long after boot, or a framebuffer the kernel tears
     * down and rebuilds on a hotplug, must not cost the arrow for the life of the
     * process. There is no second chance -- fb_cursor_start() has already set
     * `started`, so nothing will call this again.
     *
     * The log is loud on the first attempt and then once a minute (60 x 5 s), so a
     * permanently absent fb costs one line a minute rather than one per attempt.
     * Note this is NOT what recovers a geometry change: that is display-watch.sh's
     * restart, because rbp's own present path holds the same stale geometry and a
     * correct arrow over a sheared picture would not be a fix. */
    attempts = 0;
    while (map_fb(&g, dev, attempts == 0 || attempts % 60 == 0) != 0) {
        attempts++;
        usleep(CURSOR_RETRY_US);
    }

    for (;;) {
        /* The picture's top-left inside the page. Nothing else is needed to keep
         * the arrow inside the picture: cursor_paint() and cursor_restore() clamp
         * their x and y to the width and height they are handed, so giving them
         * the rectangle instead of the page confines the arrow to it -- they never
         * have to know that bars exist. */
        unsigned char *base = (unsigned char *)g.pix
                              + (size_t)g.by * (size_t)g.pitch * (size_t)(g.bpp / 8)
                              + (size_t)g.bx * (size_t)(g.bpp / 8);
        int lx, ly, down, fx, fy;

        if (!pointsrc_cursor(&lx, &ly, &down)) {
            /* Nothing relative attached. If an arrow is still on the screen —
             * the mouse was just unplugged — take it back off. */
            if (g.drawn) {
                cursor_restore(base, g.pitch, g.dw, g.dh, g.bpp,
                               g.x, g.y, g.pressed, g.saved);
                g.drawn = 0;
            }
            usleep((useconds_t)period_us);
            continue;
        }

        fx = scale_to(lx, POINT_LOGICAL_W, g.dw);
        fy = scale_to(ly, POINT_LOGICAL_H, g.dh);

        /* A move (or a press, which inverts the arrow — the only click feedback
         * there is) has to take the old arrow off first: its cells are at the old
         * position, and the restore is what puts rbp's pixels back there. */
        if (g.drawn && (fx != g.x || fy != g.y || down != g.pressed))
            cursor_restore(base, g.pitch, g.dw, g.dh, g.bpp,
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
        cursor_paint(base, g.pitch, g.dw, g.dh, g.bpp,
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
