/*
 * test_evdev.c -- the evdev reader, with no /dev/input.
 *
 * evdev_io.c is the one module in the controls shim that cannot be exercised on a
 * machine with no keyboard attached: it opens /dev/input/event*, calls ioctl() on
 * the fd, and blocks in poll(). All of those are syscalls, and all of them go
 * through the single `syscall()` symbol in syscalls.h -- which is what makes this
 * test possible without changing a line of the module that ships.
 *
 * THE SEAM IS THE SYSCALL BOUNDARY, NOT THE MODULE. The alternative -- an `ops`
 * struct of function pointers in evdev_io.c -- would put test-only indirection
 * into production code and would leave the real open/ioctl/read/poll path
 * unexercised. Instead the PRODUCTION evdev_io.o is linked unmodified
 * (-Wl,--wrap=syscall) and what is faked is what the kernel would have said.
 * Three things make that work:
 *
 *   - syscalls.h's real_* helpers are `static inline` around syscall(), so
 *     wrapping the public symbol redirects every one of them.
 *   - glibc's *internal* syscalls do not go through that symbol -- measured, not
 *     assumed: a spike whose __wrap_syscall counted calls saw zero from printf and
 *     fflush. So the fake sees evdev_io.c's and shimutil.c's calls and nothing
 *     else, and klog()'s output lands in this file's buffer rather than in
 *     /tmp/knobshim.log.
 *   - the reader runs for real. A real pthread, the real loop, the real deadline
 *     arithmetic, the real held-key bit set. Only the clock and the devices are
 *     simulated.
 *
 * THE CLOCK IS VIRTUAL, because the thing under test is a latency. clock_gettime
 * is wrapped so shim_now_ms() -- the module's only notion of time -- reads a
 * counter this file advances: poll() advances it by its timeout when it blocks and
 * by one simulated millisecond when it returns readable (what a mouse that never
 * stops moving looks like), and usleep() advances it by what it was asked to
 * sleep. A virtual second therefore costs microseconds of real time, and the
 * absolute deadline becomes something a test can hold still and measure -- which
 * is exactly what the hot-plug rule was about. One consequence is worth stating:
 * the reader never actually blocks, so every timing assertion below is made
 * against a stamp the reader itself wrote, never against when this file next got
 * a turn to look.
 *
 * WHAT IS CHECKED, and each of these was a defect measured on the unit:
 *
 *   1. A device plugged in while another is chattering is noticed within
 *      EVDEV_UNSEEN_MS, and the poll timeouts are seen shrinking toward the
 *      deadline. The old loop re-armed a full second on every poll round, so a
 *      mouse at 1000 Hz postponed the scan without bound and a replug needed a
 *      restart.
 *   2. A rescan never closes a live fd, so a release queued between two rescan
 *      boundaries still arrives -- and the node was opened exactly once. evdev
 *      discards a closed client's queue, so the old "close everything, then
 *      reopen everything" rescan silently dropped a release and left map_kbd.c's
 *      held-key latch set: the operator's "dropped", and the reason a key died
 *      after one use.
 *   3. An unplug releases the keys the reader believes that device is holding,
 *      closes only that fd, and leaves the others open and working.
 *   4. With nothing open the reader does not spin: a bounded number of openat()
 *      calls per virtual second, and they are spent in a sleep, not a busy loop.
 *   5. KBD_DEV pins the reader to one node -- no other node is opened at all --
 *      and the pin is re-probed rather than assumed, so it survives a replug.
 *
 * WHAT IS NOT CHECKED, and cannot be from here:
 *   - whether a real kernel reports what this fake reports. That is what the
 *     on-unit steps in docs/16-input-and-hotplug.md are for.
 *   - map_kbd.c. What is checked here is that the reader EMITS the synthetic
 *     release; that the map then treats it as an ordinary release is test_kbd.c's
 *     business. The two together pin the whole path.
 *   - the MIDI side and the audio reopen. test_midi.c and test_audio.c own those.
 *
 * EACH SCENARIO RUNS IN A FORKED CHILD, because evdev_io.c's state is static and
 * evdev_start() is once-per-process by design (correctly: it starts a thread that
 * never returns). A child is the only way to give each scenario a virgin reader.
 * The parent starts no thread, so it stays single-threaded and forking is safe at
 * any point, and a scenario that segfaults is one failed scenario rather than a
 * lost suite.
 *
 * Build + run (static, so there is no rootfs to load and no device to open):
 *     make -C scripts/shims test
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "evdev_io.h"

/* ==========================================================================
 * The virtual clock
 *
 * A 32-bit unsigned on purpose: it is written by the reader's thread while this
 * one reads it, and an aligned 32-bit load cannot tear. The suite runs for a few
 * seconds of virtual time, so a rollover at 49 days is not a concern.
 * ========================================================================== */
