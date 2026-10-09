/*
 * vnc_input.c -- see vnc_input.h.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "vnc_input.h"

/* The evdev ioctls this file needs, spelled out rather than taken from <linux/input.h>.
 * That header is not on a Mac, where the tests run, and it is not what this file wants
 * to be in any case: everything here is about ONE panel with ONE known descriptor, and
 * the constants below are the whole of it. EVIOCGNAME(len) and EVIOCGABS(abs) are the
 * two requests the kernel defines.
 *
 * THE DIRECTION WORD IS SPELLED PER PLATFORM because the two `_IOC` macros do not agree
 * on how to say "read": Linux takes a two-bit direction field and has _IOC_READ for it,
 * while Darwin's <sys/ioccom.h> wants the assembled bit and calls it IOC_OUT. Nothing
 * here runs on the Mac -- only the pure functions are tested there -- but this file has
 * to compile there, and a macro that only expands on Linux is not a portable file. */
#if defined(__APPLE__)
#  define VNC_IOC_READ IOC_OUT
#else
#  define VNC_IOC_READ _IOC_READ
#endif
#define VNC_EVIOCGNAME(len) _IOC(VNC_IOC_READ, 'E', 0x06, (len))
#define VNC_EVIOCGABS(abs)  _IOC(VNC_IOC_READ, 'E', 0x40 + (abs), (int)sizeof(struct vnc_absinfo))

struct vnc_absinfo {
    int32_t value, minimum, maximum, fuzz, flat, resolution;
};

/* The event codes, as in work/poke.py. */
#define VNC_EV_SYN 0x00
#define VNC_EV_KEY 0x01
#define VNC_EV_ABS 0x03
#define VNC_SYN_REPORT 0
#define VNC_ABS_X 0x00
#define VNC_ABS_Y 0x01
#define VNC_ABS_MT_SLOT 0x2f
#define VNC_ABS_MT_POSITION_X 0x35
#define VNC_ABS_MT_POSITION_Y 0x36
#define VNC_ABS_MT_TRACKING_ID 0x39
#define VNC_BTN_TOUCH 0x14a
#define VNC_TRACKING_NONE (-1)

/* `struct stat`'s nanosecond stamp is st_mtim on glibc, where the unit runs, and
 * st_mtimespec on macOS, where the tests run. Sub-second resolution is not a luxury
 * here: a switch flipped twice inside one second must not read as flipped once. */
#if defined(__APPLE__)
#  define VNC_ST_MTIM(st) ((st).st_mtimespec)
#else
#  define VNC_ST_MTIM(st) ((st).st_mtim)
#endif

/* --- the switch ------------------------------------------------------------- */

int vnc_input_switch_note(struct vnc_input_switch *s, char *out, size_t outlen)
{
    if (!s->note[0] || outlen == 0)
        return 0;
    snprintf(out, outlen, "%s", s->note);
    s->note[0] = '\0';
    return 1;
}

const char *vnc_input_name(int on)
{
    /* Anything that is not exactly ON reads as off. vnc_mode_name makes the opposite
     * choice -- anything not hwjpeg is "raw" -- and the difference is deliberate: a
     * mode this program cannot name still has to send something, while a switch this
     * program cannot name must not be the one that lets a machine press a live
     * player's buttons. */
    return on == VNC_INPUT_ON ? "on" : "off";
}

