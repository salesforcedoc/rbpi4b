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

#endif /* RBLIVE4_SYSCALLS_H */
