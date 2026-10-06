/*
 * syscalls.h — the shim's escape hatch back to the real kernel entry points.
 *
 * A preloaded shim that interposes open/read/ioctl cannot call those names to do
 * its own real work: it would call itself. Everything internal must therefore go
 * through the raw syscall, and this header is the single place that is spelled
 * out.
 *
 * These are `static inline` on purpose. Each translation unit that needs them
 * gets its own copy, which costs a few instructions and buys two things: no new
 * shared object the shims must link against, and no symbol for anyone else to
 * interpose. That matters for `make check`, which fails the build if a shim
 * references any GLIBC version above 2.7 — syscall() is GLIBC_2.4.
 */
#ifndef RBLIVE4_SYSCALLS_H
#define RBLIVE4_SYSCALLS_H

#include <sys/syscall.h>
#include <sys/types.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>     /* syscall() itself; omitting it is an implicit declaration */

static inline int real_ioctl(int fd, unsigned long request, void *arg)
{
    return (int)syscall(SYS_ioctl, fd, request, arg);
}

/* `mode` is threaded through rather than dropped: open()'s O_CREAT branch reads
 * it from the varargs precisely because the caller supplied it, and the previous
 * version passed a literal 0 to openat(). That created files with mode 000 —
 * invisible only because rbp runs as root, and only until something reopens one
 * of them as another user. */
static inline int real_open(const char *p, int flags, mode_t mode)
{
    return (int)syscall(SYS_openat, AT_FDCWD, p, flags, mode);
}

static inline ssize_t real_read(int fd, void *buf, size_t n)
{
    return syscall(SYS_read, fd, buf, n);
}

/* Here for klog(), which is the one writer in the controls shim that must not go
 * through a name another library could interpose: a shim that ever wrapped
 * write() would send its own log into itself. */
static inline ssize_t real_write(int fd, const void *buf, size_t n)
{
    return syscall(SYS_write, fd, buf, n);
}

static inline int real_close(int fd)
{
    return (int)syscall(SYS_close, fd);
}

static inline int real_poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
    return (int)syscall(SYS_poll, fds, nfds, timeout);
}

static inline int real_pipe2(int fds[2])
{
    return (int)syscall(SYS_pipe2, fds, 0);
}

static inline int real_dup(int fd)
{
    return (int)syscall(SYS_dup, fd);
}

/* mmap, for the framebuffer. ARM has no SYS_mmap in the 32-bit ABI — only
 * SYS_mmap2, whose offset is in *pages* — so the byte offset is shifted here
 * rather than at every call site. The error test is the kernel's own convention
 * (−1..−4095 in the return register) and not `r < 0`: a successful mmap on ARM
 * routinely lands in the 0xb6xxxxxx range, which is negative as a signed long.
 * MAP_FAILED is (void *)-1, which is what this returns on failure.
 *
 * `off` IS 64 BITS AND MUST STAY 64 BITS, even though `long` on this target is 32.
 * Both existing callers pass 0 and would not notice either way, but a DRM dumb
 * buffer does not: drm_mode_map_dumb's offset starts at DRM_FILE_PAGE_OFFSET_START
 * = 0x100000000, i.e. exactly 4 GiB, so a 32-bit parameter truncates it to 0
 * *before* the shift and the kernel is asked for page 0 of the wrong object. That
 * surfaces as EINVAL from mmap, which names the buffer rather than the argument.
 * Same family as the 32-bit clock wrap: on this target, check the width before
 * believing the error. The shift is done in 64 bits and only the RESULT is
 * narrowed, which is exact rather than lucky -- a page number stays small even
 * when the byte offset is not (0x1001f4000 is 1,048,052 pages). */
static inline void *real_mmap(void *addr, size_t len, int prot, int flags,
                              int fd, unsigned long long off)
{
    long r;
#if defined(SYS_mmap2)
    r = syscall(SYS_mmap2, addr, len, prot, flags, fd,
                (unsigned long)(off >> 12));
#else
    r = syscall(SYS_mmap, addr, len, prot, flags, fd, (unsigned long)off);
#endif
    if ((unsigned long)r >= (unsigned long)-4095)
        return (void *)-1;
    return (void *)r;
}

#endif /* RBLIVE4_SYSCALLS_H */
