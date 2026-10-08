/* fb_shim.c — the framebuffer half of the old fbshim-tsc.c, plus the five
 * interposed libc entry points that route everything else.
 *
 * What it still does, and why each one is load-bearing:
 *
 *   FBIOGET_VSCREENINFO -> lies: 1280x800, XRGB at 32 bpp -- the default, and the
 *     depth the player itself runs at -- or RGB565 at 16 bpp when FB_LIE_BPP=16.
 *     rbp and DirectFB both size their layers from this. The lie is what makes
 *     the logical geometry 1280x800 regardless of what the panel actually is,
 *     and the DirectFB fbdev driver reads the *real* geometry with a raw
 *     syscall() to bypass it. Do not "fix" this.
 *   FBIOGET_FSCREENINFO -> line_length = 1280 * the lied bpp / 8: 5120 at 32 bpp,
 *     2560 at 16, because the depth and the pitch must move together.
 *   FBIOPUT_VSCREENINFO -> accepted and ignored: the real mode is the kernel's.
 *   FBIOPAN_DISPLAY -> the frame pacer (see below). Not reached on this unit.
 *   FBIO_WAITFORVSYNC -> the top menu's compositor, before the wait (see below).
 *   /dev/mem -> EACCES, which is how an mmap of it ends up anonymous.
 *   gpiodrv -> one zero byte per read, and poll() sleeps instead of spinning.
 *
 * The pan pacer: vc4's drm_fbdev returns from FBIOPAN_DISPLAY without waiting
 * for vblank, exactly as the Rockchip driver did, so whichever thread drives the
 * present path spins as fast as it can. With rbp's render thread at SCHED_FIFO
 * 98 on core 0, that starved gui_task and took the frame rate to ~7 fps. Pacing
 * each pan to a 60 Hz budget is what fixed it. It is env-gated as PAN_PACER_MS
 * (fractional, default 16.666666; 0 disables it) so the behaviour can be turned
 * off for an experiment without a rebuild.
 *
 * MEASURED 2026-09-29, and it corrects what this comment used to claim: with
 * RB_DFB_PRESENT=off, FBIOPAN_DISPLAY is never issued on this unit at all. Every
 * distinct framebuffer ioctl that reached this switch over a whole session was
 * logged at its first occurrence -- 0x4600 FBIOGET_VSCREENINFO, 0x4602
 * FBIOGET_FSCREENINFO, 0x4601 FBIOPUT_VSCREENINFO, 0x4604 FBIOGETCMAP, 0x4605
 * FBIOPUTCMAP and 0x40044620 FBIO_WAITFORVSYNC -- and 0x4606 is not among them.
 * It is not DirectFB's panstep early-out either: this unit's vc4drmfb reports
 * xpanstep 1 / ypanstep 1, so that test passes; the call simply does not happen.
 * So on this configuration the pacer above is dead code, and the boundary that
 * really paces rbp's render thread is the vsync wait, at a measured 57.1/s. The
 * pacer is kept because it governs the present modes that do pan -- it is what
 * the docs/13 frame-rate fix rests on -- but nothing may hang off it, and the
 * top menu does not: it draws from the vsync case below.
 *
 * Removed in the M2a split: an unused `pthread_mutex_t fb_lock` (dead since the
 * pacer became lock-free — hence "zero mutexes" below), and the touch emulation,
 * which moved to tscfake.c/pointsrc.c. One stale comment went with it: the old
 * file header claimed the transform produced `lx = 1279 - ry*1280/2048`, while
 * transform() in the same file produced `lx = ry*1280/2048`. The code is what
 * ran and what is preserved; the in-code derivation next to it is the reasoned
 * one, and POINT_INVERT_X is the knob if it turns out to be wrong.
 */
#define _GNU_SOURCE
#include "tscfake.h"
#include "fbdev.h"
#include "menu_draw.h"
#include "syscalls.h"
#include "envutil.h"
#include "pointsrc.h"          /* pointsrc_log, for the vsync counter below */

#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_GPIO_FDS 256
static char is_gpio_fd[MAX_GPIO_FDS];

