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
#include "touch_zone.h"
#include "menu_zone.h"
#include "menu_window.h"
#include "syscalls.h"
#include "envutil.h"

/* The one rbp thing this file knows: the keycode a QUANTIZE tap becomes, and
 * how to send it. rbp_key.h is a leaf (rbp_abi.h and libc, nothing more), which
 * is why fbshim can reach rbp's key path without linking anything that carries
 * rbp's meter hook -- see rbp_key.h. Every send is a no-op unless the process
 * is rbp and its KeyManager exists yet, so this needs no guard of its own. */
#include "rbp_abi.h"
#include "rbp_key.h"

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
/* The same number as the dwell, for the same measured reason: rbp's
 * TouchAdValueHysteresis discards the first frame of a down burst, so a press rbp
 * is to act on has to be held for a few of its ~57 Hz frames. This one is the
 * replay of a swallowed strip tap (menu_replay_tap below), which rbp never saw the
 * down edge of and so has to be given a whole press. POINT_MENU_TAP_MS overrides it
 * for the bench -- deliberately *not* in start-rb.sh's SHIM_VARS, like the menu's
 * other bench knobs, so it cannot be set from rb.conf by accident. */
#define POINT_MENU_TAP_DEFAULT_MS 45
/* The panel's press-and-hold, as two numbers, both bench-overridable and both
 * deliberately *not* in start-rb.sh's SHIM_VARS -- the same rule as the menu's
 * other bench knobs, so neither can be set from rb.conf by accident:
 *
 *   POINT_MENU_HOLD_MS       how long the FINGER must be down (MZ_HOLD_FINGER_MS;
 *                            0 turns the hold off and every press is a tap again)
 *   POINT_MENU_HOLD_SEND_MS  how long the KEY is then held (MZ_HOLD_KEY_MS)
 *
 * menu_zone.h's MZ_HOLD_FINGER_MS carries the measurement behind both: rbp gives
 * one keycode two meanings, press = the menu and hold = UTILITY, and its own timer
 * is what decides -- between 300 and 400 ms, measured 2026-09-29. */
#define POINT_RESCAN_MS        500

/* How often read_loop_abs() looks at the clock while a press is still down on a menu
 * button -- i.e. the resolution of the moment the panel lets go of the screen by
 * itself. It exists only so that loop does not have to block through that moment,
 * and it is deliberately not a bench knob: it is how often a question is asked, not
 * an answer to it, and there is no feel to tune. 20 ms is under the point where a
 * delay is visible and coarse enough that a hold costs fifty wake-ups a second on a
 * thread that is idle the rest of the time. */
#define POINT_MENU_HOLD_TICK_MS 20

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
static int  log_debug = 0;

/* POINT_QUANTIZE_TAP: whether a press on a deck QUANTIZE box is turned into
 * rbp's keycode for it. On by default -- it is the operator's ask, and the box
 * is inert without it. It is not, however, a part of the screen no other screen
 * uses: on the BROWSE screen rbp binds its own control over the same rectangle
 * (the deck strip's time display, TIME/REMAIN), so a press there does both
 * things at once. That is measured and written down rather than discovered --
 * docs/07-touch.md, docs/13-raspberrypi4.md -- and this gate is what lets the
 * operator end it from the unit without a rebuild if the double action is worse
 * than the feature is worth. */
static int  quantize_enabled = 1;

/* POINT_MENU: whether the swipe-down top menu exists at all. On by default, like
 * POINT_QUANTIZE_TAP, because it is the operator's ask. With it off, no report is
 * ever offered to menu_zone.c -- so rbp's input stream is byte-identical to what
 * it is without this feature, which is the property the drill checks and the
 * reason the gate is one call site rather than a parallel path.
 *
 * POINT_MENU_MOUSE: whether the menu also answers the *relative* device. Off by
 * default, and that is a design choice rather than an omission: a mouse has no
 * natural swipe, and more to the point the menu and the mouse arrow both write
 * the one framebuffer page this port has (cursor_paint.c's conditional restore
 * assumes it is the only writer in its rectangle). Off means ABS-only, i.e. the
 * two never overlap. With it on they can, and they self-heal against each other
 * at the cost of a flickering arrow inside the panel -- see docs/07-touch.md. */
static int  menu_enabled = 1;
static int  menu_mouse = 0;

/* The buttons' keycodes, in menu_zone.c's column order: the first six are rbp's own
 * browse keycodes, all six now measured (rbp_abi.h carries the derivation and the
 * runs), and they are the same six the keyboard map sends from its digit row. The
 * seventh is USB STOP, which no keyboard row has and which is here alone.
 *
 * PLAYLIST and SEARCH were 0 here for one release: the two slots the derivation
 * named and nothing had measured, drawn and named and visibly pending, firing
 * nothing and saying so. POINT_MENU_KEY_EXTRA is how they were measured, and it
 * survives the promotion -- it is the instrument, and the next unnamed key will
 * want it again. */