static void strip_lower(const char *text, char *buf, size_t buflen)
{
    size_t n = 0;

    while (*text && n < buflen - 1) {
        char ch = *text++;
        if (ch == '\n' || ch == '\r')
            continue;
        buf[n++] = (char)(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
    }
    buf[n] = '\0';
}

/* Leading and trailing blanks off, in place, by hand: strtok would eat the interior of
 * a word and `sscanf %31s` would quietly take the first of two, which is a mistake in
 * a control file that should read as one. */
static char *trim(char *s)
{
    size_t i = 0, j;

    while (s[i] == ' ' || s[i] == '\t')
        i++;
    j = strlen(s);
    while (j > i && (s[j - 1] == ' ' || s[j - 1] == '\t'))
        j--;
    s[j] = '\0';
    return s + i;
}

int vnc_input_parse(const char *text, int *out)
{
    char buf[32];
    char *w;

    strip_lower(text, buf, sizeof buf);
    w = trim(buf);

    if (!strcmp(w, "on") || !strcmp(w, "1") || !strcmp(w, "yes") || !strcmp(w, "true")) {
        *out = VNC_INPUT_ON;
        return 0;
    }
    if (!strcmp(w, "off") || !strcmp(w, "0") || !strcmp(w, "no") || !strcmp(w, "false")) {
        *out = VNC_INPUT_OFF;
        return 0;
    }
    return -1;
}

void vnc_input_switch_init(struct vnc_input_switch *s, const char *path, int dflt)
{
    memset(s, 0, sizeof *s);
    s->path = path;
    s->on = dflt ? VNC_INPUT_ON : VNC_INPUT_OFF;
}

int vnc_input_switch_get(struct vnc_input_switch *s)
{
    struct stat st;
    char buf[32], text[33];
    int fd;
    ssize_t n;
    size_t k;

    if (stat(s->path, &st) != 0) {
        if (s->have_mtime) {
            s->have_mtime = 0;
            snprintf(s->note, sizeof s->note, "input file %s is gone; keeping %s",
                     s->path, vnc_input_name(s->on));
        }
        return s->on;
    }
    if (s->have_mtime &&
        VNC_ST_MTIM(st).tv_sec == s->mtime.tv_sec &&
        VNC_ST_MTIM(st).tv_nsec == s->mtime.tv_nsec)
        return s->on;

    fd = open(s->path, O_RDONLY);
    if (fd < 0)
        return s->on;
    n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0)
        return s->on;
    buf[n] = '\0';
    for (k = 0; k < (size_t)n && k < sizeof text - 1 && buf[k] != '\n' && buf[k] != '\r'; k++)
        text[k] = buf[k];
    text[k] = '\0';

    s->mtime = VNC_ST_MTIM(st);
    s->have_mtime = 1;
    s->reads++;

    {
        int want;
        if (vnc_input_parse(buf, &want) < 0) {
            snprintf(s->note, sizeof s->note,
                     "input file %s says \"%s\", which is neither on nor off; keeping %s",
                     s->path, text, vnc_input_name(s->on));
            return s->on;
        }
        if (want != s->on) {
            snprintf(s->note, sizeof s->note, "turned %s", vnc_input_name(want));
            s->on = want;
        }
    }
    return s->on;
}

int vnc_input_switch_set(struct vnc_input_switch *s, int on)
{
    char buf[8];
    int fd, len;

    len = snprintf(buf, sizeof buf, "%s\n", vnc_input_name(on));
    /* O_TRUNC on this same open rather than write-a-temp-and-rename, for the reason
     * vnc_mode_set gives: the only reader is this process on its next turn and the
     * file is a four-byte word, so a half-written read is not a real race -- while a
     * rename needs a temporary, a directory that may not exist yet, and a cleanup path
     * on every failure. */
    fd = open(s->path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        snprintf(s->note, sizeof s->note, "cannot write %s: %s", s->path, strerror(errno));
        return s->on;
    }
    if (write(fd, buf, (size_t)len) != len) {
        snprintf(s->note, sizeof s->note, "short write to %s: %s", s->path, strerror(errno));
        close(fd);
        return s->on;
    }
    close(fd);
    {
        struct stat st;
        if (stat(s->path, &st) == 0) {
            s->mtime = VNC_ST_MTIM(st);
            s->have_mtime = 1;
        }
    }
    if (on != s->on) {
        snprintf(s->note, sizeof s->note, "vnc input turned %s", vnc_input_name(on));
        s->on = on;
    }
    return s->on;
}

/* --- the mapping ------------------------------------------------------------ */

