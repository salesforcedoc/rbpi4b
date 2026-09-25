/* fbshim-tsc.c — combined LD_PRELOAD shim for the Prime GO.
 *
 * PART 1 (fb): make rbp (and DirectFB) believe the framebuffer is 1280x800 /
 * pitch 5120 (logical layer geometry) while the physical fb (800x1280) is
 * left untouched — the fbdev driver presents the rotated image.
 *
 * PART 2 (touch): emulate the XDJ-RX3's custom tsc2007 touch device
 * (/dev/tsc2007_2-0048) by translating the ILI2117 evdev touchscreen
 * (/dev/input/event0):
 *   ioctl(fd, 0x80046b00, &x) -> return max X (=3)
 *   ioctl(fd, 0x40046b00, &x) -> accept
 *   ioctl(fd, 0x80026b01, &y) -> return max Y (=3900)
 *   ioctl(fd, 0x40026b01, &y) -> accept
 *   read(fd, buf, 6) -> ts_data { u8 flag; u8 pad; u16 x; u16 y }
 * Coordinates are translated to the logical 1280x800 space:
 *   raw rx,ry in [0,2048): lx = 1279 - ry*1280/2048, ly = rx*800/2048
 *
 * Build: arm-linux-gnueabi-gcc -O2 -mfloat-abi=soft -fno-stack-protector
 *   -fPIC -shared -o fbshim.so fbshim-tsc.c -L/tmp/arm213sysroot/usr/lib
 *   -L/tmp/arm213sysroot/lib -lc -lpthread -Wl,--allow-shlib-undefined
 *   -Wl,-rpath-link,/tmp/arm213sysroot/lib
 */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <stdint.h>
#include <poll.h>

#ifndef FBIOGET_VSCREENINFO
#define FBIOGET_VSCREENINFO 0x4600
#define FBIOPUT_VSCREENINFO 0x4601
#define FBIOGET_FSCREENINFO 0x4602
#define FBIOPAN_DISPLAY     0x4606
#endif

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

static pthread_mutex_t fb_lock = PTHREAD_MUTEX_INITIALIZER;

/* ============ real syscalls (no libdl) ============ */
static int real_ioctl(int fd, unsigned long request, void *arg)
{
    return syscall(SYS_ioctl, fd, request, arg);
}
static int real_open(const char *p, int flags)
{
    return syscall(SYS_openat, AT_FDCWD, p, flags, 0);
}
static int real_read(int fd, void *buf, size_t n)
{
    return syscall(SYS_read, fd, buf, n);
}
static int real_close(int fd)
{
    return syscall(SYS_close, fd);
}
static int real_poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
    return syscall(SYS_poll, fds, nfds, timeout);
}
static int real_pipe2(int fds[2])
{
    return syscall(SYS_pipe2, fds, 0);
}

/* ============ PART 2: tsc2007 emulation ============ */
#define TSC_DEVICE  "/dev/tsc2007_2-0048"
#define EVDEV_PATH  "/dev/input/event0"
#define RAW_MAX     2048
#define TSC_MAX_X   3
#define TSC_MAX_Y   3900

struct input_event {
    struct { uint32_t sec, usec; } time;
    uint16_t type, code;
    int32_t  value;
};
#define EV_KEY 0x01
#define EV_ABS 0x03
#define EV_SYN 0x00
#define ABS_X  0x00
#define ABS_Y  0x01
#define BTN_TOUCH 0x14a
#define SYN_REPORT 0

static int  evfd = -1;
static int  out_pipe[2] = { -1, -1 };   /* [0] read end (dup'd for rbp), [1] write end */
static pthread_t reader_tid;
static int  reader_started = 0;
static int  cur_flag = 0;

static int  fake_pipe_rd[64];
static int  fake_used[64];

#define MAX_GPIO_FDS 256
static char is_gpio_fd[MAX_GPIO_FDS];