static volatile unsigned long g_virt_ms;

/* Give the reader a turn. This must be a REAL sleep: usleep() is wrapped below,
 * so the test's own yields go through nanosleep, which is not. */
static void yield_real(void)
{
    struct timespec ts;

    ts.tv_sec = 0;
    ts.tv_nsec = 500000;      /* 500 us */
    nanosleep(&ts, NULL);
}

/* ==========================================================================
 * The kernel's input ABI, as much of it as this fake needs
 * ========================================================================== */
#define FAKE_EV_KEY     1
#define FAKE_EV_REL     2
#define FAKE_KEY_SPACE  57
#define FAKE_KEY_Z      44
#define FAKE_REL_X      0

/* The layout evdev_io.c pins with its own _Static_assert: on arm32
 * struct input_event is 16 bytes whichever __USE_TIME_BITS64 branch the build
 * headers take. A wrong size here would not fail loudly, it would make every
 * read() short and the reader would never dispatch a single event. */
struct fake_event {
    unsigned sec, usec;
    unsigned short type, code;
    int value;
};

_Static_assert(sizeof(struct fake_event) == 16, "fake event layout");

/* ==========================================================================
 * The faked devices
 * ========================================================================== */
#define FAKE_MAX 8
#define FAKE_Q   64

struct fake_dev {
    char path[32];
    char name[32];
    int  present;        /* openat() on this path succeeds */
    int  evkey;          /* EVIOCGBIT(0) reports EV_KEY */
    int  dead;           /* opened, then unplugged: read() returns 0 (EOF) */
    int  refill;         /* queue one EV_REL per poll round: a chatty device */
    int  fd;             /* the fd the reader holds, -1 when not open */
    int  opens;          /* how many times the reader has opened this node */
    struct fake_event q[FAKE_Q];
    int  qn;             /* entries live in q[0..qn-1] */
};

static struct fake_dev devs[FAKE_MAX];
static int n_devs;

/* Fake fds for the input nodes, and one for the log. Both are far above any fd
 * this test itself holds, so a mix-up is a failed lookup and not a coincidence. */
static int next_fake_fd = 300;
static int log_fake_fd = 900;

static int dev_add(const char *path, const char *name, int evkey, int refill,
                   int present)
{
    struct fake_dev *d;

    if (n_devs >= FAKE_MAX)
        return -1;
    d = &devs[n_devs];
    memset(d, 0, sizeof *d);
    snprintf(d->path, sizeof d->path, "%s", path);
    snprintf(d->name, sizeof d->name, "%s", name);
    d->present = present;
    d->evkey = evkey;
    d->refill = refill;
    d->fd = -1;
    return n_devs++;
}

static int dev_find_path(const char *path)
{
    for (int i = 0; i < n_devs; i++)
        if (strcmp(devs[i].path, path) == 0)
            return i;
    return -1;
}

static int dev_find_fd(int fd)
{
    for (int i = 0; i < n_devs; i++)
        if (fd >= 0 && devs[i].fd == fd)
            return i;
    return -1;
}

/* Unplug: the node stops existing (openat fails from now on) and any fd on it
 * reports EOF rather than queued data. Both halves matter -- the reader has to
 * see the loss AND be able to find the node again after a replug. */
static void dev_unplug(int i)
{
    devs[i].present = 0;
    devs[i].dead = 1;
}

static void dev_replug(int i)
{
    devs[i].present = 1;
    devs[i].dead = 0;
    devs[i].fd = -1;
}

static void dev_queue(int i, unsigned short type, unsigned short code, int value)
{
    struct fake_dev *d = &devs[i];

    if (!d->present || d->qn >= FAKE_Q)
        return;
    d->q[d->qn].type = type;
    d->q[d->qn].code = code;
    d->q[d->qn].value = value;
    d->qn++;
}

/* ==========================================================================
 * What the reader did -- the counters and the log
 *
 * A close of a node that is STILL PRESENT is defect 2. A close of one that is
 * gone is the correct teardown, and drop_dead() is the only place allowed to do
 * it, which is what makes this counter a direct test of the first rule.
 * ========================================================================== */
static volatile int closed_while_present;
static volatile int openat_input;      /* openat() calls under /dev/input */

