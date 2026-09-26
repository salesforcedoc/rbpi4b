/*
 * evdev_io.c -- see evdev_io.h.
 *
 * Layout: discovery (every /dev/input/event* with EV_KEY) -> one poll() loop
 * over all of them -> the handler. The reader thread wraps discovery in a retry
 * loop, so "nothing plugged in yet" and "a device was unplugged" are the same
 * state and a replug needs no special case -- the same shape fbshim.so's
 * pointsrc.c uses for the pointer.
 *
 * Two things about that loop are load-bearing, and both are fixes rather than
 * design choices (docs/16-input-and-hotplug.md):
 *
 *  - Devices are opened ONCE and left open. The loop used to run its discovery
 *    at the top of every iteration, so every poll round closed and reopened
 *    every node; evdev queues events per open client, so a key release that
 *    landed in that window was discarded, map_kbd.c's held-key latch stayed set
 *    and the next press of that key was ignored. A human press is ~100 ms and
 *    was reliably lost.
 *  - poll() has a deadline (EVDEV_IDLE_MS), on which the device set is
 *    re-checked. A hot-plugged device is otherwise never noticed: with no
 *    events there is no iteration to notice it in.
 *
 * Everything here is `static` except evdev_start(): the objects in this shim are
 * compiled -fvisibility=hidden, and nothing in this file belongs in the dynamic
 * symbol table (see the Makefile's note on knobshim.so's 17 exports).
 */
#define _GNU_SOURCE
#include "evdev_io.h"
#include "shimutil.h"     /* klog, env_text */
#include "syscalls.h"     /* real_open/read/ioctl/poll/close */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* --- the kernel's input ABI, declared here rather than taken from
 * <linux/input.h> ------------------------------------------------------------
 *
 * The same reasoning as pointsrc.c's copy: the shims are cross-compiled against
 * headers of one vintage and run against a kernel of another, and
 * <linux/input.h> already selects a different struct input_event between its
 * __USE_TIME_BITS64 branches. These two definitions are the small, stable
 * subset this module needs, so it does not care which branch the build host's
 * headers take. tools/evdevdump.c is the tool that reads the same events
 * through the real header.
 */
struct evdev_event {
    struct { uint32_t sec, usec; } time;   /* 8 bytes on arm32, either branch */
    uint16_t type, code;
    int32_t  value;
};

/* A wrong size here is a silent failure rather than a loud one: read() would
 * return short on every event and the loop below would spin without ever
 * dispatching one. Pin it. */
_Static_assert(sizeof(struct evdev_event) == 16,
               "unexpected struct input_event layout for arm32");

#define EV_KEY 0x01

/* _IOC(_IOC_READ, 'E', nr, size): 'E' is 0x45. EVIOCGBIT(0, len) is the set of
 * event types the device can report, which is the bit set this file filters on.
 * EVIOCGNAME is only here so the log can name what was opened. */
#define EVIOCGNAME(len)     (0x80000000UL | ((unsigned long)(len) << 16) | (0x45UL << 8) | 0x06UL)
#define EVIOCGBIT(ev, len)  (0x80000000UL | ((unsigned long)(len) << 16) | (0x45UL << 8) | (0x20UL + (unsigned long)(ev)))

/* /dev/input/event0..event31, and at most that many devices open at once. A
 * machine with more input devices than this has something wrong with it that a
 * bring-up shim is not the place to fix. */
#define EVDEV_SCAN_MAX 32

#define EVDEV_PATH_MAX 32
#define EVDEV_NAME_MAX 128

/* How long to wait before looking again when there is nothing to read, and how
 * many events to take from one device before giving the others a turn. */
#define EVDEV_RESCAN_MS 500
#define EVDEV_DRAIN_MAX 64

/* How long poll() waits before it reports "nothing happened". This is a tick and
 * not a timeout in the usual sense: nothing is torn down when it expires, it is
 * the moment the device set is re-checked (unseen_source below). Without it the
 * thread parks in poll() for as long as the machine is idle -- which is exactly
 * when someone plugs a keyboard back in. */
#define EVDEV_IDLE_MS 1000

struct evdev_src {
    int  fd;
    char path[EVDEV_PATH_MAX];
};

static struct evdev_src sources[EVDEV_SCAN_MAX];
static int n_sources;