static void transform(int rx, int ry, int *lx, int *ly)
{
    /* Physical touchscreen: ILI2117 [0, 2048) on 800x1280 portrait panel.
     * DirectFB rot=left maps logical (1280x800) to physical (800x1280) via:
     *   px = ly, py = 1279 - lx
     * In Pioneer firmware (commRxDataProc @ 0x2d74b0):
     *   calX = 1280 - rawX  (because invertX = 1)
     *   calY = rawY         (because invertY = 0)
     * To make calX equal logical screen_x (1279 - py):
     *   1280 - rawX = 1279 - py  =>  rawX = py + 1 (approx py)
     * To make calY equal logical screen_y (px):
     *   calY = rawY = px         =>  rawY = px
     */
    int px = (rx * 800) / RAW_MAX;
    int py = (ry * 1280) / RAW_MAX;
    if (px < 0) px = 0; else if (px > 799) px = 799;
    if (py < 0) py = 0; else if (py > 1279) py = 1279;
    *lx = py;
    *ly = px;
}

static void push_touch(int flag, int x, int y)
{
    unsigned char buf[6];
    buf[0] = (unsigned char)(flag ? 1 : 0);
    buf[1] = 0;
    buf[2] = (unsigned char)(x & 0xff);
    buf[3] = (unsigned char)((x >> 8) & 0xff);
    buf[4] = (unsigned char)(y & 0xff);
    buf[5] = (unsigned char)((y >> 8) & 0xff);
    if (out_pipe[1] >= 0)
        (void)write(out_pipe[1], buf, 6);
}

static void *reader_thread(void *arg)
{
    struct input_event ev;
    int rx = 0, ry = 0;
    int last_flag = -1, last_x = 0, last_y = 0;
    (void)arg;

    for (;;) {
        int n = real_read(evfd, &ev, sizeof(ev));
        if (n != (int)sizeof(ev)) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN))
                continue;
            real_close(evfd);
            evfd = -1;
            while (evfd < 0) {
                evfd = real_open(EVDEV_PATH, O_RDWR);
                if (evfd < 0)
                    usleep(500000);
            }
            continue;
        }
        switch (ev.type) {
        case EV_ABS:
            if (ev.code == ABS_X || ev.code == 0x35) rx = ev.value;
            else if (ev.code == ABS_Y || ev.code == 0x36) ry = ev.value;
            else if (ev.code == 0x39) {
                /* ABS_MT_TRACKING_ID: >= 0 is down, -1 is up */
                cur_flag = (ev.value >= 0) ? 1 : 0;
            }
            break;
        case EV_KEY:
            if (ev.code == BTN_TOUCH)
                cur_flag = ev.value ? 1 : 0;
            break;
        case EV_SYN:
            if (ev.code == SYN_REPORT) {
                int lx, ly;
                transform(rx, ry, &lx, &ly);
                if (cur_flag != last_flag || lx != last_x || ly != last_y) {
                    if (cur_flag && !last_flag) {
                        /* Touch down: send burst of 2 frames so TouchAdValueHysteresis
                         * (which zeros the first frame as debounce) transitions 0->1->2 immediately */
                        push_touch(1, lx, ly);
                        push_touch(1, lx, ly);
                    } else {
                        push_touch(cur_flag, lx, ly);
                    }
                    last_flag = cur_flag;
                    last_x = lx;
                    last_y = ly;
                }
            }
            break;
        default:
            break;
        }
    }
    return NULL;
}

static void ensure_reader(void)
{
    if (reader_started)
        return;
    evfd = real_open(EVDEV_PATH, O_RDWR);
    if (evfd < 0)
        return;
    if (real_pipe2(out_pipe) < 0) {
        real_close(evfd);
        evfd = -1;
        return;
    }
    if (pthread_create(&reader_tid, NULL, reader_thread, NULL) != 0) {
        real_close(out_pipe[0]);
        real_close(out_pipe[1]);
        out_pipe[0] = out_pipe[1] = -1;
        real_close(evfd);
        evfd = -1;
        return;
    }
    reader_started = 1;
}