int vnc_input_raw_of_logical(int l, int lspan, int rspan)
{
    long v;

    if (lspan <= 0 || rspan <= 0)
        return 0;
    if (l < 0)
        l = 0;
    if (l > lspan - 1)
        l = lspan - 1;
    /* The ceiling, written as a floor of the negation. The eye wants `l * rspan /
     * lspan`, and that is the answer that is one row and one column short at the top
     * and right edge: it was measured on this panel, twice -- a fader asked for value
     * 0 landed on 2, and logical x 1279 came back as 1278. */
    v = ((long)l * rspan + lspan - 1) / lspan;
    if (v > rspan - 1)
        v = rspan - 1;
    return (int)v;
}

void vnc_input_map(int lx, int ly, int lspan_w, int lspan_h,
                   int raw_w, int raw_h, int *rx, int *ry)
{
    if (rx)
        *rx = vnc_input_raw_of_logical(lx, lspan_w, raw_w);
    if (ry)
        *ry = vnc_input_raw_of_logical(ly, lspan_h, raw_h);
}

/* --- the reports ------------------------------------------------------------ */

/* ONE EVENT, in whichever layout write() has been shown to accept.
 *
 * The timestamp is left at zero. It is not read by anything downstream -- pointsrc
 * reads the type, the code and the value and nothing else -- and poke.py has always
 * written zeroes, so a non-zero stamp here would be this file inventing a field that
 * the only known-good writer on this unit does not use. */
static size_t put_ev(uint8_t *buf, size_t cap, int evsize, int type, int code, int value)
{
    size_t tail = evsize == 24 ? 16 : 8;

    if (cap < (size_t)evsize || (evsize != 16 && evsize != 24))
        return 0;
    memset(buf, 0, (size_t)evsize);
    buf[tail + 0] = (uint8_t)(type & 0xff);
    buf[tail + 1] = (uint8_t)((type >> 8) & 0xff);
    buf[tail + 2] = (uint8_t)(code & 0xff);
    buf[tail + 3] = (uint8_t)((code >> 8) & 0xff);
    buf[tail + 4] = (uint8_t)(value & 0xff);
    buf[tail + 5] = (uint8_t)((value >> 8) & 0xff);
    buf[tail + 6] = (uint8_t)((value >> 16) & 0xff);
    buf[tail + 7] = (uint8_t)((value >> 24) & 0xff);
    return (size_t)evsize;
}

/* The sequence work/poke.py's prime() writes: publish a position with NO touch. It is
 * not decoration. The input core drops an ABS value equal to the one the device
 * already holds, ONE per axis and not one per contact -- so a press aimed at the point
 * a previous press left the pointer on carries no position at all, and rbp then acts
 * wherever the last real finger was, which reads on the glass as a tap that did
 * nothing. Moving the stored value out of the way first is the whole cure, and it is
 * cheap: seven events, once per press. */
size_t vnc_input_build_prime(uint8_t *buf, size_t cap, int evsize, int rx, int ry)
{
    size_t n = 0, k;

    k = put_ev(buf + n, cap - n, evsize, VNC_EV_ABS, VNC_ABS_MT_SLOT, 0);        n += k; if (!k) return 0;
    k = put_ev(buf + n, cap - n, evsize, VNC_EV_ABS, VNC_ABS_MT_TRACKING_ID, VNC_TRACKING_NONE);
    n += k; if (!k) return 0;
    k = put_ev(buf + n, cap - n, evsize, VNC_EV_ABS, VNC_ABS_MT_POSITION_X, rx); n += k; if (!k) return 0;
    k = put_ev(buf + n, cap - n, evsize, VNC_EV_ABS, VNC_ABS_MT_POSITION_Y, ry); n += k; if (!k) return 0;
    k = put_ev(buf + n, cap - n, evsize, VNC_EV_ABS, VNC_ABS_X, rx);             n += k; if (!k) return 0;
    k = put_ev(buf + n, cap - n, evsize, VNC_EV_ABS, VNC_ABS_Y, ry);             n += k; if (!k) return 0;
    k = put_ev(buf + n, cap - n, evsize, VNC_EV_SYN, VNC_SYN_REPORT, 0);         n += k; if (!k) return 0;
    return n;
}

