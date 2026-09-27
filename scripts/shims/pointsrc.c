/*
 * pointsrc.c — see pointsrc.h.
 *
 * Layout: discovery (find a device whose name/capabilities match) -> a reader
 * loop per device kind -> tscfake_emit(). The reader thread wraps discovery in a
 * retry loop, so "no device yet" and "device unplugged" are the same state and
 * a replug needs no special case.
 *
 * The absolute loop is the SC Live 4's, carried over: same event codes (it
 * accepted both ABS_X/ABS_Y and the multitouch ABS_MT_POSITION_* pair), same
 * "down" sources (BTN_TOUCH and ABS_MT_TRACKING_ID), same emit-on-SYN_REPORT.
 * The relative loop is new and exists because an HDMI monitor has no digitiser.
 */
#define _GNU_SOURCE
#include "pointsrc.h"
#include "point_xform.h"
#include "tscfake.h"
#include "syscalls.h"
#include "envutil.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* --- the kernel's input ABI, declared here rather than taken from
 * <linux/input.h> ----------------------------------------------------------
 *
 * The shims are cross-compiled against armel headers of one vintage and run
 * against a kernel of another, and <linux/input.h> has already changed shape
 * under us once: since Linux 5.4 it selects between a `struct timeval time` and
 * a `__sec/__usec` pair depending on __USE_TIME_BITS64, which is why
 * tools/evdevdump.c has to name its fields through input_event_sec/usec. These
 * definitions are the small, stable subset the shim needs, so the shim does not
 * care which branch the build host's headers take.
 */
struct ev_input_event {
    struct { uint32_t sec, usec; } time;
    uint16_t type, code;
    int32_t  value;
};

struct ev_absinfo {
    int32_t value, minimum, maximum, fuzz, flat, resolution;
};

/* EVIOCGABS encodes sizeof(struct input_absinfo) in the request, so a mismatch
 * would silently ask the kernel for a different thing than we read back. */
_Static_assert(sizeof(struct ev_absinfo) == 24,
               "struct input_absinfo must be 24 bytes for EVIOCGABS");

#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_REL 0x02
#define EV_ABS 0x03

#define SYN_REPORT 0

#define ABS_X                 0x00
#define ABS_Y                 0x01
#define ABS_MT_POSITION_X     0x35
#define ABS_MT_POSITION_Y     0x36
#define ABS_MT_TRACKING_ID    0x39

#define REL_X 0x00
#define REL_Y 0x01

#define BTN_LEFT  0x110
#define BTN_TOUCH 0x14a

/* _IOC(_IOC_READ, 'E', nr, size): 'E' is 0x45. */
#define EVIOCGNAME(len)     (0x80000000UL | ((unsigned long)(len) << 16) | (0x45UL << 8) | 0x06UL)
#define EVIOCGBIT(ev, len)  (0x80000000UL | ((unsigned long)(len) << 16) | (0x45UL << 8) | (0x20UL + (unsigned long)(ev)))
#define EVIOCGABS(abs)      (0x80000000UL | (24UL << 16) | (0x45UL << 8) | (0x40UL + (unsigned long)(abs)))

#define POINT_KIND_NONE 0
#define POINT_KIND_ABS  1
#define POINT_KIND_REL  2
#define POINT_KIND_AUTO 3

#define POINT_SCAN_MAX 32          /* /dev/input/event0 .. event31 */
#define POINT_NAME_MAX 256
#define POINT_PATH_MAX 64

#define POINT_DWELL_DEFAULT_MS 45
#define POINT_RESCAN_MS        500

static pthread_t reader_tid;
static int reader_started = 0;

/* Written by the reader thread, read by pointsrc_status() from another thread.
 * Deliberately not synchronised: this is a diagnostic snapshot, and a torn
 * string would still be a readable one. The alternative (a mutex taken on every
 * pointer event) would put a lock in the path this file exists to keep cheap. */
static volatile int  st_kind = POINT_KIND_NONE;
static volatile int  st_down = 0;
static volatile int  st_cursor_x = POINT_LOGICAL_W / 2;
static volatile int  st_cursor_y = POINT_LOGICAL_H / 2;
static char st_path[POINT_PATH_MAX];
static char st_name[POINT_NAME_MAX];