/* The smallest poll timeout the reader asked for, over the whole scenario. A
 * reader that re-armed its full second on every round could never ask for less
 * than that second, so this is a one-number answer to "is the deadline absolute?" */
static volatile int poll_min_to = 1 << 30;

#define LOG_MAX 8192
static char log_buf[LOG_MAX];
static volatile int log_len;

/* A snapshot of the log so far. The reader appends to log_buf from its own
 * thread, so this file never reads it in place: it copies up to the published
 * length and terminates its own copy. The writer publishes log_len last, so a
 * length this file has seen implies the bytes before it are there. */
static char log_view[LOG_MAX];

static void log_snapshot(void)
{
    int n = log_len;

    if (n < 0)
        n = 0;
    if (n > LOG_MAX - 1)
        n = LOG_MAX - 1;
    if (n > 0)
        memcpy(log_view, log_buf, (size_t)n);
    log_view[n] = '\0';
}

static int log_has(const char *needle)
{
    log_snapshot();
    return strstr(log_view, needle) != NULL;
}

/* The millisecond stamp the reader puts on its own lines -- EVLOG's "[%llu]".
 * Pulling it out is the point of the stamp existing: the latency of a hot plug is
 * a number the module wrote down, not something this test has to infer from when
 * it next got a turn to look. Returns -1 while the line is not there. */
static long log_stamp(const char *needle)
{
    char *at, *open = NULL, *end;
    unsigned long long v;

    log_snapshot();
    at = strstr(log_view, needle);
    if (!at)
        return -1;
    for (char *p = log_view; p + 6 <= at; p++)
        if (strncmp(p, "evdev[", 6) == 0)
            open = p;                  /* the nearest one at or before the line */
    if (!open)
        return -1;
    errno = 0;
    v = strtoull(open + 6, &end, 10);
    if (end == open + 6 || errno != 0)
        return -1;
    return (long)v;
}

/* ==========================================================================
 * The faked syscall boundary
 *
 * The __real_* half of each wrap is supplied by the linker (that is what
 * --wrap= does), so these two declarations are the whole of the interface and
 * there is no header to include.
 * ========================================================================== */
extern long __real_syscall(long number, ...);
extern int __real_clock_gettime(clockid_t id, struct timespec *ts);

static void fake_poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
    int readable = 0;

    if (timeout > 0 && timeout < poll_min_to)
        poll_min_to = timeout;

    for (nfds_t i = 0; i < nfds; i++) {
        int di = dev_find_fd(fds[i].fd);

        fds[i].revents = 0;
        if (di < 0) {
            fds[i].revents = POLLNVAL;      /* an fd this fake did not hand out */
            readable++;
            continue;
        }
        if (devs[di].dead) {
            fds[i].revents = POLLHUP;       /* how a vanished node shows up */
            readable++;
            continue;
        }
        if (devs[di].refill && devs[di].qn == 0)
            dev_queue(di, FAKE_EV_REL, FAKE_REL_X, 1);
        if (devs[di].qn > 0) {
            fds[i].revents = POLLIN;
            readable++;
        }
    }

    /* A poll that returns readable costs almost no time -- that is what a moving
     * mouse does, and modelling it is the whole point of scenario 1: the old
     * reader re-armed its full second on every such round. The 1 ms step is also
     * what keeps a permanently readable device from stopping the virtual clock
     * dead, so a hot plug behind it still has a deadline to arrive at. */
    if (timeout == 0 || readable)
        g_virt_ms += 1;
    else
        g_virt_ms += (unsigned long)timeout;
}