/* The press. Every field is repeated rather than left to the prime, because a client
 * can move the pointer and press in the same message and the kernel will then have
 * dropped the prime's position -- this report has to stand on its own. */
size_t vnc_input_build_press(uint8_t *buf, size_t cap, int evsize, int rx, int ry)
{
    size_t n = 0, k;

    k = put_ev(buf + n, cap - n, evsize, VNC_EV_ABS, VNC_ABS_MT_SLOT, 0);         n += k; if (!k) return 0;
    k = put_ev(buf + n, cap - n, evsize, VNC_EV_ABS, VNC_ABS_MT_TRACKING_ID, 1); n += k; if (!k) return 0;
    k = put_ev(buf + n, cap - n, evsize, VNC_EV_ABS, VNC_ABS_MT_POSITION_X, rx); n += k; if (!k) return 0;
    k = put_ev(buf + n, cap - n, evsize, VNC_EV_ABS, VNC_ABS_MT_POSITION_Y, ry); n += k; if (!k) return 0;
    k = put_ev(buf + n, cap - n, evsize, VNC_EV_ABS, VNC_ABS_X, rx);             n += k; if (!k) return 0;
    k = put_ev(buf + n, cap - n, evsize, VNC_EV_ABS, VNC_ABS_Y, ry);             n += k; if (!k) return 0;
    k = put_ev(buf + n, cap - n, evsize, VNC_EV_KEY, VNC_BTN_TOUCH, 1);          n += k; if (!k) return 0;
    k = put_ev(buf + n, cap - n, evsize, VNC_EV_SYN, VNC_SYN_REPORT, 0);         n += k; if (!k) return 0;
    return n;
}

/* A drag: the same press, moved. No tracking id and no BTN_TOUCH, because nothing has
 * changed about the contact -- only where it is. */
size_t vnc_input_build_move(uint8_t *buf, size_t cap, int evsize, int rx, int ry)
{
    size_t n = 0, k;

    k = put_ev(buf + n, cap - n, evsize, VNC_EV_ABS, VNC_ABS_MT_POSITION_X, rx); n += k; if (!k) return 0;
    k = put_ev(buf + n, cap - n, evsize, VNC_EV_ABS, VNC_ABS_MT_POSITION_Y, ry); n += k; if (!k) return 0;
    k = put_ev(buf + n, cap - n, evsize, VNC_EV_ABS, VNC_ABS_X, rx);             n += k; if (!k) return 0;
    k = put_ev(buf + n, cap - n, evsize, VNC_EV_ABS, VNC_ABS_Y, ry);             n += k; if (!k) return 0;
    k = put_ev(buf + n, cap - n, evsize, VNC_EV_SYN, VNC_SYN_REPORT, 0);         n += k; if (!k) return 0;
    return n;
}

size_t vnc_input_build_release(uint8_t *buf, size_t cap, int evsize)
{
    size_t n = 0, k;

    k = put_ev(buf + n, cap - n, evsize, VNC_EV_ABS, VNC_ABS_MT_TRACKING_ID, VNC_TRACKING_NONE);
    n += k; if (!k) return 0;
    k = put_ev(buf + n, cap - n, evsize, VNC_EV_KEY, VNC_BTN_TOUCH, 0);  n += k; if (!k) return 0;
    k = put_ev(buf + n, cap - n, evsize, VNC_EV_SYN, VNC_SYN_REPORT, 0); n += k; if (!k) return 0;
    return n;
}

size_t vnc_input_build_syn(uint8_t *buf, size_t cap, int evsize)
{
    return put_ev(buf, cap, evsize, VNC_EV_SYN, VNC_SYN_REPORT, 0);
}

/* --- the shell -------------------------------------------------------------- */

int vnc_input_note(struct vnc_input *in, char *out, size_t outlen)
{
    if (!in->note[0] || outlen == 0)
        return 0;
    snprintf(out, outlen, "%s", in->note);
    in->note[0] = '\0';
    return 1;
}