static const int menu_key[MZ_COLS] = {
    K_SOURCE, K_BROWSE, K_TAGLIST, K_PLAYLIST, K_SEARCH, K_MENU, K_USBSTOP
};

/* Which of the keys above rbp only finishes on when it sees a SECOND edge. One
 * entry today, so this is a predicate and not a table -- but it is asked by keycode
 * rather than by column, deliberately: POINT_MENU_KEY_EXTRA repoints a column at
 * some other key, and the edge sequence a key needs belongs to the KEY. Overriding
 * USB STOP's column with something ordinary should stop sending it a repeat, and it
 * does.
 *
 * K_USBSTOP is the odd one of the seven: it is not "repeat-only" but "press AND
 * repeat", because its handler does half the job on each edge (rbp_abi.h's entry
 * has the branch table and the measurement). map_flx4.c's note table carries the
 * same idea as a per-note flag; this is the same mechanism one layer down. */
static int menu_key_needs_repeat(int code)
{
    return code == K_USBSTOP;
}

/* The table actually used: menu_key, overridden in column order by the bench knob
 * POINT_MENU_KEY_EXTRA (a comma-separated list) at start-up.
 *
 * It is deliberately NOT in start-rb.sh's SHIM_VARS and not in rb.conf. That list
 * exports every name it holds -- *including* the ones nobody set, as the empty
 * string -- so a shipped default would overwrite the ad-hoc value with "" on the
 * next service restart, which is exactly the trap start-rb.sh:176-180 documents.
 * An operator setting it in the environment by hand is the only user it has, and
 * the service does pass one through: the unit file has no EnvironmentFile, but
 * `systemctl set-environment POINT_MENU_KEY_EXTRA=0x204,0x205` reaches rbp on the
 * next restart and `unset-environment` takes it away again.
 *
 * Note the *spelling*, because it is the whole reason this works: POINT_MENU_* is
 * the shim's name, which SHIM_VARS does not list and therefore does not rewrite.
 * The RB_POINT_MENU_* spelling is the deployed-config one, and it only becomes
 * POINT_MENU_* through that same SHIM_VARS loop -- so on a unit whose
 * start-rb.sh predates the knob, `systemctl set-environment RB_POINT_MENU=0`
 * reaches the service and stops there, with the shim silently using its in-source
 * default. Measured 2026-09-29, and it cost three runs before the launcher was
 * read rather than the shim. `grep POINT_MENU /opt/rblive4/start-rb.sh` is the
 * one-line check. */
static int  menu_key_eff[MZ_COLS];

#define POINT_LOG "/tmp/pointsrc.log"

/* Also called by fb_cursor.c, so the pointer's two halves land in one file in
 * the order the events happened. That makes this the one place in the pointer
 * path that takes a lock — the file's whole design keeps locks out of the reader
 * loop, and a vfprintf racing another thread's is undefined rather than merely
 * interleaved.
 *
 * The per-event lines are written only when POINT_DEBUG is on: a report per
 * SYN_REPORT would be the flood this file exists to avoid. What is written
 * unconditionally is the rare, one-off event — attach, detach, a device that
 * went away, and a recognized QUANTIZE tap — because those are the lines a
 * drill reads back afterwards and a silent feature is not evidence. */
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

/* THE OPEN IS A ONE-SHOT, AND IT USED TO BE ABLE TO EAT A LINE.
 *
 * This was:
 *
 *     if (!log_tried) { log_tried = 1; log_fp = fopen(POINT_LOG, "a"); }
 *     if (log_fp == NULL) return;
 *
 * which publishes its two halves in the wrong order and under no lock at all.
 * Thread A sets log_tried = 1 and is still inside fopen(); thread B arrives,
 * reads log_tried, concludes somebody has already opened the file, and then
 * consults log_fp -- which is still NULL -- and returns having written nothing.
 * Silently, because that same NULL is also how a legitimate "the file could not
 * be opened, say nothing" is spelled. The window is one fopen wide, which is
 * microseconds, and it is open only at process start, because before it every
 * call is a candidate and after it log_fp is set. Three threads log in that
 * window (the reader's attach line, the cursor thread's, and whichever of rbp's
 * own threads called in), so it is a coin toss rather than a rarity.
 *
 * WHY IT COST WHOLE DRILL PHASES. The lines a drill gates on are one-shot: the
 * reader attaches once, the cursor opens the fb once, and the menu's "drawing
 * the panel" is written once when that thread opens the fb. A phase whose
 * signature line is the one that loses the race never produces it, at all, for
 * the rest of the process's life -- so the drill waits out its timeout and
 * reports "the phase never reached the state it claimed". That is exactly the
 * anomaly that has been read as a mystery since menu11: phases failing their own
 * gate with the rest of the log healthy, and by preference the FIRST phase of a
 * run, which is the one whose signature is written soonest after start.
 *
 * Measured on the unit 2026-09-29 (work/menu19.sh, both phases lost theirs in one
 * run): the log held the reader's attach line, the raw-range line and the cursor
 * line -- all written by threads that log *later* -- and no menu line of either
 * kind, in a run where rbp's environ has no POINT_MENU and so the ON branch had
 * to run. The survivors are the tell: the loser is whichever call is in flight
 * while the winner is inside its fopen.
 *
 * pthread_once() is the whole fix: one thread runs log_open, every other caller
 * blocks until it has returned, and NULL then means only what it says.
 *
 * shimutil.c's klog() has the same missing lock and is *not* wrong, which is why
 * this one deserves the paragraph: klog's flag and its value are one variable
 * (`log_fd < 0`, then `log_fd = open(...)`), so a thread that loses the race
 * opens a second fd and still writes its line -- a leaked fd, no lost line. Here
 * the flag and the value are two variables, and that is what makes it a race with
 * a victim. */