long __wrap_syscall(long number, ...)
{
    va_list ap;
    long r = -1;

    va_start(ap, number);
    switch (number) {
    case SYS_openat: {
        int dirfd = va_arg(ap, int);
        const char *path = va_arg(ap, const char *);
        int flags = va_arg(ap, int);
        unsigned int mode = va_arg(ap, unsigned int);
        int di;

        (void)dirfd; (void)flags; (void)mode;
        if (path && strncmp(path, "/dev/input/", 11) == 0) {
            openat_input++;
            di = dev_find_path(path);
            if (di < 0 || !devs[di].present) {
                errno = ENOENT;
                r = -1;
                break;
            }
            devs[di].fd = next_fake_fd++;
            devs[di].opens++;
            r = devs[di].fd;
            break;
        }
        /* Everything else is the log: klog() opens it once and keeps the fd, so
         * one fake fd answers every open this test will ever see. */
        r = log_fake_fd;
        break;
    }
    case SYS_ioctl: {
        int fd = va_arg(ap, int);
        unsigned long req = va_arg(ap, unsigned long);
        void *arg = va_arg(ap, void *);
        /* _IOC's layout: nr is bits 0-7 and the TYPE byte is bits 8-15. The size
         * field is bits 16-29, which is where both of these requests carry their
         * buffer length. Reading the type byte as the nr is the mistake that makes
         * every ioctl answer ENOTTY -- and open_source() then closes the node it
         * just opened, so the reader looks like it is opening and losing devices
         * it never really had. */
        unsigned int ioctl_type = (unsigned int)((req >> 8) & 0xff);
        unsigned int ioctl_nr = (unsigned int)(req & 0xff);
        unsigned int len = (unsigned int)((req >> 16) & 0x3fff);
        int di = dev_find_fd(fd);

        if (di < 0 || ioctl_type != 0x45) {      /* 0x45 = 'E' */
            errno = ENOTTY;
            r = -1;
            break;
        }
        switch (ioctl_nr) {
        case 0x20:                       /* EVIOCGBIT(0, len) */
            memset(arg, 0, len < 8 ? len : 8);
            if (devs[di].evkey && len >= 1)
                ((unsigned char *)arg)[0] |= 0x02;   /* EV_KEY */
            r = (long)(len < 8 ? len : 8);
            break;
        case 0x06:                       /* EVIOCGNAME(len) */
            snprintf((char *)arg, len ? len : 1, "%s", devs[di].name);
            r = (long)strlen((char *)arg) + 1;
            break;
        default:
            errno = ENOTTY;
            r = -1;
            break;
        }
        break;
    }
    case SYS_read: {
        int fd = va_arg(ap, int);
        void *buf = va_arg(ap, void *);
        size_t n = va_arg(ap, size_t);
        int di = dev_find_fd(fd);

        if (di < 0 || n < sizeof(struct fake_event)) {
            errno = EBADF;
            r = -1;
            break;
        }
        if (devs[di].dead) {
            r = 0;                       /* EOF: what an unplug looks like */
            break;
        }
        if (devs[di].qn == 0) {
            errno = EAGAIN;              /* nothing queued */
            r = -1;
            break;
        }
        memcpy(buf, &devs[di].q[0], sizeof devs[di].q[0]);
        memmove(&devs[di].q[0], &devs[di].q[1],
                (size_t)(devs[di].qn - 1) * sizeof devs[di].q[0]);
        devs[di].qn--;
        r = (long)sizeof devs[di].q[0];
        break;
    }
    case SYS_write: {
        int fd = va_arg(ap, int);
        const void *buf = va_arg(ap, const void *);
        size_t n = va_arg(ap, size_t);

        if (fd == log_fake_fd) {
            size_t room = (size_t)(LOG_MAX - 1 - log_len);

            if (n > room)
                n = room;
            if (n > 0) {
                memcpy(log_buf + log_len, buf, n);
                log_buf[log_len + (int)n] = '\0';
                log_len += (int)n;       /* published last: see log_snapshot */
            }
        }
        r = (long)n;
        break;
    }
    case SYS_close: {
        int fd = va_arg(ap, int);
        int di = dev_find_fd(fd);

        if (di >= 0) {
            /* Closing a device that is still there is the defect this whole
             * suite is built around. */
            if (devs[di].present)
                closed_while_present++;
            devs[di].fd = -1;
            /* evdev drops a closed client's queue: this is the mechanism behind
             * the lost release, the one the old rescan triggered every second. */
            devs[di].qn = 0;
        }
        r = 0;
        break;
    }
    case SYS_poll: {
        struct pollfd *fds = va_arg(ap, struct pollfd *);
        nfds_t nfds = va_arg(ap, nfds_t);
        int timeout = va_arg(ap, int);

        if (nfds > 0) {
            fake_poll(fds, nfds, timeout);
            r = 0;
            for (nfds_t i = 0; i < nfds; i++)
                if (fds[i].revents)
                    r++;
        } else {
            /* No fds at all. The shipped reader does not do this -- an empty
             * table has its own branch -- but the fake must not answer with a
             * count that would make a caller skip its sleep. */
            g_virt_ms += timeout > 0 ? (unsigned long)timeout : 1;
            r = 0;
        }
        break;
    }
    default:
        va_end(ap);
        /* Anything this file does not model goes to the real kernel. Neither
         * evdev_io.c nor shimutil.c does that today; the branch is here so that a
         * future call is loud rather than silently answering -1. */
        return __real_syscall(number);
    }
    va_end(ap);
    return r;
}

