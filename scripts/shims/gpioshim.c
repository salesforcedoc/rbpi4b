/*
 * gpioshim.c — LD_PRELOAD shim: make /dev/gpiodrv READS constant (all-zero)
 * inside rbp (and any other preloaded process).
 *
 * WHY: on the real XDJ-RX3, /dev/gpiodrv is an i.MX6 GPIO driver where reads
 * return the actual PIN state and writes set OUTPUT pins — read/write are
 * independent. On the Prime GO, /dev/gpiodrv is a plain regular-file stub
 * (created by fix-dev.sh), so a write() lands in the file and a subsequent
 * read() of the same offset returns what was WRITTEN. rbp's GpioManager
 * threads poll/read these bytes and fire a callback on CHANGE; rbp's UiMain
 * thread writes the same bytes every ~1s cycle. The result is a feedback
 * loop: UiMain's output writes are seen by the GpioManager as "input
 * changes" → callback → the mount-executor re-runs → after a USB mount the
 * UI update path hits a destroyed object → "pure virtual method called" →
 * libstdc++ terminate → SIGABRT (rbp dies ~4 min after mounting a stick).
 *
 * The USB mount flow itself does NOT need the GpioManager: the
 * UsbMountManager posts the mount message to mailbox 38 directly
 * (futex_wake on 0x2436ec8). So making the GpioManager never see a change
 * is safe: it only removes the spurious feedback events.
 *
 * FIX: intercept read() on /dev/gpiodrv fds and return constant 0 (the
 * "no input change" state) without touching the file. Writes pass through
 * untouched (harmless — reads no longer reflect them). dup/dup2/dup3 and
 * close keep the fd→gpiodrv map consistent.
 *
 * Build (soft-float, glibc-2.13 chroot compatible — MUST resolve only
 * GLIBC_2.0..2.4 symbols):
 *   arm-linux-gnueabi-gcc -O2 -mfloat-abi=soft -fno-stack-protector \
 *     -fPIC -shared -o gpioshim.so gpioshim.c -ldl -lc \
 *     -L/tmp/arm213sysroot/lib -Wl,-rpath-link,/tmp/arm213sysroot/lib
 *   # verify: arm-linux-gnueabi-objdump -T gpioshim.so | grep GLIBC
 *
 * Deploy: LD_PRELOAD=/usr/lib/fbshim.so:/usr/lib/knobshim.so:/usr/lib/gpioshim.so
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdarg.h>
#include <dlfcn.h>

#define MAX_FDS 4096
static char is_gpio[MAX_FDS];          /* fd -> is /dev/gpiodrv */
static char gpio_lazy;                 /* dlsym done flag */

static int (*real_open_fn)(const char *, int, ...);
static int (*real_openat_fn)(int, const char *, int, ...);
static ssize_t (*real_read_fn)(int, void *, size_t);
static int (*real_close_fn)(int);
static int (*real_dup_fn)(int);
static int (*real_dup2_fn)(int, int);
static int (*real_dup3_fn)(int, int, int);

static void resolve_all(void)
{
    if (gpio_lazy) return;
    gpio_lazy = 1;
    real_open_fn  = dlsym(RTLD_NEXT, "open");
    real_openat_fn = dlsym(RTLD_NEXT, "openat");
    real_read_fn  = dlsym(RTLD_NEXT, "read");
    real_close_fn = dlsym(RTLD_NEXT, "close");
    real_dup_fn   = dlsym(RTLD_NEXT, "dup");
    real_dup2_fn  = dlsym(RTLD_NEXT, "dup2");
    real_dup3_fn  = dlsym(RTLD_NEXT, "dup3");
}

static int path_is_gpiodrv(const char *p)
{
    size_t n;
    if (!p) return 0;
    n = strlen(p);
    if (n < 8) return 0;               /* "/dev/gpiodrv" = 12; just need suffix */
    return strcmp(p + n - 7, "gpiodrv") == 0;
}

static int set_fd(int fd)
{
    if (fd >= 0 && fd < MAX_FDS) { is_gpio[fd] = 1; return fd; }
    return fd;
}

int open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    int fd;
    resolve_all();
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    fd = real_open_fn(path, flags, mode);
    if (fd >= 0 && path_is_gpiodrv(path)) return set_fd(fd);
    return fd;
}

int open64(const char *path, int flags, ...)
{
    mode_t mode = 0;
    int fd;
    resolve_all();
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    fd = real_open_fn(path, flags, mode);
    if (fd >= 0 && path_is_gpiodrv(path)) return set_fd(fd);
    return fd;
}

int openat(int dirfd, const char *path, int flags, ...)
{
    mode_t mode = 0;
    int fd;
    resolve_all();
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    fd = real_openat_fn(dirfd, path, flags, mode);
    if (fd >= 0 && path_is_gpiodrv(path)) return set_fd(fd);
    return fd;
}

int openat64(int dirfd, const char *path, int flags, ...)
{
    mode_t mode = 0;
    int fd;
    resolve_all();
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    fd = real_openat_fn(dirfd, path, flags, mode);
    if (fd >= 0 && path_is_gpiodrv(path)) return set_fd(fd);
    return fd;
}

ssize_t read(int fd, void *buf, size_t count)
{
    resolve_all();
    if (fd >= 0 && fd < MAX_FDS && is_gpio[fd] && count >= 1) {
        /* Simulate a GPIO input pin that never changes: read returns 0. */
        memset(buf, 0, 1);
        return 1;
    }
    return real_read_fn(fd, buf, count);
}

int close(int fd)
{
    resolve_all();
    if (fd >= 0 && fd < MAX_FDS) is_gpio[fd] = 0;
    return real_close_fn(fd);
}

int dup(int oldfd)
{
    int n;
    resolve_all();
    n = real_dup_fn(oldfd);
    if (n >= 0 && n < MAX_FDS && oldfd >= 0 && oldfd < MAX_FDS)
        is_gpio[n] = is_gpio[oldfd];
    return n;
}

int dup2(int oldfd, int newfd)
{
    int n;
    resolve_all();
    n = real_dup2_fn(oldfd, newfd);
    if (n >= 0 && n < MAX_FDS && oldfd >= 0 && oldfd < MAX_FDS)
        is_gpio[n] = is_gpio[oldfd];
    return n;
}

int dup3(int oldfd, int newfd, int flags)
{
    int n;
    resolve_all();
    n = real_dup3_fn(oldfd, newfd, flags);
    if (n >= 0 && n < MAX_FDS && oldfd >= 0 && oldfd < MAX_FDS)
        is_gpio[n] = is_gpio[oldfd];
    return n;
}
