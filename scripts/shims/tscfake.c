/*
 * tscfake.c — see tscfake.h. The record encoding, the fd table and the three
 * rbp-facing quirks (dedup, down-burst, x reflection) are carried over from
 * fbshim-tsc.c; only the plumbing around them is new, and the reflection is the
 * one of the three that had to be measured rather than inherited.
 */
#define _GNU_SOURCE
#include "tscfake.h"
#include "pointsrc.h"
#include "fb_cursor.h"
#include "menu_draw.h"
#include "point_xform.h"
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

/* --- the third consumer quirk: rbp reflects x -------------------------------
 *
 * rbp acts at `POINT_LOGICAL_W - 1 - x` of the x this file writes, and takes y
 * as written. So the record carries the *reflection* of the position the pointer
 * is at, and rbp lands on the pixel the finger is on.
 *
 * Measured on the unit 2026-09-27 by writing known records into the pipe rbp
 * holds both ways (work/tap.py), and reading the result off the framebuffer:
 *
 *   wrote x=1229 -> rbp selected the sidebar cell drawn at x 8..50 (BPM)
 *   wrote x=  90 -> rbp loaded deck 2, whose LOAD button is drawn at 1152..1272
 *   wrote x=  42 -> rbp hit INFO, drawn at 1180..1275
 *   wrote x=  50 -> fell *through* the sidebar column (x 0..100) into the list
 *   wrote x= 200 -> fell outside the LOAD 2 button
 *
 * That pins slope -1 and intercept 1279, i.e. `1279 - x`, at five points whose
 * hit rectangles bracket it. It is NOT the panel and NOT point_xform_abs(): with
 * POINT_DEBUG=1 the operator's own finger at raw (1874,1071) emitted logical
 * (1248,792) -- the bottom-right corner, honest, exactly as docs/07's S3.2 table
 * records. The reflection therefore happens inside rbp on the way in, and is
 * undone here rather than in point_xform.c because that file describes the
 * *device's* axes: this is a property of the consumer, which is the same reason
 * the dedup and the press burst live here.
 *
 * It applies to both pointer kinds by construction, not by choice: rbp cannot
 * tell them apart, because an absolute panel and a relative mouse both arrive as
 * records on this one pipe and nothing in the record says which produced it.
 * (Which is why the correction cannot go in point_xform_rel()'s dx either -- that
 * would fix a mouse's click and, since the arrow is drawn from the same variable,
 * leave it pointing at the mirror of the pixel the click lands on.)
 *
 * Reflecting here, at the wire, and not in pointsrc, keeps pointsrc's own idea of
 * where the pointer is honest -- which is what the visible arrow is drawn from
 * (fb_cursor.c) and what pointsrc_status() reports.
 */
int tscfake_wire_x(int x)
{
    return (POINT_LOGICAL_W - 1) - x;
}

void tscfake_emit(int down, int x, int y)
{
    /* `primed` is not bookkeeping, and the initialiser it replaced was a defect.
     * This was `last_down = -1`, meaning "nothing emitted yet" -- but the only
     * thing that reads last_down is the dedup below and the up->down test, and
     * -1 is *truthy*, so `down && !last_down` was FALSE for the process's first
     * down. The first press of every process therefore went out as a SINGLE
     * frame, which is the one thing this file's own header says must never
     * happen: rbp's TouchAdValueHysteresis discards the first frame after a gap,
     * so the first touch after rbp starts did nothing at all.
     *
     * Measured on the unit 2026-09-29, and it is the whole of work/menu17.sh's
     * "press #1": seven presses at rbp's INFO control with the gate off, the
     * first one (the first touch of that process) moved nothing, and all six
     * after it flipped rbp's INFO view -- at 150, 150, 45, 45, 30 and 75 ms, so
     * the press length was never the variable. See [[rbp-first-touch-lost]].
     *
     * rbp's touch stream starts *released* -- a device nobody is touching reads
     * BTN_TOUCH=0 -- so the process's first down IS an up->down transition and
     * gets the burst. `primed` keeps the dedup's other half honest independently
     * of that: before the first emit there is nothing to dedup against, so an
     * opening release at exactly (0,0) is still published rather than silently
     * matching the initial state. */
    static int last_down = 0, last_x = 0, last_y = 0, primed = 0;
    unsigned char buf[TSC_RECORD_LEN];

    x = tscfake_wire_x(x);

    if (primed && down == last_down && x == last_x && y == last_y)
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
    primed = 1;
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

    /* Same trigger, same "start once" property, one difference: this is the
     * process that owns the screen. rbp's UI is written for a touchscreen and
     * draws no pointer of its own — measured on the unit, the operator aiming at
     * INFO clicked (1279,0) because the accumulating position was invisible — so
     * the arrow has to be composited by the process that knows where the pointer
     * is and can map the page it is drawn on. */
    fb_cursor_start();

    /* Same trigger again, and the same "start once" property: the top menu's panel
     * is composited onto the same page by the same kind of thread, so it is
     * started here for the same reason. Gated by POINT_MENU, which menu_draw.c
     * explains. */
    menu_draw_start();

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