/* Set once, by evdev_start(), before the thread is created. Read-only after
 * that, so no lock: the handler pointer never changes while the thread runs. */
static void (*g_handler)(int type, int code, int value);

/* --- discovery ------------------------------------------------------------- */

static int bit_set(const unsigned char *bits, unsigned int bit)
{
    return (bits[bit >> 3] >> (bit & 7)) & 1;
}

/* Open one node if it can report key events, else -1.
 *
 * EV_KEY is the whole filter, on purpose. It admits the keyboard and the mouse
 * (BTN_LEFT/BTN_RIGHT are EV_KEY codes) and also things like a Pi's gpio-keys
 * power button; the last group is harmless, because an unmapped code is dropped
 * by the map and nothing here ever writes to a device. */
static int open_source(const char *path, char *name_out, unsigned long name_len)
{
    unsigned char evbits[8];
    int fd;

    fd = real_open(path, O_RDONLY | O_NONBLOCK, 0);
    if (fd < 0)
        return -1;

    memset(evbits, 0, sizeof evbits);
    if (real_ioctl(fd, EVIOCGBIT(0, sizeof evbits), evbits) < 0 ||
        !bit_set(evbits, EV_KEY)) {
        real_close(fd);
        return -1;
    }

    name_out[0] = '\0';
    if (real_ioctl(fd, EVIOCGNAME(name_len - 1), name_out) < 0)
        name_out[0] = '\0';
    name_out[name_len - 1] = '\0';
    return fd;
}

static void add_source(int fd, const char *path, const char *name)
{
    if (n_sources >= EVDEV_SCAN_MAX) {
        real_close(fd);
        return;
    }
    if (!name || !name[0])
        name = "(unnamed)";
    sources[n_sources].fd = fd;
    snprintf(sources[n_sources].path, sizeof sources[n_sources].path, "%s", path);
    n_sources++;
    klog("knobshim2: evdev: %s '%s'\n", path, name);
}

/* Open every input device that can report key events, or the single node the
 * environment pinned. Returns how many are open now. Only ever called with the
 * table empty -- the reader closes it before re-scanning. */
static int scan_sources(const char *pin)
{
    int i;

    if (pin) {
        char name[EVDEV_NAME_MAX];
        int fd = open_source(pin, name, sizeof name);
        if (fd >= 0)
            add_source(fd, pin, name);
        return n_sources;
    }

    for (i = 0; i < EVDEV_SCAN_MAX && n_sources < EVDEV_SCAN_MAX; i++) {
        char path[EVDEV_PATH_MAX];
        char name[EVDEV_NAME_MAX];
        int fd;

        snprintf(path, sizeof path, "/dev/input/event%d", i);
        fd = open_source(path, name, sizeof name);
        if (fd >= 0)
            add_source(fd, path, name);
    }
    return n_sources;
}

static void close_sources(void)
{
    for (int i = 0; i < n_sources; i++)
        real_close(sources[i].fd);
    n_sources = 0;
}

/* --- reading --------------------------------------------------------------- */

/* Take what is readable from one device. Returns 0 when it is still usable
 * (drained, or nothing queued), -1 when it has to be re-discovered: a short read
 * or EOF means the node is gone -- that is how an unplug shows up -- and
 * anything else that is not EINTR/EAGAIN is a device the kernel has given up on.
 *
 * The per-round cap is what keeps one chatty device (a mouse at 1000 Hz) from
 * starving the others; the next poll() round picks up where this one stopped. */
static int drain_source(int idx)
{
    struct evdev_event ev;
    int taken = 0;

    while (taken < EVDEV_DRAIN_MAX) {
        ssize_t n = real_read(sources[idx].fd, &ev, sizeof ev);

        if (n == (ssize_t)sizeof ev) {
            if (g_handler)
                g_handler(ev.type, ev.code, ev.value);
            taken++;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && errno == EAGAIN)
            return 0;          /* nothing more queued */
        return -1;             /* EOF, a short read, or a real error */
    }
    return 0;
}

/* One poll() round over every open device. Returns -1 when one of them has to
 * be re-discovered, 1 when the deadline passed with nothing to read, 0
 * otherwise -- events handled, or the poll was interrupted by a signal, which
 * is not a reason to tear anything down. The caller must not rescan on 0 or 1
 * (see the top of this file): only -1 means the open set is wrong. */