int __wrap_usleep(useconds_t usec)
{
    /* The reader's only sleeps are the empty-table one and the poll-error one.
     * Both must add the time they claim or the deadline arithmetic cannot be
     * tested; neither should cost real time. The yield is so that this thread is
     * not the only one running. */
    g_virt_ms += (unsigned long)(usec / 1000);
    yield_real();
    return 0;
}

int __wrap_clock_gettime(clockid_t id, struct timespec *ts)
{
    if (id != CLOCK_MONOTONIC)
        return __real_clock_gettime(id, ts);
    ts->tv_sec = (time_t)(g_virt_ms / 1000);
    ts->tv_nsec = (long)(g_virt_ms % 1000) * 1000000L;
    return 0;
}

/* ==========================================================================
 * What the reader handed the map
 *
 * Only EV_KEY events are recorded, on purpose: scenario 1 deliberately floods the
 * handler with EV_REL from a chatty device, and a record those could fill is a
 * record that would then miss the keys. The total count is kept too, because
 * "the chatty device is being read at all" is worth asserting.
 * ========================================================================== */
struct seen { int type, code, value; };
#define SEEN_MAX 64

static volatile int seen_n;                 /* every event the handler saw */
static volatile struct seen keys[SEEN_MAX];
static volatile int key_n;

static void on_event(int type, int code, int value)
{
    int n;

    seen_n++;
    if (type != FAKE_EV_KEY)
        return;
    n = key_n;
    if (n >= SEEN_MAX)
        return;
    keys[n].type = type;
    keys[n].code = code;
    keys[n].value = value;
    key_n = n + 1;              /* published last: readers look at key_n first */
}

static int key_count(int type, int code, int value)
{
    int n = 0;

    for (int i = 0; i < key_n; i++)
        if (keys[i].type == type && keys[i].code == code &&
            keys[i].value == value)
            n++;
    return n;
}

/* Where the first event matching (type, code, value) landed, or -1. Used to pin
 * that a release follows its press rather than overtaking it. */
static int key_first(int type, int code, int value)
{
    for (int i = 0; i < key_n; i++)
        if (keys[i].type == type && keys[i].code == code &&
            keys[i].value == value)
            return i;
    return -1;
}

/* ==========================================================================
 * The harness
 * ========================================================================== */
static int checks, failures;

#define CHECK(cond, ...) do {                                       \
        checks++;                                                   \
        if (!(cond)) {                                              \
            failures++;                                             \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);             \
            printf(__VA_ARGS__);                                    \
            printf("\n");                                           \
        }                                                           \
    } while (0)

/* What wait_goal() is waiting for. One slot rather than a family of predicate
 * functions, because every wait in this file is "wait for exactly this" and a
 * named function per case reads as more code than the thing it waits for. */
enum { GO_NONE = 0, GO_SEEN, GO_OPEN, GO_LOG, GO_TRAFFIC };

static int goal_mode;
static int goal_type, goal_code, goal_value;
static int goal_dev = -1;
static const char *goal_text;

static int goal_met(void)
{
    switch (goal_mode) {
    case GO_SEEN:    return key_count(goal_type, goal_code, goal_value) > 0;
    case GO_OPEN:    return goal_dev >= 0 && devs[goal_dev].fd >= 0;
    case GO_LOG:     return goal_text != NULL && log_has(goal_text);
    case GO_TRAFFIC: return seen_n > 0;
    default:         return 0;
    }
}

static void want_seen(int type, int code, int value)
{
    goal_mode = GO_SEEN; goal_type = type; goal_code = code; goal_value = value;
}

static void want_open(int dev)
{
    goal_mode = GO_OPEN; goal_dev = dev;
}

static void want_log(const char *text)
{
    goal_mode = GO_LOG; goal_text = text;
}

static void want_traffic(void)
{
    goal_mode = GO_TRAFFIC;
}

/* Let the reader run until the goal is met. Returns the virtual time it was met
 * at, or 0. The budget is in VIRTUAL milliseconds, because that is the clock the
 * module reasons about; the iteration guard is a real-time backstop, so that a
 * module which stopped advancing the clock fails this suite instead of hanging
 * it. */
static unsigned long wait_goal(unsigned long budget_ms)
{
    unsigned long start = g_virt_ms;
    int guard = 0;

    while (!goal_met()) {
        if (g_virt_ms - start > budget_ms || guard > 6000)
            return 0;
        yield_real();
        guard++;
    }
    return g_virt_ms;
}