/* The depth rbp is TOLD the fb is, and the line length that goes with it. 32 is
 * the DEFAULT, and it is what rbp is actually run at: it is the depth the
 * rx3-handoff port renders its UI at, and the depth `.239` was measured to need
 * -- at 16 the deck-1 strip frame's right vertical comes out at 1/4 and 1/8
 * coverage instead of whole. 32 makes DirectFB's AUTO-FALLBACK pick `convert`:
 * a bpp-only mismatch at an unchanged geometry (fbdev.c), which is what lets the
 * layer surface be 32 bpp while the panel stays 16. 16 is kept because it is the
 * RX3's own framebuffer and the configuration everything else was verified on.
 *
 * The fallback here is only reached when the variable is absent ENTIRELY, which
 * the launcher never does -- rb.conf always sets it and start-rb.sh's SHIM_VARS
 * always exports it. It is 32 so that it agrees with rb.conf's default: the two
 * disagreeing is the one way this can run silently wrong, since rbp's own
 * pixel-format word must say the same thing (its DS_HW driver plugin refuses the
 * layer otherwise) and nothing here can see that word. Read the shim-side name:
 * rb.conf's RB_FB_LIE_BPP reaches it through SHIM_VARS. */
static int fb_lie_bpp(void)
{
    return strcmp(env_str("FB_LIE_BPP", "32"), "32") == 0 ? 32 : 16;
}

/* Every consumer of FSCREENINFO (DirectFB's pitch, rbp's stride arithmetic)
 * assumes the logical width, not the panel's. The depth must move with it: the
 * driver tells a format-only mismatch from a geometry one by the PIXEL stride
 * (pitch / bpp), so 32 bpp with a 2560-byte line reads as 640 px against the
 * real 1280 and selects `scale` instead of `convert`. */
#define FB_LOGICAL_LINE_LENGTH (1280 * fb_lie_bpp() / 8)

/* The toolchain's linux/fb.h predates this uapi entry, so it is spelled out here
 * -- and its value is not a guess: _IOW('F', 0x20, __u32) is (1<<30)|(4<<16)|
 * ('F'<<8)|0x20 = 0x40044620, which is the number measured arriving at this
 * switch from rbp's render thread at 57.1/s. The case below is dead code if this
 * is ever wrong, and dead code here means the top menu loses its frame boundary,
 * not that anything breaks. */
#ifndef FBIO_WAITFORVSYNC
#define FBIO_WAITFORVSYNC 0x40044620UL
#endif

#define PAN_PACER_DEFAULT_MS 16.666666

static long pan_pacer_ns(void)
{
    static long cached = -1;
    if (cached < 0) {
        double ms = env_double("PAN_PACER_MS", PAN_PACER_DEFAULT_MS);
        cached = (long)(ms * 1000000.0);
        if (cached < 0)
            cached = 0;
    }
    return cached;
}

/* The frame rate, counted where it is cheapest and on the thread that actually
 * draws it. The FBIO_WAITFORVSYNC case below IS rbp's frame boundary -- that
 * case documents the measurement: one issue per frame, ~57/s -- so counting the
 * calls and printing once a second gives the frame rate directly, rather than
 * inferring it from the process's CPU. That distinction is the whole point of
 * the instrument: it is the one number that says whether a heavier present path
 * costs *frames* or only cycles.
 *
 * Env-gated by FB_VSYNC_RATE, because the read is a clock_gettime and the print
 * is a lock plus a write, on rbp's SCHED_FIFO 98 render thread -- cheap, but not
 * free enough to leave on. Set once and cached: a getenv() in this path is a
 * lock and a write to the process's environment block from the render thread.
 *
 * A DIAGNOSTIC, deliberately, and so it must NOT go into start-rb.sh's
 * SHIM_VARS: that loop exports every name in its list unconditionally, so an
 * ad-hoc `FB_VSYNC_RATE=1 sh start-rb.sh` would be overwritten with the empty
 * string. Set it in the launcher's environment instead -- a systemd drop-in
 * carrying `Environment=FB_VSYNC_RATE=1` reaches rbp, because nothing in the
 * launcher uses `env -i`. Same rule as POINT_MENU_MS and MIDI_DUMP's contents.
 *
 * The window is held in MILLISECONDS IN A 64-BIT TYPE, and that is not style:
 * `long` is 4 bytes in this soft-float ARM32 build, so a nanosecond
 * CLOCK_MONOTONIC held in one wraps every 4.29 s
 * (rblive4-32bit-long-clock-wrap). */