/* --- logging ---------------------------------------------------------------
 *
 * One FILE* opened on first use and never closed, rather than an fopen/fclose
 * per line: the log is written from the pointer path, and a per-event open is
 * exactly the mistake Milestone 1 is removing from the DirectFB driver. */
static FILE *log_fp = NULL;
static int  log_tried = 0;
static int  log_debug = 0;

#define POINT_LOG "/tmp/pointsrc.log"

/* Also called by fb_cursor.c, so the pointer's two halves land in one file in
 * the order the events happened. That makes this the one place in the pointer
 * path that takes a lock — the file's whole design keeps locks out of the reader
 * loop, and a vfprintf racing another thread's is undefined rather than merely
 * interleaved. It is only reached when POINT_DEBUG is on. */
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

void pointsrc_log(const char *fmt, ...)
{
    va_list ap;
    if (!log_tried) {
        log_tried = 1;
        log_fp = fopen(POINT_LOG, "a");
    }
    if (log_fp == NULL)
        return;
    pthread_mutex_lock(&log_lock);
    va_start(ap, fmt);
    vfprintf(log_fp, fmt, ap);
    va_end(ap);
    fputc('\n', log_fp);
    fflush(log_fp);
    pthread_mutex_unlock(&log_lock);
}

/* --- capability/name helpers ---------------------------------------------- */

static int bit_set(const unsigned char *bits, unsigned int bit)
{
    return (bits[bit >> 3] >> (bit & 7)) & 1;
}

static int want_kind(void)
{
    const char *k = env_str("POINT_KIND", "auto");
    if (strcmp(k, "none") == 0) return POINT_KIND_NONE;
    if (strcmp(k, "abs") == 0)  return POINT_KIND_ABS;
    if (strcmp(k, "rel") == 0)  return POINT_KIND_REL;
    if (strcmp(k, "auto") == 0) return POINT_KIND_AUTO;
    pointsrc_log("pointsrc: unknown POINT_KIND '%s'; treating as auto", k);
    return POINT_KIND_AUTO;
}

/* auto means "abs, then rel": on a machine that has both, the touchscreen is the
 * device the operator is reaching for and the mouse is the fallback. Expanded
 * here into the list open_pointer() walks, so the retry loop has one shape. */
static int kind_candidates(int out[2], int kind)
{
    if (kind == POINT_KIND_AUTO) {
        out[0] = POINT_KIND_ABS;
        out[1] = POINT_KIND_REL;
        return 2;
    }
    out[0] = kind;
    out[1] = POINT_KIND_NONE;
    return 1;
}

static int device_has_kind(int fd, int kind)
{
    unsigned char evbits[8];
    unsigned char absbits[16];
    unsigned char keybits[96];

    memset(evbits, 0, sizeof evbits);
    if (real_ioctl(fd, EVIOCGBIT(0, sizeof evbits), evbits) < 0)
        return 0;

    if (kind == POINT_KIND_ABS) {
        if (!bit_set(evbits, EV_ABS) || !bit_set(evbits, EV_KEY))
            return 0;
        memset(absbits, 0, sizeof absbits);
        memset(keybits, 0, sizeof keybits);
        if (real_ioctl(fd, EVIOCGBIT(EV_ABS, sizeof absbits), absbits) < 0)
            return 0;
        if (real_ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keybits), keybits) < 0)
            return 0;
        /* Both ABS_X and ABS_Y, plus something that can say "pressed". The
         * BTN_TOUCH requirement is what keeps a joystick or an accelerometer
         * from being adopted as a pointer. */
        return bit_set(absbits, ABS_X) && bit_set(absbits, ABS_Y) &&
               bit_set(keybits, BTN_TOUCH);
    }

    if (!bit_set(evbits, EV_REL) || !bit_set(evbits, EV_KEY))
        return 0;
    memset(absbits, 0, sizeof absbits);
    memset(keybits, 0, sizeof keybits);
    if (real_ioctl(fd, EVIOCGBIT(EV_REL, sizeof absbits), absbits) < 0)
        return 0;
    if (real_ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keybits), keybits) < 0)
        return 0;
    /* BTN_LEFT is required for the same reason BTN_TOUCH is above: REL_X/REL_Y
     * are also reported by scroll wheels, which are not pointers. */
    return bit_set(absbits, REL_X) && bit_set(absbits, REL_Y) &&
           bit_set(keybits, BTN_LEFT);
}

