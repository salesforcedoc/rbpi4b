/* fb_shim.c — the framebuffer half of the old fbshim-tsc.c, plus the five
 * interposed libc entry points that route everything else.
 *
 * What it still does, and why each one is load-bearing:
 *
 *   FBIOGET_VSCREENINFO -> lies: 1280x800, 16 bpp, RGB565.
 *     rbp and DirectFB both size their layers from this. The lie is what makes
 *     the logical geometry 1280x800 regardless of what the panel actually is,
 *     and the DirectFB fbdev driver reads the *real* geometry with a raw
 *     syscall() to bypass it. Do not "fix" this.
 *   FBIOGET_FSCREENINFO -> line_length = 2560 (1280 * 16 bpp / 8).
 *   FBIOPUT_VSCREENINFO -> accepted and ignored: the real mode is the kernel's.
 *   FBIOPAN_DISPLAY -> the frame pacer (see below).
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
#include "syscalls.h"
#include "envutil.h"

#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef FBIOGET_VSCREENINFO
#define FBIOGET_VSCREENINFO 0x4600
#define FBIOPUT_VSCREENINFO 0x4601
#define FBIOGET_FSCREENINFO 0x4602
#define FBIOPAN_DISPLAY     0x4606
#endif

/* Hand-rolled rather than taken from <linux/fb.h>: these are the layout rbp was
 * compiled against (32-bit `unsigned long smem_start`), and the build host's
 * headers are not guaranteed to agree. */
struct fb_var_screeninfo {
    unsigned int xres, yres, xres_virtual, yres_virtual, xoffset, yoffset;
    unsigned int bits_per_pixel, grayscale;
    struct { unsigned int offset, length, msb_right; } red, green, blue, transp;
    unsigned int nonstd;
    unsigned int activate;
    unsigned int height, width;
    unsigned int accel_flags;
    unsigned int pixclock, left_margin, right_margin, upper_margin, lower_margin;
    unsigned int hsync_len, vsync_len, sync, vmode;
    unsigned int rotate;
    unsigned int colorspace;
    unsigned int reserved[4];
};

struct fb_fix_screeninfo {
    char id[16];
    unsigned long smem_start;
    unsigned int smem_len;
    unsigned int type;
    unsigned int type_aux;
    unsigned int visual;
    unsigned short xpanstep, ypanstep, ywrapstep;
    unsigned int line_length;
    unsigned long mmio_start;
    unsigned int mmio_len;
    unsigned int accel;
    unsigned short capabilities;
    unsigned short reserved[2];
};

#define MAX_GPIO_FDS 256
static char is_gpio_fd[MAX_GPIO_FDS];

/* 1280 * 16 / 8. Every consumer of FSCREENINFO (DirectFB's pitch, rbp's stride
 * arithmetic) assumes the logical width, not the panel's. */
#define FB_LOGICAL_LINE_LENGTH 2560

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
            v->xres = 1280; v->yres = 800;
            v->xres_virtual = 1280; v->yres_virtual = 800;
            v->bits_per_pixel = 16;
            v->grayscale = 0; v->nonstd = 0;
            v->red.offset = 11; v->red.length = 5; v->red.msb_right = 0;
            v->green.offset = 5; v->green.length = 6; v->green.msb_right = 0;
            v->blue.offset = 0; v->blue.length = 5; v->blue.msb_right = 0;
            v->transp.offset = 0; v->transp.length = 0; v->transp.msb_right = 0;
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