/* Let `ms` of virtual time pass with no particular goal: for the scenarios that
 * have to sit through rescan boundaries. Returns whether it got there. */
static int advance_virtual(unsigned long ms)
{
    unsigned long start = g_virt_ms;
    int guard = 0;

    while (g_virt_ms - start < ms && guard < 8000) {
        yield_real();
        guard++;
    }
    return (g_virt_ms - start) >= ms;
}

/* --- 1: a hot plug, under continuous traffic --------------------------------
 *
 * A chatty device is open and moving before the keyboard is plugged in, so every
 * poll round returns readable. A reader that re-armed its "has anything appeared?"
 * second on every round would never reach a scan at all -- that is what the unit
 * did, and why a replug needed a restart -- so the assertion is a bound on how
 * long the plug may go unnoticed, taken from the stamp on the reader's own line.
 */
static void scenario_hot_add(void)
{
    unsigned long t_plug;
    long stamped, delta;
    int mouse, kbd;

    /* Present but not yet: the empty-device state has to be seen first, or the
     * scan below would find the mouse on its very first pass. */
    mouse = dev_add("/dev/input/event2", "fake-mouse", 1, 1 /* chatty */, 0);
    CHECK(mouse >= 0, "the fake device table is full");

    CHECK(evdev_start(on_event) == 0, "evdev_start() refused to start");

    want_log("no device with EV_KEY in");
    CHECK(wait_goal(3000) > 0, "the reader never reported an empty device set");

    devs[mouse].present = 1;
    want_open(mouse);
    CHECK(wait_goal(4000) > 0, "the chatty device was never opened");
    want_traffic();
    CHECK(wait_goal(4000) > 0, "no event arrived from the chatty device");

    /* Sit through a whole scan interval with the device live and moving. */
    CHECK(advance_virtual(1200), "the virtual clock stopped");
    CHECK(devs[mouse].opens == 1, "the chatty device was opened %d time(s): a "
          "rescan closed a live fd", devs[mouse].opens);
    CHECK(poll_min_to <= 200, "the smallest poll timeout asked for was %d ms; a "
          "reader that re-arms its whole second every round cannot ask for less, "
          "and that is what made a hot plug wait for a quiet moment",
          poll_min_to);
    CHECK(closed_while_present == 0, "%d live device(s) were closed during "
          "rescans", closed_while_present);

    /* Now the plug. */
    kbd = dev_add("/dev/input/event0", "fake-kbd", 1, 0, 0);
    CHECK(kbd >= 0, "the fake device table is full");
    t_plug = g_virt_ms;
    devs[kbd].present = 1;

    want_log("'fake-kbd' appeared");
    CHECK(wait_goal(4000) > 0, "a device plugged in under continuous traffic was "
          "never noticed");

    stamped = log_stamp("'fake-kbd' appeared");
    CHECK(stamped > 0, "the reader's 'appeared' line carries no timestamp");
    delta = stamped > 0 ? stamped - (long)t_plug : -1;
    CHECK(delta >= 0 && delta <= 1000, "the hot plug was noticed %ld ms later "
          "(plug at %lu, reader's stamp %ld); traffic must not postpone the scan "
          "past EVDEV_UNSEEN_MS", delta, t_plug, stamped);

    want_open(kbd);
    CHECK(wait_goal(2000) > 0, "the new device was not opened");
    CHECK(closed_while_present == 0, "%d live device(s) were closed during "
          "rescans", closed_while_present);
}