static int poll_round(int timeout_ms)
{
    struct pollfd pfd[EVDEV_SCAN_MAX];
    int r;

    for (int i = 0; i < n_sources; i++) {
        pfd[i].fd = sources[i].fd;
        pfd[i].events = POLLIN;
        pfd[i].revents = 0;
    }

    r = real_poll(pfd, (nfds_t)n_sources, timeout_ms);
    if (r < 0) {
        if (errno == EINTR)
            return 0;
        klog("knobshim2: evdev: poll: %s; rescanning\n", strerror(errno));
        return -1;
    }
    if (r == 0)
        return 1;              /* the deadline: nothing to read */

    for (int i = 0; i < n_sources; i++) {
        if (pfd[i].revents & POLLNVAL) {
            klog("knobshim2: evdev: %s: not a valid fd any more; rescanning\n",
                 sources[i].path);
            return -1;
        }
        if (pfd[i].revents & (POLLIN | POLLERR | POLLHUP)) {
            if (drain_source(i) < 0) {
                klog("knobshim2: evdev: %s went away; rescanning\n",
                     sources[i].path);
                return -1;
            }
        }
    }
    return 0;
}

/* Is there a device carrying EV_KEY that this reader does not have open?
 *
 * Called on the idle tick, and cheap because it only opens what we do NOT
 * already hold: the nodes being read are left alone (closing one is what loses
 * a queued release), and a node we keep rejecting -- the HDMI CEC interfaces
 * have no EV_KEY -- costs an open and one ioctl each time.
 *
 * A pinned KBD_DEV has nothing to discover: the only change that matters is
 * holding some other node, which the caller's scan already fails on. */
static int unseen_source(const char *pin)
{
    if (pin)
        return n_sources != 1 || strcmp(sources[0].path, pin) != 0;

    for (int i = 0; i < EVDEV_SCAN_MAX; i++) {
        char path[EVDEV_PATH_MAX];
        char name[EVDEV_NAME_MAX];
        int fd, have = 0;

        snprintf(path, sizeof path, "/dev/input/event%d", i);
        for (int s = 0; s < n_sources; s++)
            if (strcmp(sources[s].path, path) == 0) {
                have = 1;
                break;
            }
        if (have)
            continue;

        /* Opening it is the test: open_source() rejects anything without
         * EV_KEY, which is the same filter discovery uses. */
        fd = open_source(path, name, sizeof name);
        if (fd >= 0) {
            real_close(fd);
            klog("knobshim2: evdev: %s '%s' appeared; rescanning\n", path, name);
            return 1;
        }
    }
    return 0;
}

static void *evdev_thread(void *arg)
{
    const char *pin = env_text("KBD_DEV", NULL);
    /* 1 until the first empty scan reports, so an input-less machine says so
     * once instead of either silently or every 500 ms. */
    int had_sources = 1;
    (void)arg;

    if (pin)
        klog("knobshim2: evdev: KBD_DEV=%s: reading that node instead of scanning\n",
             pin);

    for (;;) {
        close_sources();
        if (scan_sources(pin) <= 0) {
            if (had_sources)
                klog("knobshim2: evdev: no device with EV_KEY in %s; rescanning\n",
                     pin ? pin : "/dev/input/event0..31");
            had_sources = 0;
            usleep(EVDEV_RESCAN_MS * 1000);
            continue;
        }
        had_sources = 1;

        /* Read until something goes wrong. A round that read events, or that
         * found nothing to read, leaves the devices open: the only rescan is
         * the idle tick's answer to "has a device appeared?", and the failure
         * path. */
        for (;;) {
            int r = poll_round(EVDEV_IDLE_MS);

            if (r < 0)
                break;                       /* rescan */
            if (r == 1 && unseen_source(pin))
                break;                       /* a device appeared: rescan */
        }
        usleep(EVDEV_RESCAN_MS * 1000);
    }
    return NULL;
}

int evdev_start(void (*on_event)(int type, int code, int value))
{
    static int started;
    static pthread_t tid;

    if (started)
        return 0;
    if (!on_event)
        return -1;
    g_handler = on_event;
    if (pthread_create(&tid, NULL, evdev_thread, NULL) != 0)
        return -1;
    /* Detached like the front end's other threads: it never returns, and the
     * process it lives in is rbp. */
    pthread_detach(tid);
    started = 1;
    return 0;
}