void vnc_input_init(struct vnc_input *in, int scr_w, int scr_h, int raw_w, int raw_h)
{
    memset(in, 0, sizeof *in);
    in->fd = -1;
    in->evsize = 16;
    in->scr_w = scr_w > 0 ? scr_w : VNC_INPUT_SCREEN_W;
    in->scr_h = scr_h > 0 ? scr_h : VNC_INPUT_SCREEN_H;
    in->raw_w = raw_w > 0 ? raw_w : VNC_INPUT_RAW_W_DEFAULT;
    in->raw_h = raw_h > 0 ? raw_h : VNC_INPUT_RAW_H_DEFAULT;
}

void vnc_input_set_fd(struct vnc_input *in, int fd, int evsize)
{
    in->fd = fd;
    in->evsize = evsize == 24 ? 24 : 16;
}

static void write_report(struct vnc_input *in, const uint8_t *buf, size_t n)
{
    ssize_t w;

    if (in->fd < 0 || n == 0)
        return;
    w = write(in->fd, buf, n);
    if (w != (ssize_t)n) {
        /* THE NOTE IS OVERWRITTEN, NOT APPENDED, and that is on purpose: a panel that
         * has gone away fails every report, and a server that says so once per event
         * fills a tmpfs (see RB_VERBOSE in rb.conf). The count below is the honest
         * record of how often it happened. */
        snprintf(in->note, sizeof in->note,
                 "the panel took %ld of %zu bytes: %s", (long)w, n, strerror(errno));
        in->refused++;
    }
}

/* THE RECORD SIZE IS MEASURED, NEVER ASSUMED. The kernel here is 64-bit and the
 * userland is 32-bit, so a 32-bit writer's `struct input_event` is 16 bytes and the
 * kernel's compat path is built for exactly that -- but poke.py writes 16 and
 * keysend.py writes 24, and both cannot be right. The distinguishing signature is
 * clean: evdev_write only consumes whole records and returns how many bytes it took,
 * so a 16-byte write to a kernel expecting 24 returns 0, not an error.
 *
 * The 24 BYTE ATTEMPT GOES FIRST AND IS HARMLESS EITHER WAY. If the kernel wants 16 it
 * reads the first 16 bytes of that buffer as one record -- and those 16 bytes decode
 * as type 0, code 0, value 0, which is a SYN_REPORT and a genuine no-op, exactly what
 * this probe means to send. */
static int measure_evsize(int fd, int *out, char *err, size_t errlen)
{
    uint8_t buf[32];
    size_t n;
    ssize_t w;

    n = vnc_input_build_syn(buf, sizeof buf, 24);
    w = write(fd, buf, n);
    if (w == (ssize_t)n) { *out = 24; return 0; }

    n = vnc_input_build_syn(buf, sizeof buf, 16);
    w = write(fd, buf, n);
    if (w == (ssize_t)n) { *out = 16; return 0; }

    snprintf(err, errlen, "the panel accepted neither the 16-byte nor the 24-byte "
             "record this program can write -- a no-op SYN_REPORT got %ld bytes back "
             "-- so input is disabled", (long)w);
    return -1;
}

static int read_abs_range(int fd, int code, int *span, int *lo)
{
    struct vnc_absinfo ai;

    memset(&ai, 0, sizeof ai);
    if (ioctl(fd, VNC_EVIOCGABS(code), &ai) < 0)
        return -1;
    if (ai.maximum <= ai.minimum)
        return -1;
    *lo = ai.minimum;
    *span = ai.maximum - ai.minimum + 1;
    return 0;
}

/* Find the panel by name. The scan is the same one pointsrc does, over the same 32
 * nodes, and it looks for the same word: on this unit the panel is /dev/input/event0
 * and calls itself "TSTP CTouch". A missing panel is reported with the names that WERE
 * there, because "no input" and "no input, and here is what I did find" are different
 * messages to whoever is reading the log at midnight. */
