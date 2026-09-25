/*
 * tscfake.c — see tscfake.h. The record encoding, the fd table and the two
 * rbp-facing quirks (dedup, down-burst) are carried over from fbshim-tsc.c
 * unchanged; only the plumbing around them is new.
 */
#define _GNU_SOURCE
#include "tscfake.h"
#include "pointsrc.h"
#include "syscalls.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

static int out_pipe[2] = { -1, -1 };  /* [0] read end (dup'd per open), [1] write end */

/* Which fds are ours, indexed by fd, plus how many are open (the concurrent-open
 * limit rbp has never come near).
 *
 * The inherited version of this stored the dup'd fd in the first free slot of a
 * 64-entry array and tested membership with `fake_used[fd] && fake_pipe_rd[fd] ==
 * fd` — slot index against fd number, which can only be true by coincidence. It
 * never mattered, because the fd behind the fake device *is* a real pipe fd, so
 * the fall-through to real_read()/real_close() did the right thing either way;
 * the table's only real effect was to cap concurrent opens. Routing that works by
 * coincidence is worth replacing when the file is already open. The protocol
 * itself — the record bytes, the burst, the dedup — is untouched by this. */
#define TSC_MAX_OPEN_FDS 256
static char fake_fd[TSC_MAX_OPEN_FDS];
static int  fake_open_count;

void tscfake_record(int down, int x, int y, unsigned char out[TSC_RECORD_LEN])
{
    out[0] = (unsigned char)(down ? 1 : 0);
    out[1] = 0;                                    /* pad */
    out[2] = (unsigned char)(x & 0xff);
    out[3] = (unsigned char)((x >> 8) & 0xff);
    out[4] = (unsigned char)(y & 0xff);
    out[5] = (unsigned char)((y >> 8) & 0xff);
}

static void push_record(const unsigned char *buf)
{
    if (out_pipe[1] >= 0)
        (void)write(out_pipe[1], buf, TSC_RECORD_LEN);
}

void tscfake_emit(int down, int x, int y)
{
    static int last_down = -1, last_x = 0, last_y = 0;
    unsigned char buf[TSC_RECORD_LEN];

    if (down == last_down && x == last_x && y == last_y)
        return;

    tscfake_record(down, x, y, buf);
    if (down && !last_down) {
        /* Up -> down. Two frames, so rbp's debounce sees 0 -> 1 -> 1 rather than
         * 0 -> 1 and one discarded frame. */
        push_record(buf);
        push_record(buf);
    } else {
        push_record(buf);
    }
    last_down = down;
    last_x = x;
    last_y = y;
}

static int ensure_pipe(void)
{
    if (out_pipe[0] >= 0)
        return 0;
    if (real_pipe2(out_pipe) < 0) {
        out_pipe[0] = out_pipe[1] = -1;
        return -1;
    }
    return 0;
}

int tscfake_open(void)
{
    int rd_end;

    if (ensure_pipe() < 0)
        return -1;
    if (fake_open_count >= TSC_MAX_FDS) {
        errno = EMFILE;
        return -1;
    }

    /* Best effort, and deliberately not fatal: a pointer is optional, the device
     * is not. pointsrc keeps re-scanning in the background if this finds
     * nothing, so a mouse plugged in after rbp started still works. */
    pointsrc_start();

    rd_end = real_dup(out_pipe[0]);
    if (rd_end < 0)
        return -1;
    /* A dup above the table's bound is not a failure of the caller's open: the
     * fd still works for reading, it is just not one we can recognise later. Say
     * so rather than pretend, because fb_shim.c would then route its read()
     * through the fall-through (correct) while this file believed it had a
     * tracked fd (wrong). */
    if (rd_end >= TSC_MAX_OPEN_FDS) {
        real_close(rd_end);
        errno = EMFILE;
        return -1;
    }
    fake_fd[rd_end] = 1;
    fake_open_count++;
    return rd_end;
}

int tscfake_is_fd(int fd)
{
    return fd >= 0 && fd < TSC_MAX_OPEN_FDS && fake_fd[fd];
}

ssize_t tscfake_read(int fd, void *buf, size_t count)
{
    /* fd IS the pipe's read end (see tscfake_open), so there is no second
     * variable to consult. */
    return real_read(fd, buf, count);
}

int tscfake_close(int fd)
{
    if (!tscfake_is_fd(fd))
        return real_close(fd);
    fake_fd[fd] = 0;
    fake_open_count--;
    return real_close(fd);
}

int tscfake_ioctl(unsigned long request, void *arg)
{
    /* rbp issues these only on the fake touch fd, but they are answered for any
     * fd: the requests are unambiguous, and the alternative — tracking which fd
     * is the fake one across dup() and fork() — is more machinery than the
     * discrimination is worth. */
    switch (request) {
    case 0x80046b00: {            /* _IOR(0x6b,0,4): read max X */
        unsigned int *p = (unsigned int *)arg;
        if (p) *p = TSC_MAX_X;
        return 1;
    }
    case 0x40046b00:              /* _IOW(0x6b,0,4): write max X */
        return 1;
    case 0x80026b01: {            /* _IOR(0x6b,1,2): read max Y */
        unsigned short *p = (unsigned short *)arg;
        if (p) *p = TSC_MAX_Y;
        return 1;
    }
    case 0x40026b01:              /* _IOW(0x6b,1,2): write max Y */
        return 1;
    default:
        return 0;
    }
}