static void read_abs_range(int fd, int code, int *min, int *max)
{
    struct ev_absinfo ai;
    memset(&ai, 0, sizeof ai);
    if (real_ioctl(fd, EVIOCGABS(code), &ai) < 0)
        return;
    /* A degenerate range (max == min) is what a device reports when the kernel
     * has no calibration for that axis; the caller's default is better than a
     * divide-by-zero. */
    if (ai.maximum > ai.minimum) {
        *min = ai.minimum;
        *max = ai.maximum;
    }
}

static int open_matching_device(int kind, const char *match, char *path_out,
                                unsigned long path_len, char *name_out,
                                unsigned long name_len)
{
    int i;

    /* An explicit node path skips the scan entirely. It is the escape hatch for
     * a device whose capabilities are unusual enough that the filters above
     * reject it. */
    if (match != NULL && match[0] == '/') {
        int fd = real_open(match, O_RDONLY, 0);
        if (fd < 0)
            return -1;
        snprintf(path_out, path_len, "%s", match);
        name_out[0] = '\0';
        (void)real_ioctl(fd, EVIOCGNAME(name_len - 1), name_out);
        name_out[name_len - 1] = '\0';
        return fd;
    }

    for (i = 0; i < POINT_SCAN_MAX; i++) {
        char path[POINT_PATH_MAX];
        char name[POINT_NAME_MAX];
        int fd;

        snprintf(path, sizeof path, "/dev/input/event%d", i);
        fd = real_open(path, O_RDONLY, 0);
        if (fd < 0)
            continue;

        memset(name, 0, sizeof name);
        if (real_ioctl(fd, EVIOCGNAME(sizeof name - 1), name) < 0)
            name[0] = '\0';
        name[sizeof name - 1] = '\0';

        if (match != NULL && strstr(name, match) == NULL) {
            real_close(fd);
            continue;
        }
        if (!device_has_kind(fd, kind)) {
            real_close(fd);
            continue;
        }

        snprintf(path_out, path_len, "%s", path);
        snprintf(name_out, name_len, "%s", name);
        return fd;
    }
    return -1;
}

/* --- reader loops ---------------------------------------------------------- */

static void log_attach(int kind, const char *path, const char *name)
{
    pointsrc_log("pointsrc: %s device %s name='%s'",
         kind == POINT_KIND_ABS ? "absolute" : "relative", path, name);
}

static void absorb_into_xform(struct point_xform *x, int fd, int kind)
{
    point_xform_init(x);
    if (kind == POINT_KIND_ABS) {
        /* Only the absolute path reads the device's range; a relative device
         * reports counts with no range at all, and point_xform_rel() never looks
         * at these fields. */
        read_abs_range(fd, ABS_X, &x->raw_min_x, &x->raw_max_x);
        read_abs_range(fd, ABS_Y, &x->raw_min_y, &x->raw_max_y);
        pointsrc_log("pointsrc: raw range x=[%d..%d] y=[%d..%d] swap=%d inv_x=%d inv_y=%d",
             x->raw_min_x, x->raw_max_x, x->raw_min_y, x->raw_max_y,
             x->swap_xy, x->invert_x, x->invert_y);
    }
}