/* --- 2: a rescan never costs a queued release ------------------------------- */
static void scenario_rescan(void)
{
    unsigned long t0;
    int i, press, release;

    i = dev_add("/dev/input/event0", "fake-kbd", 1, 0, 1);
    CHECK(i >= 0, "the fake device table is full");
    CHECK(evdev_start(on_event) == 0, "evdev_start() refused to start");

    want_open(i);
    CHECK(wait_goal(4000) > 0, "the device was never opened");

    dev_queue(i, FAKE_EV_KEY, FAKE_KEY_SPACE, 1);
    want_seen(FAKE_EV_KEY, FAKE_KEY_SPACE, 1);
    CHECK(wait_goal(4000) > 0, "the press never reached the handler");

    /* The release is queued while the reader is between two rescan boundaries.
     * evdev discards a closed client's queue, so under the old "close the whole
     * set, then reopen it" rescan this is the event that was eaten -- and the
     * held-key latch left behind by an eaten release is what made a key dead
     * after one use. */
    t0 = g_virt_ms;
    dev_queue(i, FAKE_EV_KEY, FAKE_KEY_SPACE, 0);
    want_seen(FAKE_EV_KEY, FAKE_KEY_SPACE, 0);
    CHECK(wait_goal(4000) > 0, "a release queued between two rescan boundaries "
          "never arrived");

    /* And it arrived in order, not overtaking its own press. */
    press = key_first(FAKE_EV_KEY, FAKE_KEY_SPACE, 1);
    release = key_first(FAKE_EV_KEY, FAKE_KEY_SPACE, 0);
    CHECK(press >= 0 && release > press, "the release did not follow its press "
          "(press at %d, release at %d)", press, release);

    /* Let two more rescan boundaries go by with the device idle, then check that
     * nothing was torn down in either of them. This is the assertion that pins
     * the rule: the release above could survive by luck, an fd that is opened
     * exactly once cannot. */
    CHECK(advance_virtual(2500), "the virtual clock stopped after %lu ms",
          g_virt_ms - t0);
    CHECK(devs[i].opens == 1, "the node was opened %d time(s): a rescan closed "
          "and reopened a live device", devs[i].opens);
    CHECK(closed_while_present == 0, "%d live device(s) were closed during "
          "rescans", closed_while_present);
}

/* --- 3: an unplug releases what it was holding ------------------------------ */
static void scenario_unplug(void)
{
    int a, b;

    a = dev_add("/dev/input/event0", "fake-kbd", 1, 0, 1);
    b = dev_add("/dev/input/event2", "fake-mouse", 1, 0, 1);
    CHECK(a >= 0 && b >= 0, "the fake device table is full");
    CHECK(evdev_start(on_event) == 0, "evdev_start() refused to start");

    want_open(a);
    CHECK(wait_goal(4000) > 0, "%s was never opened", devs[a].path);
    want_open(b);
    CHECK(wait_goal(4000) > 0, "%s was never opened", devs[b].path);

    /* Hold a key on A. */
    dev_queue(a, FAKE_EV_KEY, FAKE_KEY_SPACE, 1);
    want_seen(FAKE_EV_KEY, FAKE_KEY_SPACE, 1);
    CHECK(wait_goal(4000) > 0, "the press on %s never reached the handler",
          devs[a].path);

    /* Pull it out while that key is down. The kernel sends no release for a key
     * whose device has gone, so the reader has to synthesise one, or the map's
     * latch stays set and the key is dead until rbp restarts. */
    dev_unplug(a);
    want_seen(FAKE_EV_KEY, FAKE_KEY_SPACE, 0);
    CHECK(wait_goal(3000) > 0, "the key held when its device went away was never "
          "released: the map would keep it latched down and swallow the next "
          "press");
    want_log("went away");
    CHECK(wait_goal(3000) > 0, "the reader never reported the loss");
    CHECK(log_has("released 1 held key(s)"), "the reader did not report the "
          "synthetic release");

    /* That line is written after the close, so what follows is the post-mortem. */
    CHECK(devs[a].fd == -1, "the dead device's fd is still recorded as open");
    CHECK(devs[b].fd >= 0, "the surviving device was closed too");
    CHECK(closed_while_present == 0, "%d live device(s) were closed",
          closed_while_present);

    /* And the survivor still works. */
    dev_queue(b, FAKE_EV_KEY, FAKE_KEY_Z, 1);
    want_seen(FAKE_EV_KEY, FAKE_KEY_Z, 1);
    CHECK(wait_goal(3000) > 0, "the surviving device stopped delivering events");
}

/* --- 4: nothing plugged in is quiet, not a spin ----------------------------- */
static void scenario_empty(void)
{
    CHECK(evdev_start(on_event) == 0, "evdev_start() refused to start");
    CHECK(advance_virtual(3000), "the virtual clock stopped");

    /* Six 500 ms looks in three virtual seconds, each looking at 32 nodes. The
     * lower bound catches a reader that stopped looking at all; the upper one
     * catches a reader that is spinning -- a busy loop would make thousands. */
    CHECK(openat_input >= 32, "only %d openat() call(s) in 3 s: the reader is not "
          "looking for devices at all", openat_input);
    CHECK(openat_input <= 256, "the reader made %d openat() calls in 3 s of "
          "virtual time with nothing plugged in: it is spinning", openat_input);
    CHECK(log_has("no device with EV_KEY in"), "the empty device set was never "
          "reported");
    CHECK(closed_while_present == 0, "%d device(s) were closed when none was "
          "ever open", closed_while_present);
}