static void vsync_rate_tick(void)
{
    static int enabled = -1;
    static unsigned long long window_start_ms;
    static unsigned int frames;
    struct timespec now;
    unsigned long long ms, elapsed;

    if (enabled < 0)
        enabled = env_flag("FB_VSYNC_RATE", 0);
    if (!enabled)
        return;

    frames++;
    clock_gettime(CLOCK_MONOTONIC, &now);
    ms = (unsigned long long)now.tv_sec * 1000ULL
       + (unsigned long long)(now.tv_nsec / 1000000);
    if (window_start_ms == 0) {
        /* The first call starts the window rather than reporting a partial one:
         * a line covering the launch's slow first milliseconds would read as a
         * frame drop that never happened. */
        window_start_ms = ms;
        return;
    }
    elapsed = ms - window_start_ms;
    if (elapsed >= 1000ULL) {
        pointsrc_log("vsync: %u frames in %llu ms = %.1f/s",
                     frames, elapsed, (double)frames * 1000.0 / (double)elapsed);
        frames = 0;
        window_start_ms = ms;
    }
}

int open(const char *pathname, int flags, ...)
{
    mode_t mode = 0;
    va_list ap;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    /* rbp's user_space_rtc_init() mmaps /dev/mem to reach a hardware RTC. There
     * is no such mapping here, and refusing the open is what funnels that mmap
     * into the anonymous-memory path the audio shim then relies on. On a Pi it
     * would fail anyway (CONFIG_STRICT_DEVMEM), but only after a kernel warning. */
    if (pathname && strcmp(pathname, "/dev/mem") == 0) {
        errno = EACCES;
        return -1;
    }
    if (pathname && strcmp(pathname, TSC_DEVICE_PATH) == 0)
        return tscfake_open();
    int fd = real_open(pathname, flags, mode);
    if (fd >= 0 && fd < MAX_GPIO_FDS && pathname && strstr(pathname, "gpiodrv"))
        is_gpio_fd[fd] = 1;
    return fd;
}

int open64(const char *pathname, int flags, ...)
{
    mode_t mode = 0;
    va_list ap;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    if (pathname && strcmp(pathname, "/dev/mem") == 0) {
        errno = EACCES;
        return -1;
    }
    if (pathname && strcmp(pathname, TSC_DEVICE_PATH) == 0)
        return tscfake_open();
    int fd = real_open(pathname, flags | O_LARGEFILE, mode);
    if (fd >= 0 && fd < MAX_GPIO_FDS && pathname && strstr(pathname, "gpiodrv"))
        is_gpio_fd[fd] = 1;
    return fd;
}

ssize_t read(int fd, void *buf, size_t count)
{
    if (fd >= 0 && fd < MAX_GPIO_FDS && is_gpio_fd[fd] && count >= 1) {
        memset(buf, 0, 1);
        return 1;
    }
    if (tscfake_is_fd(fd))
        return tscfake_read(fd, buf, count);
    return real_read(fd, buf, count);
}

int close(int fd)
{
    if (fd >= 0 && fd < MAX_GPIO_FDS)
        is_gpio_fd[fd] = 0;
    if (tscfake_is_fd(fd))
        return tscfake_close(fd);
    return real_close(fd);
}

int poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
    if (fds && nfds == 1 && fds[0].fd >= 0 && fds[0].fd < MAX_GPIO_FDS && is_gpio_fd[fds[0].fd]) {
        /* GpioManager polls /dev/gpiodrv for hardware interrupts that do not
         * exist here. A regular file is always "ready", so without this the poll
         * returns immediately and the caller spins at 100% CPU — under
         * SCHED_FIFO 98, at the expense of the render and audio threads. */
        if (timeout < 0) {
            sleep(3600);
            return 0;
        } else if (timeout > 0) {
            usleep((useconds_t)timeout * 1000);
            return 0;
        } else {
            return 0;
        }
    }
    return real_poll(fds, nfds, timeout);
}