static int tsc_open_impl(void)
{
    int i, rd_end;
    ensure_reader();
    if (out_pipe[0] < 0)
        return -1;
    rd_end = dup(out_pipe[0]);
    if (rd_end < 0)
        return -1;
    for (i = 0; i < 64; i++) {
        if (!fake_used[i]) {
            fake_used[i] = 1;
            fake_pipe_rd[i] = rd_end;
            return rd_end;
        }
    }
    real_close(rd_end);
    return -1;
}

/* ============ entry points ============ */

int open(const char *pathname, int flags, ...)
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
    if (strcmp(pathname, TSC_DEVICE) == 0)
        return tsc_open_impl();
    int fd = real_open(pathname, flags);
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
    if (strcmp(pathname, TSC_DEVICE) == 0)
        return tsc_open_impl();
    int fd = real_open(pathname, flags | O_LARGEFILE);
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
    if (fd >= 0 && fd < 64 && fake_used[fd] && fake_pipe_rd[fd] == fd)
        return real_read(fake_pipe_rd[fd], buf, count);
    return real_read(fd, buf, count);
}

int close(int fd)
{
    if (fd >= 0 && fd < MAX_GPIO_FDS)
        is_gpio_fd[fd] = 0;
    if (fd >= 0 && fd < 64 && fake_used[fd] && fake_pipe_rd[fd] == fd) {
        fake_used[fd] = 0;
        return real_close(fake_pipe_rd[fd]);
    }
    return real_close(fd);
}

int poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
    if (fds && nfds == 1 && fds[0].fd >= 0 && fds[0].fd < MAX_GPIO_FDS && is_gpio_fd[fds[0].fd]) {
        /* GpioManager is polling /dev/gpiodrv for hardware interrupts that never occur on Prime GO.
         * Without this, polling on a regular file stub spins at 100% CPU under SCHED_FIFO RT priority 98
         * on Core 0, starving gui_task and cutting display frame rate from 60 FPS down to ~7 FPS. */
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

    /* tsc2007 emulated ioctls (only meaningful on our fake fds, but
       harmless to answer for any fd since rbp only issues them on the
       fake touch fd) */
    switch (request) {
    case 0x80046b00: {            /* _IOR(0x6b,0,4): read max X */
        unsigned int *p = (unsigned int *)arg;
        if (p) *p = TSC_MAX_X;
        return 0;
    }
    case 0x40046b00:              /* _IOW(0x6b,0,4): write max X */
        return 0;
    case 0x80026b01: {            /* _IOR(0x6b,1,2): read max Y */
        unsigned short *p = (unsigned short *)arg;
        if (p) *p = TSC_MAX_Y;
        return 0;
    }
    case 0x40026b01:              /* _IOW(0x6b,1,2): write max Y */
        return 0;
    default:
        break;
    }

    /* Fast-path non-framebuffer ioctls (ALSA PCM, ALSA sequencer, sockets, etc.)
     * without any locks or overhead so audio and event threads run at full hardware speed. */
    if ((request & 0xffff) < 0x4600 || (request & 0xffff) > 0x4620) {
        return real_ioctl(fd, request, arg);
    }

    /* Framebuffer ioctls */
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
            f->line_length = 2560;
        return res;
    }
    case FBIOPUT_VSCREENINFO:
        return 0;
    case FBIOPAN_DISPLAY: {
        /* Lock-free high-precision 60 FPS pacing for DirectFB.
         * Rockchip DRM returns immediately from FBIOPAN_DISPLAY.
         * Pace each flip with nanosecond clock_nanosleep so gui_task renders at 60 FPS
         * without burning 100% CPU on Core 0. Zero mutexes, no audio/event stalling. */
        static struct timespec last_pan;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (last_pan.tv_sec > 0) {
            long elapsed_ns = (now.tv_sec - last_pan.tv_sec) * 1000000000L + (now.tv_nsec - last_pan.tv_nsec);
            if (elapsed_ns > 0 && elapsed_ns < 16666666L) {
                struct timespec req = { 0, 16666666L - elapsed_ns };
                nanosleep(&req, NULL);
            }
        }
        clock_gettime(CLOCK_MONOTONIC, &last_pan);
        return real_ioctl(fd, request, arg);
    }
    default:
        return real_ioctl(fd, request, arg);
    }
}