static int find_panel(char *path_out, size_t path_len,
                      char *name_out, size_t name_len,
                      char *err, size_t errlen)
{
    char seen[512];
    size_t used = 0;
    int i, fd;

    seen[0] = '\0';
    for (i = 0; i < VNC_INPUT_SCAN_MAX; i++) {
        char path[64], name[64] = "";

        snprintf(path, sizeof path, "/dev/input/event%d", i);
        fd = open(path, O_WRONLY);
        if (fd < 0)
            continue;
        if (ioctl(fd, VNC_EVIOCGNAME(sizeof name - 1), name) < 0)
            name[0] = '\0';
        name[sizeof name - 1] = '\0';

        if (strstr(name, VNC_INPUT_NAME_MATCH) != NULL) {
            snprintf(path_out, path_len, "%s", path);
            snprintf(name_out, name_len, "%s", name);
            return fd;
        }
        if (used < sizeof seen - 40 && name[0]) {
            int n = snprintf(seen + used, sizeof seen - used, "%s%s=\"%s\"",
                             used ? ", " : "", path, name);
            if (n > 0) used += (size_t)n;
        }
        close(fd);
    }
    snprintf(err, errlen, "no input device calls itself \"%s\". What is there: %.380s",
             VNC_INPUT_NAME_MATCH, seen[0] ? seen : "(nothing openable in "
             "/dev/input/event0..31)");
    return -1;
}

int vnc_input_open(struct vnc_input *in, const char *dev, char *err, size_t errlen)
{
    char path[64], name[64];
    int fd, span, lo, evsize = 0;
    int opened_rdwr = 0;

    if (in->fd >= 0)
        return 0;                       /* already open; opening twice would leak */

    /* WRITE-ONLY IS THE POINT, AND THE FALLBACK IS NOT A LICENCE TO READ. evdev
     * delivers every event to every client that has the node open, so a reader here
     * would not take anything from pointsrc -- but this process has no business
     * holding a read end of the operator's panel at all, and the surest way never to
     * consume a report meant for the glass is not to have the ability. If the kernel
     * refuses O_WRONLY, O_RDWR is taken and simply never read from. */
    if (dev && *dev) {
        snprintf(path, sizeof path, "%s", dev);
        name[0] = '\0';
        fd = open(path, O_WRONLY);
        if (fd < 0) {
            fd = open(path, O_RDWR);
            if (fd < 0) {
                snprintf(err, errlen, "cannot open %s: %s", path, strerror(errno));
                return -1;
            }
            opened_rdwr = 1;
        }
    } else {
        fd = find_panel(path, sizeof path, name, sizeof name, err, errlen);
        if (fd < 0)
            return -1;
    }

    if (measure_evsize(fd, &evsize, err, errlen) < 0) {
        close(fd);
        return -1;
    }

    /* The panel's own range, which is what turns a framebuffer coordinate into
     * something it will accept. The defaults are this panel's measured numbers; a
     * device that answers EVIOCGABS gets to say otherwise. */
    if (read_abs_range(fd, VNC_ABS_X, &span, &lo) == 0)
        in->raw_w = span;
    if (read_abs_range(fd, VNC_ABS_Y, &span, &lo) == 0)
        in->raw_h = span;

    vnc_input_set_fd(in, fd, evsize);
    /* The caller's logger already says "input:", so this note does not. It says what the
     * panel turned out to be, which is the whole point of opening it: the record size is
     * the one thing that had to be measured rather than assumed, and the raw range is
     * what every coordinate written from now on is scaled into. */
    {
        char who[80];
        if (name[0])
            snprintf(who, sizeof who, " (%.64s)", name);
        else
            who[0] = '\0';
        snprintf(in->note, sizeof in->note,
                 "%s%s opened %s, %d-byte records, raw range %dx%d",
                 path, who, opened_rdwr ? "read-write (never read)" : "write-only",
                 evsize, in->raw_w, in->raw_h);
    }
    return 0;
}

void vnc_input_close(struct vnc_input *in)
{
    if (in->fd >= 0)
        close(in->fd);
    in->fd = -1;
    in->down = 0;
    in->pending = 0;
}

int vnc_input_is_down(const struct vnc_input *in)
{
    return in->down;
}