static void read_loop_abs(int fd, const struct point_xform *x)
{
    struct ev_input_event ev;
    int rx = 0, ry = 0, down = 0;

    for (;;) {
        ssize_t n = real_read(fd, &ev, sizeof ev);
        if (n != (ssize_t)sizeof ev) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN))
                continue;
            return;                     /* device gone: rediscover */
        }
        switch (ev.type) {
        case EV_ABS:
            if (ev.code == ABS_X || ev.code == ABS_MT_POSITION_X) rx = ev.value;
            else if (ev.code == ABS_Y || ev.code == ABS_MT_POSITION_Y) ry = ev.value;
            else if (ev.code == ABS_MT_TRACKING_ID)
                down = (ev.value >= 0) ? 1 : 0;   /* >= 0 is down, -1 is up */
            break;
        case EV_KEY:
            if (ev.code == BTN_TOUCH)
                down = ev.value ? 1 : 0;
            break;
        case EV_SYN:
            if (ev.code == SYN_REPORT) {
                int lx, ly;
                point_xform_abs(x, rx, ry, &lx, &ly);
                st_down = down;
                st_cursor_x = lx;
                st_cursor_y = ly;
                tscfake_emit(down, lx, ly);
                if (log_debug)
                    /* Both ends of the transform, because they are not the same
                     * value: tscfake_emit reflects x (see tscfake.h's third
                     * quirk), so `wire` is what rbp consumes and `logical` is
                     * where the finger is. The raw->logical fit is checked
                     * against the middle pair; a tap landing on the wrong side
                     * of the screen is read from the last. */
                    pointsrc_log("pointsrc: abs raw=(%d,%d) logical=(%d,%d) wire=(%d,%d) down=%d",
                         rx, ry, lx, ly, tscfake_wire_x(lx), ly, down);
            }
            break;
        default:
            break;
        }
    }
}

static void read_loop_rel(int fd, const struct point_xform *x)
{
    struct ev_input_event ev;
    int dx = 0, dy = 0, down = 0;
    int cx = POINT_LOGICAL_W / 2, cy = POINT_LOGICAL_H / 2;
    int dwell_ms = env_int("POINT_MIN_DWELL_MS", POINT_DWELL_DEFAULT_MS);
    long down_ns = 0;

    for (;;) {
        ssize_t n = real_read(fd, &ev, sizeof ev);
        if (n != (ssize_t)sizeof ev) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN))
                continue;
            return;
        }
        switch (ev.type) {
        case EV_REL:
            if (ev.code == REL_X) dx += ev.value;
            else if (ev.code == REL_Y) dy += ev.value;
            break;
        case EV_KEY:
            if (ev.code == BTN_LEFT) {
                int nd = ev.value ? 1 : 0;
                if (nd && !down) {
                    /* Press: publish the press at the current position, which is
                     * where the operator is looking. */
                    struct timespec ts;
                    clock_gettime(CLOCK_MONOTONIC, &ts);
                    down_ns = (long)ts.tv_sec * 1000000000L + ts.tv_nsec;
                    down = 1;
                    st_down = 1;
                    tscfake_emit(1, cx, cy);
                    if (log_debug)
                        pointsrc_log("pointsrc: rel press at (%d,%d)", cx, cy);
                } else if (!nd && down) {
                    /* Release. A click shorter than the dwell is held open: rbp's
                     * TouchAdValueHysteresis needs a few frames of "down" to
                     * register a tap, and a fast physical click can occupy fewer
                     * than that, so without this the click is simply swallowed. */
                    struct timespec ts;
                    long now_ns, elapsed_ms;
                    clock_gettime(CLOCK_MONOTONIC, &ts);
                    now_ns = (long)ts.tv_sec * 1000000000L + ts.tv_nsec;
                    elapsed_ms = (down_ns > 0) ? (now_ns - down_ns) / 1000000L : dwell_ms;
                    if (dwell_ms > 0 && elapsed_ms < dwell_ms) {
                        usleep((useconds_t)(dwell_ms - elapsed_ms) * 1000);
                        if (log_debug)
                            pointsrc_log("pointsrc: rel release held %ldms (dwell %d)",
                                 (long)dwell_ms - elapsed_ms, dwell_ms);
                    }
                    down = 0;
                    st_down = 0;
                    tscfake_emit(0, cx, cy);
                }
            }
            break;
        case EV_SYN:
            if (ev.code == SYN_REPORT && (dx != 0 || dy != 0)) {
                point_xform_rel(x, dx, dy, &cx, &cy);
                dx = dy = 0;
                st_cursor_x = cx;
                st_cursor_y = cy;
                /* Motion is published only while pressed. Emitting the released
                 * position on every movement would flood rbp with up-state
                 * frames — it needs the coordinate, not the traffic — and the
                 * press that follows carries the position anyway. */
                if (down) {
                    tscfake_emit(1, cx, cy);
                    if (log_debug)
                        pointsrc_log("pointsrc: rel drag to (%d,%d)", cx, cy);
                }
            }
            break;
        default:
            break;
        }
    }
}