/* --- 5: KBD_DEV pins the reader, and the pin is re-probed ------------------- */
static void scenario_pin(void)
{
    int other, pin;

    CHECK(setenv("KBD_DEV", "/dev/input/event3", 1) == 0, "setenv failed");
    other = dev_add("/dev/input/event2", "fake-other", 1, 0, 1);
    pin = dev_add("/dev/input/event3", "fake-pinned", 1, 0, 1);
    CHECK(other >= 0 && pin >= 0, "the fake device table is full");
    CHECK(evdev_start(on_event) == 0, "evdev_start() refused to start");

    want_open(pin);
    CHECK(wait_goal(4000) > 0, "the pinned node was never opened");
    CHECK(devs[other].fd == -1 && devs[other].opens == 0,
          "KBD_DEV=/dev/input/event3 still opened %s %d time(s): a pin must admit "
          "only that node", devs[other].path, devs[other].opens);

    dev_queue(pin, FAKE_EV_KEY, FAKE_KEY_SPACE, 1);
    want_seen(FAKE_EV_KEY, FAKE_KEY_SPACE, 1);
    CHECK(wait_goal(3000) > 0, "no event arrived from the pinned node");

    /* A pinned keyboard that is unplugged and replugged must be picked up again:
     * the pin is re-probed, not assumed. */
    dev_unplug(pin);
    want_seen(FAKE_EV_KEY, FAKE_KEY_SPACE, 0);
    CHECK(wait_goal(3000) > 0, "the pinned device's held key was not released "
          "when it was unplugged");
    want_log("went away");
    CHECK(wait_goal(3000) > 0, "the pinned device's loss was not reported");

    dev_replug(pin);
    want_open(pin);
    CHECK(wait_goal(4000) > 0, "a pinned node that was unplugged and replugged "
          "was never picked up again");
    CHECK(devs[pin].opens == 2, "the pinned node was opened %d time(s), expected "
          "2 (once, then once more after the replug)", devs[pin].opens);
    CHECK(devs[other].opens == 0, "the replug opened %s too", devs[other].path);
}

/* ========================================================================== */

struct scenario {
    const char *name;
    void (*fn)(void);
};

static const struct scenario scenarios[] = {
    { "hot-add under traffic", scenario_hot_add },
    { "rescan keeps the fd",   scenario_rescan  },
    { "unplug releases keys",  scenario_unplug  },
    { "nothing plugged in",    scenario_empty   },
    { "KBD_DEV pinning",       scenario_pin     },
};

int main(void)
{
    int n = (int)(sizeof scenarios / sizeof scenarios[0]);
    int failed = 0;

    /* Every scenario but the last scans the node range, which is the opposite of
     * what a pin is for. Cleared once here rather than in each child. */
    unsetenv("KBD_DEV");

    for (int i = 0; i < n; i++) {
        pid_t pid;
        int st;

        printf("-- %s\n", scenarios[i].name);
        fflush(stdout);

        pid = fork();
        if (pid < 0) {
            printf("test_evdev: fork: %s\n", strerror(errno));
            return 1;
        }
        if (pid == 0) {
            /* A virgin reader: evdev_io.c's state is static and evdev_start() is
             * once-per-process, so this is the only way to run it five times. */
            checks = failures = 0;
            g_virt_ms = 1000;      /* so a wait's 0 return unambiguously means no */
            scenarios[i].fn();
            printf("   %d checks, %d failures\n", checks, failures);
            if (failures > 0) {
                /* The reader's own account of what it did, which is where a
                 * failure's cause usually is: every add and every drop it makes
                 * is one line here. Printed only on failure so a passing run
                 * stays three lines per scenario. */
                log_snapshot();
                printf("   -- the reader's log --\n%s", log_view);
            }
            fflush(stdout);
            _exit(failures == 0 ? 0 : 1);
        }

        if (waitpid(pid, &st, 0) != pid) {
            printf("test_evdev: waitpid: %s\n", strerror(errno));
            return 1;
        }
        if (!WIFEXITED(st)) {
            printf("   scenario died on signal %d\n", WTERMSIG(st));
            failed++;
        } else if (WEXITSTATUS(st) != 0) {
            failed++;
        }
    }

    printf("test_evdev: %d scenario(s), %d failed\n", n, failed);
    return failed == 0 ? 0 : 1;
}