static void send_release(struct vnc_input *in)
{
    uint8_t buf[VNC_INPUT_REPORT_MAX];
    size_t n = vnc_input_build_release(buf, sizeof buf, in->evsize);

    write_report(in, buf, n);
    in->down = 0;
    in->pending = 0;
    in->releases++;
}

static void begin_press(struct vnc_input *in, int rx, int ry, unsigned long long now)
{
    uint8_t buf[VNC_INPUT_REPORT_MAX];

    write_report(in, buf, vnc_input_build_prime(buf, sizeof buf, in->evsize, rx, ry));
    write_report(in, buf, vnc_input_build_press(buf, sizeof buf, in->evsize, rx, ry));
    in->down = 1;
    in->pending = 0;
    in->queued = 0;
    in->press_ms = now;
    in->have_last = 1;
    in->last_x = rx;
    in->last_y = ry;
    in->presses++;
}

void vnc_input_pointer(struct vnc_input *in, int buttons, int lx, int ly,
                       unsigned long long now)
{
    uint8_t buf[VNC_INPUT_REPORT_MAX];
    size_t n;
    int rx, ry, down;

    if (in->fd < 0)
        return;

    /* ONLY BIT 0 MEANS ANYTHING. The panel has one contact and no buttons, so RFB's
     * middle and right buttons have nothing to press. They are counted and dropped
     * rather than guessed at: a right-click that silently became a left-click on a
     * live player would be worse than one that does nothing. */
    down = (buttons & 1) ? 1 : 0;

    vnc_input_map(lx, ly, in->scr_w, in->scr_h, in->raw_w, in->raw_h, &rx, &ry);

    if (down && !in->down) {
        begin_press(in, rx, ry, now);
        return;
    }

    if (down && in->down) {
        /* A SECOND PRESS INSIDE THE FLOOR. The client has clicked twice in less than
         * VNC_INPUT_MIN_PRESS_MS, so its release of the first press is still held back
         * here and this one is a press in its own right, at a different place. It is
         * remembered rather than merged into the first as a drag -- merged, rbp would
         * see one long press whose finger wandered, which on the menu means the second
         * button never fires and the first fires twice -- and it is not sent yet
         * either, because the first press has not lasted long enough to be a tap. It
         * waits for vnc_input_tick() to finish the first one. */
        if (in->pending) {
            in->queued = 1;
            in->qx = rx;
            in->qy = ry;
            return;
        }
        /* A drag. Only reported when the raw point has actually moved: the kernel
         * would drop the repeated ABS values anyway, leaving a bare SYN_REPORT that
         * wakes pointsrc and rbp's whole pointer chain for nothing. */
        if (!in->have_last || rx != in->last_x || ry != in->last_y) {
            n = vnc_input_build_move(buf, sizeof buf, in->evsize, rx, ry);
            write_report(in, buf, n);
            in->last_x = rx;
            in->last_y = ry;
            in->have_last = 1;
            in->moves++;
        }
        return;
    }

    if (!down && in->down && !in->pending) {
        if (now - in->press_ms < VNC_INPUT_MIN_PRESS_MS) {
            in->pending = 1;            /* vnc_input_tick() will send it */
            return;
        }
        send_release(in);
    }
    /* A move with no button held is dropped on purpose, and it is pointsrc's own rule
     * rather than a shortcut: its relative-mouse path says "motion is published only
     * while pressed ... the press that follows carries the position anyway". A hover
     * report here would be traffic rbp's zones have never been given by any device on
     * this unit, for no gain -- rbp draws no cursor for one to move. */
}

void vnc_input_tick(struct vnc_input *in, unsigned long long now)
{
    if (!in->pending || !in->down)
        return;
    if (now - in->press_ms < VNC_INPUT_MIN_PRESS_MS)
        return;
    send_release(in);
    if (in->queued)
        begin_press(in, in->qx, in->qy, now);
}

void vnc_input_release(struct vnc_input *in)
{
    if (in->fd < 0)
        return;
    if (!in->down && !in->pending)
        return;
    /* A queued press belongs to a client that has just gone, or to a switch that has
     * just been turned off. It must not fire, and it must not leave the finger down. */
    in->queued = 0;
    send_release(in);
}