int ioctl(int fd, unsigned long request, ...)
{
    void *arg;
    va_list ap;
    va_start(ap, request);
    arg = va_arg(ap, void *);
    va_end(ap);

    /* The emulated tsc2007's ioctls. Answered for any fd on purpose: they are
     * unambiguous, and tracking which fd is the fake one across dup() and fork()
     * would be more machinery than the discrimination is worth. */
    if (tscfake_ioctl(request, arg))
        return 0;

    /* Everything outside the framebuffer ioctl range goes straight to the kernel
     * with no locks and no work in between: this is the path ALSA PCM, the ALSA
     * sequencer and every socket take, and it has to stay free for the audio and
     * event threads. (0x4600..0x4620 is the fb range; the tsc ioctls above are
     * 0x6b00-family and are handled before this test.) */
    if ((request & 0xffff) < 0x4600 || (request & 0xffff) > 0x4620)
        return real_ioctl(fd, request, arg);

    switch (request) {
    case FBIOGET_VSCREENINFO: {
        struct fb_var_screeninfo *v = arg;
        int res = real_ioctl(fd, request, v);
        if (res == 0 && v) {
            int bpp = fb_lie_bpp();

            v->xres = 1280; v->yres = 800;
            v->xres_virtual = 1280; v->yres_virtual = 800;
            v->bits_per_pixel = bpp;
            v->grayscale = 0; v->nonstd = 0;
            if (bpp == 32) {
                /* XRGB with no alpha channel, which is DirectFB's DSPF_RGB32 --
                 * the format rbp's DS_HW_Core_Layer_Create asks for when its
                 * pixel-format word carries the handoff's 0x00400c03. transp
                 * stays length 0 on purpose: an 8-bit alpha channel here would
                 * describe ARGB instead, and the whole point is that this and
                 * rbp's word agree. */
                v->red.offset = 16; v->red.length = 8; v->red.msb_right = 0;
                v->green.offset = 8; v->green.length = 8; v->green.msb_right = 0;
                v->blue.offset = 0; v->blue.length = 8; v->blue.msb_right = 0;
                v->transp.offset = 0; v->transp.length = 0; v->transp.msb_right = 0;
            } else {
                v->red.offset = 11; v->red.length = 5; v->red.msb_right = 0;
                v->green.offset = 5; v->green.length = 6; v->green.msb_right = 0;
                v->blue.offset = 0; v->blue.length = 5; v->blue.msb_right = 0;
                v->transp.offset = 0; v->transp.length = 0; v->transp.msb_right = 0;
            }
            v->rotate = 0;
        }
        return res;
    }
    case FBIOGET_FSCREENINFO: {
        struct fb_fix_screeninfo *f = arg;
        int res = real_ioctl(fd, request, f);
        if (res == 0 && f)
            f->line_length = FB_LOGICAL_LINE_LENGTH;
        return res;
    }
    case FBIOPUT_VSCREENINFO:
        return 0;
    case FBIO_WAITFORVSYNC:
        /* The frame boundary rbp actually drives its render loop from on this unit
         * -- measured at 57.1/s -- and, unlike the pan below, the boundary that is
         * really reached. menu_frame_tick() runs BEFORE the wait, not after, and
         * that is the whole reason this is the right side of rbp's frame: whether
         * the loop is `draw; wait` or `wait; draw`, the entry of the wait comes
         * immediately after a completed draw, so the panel is composited just
         * after rbp has finished a frame and then has the whole inter-frame gap to
         * itself. Painting after the wait would put it immediately before the next
         * draw, i.e. under it. menu_draw.c has the rest.
         *
         * The counter first, and it is the same frame boundary read as a rate:
         * with FB_VSYNC_RATE set this prints one "vsync: N frames in M ms" line
         * per second -- on rbp's own render thread, so the number is the rate
         * frames arrive at, not a rate inferred from CPU. */
        vsync_rate_tick();
        menu_frame_tick();
        return real_ioctl(fd, request, arg);
    case FBIOPAN_DISPLAY: {
        long pace_ns = pan_pacer_ns();
        if (pace_ns > 0) {
            static struct timespec last_pan;
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (last_pan.tv_sec > 0) {
                long elapsed_ns = (now.tv_sec - last_pan.tv_sec) * 1000000000L
                                + (now.tv_nsec - last_pan.tv_nsec);
                if (elapsed_ns > 0 && elapsed_ns < pace_ns) {
                    struct timespec req;
                    req.tv_sec = 0;
                    req.tv_nsec = pace_ns - elapsed_ns;
                    nanosleep(&req, NULL);
                }
            }
            clock_gettime(CLOCK_MONOTONIC, &last_pan);
        }
        return real_ioctl(fd, request, arg);
    }
    default:
        return real_ioctl(fd, request, arg);
    }
}