static int open_pointer(int *kind_out, char *path_out, unsigned long path_len,
                        char *name_out, unsigned long name_len)
{
    int cands[2];
    int n, i;
    const char *match;

    n = kind_candidates(cands, want_kind());
    match = env_str("POINT_DEV", NULL);

    for (i = 0; i < n; i++) {
        int fd = open_matching_device(cands[i], match, path_out, path_len,
                                      name_out, name_len);
        if (fd >= 0) {
            *kind_out = cands[i];
            return fd;
        }
    }
    return -1;
}

static void *reader_thread(void *arg)
{
    (void)arg;
    for (;;) {
        int kind = POINT_KIND_NONE;
        char path[POINT_PATH_MAX];
        char name[POINT_NAME_MAX];
        int fd;

        path[0] = '\0';
        name[0] = '\0';
        fd = open_pointer(&kind, path, sizeof path, name, sizeof name);
        if (fd < 0) {
            /* Nothing attached, or what is attached is not a pointer. Retry
             * forever rather than exiting: plugging a mouse in later has to
             * work, and rbp is already running by then. */
            usleep(POINT_RESCAN_MS * 1000);
            continue;
        }

        st_kind = kind;
        snprintf(st_path, sizeof st_path, "%s", path);
        snprintf(st_name, sizeof st_name, "%s", name);
        log_attach(kind, path, name);

        {
            struct point_xform x;
            absorb_into_xform(&x, fd, kind);
            if (kind == POINT_KIND_ABS)
                read_loop_abs(fd, &x);
            else
                read_loop_rel(fd, &x);
        }

        real_close(fd);
        st_kind = POINT_KIND_NONE;
        st_down = 0;
        if (log_debug)
            pointsrc_log("pointsrc: %s went away; rescanning", path);
    }
    return NULL;
}

int pointsrc_start(void)
{
    log_debug = env_flag("POINT_DEBUG", 0);

    if (reader_started)
        return 0;
    /* POINT_KIND=none means "no pointer at all" — the display-only bring-up
     * setting. Returning before creating the thread keeps that case free of a
     * thread that would only ever sleep. */
    if (want_kind() == POINT_KIND_NONE)
        return 0;
    if (pthread_create(&reader_tid, NULL, reader_thread, NULL) != 0)
        return -1;
    reader_started = 1;
    return 0;
}

void pointsrc_status(char *buf, unsigned long buflen)
{
    const char *kindstr;

    switch (st_kind) {
    case POINT_KIND_ABS: kindstr = "abs"; break;
    case POINT_KIND_REL: kindstr = "rel"; break;
    default:             kindstr = "none"; break;
    }

    if (st_kind == POINT_KIND_NONE) {
        snprintf(buf, buflen, "kind=%s (no pointer attached)",
                 env_str("POINT_KIND", "auto"));
        return;
    }
    snprintf(buf, buflen, "kind=%s dev=%s name='%s' at=(%d,%d) down=%d",
             kindstr, st_path, st_name, st_cursor_x, st_cursor_y, st_down);
}

int pointsrc_cursor(int *logical_x, int *logical_y, int *down)
{
    /* A relative device is the only case that needs an arrow: an absolute one
     * reports where it is being touched, so a cursor would be a second, lagging
     * mark on the screen saying the same thing. Returning 0 here is also how the
     * compositor learns that the mouse was unplugged and the arrow it drew has
     * to come back off. */
    if (st_kind != POINT_KIND_REL)
        return 0;
    if (logical_x)
        *logical_x = st_cursor_x;
    if (logical_y)
        *logical_y = st_cursor_y;
    if (down)
        *down = st_down;
    return 1;
}