static pthread_once_t log_once = PTHREAD_ONCE_INIT;

static void log_open(void)
{
    log_fp = fopen(POINT_LOG, "a");
}

void pointsrc_log(const char *fmt, ...)
{
    va_list ap;
    pthread_once(&log_once, log_open);
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

/* Where the reader starts from is the device's own position -- not zero, and not
 * the screen's centre.
 *
 * The kernel drops an ABS event whose value is the one the device already holds
 * (measured: a report can carry a y and no x at all), so a loop that has never
 * been handed an x cannot tell "the finger is at x 0" from "no x was reported".
 * It reads x 0, which is the far-left column. Measured on the unit 2026-09-28:
 * after a service restart, 18 consecutive injected taps at raw x 83 were read as
 * `raw=(0,y)` -- the shim's x was still the zero it started with -- and every one
 * was lost, while the same taps landed as soon as one report moved x. The same
 * hole applies to a finger: a first touch at the x the panel last reported
 * arrives with no x event, and the tap is delivered to rbp at logical x 0.
 *
 * Seeding gives the loop one invariant, which then holds for its whole life:
 * `rx`/`ry` are the device's stored values. True at the start because of this
 * call; preserved afterwards because a value that changes always arrives as an
 * event, and a value that does not is already held here. */
static void seed_abs_position(int fd, int *rx, int *ry)
{
    struct ev_absinfo ai;

    /* The abs candidate filter requires ABS_X and ABS_Y (read_caps above), so
     * both ioctls name an axis this device has. A failure leaves the default. */
    memset(&ai, 0, sizeof ai);
    if (real_ioctl(fd, EVIOCGABS(ABS_X), &ai) == 0)
        *rx = ai.value;
    memset(&ai, 0, sizeof ai);
    if (real_ioctl(fd, EVIOCGABS(ABS_Y), &ai) == 0)
        *ry = ai.value;
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

/* The deck QUANTIZE boxes are the one place a pointer position is ours to
 * interpret rather than rbp's: rbp draws the boxes and never lets a touch on
 * one mean anything (touch_zone.h has the measurement). So a press that starts
 * on one is turned into the keycode rbp has for that button -- K_QUANTIZE,
 * which is what a hardware press sends -- and rbp does the rest, its own screen
 * included.
 *
 * Called from pointer_report() -- one call per report that reaches rbp at all,
 * which is the contract with the zone module (touch_zone_feed: one call per
 * report, in order), and it is the module that knows which reports can mean
 * anything: a drag is the same press at a new position, and a resting finger is
 * the same press again. A report the menu swallowed never gets here at all, which
 * is deliberate: see pointer_report().
 *
 * The key goes out as a press and a release together, exactly as aloop_apply()
 * sends a pad. The gesture is complete at the press -- rbp's handler toggles on
 * the press alone -- so there is no held key to release later, and nothing to
 * lose if the device is unplugged mid-touch.
 *
 * CALL THIS BEFORE tscfake_emit(). pointer_report() is now the one place that
 * order is written down, which is why it takes both calls. It is not cosmetic.
 * uif::IKeyManager::sendKey (0x37ad64) fills ONE
 * shared IKeyInput -- the pointer at KeyManager+132 -- and only then walks the
 * listener list, handing every listener that same mutable struct (`this->vt[7]`
 * at the tail of sendKey; the press/release split before it is shift-key
 * bookkeeping, not the dispatch). So a key sent while rbp's own touch thread is
 * working through a report can have its keycode overwritten before the deck's
 * PlayerInnards reads it -- and rbp's touch thread is exactly what runs when the
 * report carrying this gesture arrives.
 *
 * The menu's keys are not exposed to that race, and it is worth saying why they
 * need no special handling: menu_fire() runs on a report the menu *swallowed*, so
 * there is no tscfake_emit() in flight and rbp's touch thread is not working
 * through anything. The ordering rule below is satisfied by construction rather
 * than by being careful.
 *
 * Where the numbers are, and what they do not say. Two runs on 2026-09-28
 * measured 2/8 and 7/10 taps flipping deck 1's flag (Player+3804, whose only
 * runtime writer is PlayerInnards::onKey_Quantize), and that is what put the
 * race above on the table. They are not evidence for it: the instrument that
 * produced them never moved x, so its reports carried no x at all and the reader
 * held the 0 it started with (seed_abs_position, above) for the whole run --
 * those taps were judged at logical x 0, outside the zone, and the mix within a
 * run is not explained by anything the shim did. The controls were clean and
 * still stand: KEY_6 (K_BROWSE, global) toggled the screen 8/8, and KEY_SPACE
 * (K_PLAY, channel 1, a deck key) started and stopped deck 1 8/8 with no touch
 * in flight.
 *
 * With the reader's x fixed, the same instrument measures 20+20 taps across both
 * decks landing on the press with 0 misses, and a 10-tap control outside the zone
 * landing none. So the ordering below rests on the structure of sendKey, which is
 * where it was found, and not on those runs -- it costs one reordering and it
 * closes a window the structure says is there. */
static void quantize_tap(int down, int x, int y)
{
    int deck = touch_zone_feed(down, x, y);
    if (!deck)
        return;
    pointsrc_log("pointsrc: deck%d QUANTIZE tap at (%d,%d)%s",
                 deck, x, y, quantize_enabled ? "" : " (disabled)");
    if (!quantize_enabled)
        return;
    send_rx_key(K_QUANTIZE, OP_PRESS, deck, 0);
    send_rx_key(K_QUANTIZE, OP_RELEASE, deck, 0);
}

/* Fill menu_key_eff from menu_key and the bench knob. Defined here rather than
 * beside its table because it logs, and pointsrc_log() is defined further down.
 *
 * The knob *overrides* the table in column order, and it keeps the ability to say
 * 0: that is what puts a button back on the sentinel this table shipped with
 * while PLAYLIST and SEARCH were unnamed, so the negative half of a measurement
 * -- "with no keycode, a tap changes nothing" -- stays reproducible. It stops
 * when its list runs out, so one number in the environment moves one button and
 * leaves the other five as shipped. */
static void menu_keys_init(void)
{
    const char *p = env_str("POINT_MENU_KEY_EXTRA", NULL);
    int i;

    for (i = 0; i < MZ_COLS; i++)
        menu_key_eff[i] = menu_key[i];

    if (p == NULL || *p == '\0')
        return;                    /* no knob: the shipped table, untouched */

    for (i = 0; i < MZ_COLS; i++) {
        char *end;
        long n;

        n = strtol(p, &end, 0);    /* base 0, so 0x204 and 204 are both accepted */
        if (end == p) {
            pointsrc_log("pointsrc: POINT_MENU_KEY_EXTRA '%s' is not a keycode list;"
                         " menu button %d '%s' and the rest keep the shipped keys",
                         p, i + 1, menu_label(i + 1));
            return;
        }
        p = end;
        while (*p == ',' || *p == ' ' || *p == '\t')
            p++;
        if (n < 0 || n > 0xffff) {
            pointsrc_log("pointsrc: POINT_MENU_KEY_EXTRA %ld is not a keycode; menu"
                         " button %d '%s' keeps the shipped key", n, i + 1,
                         menu_label(i + 1));
        } else {
            menu_key_eff[i] = (int)n;
            pointsrc_log("pointsrc: menu button %d '%s' -> keycode %#lx (bench)",
                         i + 1, menu_label(i + 1), (unsigned long)n);
        }
        if (*p == '\0')
            break;                 /* knob exhausted: the rest keep the shipped keys */
    }
}

/* A tap that landed on a menu button. Same shape as quantize_tap()'s: the
 * gesture is complete at the tap -- rbp's handler for a *press* of every one of
 * these keys acts on the press alone, with USB STOP the one exception (it is
 * finished by the repeat edge, and menu_key_needs_repeat() below sends it) -- so
 * the key goes out as a press and a release together and there is nothing left to
 * release if the panel is unplugged mid-touch.
 *
 * A *held* finger is the one case where the two edges are separated on purpose,
 * and it is the operator's second ask on the glass: *"if i long press menu it
 * should go to utility"* -- which is the MENU button's own second meaning on this
 * hardware, implemented by rbp itself on the same keycode (measurement in
 * menu_zone.h's MZ_HOLD_FINGER_MS). This function does not implement UTILITY; it
 * holds the key and rbp's timer does the rest, which is why the rule here is a
 * duration and not a screen. Two callers reach this arm and it does the same thing
 * for both: the release, and read_loop_abs() the moment the threshold passes with
 * the finger still down -- the operator's follow-up ask, *"the menu should disappear
 * and it should just go to utility by itself"*, which is menu_hold_pending().
 *
 * CH_GLOBAL, not a deck channel, because these are the browse/library family: on
 * hardware they are global keys with no deck of their own (map_kbd.c sends all
 * six browse keys the same way, and it is measured -- see docs/08-controls.md).
 * USB STOP is sent the same way for the same reason, and its channel filter in
 * rbp's handler admits channel 1.
 *
 * A button whose keycode is 0 fires nothing and *says so*, and the line names the
 * keycode when it does fire. That is not politeness: a tap that sent nothing and
 * a gesture that never arrived look identical from the outside, so the log is the
 * only thing that separates them -- it is what the PLAYLIST/SEARCH measurement
 * read, and what a re-check reads with the same two buttons put back on 0 through
 * POINT_MENU_KEY_EXTRA. */
static void menu_fire(int button, int held_ms)
{
    const char *label = menu_label(button);
    int hold_ms, send_ms, code;

    /* A button with no label fires nothing and says nothing. The menu's eighth cell
     * -- the globe that opened the browser window -- used to be the one case here;
     * it is gone with the browser (menu_zone.h's block), so menu_button_at() now
     * answers 1..MZ_COLS or 0 and this is belt-and-braces against a future cell
     * being added to the hit test before it is added to the label table. */
    if (label == NULL)
        return;
    code = menu_key_eff[button - 1];
    if (code == 0) {
        pointsrc_log("pointsrc: menu '%s' tapped (button %d): no keycode yet,"
                     " nothing sent", label, button);
        return;
    }

    /* A finger held long enough is rbp's *other* meaning for this key. The panel
     * does not know what that meaning is -- it knows the operator held the button,
     * and it sends a key down long enough for rbp's own timer to decide (see
     * menu_zone.h's MZ_HOLD_FINGER_MS for the measurement and the two numbers).
     *
     * The send blocks this reader thread for send_ms, which is the price of a
     * synthesized hold: the key's edges are not the finger's edges. rbp's timer needs
     * a key-down of its own threshold or longer, and a finger that is a hold at all
     * is only guaranteed to be *just* past MZ_HOLD_FINGER_MS -- inside rbp's measured
     * 300..400 ms band -- so replaying the finger's own duration would reach UTILITY
     * by luck. A fixed span that clears the band with room does it every time.
     * Bounded, and once per hold -- the same trade menu_replay_tap() makes for its
     * 45 ms, with the same justification (evdev buffers, so a touch arriving during
     * it is delayed and not lost). Send 0 and the two edges go out back to back: a
     * plain tap, which is the second A/B lever. */
    hold_ms = env_int("POINT_MENU_HOLD_MS", MZ_HOLD_FINGER_MS);
    if (menu_hold_fires(held_ms, hold_ms)) {
        send_ms = env_int("POINT_MENU_HOLD_SEND_MS", MZ_HOLD_KEY_MS);
        pointsrc_log("pointsrc: menu '%s' held %dms (hold is %dms) -> key %#x"
                     " down for %dms, rbp's own hold action", label, held_ms,
                     hold_ms, code, send_ms);
        send_rx_key(code, OP_PRESS, CH_GLOBAL, 0);
        if (menu_key_needs_repeat(code))
            send_rx_key(code, OP_REPEAT, CH_GLOBAL, 0);
        if (send_ms > 0)
            usleep((useconds_t)send_ms * 1000);
        send_rx_key(code, OP_RELEASE, CH_GLOBAL, 0);
        return;
    }

    pointsrc_log("pointsrc: menu '%s' tapped (button %d) -> key %#x (finger %dms"
                 " of %dms)", label, button, code, held_ms, hold_ms);
    send_rx_key(code, OP_PRESS, CH_GLOBAL, 0);
    /* The middle edge, for a key that needs one, goes out immediately after the
     * press and before the release -- the order map_flx4.c uses for K_TRACKFILTER,
     * and the order the handler expects (rbp_abi.h's K_USBSTOP: the press mutes and
     * notifies, the repeat is what asks the db to stop the device, the release
     * unwinds). A tap therefore carries all three edges back to back, which is a
     * different thing from the panel's own gap between them -- if rbp turns out to
     * want the gap, the hold path below is the shape that has one. */
    if (menu_key_needs_repeat(code))
        send_rx_key(code, OP_REPEAT, CH_GLOBAL, 0);
    send_rx_key(code, OP_RELEASE, CH_GLOBAL, 0);
}

/* A swallowed strip tap, given back to rbp.
 *
 * The operator's fourth finding on the glass was that the strip's cost was more
 * than the plan said: "touches when the bar is not displayed don't work anymore,
 * the info and timer buttons specifically". The replay was the first answer to it
 * and the entry zone is the second and better one: rbp's INFO control sits in the
 * right third of the band, which MZ_ENTRY_X0/X1 no longer swallows at all, so it
 * is answered natively now and this function is never called for it. What is left
 * for the replay is whatever rbp draws in the middle third of its own top bar,
 * which is chrome on every screen it has. The menu cannot hand rbp the original
 * report -- it is gone, and the panel must still decide the gesture -- so it
 * replays the press instead, and this is the whole of it.
 *
 * BOTH EDGES, at the point the finger landed: rbp never saw the down, so a bare
 * release would be a release for a press it does not have, and tscfake_emit()'s
 * dedup would in any case drop a repeated release. At the *start* point because
 * that is what the operator aimed at -- the press is a tap by definition here (it
 * never travelled MZ_SWIPE_PX), and a finger that drifted a few px on the way up
 * still meant the button it landed on.
 *
 * THE DWELL IS NOT OPTIONAL -- see POINT_MENU_TAP_DEFAULT_MS. It blocks this
 * reader thread for its duration, which is the same trade read_loop_rel() makes
 * for a fast physical click: evdev buffers, so a touch arriving during the replay
 * is delayed and not lost.
 *
 * quantize_tap() is deliberately NOT called for the replay, even though the normal
 * path calls it for every report it forwards: it would advance touch_zone.c's edge
 * state for a press that can never be on one of its boxes (they are at y 732..780,
 * the strip is y <= 55), and a swallowed press must not touch that state. */
static void menu_replay_tap(void)
{
    int tx = 0, ty = 0;
    int dwell_ms = env_int("POINT_MENU_TAP_MS", POINT_MENU_TAP_DEFAULT_MS);

    menu_tap_point(&tx, &ty);
    pointsrc_log("pointsrc: strip tap at (%d,%d) is not a swipe or a button ->"
                 " replayed to rbp as a press (held %dms)", tx, ty, dwell_ms);
    tscfake_emit(1, tx, ty);
    if (dwell_ms > 0)
        usleep((useconds_t)dwell_ms * 1000);
    tscfake_emit(0, tx, ty);
}

/* One report, in the one order that works: the browser WINDOW decides first if it
 * is open, then the menu decides whether rbp sees this press at all, then the
 * QUANTIZE box interprets it, then it goes to rbp.
 *
 * THE WINDOW IS FIRST because it is opaque and drawn OVER the menu's own rectangle:
 * a press that landed on the window must not also be judged by the swipe rules
 * underneath it. It answers only for points on itself, so nothing else in this
 * chain changes.
 *
 * THE MENU IS NEXT because it owns the whole report when it takes it. rbp gets
 * nothing -- not the touch, and not the QUANTIZE box either: a press that
 * dismisses the panel must not also press whatever is drawn under it, and
 * touch_zone_feed() is reached only through quantize_tap() below, so a swallowed
 * press never advances the box's edge state. That is what leaves the next real
 * press on a QUANTIZE box still a fresh press (test_point.c pins it).
 *
 * MZ_FEED_TAP is the one swallowed answer rbp does hear, and it hears a *different*
 * press: the menu's own copy, emitted here (menu_replay_tap) rather than forwarded.
 * So even that report takes neither the QUANTIZE box nor the original coordinates.
 *
 * `allow_menu` is the caller's answer to "may the menu have this one": the
 * absolute loop passes menu_enabled, the relative loop passes menu_mouse, and a
 * report that is not offered is never swallowed and never changes the menu's
 * state -- so POINT_MENU=0 really is rbp's untouched stream.
 *
 * `held_ms` is how long the press behind this report has been down, and it is the
 * caller's because this file is the one with the clock: 0 on a report that is not
 * a release (nothing can fire from those), and the measured duration at the
 * release, which is where a menu button fires and where the tap/hold split is
 * decided. */
static void pointer_report(int down, int x, int y, int allow_menu, int held_ms)
{
    int button = 0;
    int verdict = MZ_FEED_NONE;

    /* THE WINDOW FIRST, and it is asked before the menu rather than after because
     * the two overlap on the glass: the window is opaque, so a press inside its
     * rectangle must never reach the menu's gesture rules underneath it. It only
     * answers for points on itself (menu_window.h), so a press anywhere else still
     * gets both the menu and rbp exactly as before -- which is what keeps the
     * swipe-down gesture and test_menu.c's 6,373 checks untouched.
     *
     * A window report is always TAKEN, never TAP: the caller must not replay it to
     * rbp, and `button` is left alone because the window acts on its own state
     * machine inside menu_window_feed() rather than handing a button back. */
    if (allow_menu && menu_window_is_open())
        verdict = menu_window_feed(down, x, y);

    if (verdict == MZ_FEED_NONE && allow_menu)
        verdict = menu_feed(down, x, y, &button);

    if (verdict == MZ_FEED_TAP) {
        menu_replay_tap();
        return;
    }
    if (verdict) {
        if (button)
            menu_fire(button, held_ms);
        return;
    }
    quantize_tap(down, x, y);
    tscfake_emit(down, x, y);
}

/* Monotonic milliseconds, 64 bits wide on purpose -- which is the whole reason
 * this is a helper and not a line of arithmetic at the call site.
 *
 * The obvious spelling is `(long)ts.tv_sec * 1000000000L + ts.tv_nsec`, and on
 * this shim's target `long` is FOUR bytes (ARM32; docs/05-chroot.md). A
 * nanosecond clock in a `long` therefore wraps every 4.29 s of uptime and reads
 * NEGATIVE for about half of it, and a press stamped in the negative half failed
 * the `down_ns > 0` test at its own release: a real 450 ms finger measured 0 ms.
 * Measured on the unit 2026-09-29 with work/m23clock.py against the kernel's own
 * delivery times, 3 presses in 5 read 0 ms -- one of them an 1100 ms hold -- so
 * the long press reached UTILITY only when the phase happened to favour it.
 *
 * The controls shim keeps the same helper in shimutil.h. The fb shim has no
 * shared clock header and the two families' headers are included together in
 * seven translation units, where two definitions of one static inline collide,
 * so this file carries its own copy -- the same trade syscalls.h makes, and for
 * the same reason.
 *
 * The host suite cannot catch a regression here: the host's `long` is 8 bytes,
 * so both spellings behave. work/m23clock.py, which runs on the target and
 * compares the shim's number with the kernel's, is the test that can. */
static unsigned long long shim_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ULL +
           (unsigned long long)ts.tv_nsec / 1000000ULL;
}

static void read_loop_abs(int fd, const struct point_xform *x)
{
    struct ev_input_event ev;
    int rx = 0, ry = 0, down = 0;
    unsigned long long down_ms = 0;  /* when the press now down began, monotonic */
    int have_down = 0;               /* and that the stamp above is THIS press's */
    int hold_ms = env_int("POINT_MENU_HOLD_MS", MZ_HOLD_FINGER_MS);

    seed_abs_position(fd, &rx, &ry);

    for (;;) {
        ssize_t n;

        /* THE ONE PRESS THIS LOOP MUST NOT BLOCK THROUGH.
         *
         * Everywhere else here the blocking read below *is* the loop: nothing can
         * happen until the panel sends an event. A finger held on a menu button is
         * the exception, and it is the operator's own ask (2026-10-04): *"for
         * holding the MENU to get utility, after two seconds the menu should
         * disappear and it should just go to utility by itself"*. rbp's hold timer
         * runs on the key, and the key cannot go down until the shim knows the
         * finger has been there long enough -- which, if the only wake-up is an
         * event from a finger that is deliberately holding still, is never.
         *
         * So while such a press is outstanding this waits in POINT_MENU_HOLD_TICK_MS
         * slices instead, and asks menu_hold_pending() at each one. menu_pressed()
         * is the gate that keeps this off every other press in the file: it is
         * non-zero only while the panel is open with a button under the finger, so
         * a press that cannot become a hold -- anywhere on the glass, with the panel
         * shut -- is answered by the blocking read exactly as before. */
        if (menu_enabled && hold_ms > 0 && down && have_down && menu_pressed()) {
            int held = (int)(shim_now_ms() - down_ms);
            int button = menu_hold_pending(held, hold_ms);

            if (button) {
                /* Says which of the two paths answered this hold. menu_fire() below
                 * logs the key it sends, but its line looks the same either way, and
                 * "did the panel dismiss itself or did the finger have to lift" is
                 * the whole of what this block changed. */
                pointsrc_log("pointsrc: menu button %d held %dms and the finger is"
                             " still down -> the panel dismissed itself", button, held);
                menu_fire(button, held);
                /* It does not `continue`: the press is not over, and the read below
                 * is what will see the finger leave. menu_pressed() is 0 now, so
                 * this block is behind us for the rest of the press. */
            } else {
                struct pollfd p;

                p.fd = fd;
                p.events = POLLIN;
                p.revents = 0;
                if (real_poll(&p, 1, POINT_MENU_HOLD_TICK_MS) == 0)
                    continue;        /* still holding: look at the clock again */
            }
        }

        n = real_read(fd, &ev, sizeof ev);
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
                int was = st_down;
                long held_ms = 0;
                point_xform_abs(x, rx, ry, &lx, &ly);

                /* The press's duration is stamped at its two edges and nowhere
                 * else. Only a release can fire a menu button, so that is the only
                 * report the number is ever used on -- and reading the clock on
                 * every report would put a clock read in the path this file exists
                 * to keep cheap. `was` is read before st_down is published below,
                 * because that is the previous report's state.
                 *
                 * `have_down`, not `down_ms > 0`, is what says a press is
                 * outstanding: a stamp is a clock reading, and a clock reading is
                 * allowed to be small. (That test is also what the 32-bit wrap
                 * above used to defeat.) */
                if (down != was) {
                    if (down) {
                        down_ms = shim_now_ms();
                        have_down = 1;
                    } else {
                        if (have_down)
                            held_ms = (long)(shim_now_ms() - down_ms);
                        have_down = 0;
                    }
                }
                st_down = down;
                st_cursor_x = lx;
                st_cursor_y = ly;
                pointer_report(down, lx, ly, menu_enabled, (int)held_ms);
                if (log_debug)                    /* Both ends of the transform, because they are not the same
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
    unsigned long long down_ms = 0;

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
                    down_ms = shim_now_ms();
                    down = 1;
                    st_down = 1;
                    pointer_report(1, cx, cy, menu_mouse, 0);
                    if (log_debug)
                        pointsrc_log("pointsrc: rel press at (%d,%d)", cx, cy);
                } else if (!nd && down) {
                    /* Release. A click shorter than the dwell is held open: rbp's
                     * TouchAdValueHysteresis needs a few frames of "down" to
                     * register a tap, and a fast physical click can occupy fewer
                     * than that, so without this the click is simply swallowed.
                     *
                     * `down` is 1 exactly when the press above stamped `down_ms`,
                     * so it is the flag here. The 32-bit wrap this loop shared with
                     * the abs reader made `elapsed_ms` fall back to `dwell_ms` about
                     * half the time -- harmless-looking, but it meant a genuinely
                     * fast click was never recognised as short and so was never
                     * extended, which is the one case this block exists for. */
                    long elapsed_ms = down ? (long)(shim_now_ms() - down_ms) : dwell_ms;
                    if (dwell_ms > 0 && elapsed_ms < dwell_ms) {
                        usleep((useconds_t)(dwell_ms - elapsed_ms) * 1000);
                        if (log_debug)
                            pointsrc_log("pointsrc: rel release held %ldms (dwell %d)",
                                 (long)dwell_ms - elapsed_ms, dwell_ms);
                    }
                    down = 0;
                    st_down = 0;
                    pointer_report(0, cx, cy, menu_mouse, (int)elapsed_ms);
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
                    pointer_report(1, cx, cy, menu_mouse, 0);
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
        /* The device that was down is gone, so the next one's first press is a
         * first press. (Nothing to release: a QUANTIZE gesture is complete at
         * the press.) */
        touch_zone_reset();
        /* The same for the menu, and it matters more here: a panel left open by a
         * finger that went away with the device would be a panel nothing can
         * dismiss -- the gesture that closes it needs a press, and every press is
         * swallowed while it is open. */
        menu_reset();
        if (log_debug)
            pointsrc_log("pointsrc: %s went away; rescanning", path);
    }
    return NULL;
}

int pointsrc_start(void)
{
    log_debug = env_flag("POINT_DEBUG", 0);
    quantize_enabled = env_flag("POINT_QUANTIZE_TAP", 1);
    menu_enabled = env_flag("POINT_MENU", 1);
    menu_mouse = env_flag("POINT_MENU_MOUSE", 0);
    /* menu_key_eff is built even when the menu is off, so the log states the
     * table it would use rather than staying silent about a knob that was set. */
    menu_keys_init();
    if (!menu_enabled)
        menu_reset();      /* off means closed, whatever the state was */

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

/* The menu's state for the status line. "off" reports the *gate*, so it reads the
 * same whether the panel happens to be closed or was left open by an earlier
 * run; whether an open panel is really on the glass is menu_draw.c's question and
 * not one a text field can answer. */
static const char *menu_status_str(char *buf, unsigned long buflen)
{
    if (!menu_enabled)
        return "off";
    if (!menu_is_open())
        return "closed";
    snprintf(buf, buflen, "open,pressed=%d", menu_pressed());
    return buf;
}

void pointsrc_status(char *buf, unsigned long buflen)
{
    const char *kindstr;
    char ms[24];

    switch (st_kind) {
    case POINT_KIND_ABS: kindstr = "abs"; break;
    case POINT_KIND_REL: kindstr = "rel"; break;
    default:             kindstr = "none"; break;
    }

    if (st_kind == POINT_KIND_NONE) {
        snprintf(buf, buflen, "kind=%s (no pointer attached) menu=%s",
                 env_str("POINT_KIND", "auto"), menu_status_str(ms, sizeof ms));
        return;
    }
    snprintf(buf, buflen, "kind=%s dev=%s name='%s' at=(%d,%d) down=%d menu=%s",
             kindstr, st_path, st_name, st_cursor_x, st_cursor_y, st_down,
             menu_status_str(ms, sizeof ms));
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
